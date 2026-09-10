/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Scheduler Tool - LLM tool for creating/managing timers, alarms, reminders
 *
 * Actions: create, list, cancel, query, update, snooze, dismiss
 * The "details" parameter is a JSON string with action-specific fields.
 */

#include "tools/scheduler_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/iso8601.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* RESULT_BUF_SIZE is the stack buffer used by SHORT, FIXED-FORMAT response
 * paths only (handle_create result, handle_cancel/dismiss/snooze status,
 * error strings).  The unbounded handle_list path uses strbuf instead — it
 * cannot silently truncate when the user has many active events. */
#define RESULT_BUF_SIZE 2048
#define MAX_DURATION_MINUTES 43200 /* 30 days */
#define MAX_SNOOZE_MINUTES 120

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *scheduler_tool_callback(const char *action, char *value, int *should_respond);
static int scheduler_tool_init(void);
static void scheduler_tool_cleanup(void);

/* =============================================================================
 * JSON Helpers
 * ============================================================================= */

static const char *json_get_string(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int default_val) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return default_val;
   return json_object_get_int(val);
}

static bool json_get_bool(struct json_object *obj, const char *key, bool default_val) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return default_val;
   return json_object_get_boolean(val);
}

/* True if csv is a well-formed recurrence_days list: a comma-separated set of
 * day names (sun..sat), at least one, no duplicates.  Shared by create + update. */
static bool valid_recurrence_days_csv(const char *csv) {
   if (!csv || !csv[0])
      return false;
   static const char *valid_days[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
   char buf[SCHED_RECURRENCE_DAYS_MAX];
   strncpy(buf, csv, sizeof(buf) - 1);
   buf[sizeof(buf) - 1] = '\0';

   bool valid = true;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, ",", &saveptr);
   int day_count = 0;
   uint8_t seen = 0; /* bitmask for duplicate detection */
   while (tok && valid) {
      while (*tok == ' ')
         tok++;
      bool found = false;
      for (int d = 0; d < 7; d++) {
         if (strcasecmp(tok, valid_days[d]) == 0) {
            if (seen & (1 << d)) {
               valid = false; /* duplicate */
            } else {
               seen |= (1 << d);
               found = true;
               day_count++;
            }
            break;
         }
      }
      if (!found)
         valid = false;
      tok = strtok_r(NULL, ",", &saveptr);
   }
   return valid && day_count > 0;
}

/* Parse + validate a briefing `steps` JSON array into out[] (capacity
 * SCHED_BRIEFING_STEPS_MAX).  Shared by handle_create and handle_update so the
 * two never drift.  Accepts the short-key aliases tool/action/value in addition
 * to tool_name/tool_action/tool_value (the LLM intermittently emits the short
 * forms), rejects non-string fields (json_object_get_string would otherwise
 * coerce a nested object into a literal tool_value), and runs every step through
 * tool_registry_validate_schedulable.  On failure writes a complete "Error: ..."
 * message to err and returns FAILURE; on success sets *count_out and returns
 * SUCCESS.  steps_arr must be a JSON array. */
static int parse_validate_steps(struct json_object *steps_arr,
                                sched_briefing_step_t *out,
                                int *count_out,
                                char *err,
                                size_t errlen) {
   if (!steps_arr || !json_object_is_type(steps_arr, json_type_array)) {
      snprintf(err, errlen, "Error: 'steps' must be an array");
      return FAILURE;
   }
   int n = (int)json_object_array_length(steps_arr);
   if (n <= 0) {
      snprintf(err, errlen, "Error: 'steps' array is empty");
      return FAILURE;
   }
   if (n > SCHED_BRIEFING_STEPS_MAX) {
      snprintf(err, errlen, "Error: too many steps (%d, max %d).  Split into multiple briefings.",
               n, SCHED_BRIEFING_STEPS_MAX);
      return FAILURE;
   }
   memset(out, 0, sizeof(sched_briefing_step_t) * SCHED_BRIEFING_STEPS_MAX);
   for (int i = 0; i < n; i++) {
      struct json_object *step = json_object_array_get_idx(steps_arr, i);
      if (!step || !json_object_is_type(step, json_type_object)) {
         snprintf(err, errlen, "Error: steps[%d] is not an object", i);
         return FAILURE;
      }
      struct json_object *jname = NULL, *jaction = NULL, *jvalue = NULL;
      json_object_object_get_ex(step, "tool_name", &jname) ||
          json_object_object_get_ex(step, "tool", &jname);
      json_object_object_get_ex(step, "tool_action", &jaction) ||
          json_object_object_get_ex(step, "action", &jaction);
      json_object_object_get_ex(step, "tool_value", &jvalue) ||
          json_object_object_get_ex(step, "value", &jvalue);
      if (jname && !json_object_is_type(jname, json_type_string)) {
         snprintf(err, errlen, "Error: steps[%d].tool_name must be a string", i);
         return FAILURE;
      }
      if (jaction && !json_object_is_type(jaction, json_type_string)) {
         snprintf(err, errlen, "Error: steps[%d].tool_action must be a string", i);
         return FAILURE;
      }
      if (jvalue && !json_object_is_type(jvalue, json_type_string)) {
         snprintf(err, errlen, "Error: steps[%d].tool_value must be a string", i);
         return FAILURE;
      }
      const char *s_name = jname ? json_object_get_string(jname) : NULL;
      const char *s_action = jaction ? json_object_get_string(jaction) : NULL;
      const char *s_value = jvalue ? json_object_get_string(jvalue) : NULL;
      char verr[160];
      if (tool_registry_validate_schedulable(s_name, s_action, s_value, verr, sizeof(verr)) !=
          SUCCESS) {
         snprintf(err, errlen, "Error: steps[%d]: %s", i, verr);
         return FAILURE;
      }
      if (s_value && strlen(s_value) >= SCHED_TOOL_VALUE_MAX) {
         snprintf(err, errlen, "Error: steps[%d] tool_value too long (%zu bytes, max %d)", i,
                  strlen(s_value), SCHED_TOOL_VALUE_MAX - 1);
         return FAILURE;
      }
      strncpy(out[i].tool_name, s_name, SCHED_TOOL_NAME_MAX - 1);
      if (s_action)
         strncpy(out[i].tool_action, s_action, SCHED_TOOL_NAME_MAX - 1);
      if (s_value)
         strncpy(out[i].tool_value, s_value, SCHED_TOOL_VALUE_MAX - 1);
   }
   *count_out = n;
   return SUCCESS;
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_create(struct json_object *details,
                           int user_id,
                           const char *source_uuid,
                           const char *source_location,
                           sched_source_type_t source_client_type) {
   char result[RESULT_BUF_SIZE];

   const char *type_str = json_get_string(details, "type");
   if (!type_str) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK
               "Error: 'type' is required (timer, alarm, reminder, task, briefing)");
      return strdup(result);
   }

   sched_event_type_t type = sched_event_type_from_str(type_str);

   /* Build event (limits checked atomically during insert) */
   sched_event_t event;
   memset(&event, 0, sizeof(event));
   event.user_id = user_id;
   event.event_type = type;
   event.status = SCHED_STATUS_PENDING;
   event.recurrence = SCHED_RECUR_ONCE;

   /* Name */
   const char *name = json_get_string(details, "name");
   if (name) {
      /* Cap on a UTF-8 boundary so a long name isn't byte-cut mid-glyph (event
       * was memset, so the tail stays NUL-terminated). */
      size_t name_cap = focus_utf8_safe_cap(name, SCHED_NAME_MAX - 1);
      memcpy(event.name, name, name_cap);
   } else {
      /* Auto-generate name */
      snprintf(event.name, SCHED_NAME_MAX, "%s", type_str);
   }

   /* Message (for reminders) */
   const char *message = json_get_string(details, "message");
   if (message)
      strncpy(event.message, message, SCHED_MESSAGE_MAX - 1);

   /* Fire time */
   int duration_min = json_get_int(details, "duration_minutes", 0);
   const char *fire_at_str = json_get_string(details, "fire_at");

   /* Validate duration_minutes range (shared by all types) */
   if (duration_min > MAX_DURATION_MINUTES) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK "Error: duration cannot exceed %d minutes (30 days)",
               MAX_DURATION_MINUTES);
      return strdup(result);
   }

   if (duration_min > 0) {
      /* Any type can use duration_minutes as a relative offset */
      event.fire_at = time(NULL) + (time_t)duration_min * 60;
      event.duration_sec = duration_min * 60;
   } else if (fire_at_str) {
      /* Absolute time via ISO 8601 */
      time_t fire_time = iso8601_parse(fire_at_str);
      if (fire_time <= 0) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: invalid fire_at format '%s'", fire_at_str);
         return strdup(result);
      }

      /* Must be in the future */
      if (fire_time <= time(NULL)) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: fire_at must be in the future");
         return strdup(result);
      }

      /* Must be within 1 year */
      if (fire_time > time(NULL) + 365 * 86400) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: fire_at must be within 1 year");
         return strdup(result);
      }

      event.fire_at = fire_time;

      /* Store original time for recurring alarms */
      const char *time_only = strchr(fire_at_str, 'T');
      if (time_only) {
         time_only++; /* Skip 'T' */
         strncpy(event.original_time, time_only, SCHED_ORIGINAL_TIME_MAX - 1);
      } else if (strlen(fire_at_str) <= 5) {
         strncpy(event.original_time, fire_at_str, SCHED_ORIGINAL_TIME_MAX - 1);
      }
   } else {
      /* Neither provided */
      if (type == SCHED_EVENT_TIMER) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: 'duration_minutes' is required for timers");
      } else {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: 'fire_at' (ISO 8601) or 'duration_minutes' is required for %s",
                  type_str);
      }
      return strdup(result);
   }

   /* Recurrence */
   const char *recur = json_get_string(details, "recurrence");
   if (recur)
      event.recurrence = sched_recurrence_from_str(recur);

   const char *recur_days = json_get_string(details, "recurrence_days");
   if (recur_days) {
      if (!valid_recurrence_days_csv(recur_days)) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: invalid recurrence_days '%s'. Use CSV of: sun,mon,tue,wed,thu,fri,sat",
                  recur_days);
         return strdup(result);
      }
      strncpy(event.recurrence_days, recur_days, SCHED_RECURRENCE_DAYS_MAX - 1);
   }

   /* Source info */
   if (source_uuid)
      strncpy(event.source_uuid, source_uuid, SCHED_UUID_MAX - 1);
   if (source_location)
      strncpy(event.source_location, source_location, SCHED_LOCATION_MAX - 1);
   event.source_client_type = source_client_type;

   /* Announce all */
   event.announce_all = json_get_bool(details, "announce_all", false);

   /* Per-briefing TTS override (schema v53).  Tri-state — must distinguish
    * "absent" (use source heuristic) from "false" (force-silent).
    * json_get_bool collapses absent→default to a single bool, so use the
    * raw json_object_object_get_ex here.  Silently ignored when type !=
    * briefing because the field is briefing-only at the LLM surface; we
    * don't error out so the LLM can pass it speculatively (e.g. as part
    * of a generic briefing template) without rejection. */
   if (type == SCHED_EVENT_BRIEFING) {
      struct json_object *jsa = NULL;
      if (json_object_object_get_ex(details, "say_aloud", &jsa) && jsa &&
          json_object_is_type(jsa, json_type_boolean)) {
         event.say_aloud = json_object_get_boolean(jsa) ? SCHED_SAY_ALOUD_ALWAYS
                                                        : SCHED_SAY_ALOUD_NEVER;
      } else {
         event.say_aloud = SCHED_SAY_ALOUD_DEFAULT;
      }
   } else {
      event.say_aloud = SCHED_SAY_ALOUD_DEFAULT;
   }

   /* Messaging fan-out target (schema v54).  Optional string; when present
    * and non-empty the scheduler also fires the announcement / briefing
    * through the named messaging channel after the existing TTS + WebUI
    * banner path.  Engine-side ownership check enforces that the channel
    * belongs to event.user_id at delivery time; the tool surface accepts
    * any string and lets the engine reject (so the LLM can pass a
    * channel name speculatively without us needing to round-trip a
    * lookup at create time).  Explicit string type-check matches the
    * sibling `say_aloud` pattern — a prompt-injected non-string would
    * otherwise be coerced via json_object_get_string. */
   struct json_object *dt_obj = NULL;
   if (json_object_object_get_ex(details, "deliver_to", &dt_obj) && dt_obj &&
       json_object_is_type(dt_obj, json_type_string)) {
      const char *deliver_to_str = json_object_get_string(dt_obj);
      if (deliver_to_str && deliver_to_str[0]) {
         strncpy(event.deliver_to, deliver_to_str, SCHED_DELIVER_TO_MAX - 1);
         event.deliver_to[SCHED_DELIVER_TO_MAX - 1] = '\0';
      }
   }

   /* Per-briefing summarization instructions (schema v84).  Optional free text
    * that steers HOW the briefing LLM summarizes the collected tool output
    * (content emphasis, structure, length, tone).  Briefing-only: silently
    * ignored for non-briefing types so the LLM can pass it speculatively
    * without rejection (same tolerance as say_aloud).  Explicit string
    * type-check matches the deliver_to/say_aloud pattern.  Truncated on a
    * UTF-8 boundary so a multi-byte char is never split mid-sequence. */
   if (type == SCHED_EVENT_BRIEFING) {
      struct json_object *instr_obj = NULL;
      if (json_object_object_get_ex(details, "instructions", &instr_obj) && instr_obj &&
          json_object_is_type(instr_obj, json_type_string)) {
         const char *instr_str = json_object_get_string(instr_obj);
         if (instr_str && instr_str[0]) {
            size_t cap = focus_utf8_safe_cap(instr_str, SCHED_INSTRUCTIONS_MAX - 1);
            memcpy(event.instructions, instr_str, cap);
            event.instructions[cap] = '\0';
         }
      }
   }

   /* Tool scheduling — supports both legacy single-tool (top-level
    * tool_name/tool_action/tool_value) AND multi-step briefings (a `steps`
    * JSON array).  If `steps` is present, it wins; top-level tool_* is
    * ignored.  Steps are validated NOW (so we reject the create) but only
    * written to briefing_steps AFTER the event row insert succeeds. */
   sched_briefing_step_t parsed_steps[SCHED_BRIEFING_STEPS_MAX];
   int parsed_step_count = 0;
   struct json_object *steps_arr = NULL;
   json_object_object_get_ex(details, "steps", &steps_arr);
   bool has_steps_array = (steps_arr && json_object_is_type(steps_arr, json_type_array));

   if (has_steps_array) {
      if (type != SCHED_EVENT_BRIEFING) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: 'steps' is only supported for type='briefing'");
         return strdup(result);
      }
      char err[RESULT_BUF_SIZE];
      if (parse_validate_steps(steps_arr, parsed_steps, &parsed_step_count, err, sizeof(err)) !=
          SUCCESS) {
         char marked_err[RESULT_BUF_SIZE + 1];
         snprintf(marked_err, sizeof(marked_err), TOOL_RESULT_ERROR_MARK "%s", err);
         return strdup(marked_err);
      }
      /* New multi-step briefings leave the legacy tool_* fields empty.
       * Steps are written to briefing_steps after the insert succeeds. */
   } else {
      /* Legacy single-tool path */
      const char *tool_name = json_get_string(details, "tool_name");
      if ((type == SCHED_EVENT_TASK || type == SCHED_EVENT_BRIEFING) && !tool_name) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: 'tool_name' (or 'steps' array for briefings) is required for "
                  "scheduled %s. System shutdown is not available as a schedulable tool.",
                  type == SCHED_EVENT_BRIEFING ? "briefings" : "tasks");
         return strdup(result);
      }
      const char *tool_value = json_get_string(details, "tool_value");
      const char *tool_action = json_get_string(details, "tool_action");
      if (tool_name) {
         char err[160];
         if (tool_registry_validate_schedulable(tool_name, tool_action, tool_value, err,
                                                sizeof(err)) != SUCCESS) {
            snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Error: %s", err);
            return strdup(result);
         }
         strncpy(event.tool_name, tool_name, SCHED_TOOL_NAME_MAX - 1);
      }
      if (tool_action)
         strncpy(event.tool_action, tool_action, SCHED_TOOL_NAME_MAX - 1);
      if (tool_value) {
         if (strlen(tool_value) >= SCHED_TOOL_VALUE_MAX) {
            snprintf(result, sizeof(result),
                     TOOL_RESULT_ERROR_MARK "Error: tool_value too long (%zu bytes, max %d). "
                                            "Shorten the content and retry.",
                     strlen(tool_value), SCHED_TOOL_VALUE_MAX - 1);
            return strdup(result);
         }
         strncpy(event.tool_value, tool_value, SCHED_TOOL_VALUE_MAX - 1);
      }
   }

   /* Option A auto-promotion: a `task` on a purely informational (read-only)
    * tool would execute at fire time but its result is discarded — only
    * briefings summarize + deliver tool output.  Promote such a task to a
    * single-step briefing so the user actually receives what they scheduled.
    * Action tools (lights, send message) stay tasks: their result is a status
    * and "completed" is the right feedback.  The single tool is rewritten into
    * a 1-step briefing (steps table owns it, legacy event tool_* fields
    * cleared — the new-briefing convention), so it fires through the validated
    * multi-step path rather than the legacy single-tool compat branch.  The
    * tool + tool_value were already validated by the single-tool branch above,
    * so the promoted step carries validated data.  say_aloud stays
    * SCHED_SAY_ALOUD_DEFAULT here (the briefing-only override block ran earlier
    * while this was still a TASK) → fire-time source heuristic speaks for
    * voice-created events and stays silent for WebUI, which is the intent. */
   if (type == SCHED_EVENT_TASK && event.tool_name[0]) {
      const tool_metadata_t *promote_meta = tool_registry_find(event.tool_name);
      if (promote_meta && (promote_meta->capabilities & TOOL_CAP_INFORMATIONAL)) {
         memset(parsed_steps, 0, sizeof(parsed_steps));
         strncpy(parsed_steps[0].tool_name, event.tool_name, SCHED_TOOL_NAME_MAX - 1);
         strncpy(parsed_steps[0].tool_action, event.tool_action, SCHED_TOOL_NAME_MAX - 1);
         strncpy(parsed_steps[0].tool_value, event.tool_value, SCHED_TOOL_VALUE_MAX - 1);
         parsed_step_count = 1;
         event.tool_name[0] = '\0';
         event.tool_action[0] = '\0';
         event.tool_value[0] = '\0';
         type = SCHED_EVENT_BRIEFING;
         event.event_type = SCHED_EVENT_BRIEFING;
         /* Keep the confirmation text (and the LLM's relayed reply) coherent
          * with the row's real type — the WebUI panel + list already render by
          * event_type, and the event delivers a summary, not a status. */
         type_str = sched_event_type_to_str(SCHED_EVENT_BRIEFING);
         OLOG_INFO("scheduler: promoted informational task '%s' to single-step briefing "
                   "(result would otherwise be discarded)",
                   promote_meta->name);
      }
   }

   /* Atomic limit check + insert */
   int64_t id = 0;
   int insert_rc = scheduler_db_insert_checked(&event, g_config.scheduler.max_events_per_user,
                                               g_config.scheduler.max_events_total, &id);
   if (insert_rc == SCHED_DB_USER_LIMIT) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK
               "Error: maximum events per user reached (%d). Cancel some events first.",
               g_config.scheduler.max_events_per_user);
      return strdup(result);
   }
   if (insert_rc == SCHED_DB_GLOBAL_LIMIT) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK "Error: maximum total events reached (%d).",
               g_config.scheduler.max_events_total);
      return strdup(result);
   }
   if (insert_rc != SCHED_DB_SUCCESS) {
      snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Error: failed to create event");
      return strdup(result);
   }

   /* Write the parsed steps to briefing_steps now that the event row exists.
    * parsed_step_count > 0 comes from either a multi-step `steps` array OR an
    * informational-task promotion — both imply the row is a BRIEFING (the
    * steps-array branch enforces type==BRIEFING; promotion flips event_type
    * first), so this never persists steps onto a non-briefing row.  On failure,
    * cancel the just-inserted event row (status='cancelled' — row stays for the
    * retention sweep but is invisible to the queue) so we don't leave a
    * zero-step briefing pending that would silently no-op at fire time. */
   if (parsed_step_count > 0) {
      int set_rc = scheduler_db_briefing_steps_set(id, parsed_steps, parsed_step_count);
      if (set_rc != SCHED_DB_SUCCESS) {
         scheduler_db_cancel(id);
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: failed to store briefing steps (event marked cancelled)");
         return strdup(result);
      }
   }

   /* Notify scheduler thread */
   scheduler_notify_new_event();
   scheduler_broadcast_events_changed(event.user_id);

   /* Format response with current time + fire time so the LLM can relay accurately */
   time_t now = time(NULL);
   struct tm now_tm, fire_tm;
   localtime_r(&now, &now_tm);
   localtime_r(&event.fire_at, &fire_tm);

   char now_str[64], fire_str[64];
   strftime(now_str, sizeof(now_str), "%I:%M %p", &now_tm);
   strftime(fire_str, sizeof(fire_str), "%I:%M %p on %b %d", &fire_tm);

   if (type == SCHED_EVENT_TIMER) {
      int hours = duration_min / 60;
      int mins = duration_min % 60;
      char dur_str[64];
      if (hours > 0 && mins > 0)
         snprintf(dur_str, sizeof(dur_str), "%d hour%s and %d minute%s", hours,
                  hours == 1 ? "" : "s", mins, mins == 1 ? "" : "s");
      else if (hours > 0)
         snprintf(dur_str, sizeof(dur_str), "%d hour%s", hours, hours == 1 ? "" : "s");
      else
         snprintf(dur_str, sizeof(dur_str), "%d minute%s", mins, mins == 1 ? "" : "s");
      snprintf(result, sizeof(result), "%s timer set for %s (fires at %s). Current time: %s.",
               event.name, dur_str, fire_str, now_str);
   } else {
      snprintf(result, sizeof(result), "%s '%s' set for %s. Current time: %s.", type_str,
               event.name, fire_str, now_str);
   }

   return strdup(result);
}

static char *handle_list(struct json_object *details, int user_id) {
   const char *type_str = json_get_string(details, "type");
   int type_filter = type_str ? (int)sched_event_type_from_str(type_str) : -1;

   /* Heap-allocate the event array: sched_event_t grew to ~4 KB with the v84
    * instructions field, so an on-stack SCHED_MAX_RESULTS array would burn
    * ~200 KB of the LLM-worker stack.  calloc'd once, freed on every exit. */
   sched_event_t *events = calloc(SCHED_MAX_RESULTS, sizeof(sched_event_t));
   if (!events)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: out of memory.");
   int count = scheduler_db_list_user_events(user_id, type_filter, events, SCHED_MAX_RESULTS);

   if (count == 0) {
      free(events);
      if (type_str)
         return strdup("No active events of that type.");
      return strdup("No active timers, alarms, or reminders.");
   }

   /* Batch-load briefing steps for ALL events at once — one auth_db mutex
    * acquisition instead of `count` separate ones (was N+1; on the panel
    * path at SCHED_MAX_RESULTS=50 that's 51 lock cycles per list).  Steps
    * memory is significant (count × 8 × ~2 KB) so heap-alloc rather than
    * burn the LLM-worker stack; callocs at most ~870 KB at SCHED_MAX_RESULTS
    * = 50, freed before return.  Falls back to per-row lookup if the alloc
    * fails (correctness preserved, performance regresses to the old N+1
    * pattern only on OOM). */
   sched_briefing_step_t *step_table = NULL;
   int *step_counts = NULL;
   if (count > 0) {
      step_table = calloc((size_t)count * SCHED_BRIEFING_STEPS_MAX, sizeof(sched_briefing_step_t));
      step_counts = calloc((size_t)count, sizeof(int));
      if (step_table && step_counts) {
         int64_t ids[SCHED_MAX_RESULTS];
         for (int i = 0; i < count; i++)
            ids[i] = events[i].id;
         scheduler_db_briefing_steps_list_many(ids, count, step_table, step_counts);
      } else {
         /* OOM — release whichever side allocated and let the inline
          * fallback inside the loop do per-row lookups. */
         free(step_table);
         free(step_counts);
         step_table = NULL;
         step_counts = NULL;
      }
   }

   /* Build response — strbuf so a long event list cannot silently truncate
    * mid-row the way the prior fixed 2KB stack buffer did. */
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_appendf(&sb, "Active events (%d):\n", count);

   for (int i = 0; i < count; i++) {
      sched_event_t *e = &events[i];
      const char *type = sched_event_type_to_str(e->event_type);

      if (e->event_type == SCHED_EVENT_TIMER) {
         /* Show time remaining */
         int remaining = (int)(e->fire_at - time(NULL));
         if (remaining < 0)
            remaining = 0;
         int rm = remaining / 60;
         int rs = remaining % 60;
         if (strbuf_appendf(&sb, "- [id=%lld] [%s] %s: %dm %ds remaining\n", (long long)e->id, type,
                            e->name, rm, rs) < 0)
            break;
      } else {
         struct tm fire_tm;
         localtime_r(&e->fire_at, &fire_tm);
         char time_str[32];
         strftime(time_str, sizeof(time_str), "%I:%M %p %b %d", &fire_tm);
         if (strbuf_appendf(&sb, "- [id=%lld] [%s] %s: %s", (long long)e->id, type, e->name,
                            time_str) < 0)
            break;
         if (e->recurrence != SCHED_RECUR_ONCE)
            strbuf_appendf(&sb, " (%s)", sched_recurrence_to_str(e->recurrence));
         /* Tasks and briefings carry tool(s) to execute at fire time —
          * surface them so the LLM can describe what the schedule will
          * actually do without waiting for it to fire.  Briefings may have
          * multi-step rows in briefing_steps; tasks always single-tool. */
         if (e->event_type == SCHED_EVENT_BRIEFING) {
            sched_briefing_step_t steps_fallback[SCHED_BRIEFING_STEPS_MAX];
            sched_briefing_step_t *steps;
            int step_count;
            if (step_table) {
               steps = &step_table[(size_t)i * SCHED_BRIEFING_STEPS_MAX];
               step_count = step_counts[i];
            } else {
               step_count = 0;
               scheduler_db_briefing_steps_list(e->id, steps_fallback, SCHED_BRIEFING_STEPS_MAX,
                                                &step_count);
               steps = steps_fallback;
            }
            if (step_count > 0) {
               strbuf_append(&sb, " — runs ");
               for (int s = 0; s < step_count; s++) {
                  if (s > 0)
                     strbuf_append(&sb, " → ");
                  strbuf_appendf(&sb, "%s", steps[s].tool_name);
                  if (steps[s].tool_action[0])
                     strbuf_appendf(&sb, ".%s", steps[s].tool_action);
                  if (steps[s].tool_value[0])
                     strbuf_appendf(&sb, "(%s)", steps[s].tool_value);
               }
            } else if (e->tool_name[0]) {
               /* Legacy single-tool briefing — same shape as task rendering */
               strbuf_appendf(&sb, " — runs %s", e->tool_name);
               if (e->tool_action[0])
                  strbuf_appendf(&sb, ".%s", e->tool_action);
               if (e->tool_value[0])
                  strbuf_appendf(&sb, "(%s)", e->tool_value);
            }
         } else if (e->event_type == SCHED_EVENT_TASK && e->tool_name[0]) {
            strbuf_appendf(&sb, " — runs %s", e->tool_name);
            if (e->tool_action[0])
               strbuf_appendf(&sb, ".%s", e->tool_action);
            if (e->tool_value[0])
               strbuf_appendf(&sb, "(%s)", e->tool_value);
         }
         strbuf_append(&sb, "\n");
      }
   }

   free(step_table);
   free(step_counts);
   free(events);

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: response buffer exceeded safety cap.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Error: out of memory.");
}

static char *handle_cancel(struct json_object *details, int user_id) {
   char result[RESULT_BUF_SIZE];

   /* Try by event_id first */
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);
   const char *name = json_get_string(details, "name");

   sched_event_t event;

   if (event_id > 0) {
      if (scheduler_db_get(event_id, &event) != 0) {
         snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Error: event not found");
         return strdup(result);
      }
      if (event.user_id != user_id) {
         snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Error: event not found");
         return strdup(result);
      }
   } else if (name) {
      if (scheduler_db_find_by_name(user_id, name, &event) != 0) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "No active event named '%s' found.", name);
         return strdup(result);
      }
      event_id = event.id;
   } else {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK "Error: 'event_id' or 'name' required to cancel");
      return strdup(result);
   }

   if (scheduler_cancel_and_broadcast(event_id, event.user_id) == 0) {
      snprintf(result, sizeof(result), "Cancelled %s '%s'.",
               sched_event_type_to_str(event.event_type), event.name);
   } else {
      snprintf(result, sizeof(result), "Could not cancel '%s' (may have already fired).",
               event.name);
   }

   return strdup(result);
}

static char *handle_query(struct json_object *details, int user_id) {
   char result[RESULT_BUF_SIZE];

   const char *name = json_get_string(details, "name");
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);

   sched_event_t event;

   if (event_id > 0) {
      if (scheduler_db_get(event_id, &event) != 0 || event.user_id != user_id) {
         snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Event not found.");
         return strdup(result);
      }
   } else if (name) {
      if (scheduler_db_find_by_name(user_id, name, &event) != 0) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "No active event named '%s' found.", name);
         return strdup(result);
      }
   } else {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK "Error: 'event_id' or 'name' required to query");
      return strdup(result);
   }

   if (event.event_type == SCHED_EVENT_TIMER) {
      int remaining = (int)(event.fire_at - time(NULL));
      if (remaining < 0)
         remaining = 0;
      int rh = remaining / 3600;
      int rm = (remaining % 3600) / 60;
      int rs = remaining % 60;

      if (rh > 0) {
         snprintf(result, sizeof(result), "%s has %d hour%s, %d minute%s, and %d second%s left.",
                  event.name, rh, rh == 1 ? "" : "s", rm, rm == 1 ? "" : "s", rs,
                  rs == 1 ? "" : "s");
      } else if (rm > 0) {
         snprintf(result, sizeof(result), "%s has %d minute%s and %d second%s left.", event.name,
                  rm, rm == 1 ? "" : "s", rs, rs == 1 ? "" : "s");
      } else {
         snprintf(result, sizeof(result), "%s has %d second%s left.", event.name, rs,
                  rs == 1 ? "" : "s");
      }
   } else {
      struct tm fire_tm;
      localtime_r(&event.fire_at, &fire_tm);
      char time_str[32];
      strftime(time_str, sizeof(time_str), "%I:%M %p on %b %d", &fire_tm);
      int written = snprintf(result, sizeof(result), "%s '%s' [id=%lld] is set for %s. Status: %s.",
                             sched_event_type_to_str(event.event_type), event.name,
                             (long long)event.id, time_str, sched_status_to_str(event.status));
      /* For tasks and briefings, append the tool(s) the schedule will run so
       * the LLM can describe the configured behavior without waiting for fire.
       * Briefings may have multi-step rows; render as `t1.a1(v1) → t2.a2(v2)`. */
      if (written > 0 && (size_t)written < sizeof(result) &&
          event.event_type == SCHED_EVENT_BRIEFING) {
         sched_briefing_step_t steps[SCHED_BRIEFING_STEPS_MAX];
         int step_count = 0;
         scheduler_db_briefing_steps_list(event.id, steps, SCHED_BRIEFING_STEPS_MAX, &step_count);
         size_t off = (size_t)written;
         if (step_count > 0) {
            off += snprintf(result + off, sizeof(result) - off, " Runs ");
            for (int s = 0; s < step_count && off < sizeof(result); s++) {
               if (s > 0)
                  off += snprintf(result + off, sizeof(result) - off, " → ");
               if (off < sizeof(result))
                  off += snprintf(result + off, sizeof(result) - off, "%s", steps[s].tool_name);
               if (off < sizeof(result) && steps[s].tool_action[0])
                  off += snprintf(result + off, sizeof(result) - off, ".%s", steps[s].tool_action);
               if (off < sizeof(result) && steps[s].tool_value[0])
                  off += snprintf(result + off, sizeof(result) - off, "(%s)", steps[s].tool_value);
            }
         } else if (event.tool_name[0]) {
            /* Legacy single-tool briefing */
            off += snprintf(result + off, sizeof(result) - off, " Runs %s", event.tool_name);
            if (off < sizeof(result) && event.tool_action[0])
               off += snprintf(result + off, sizeof(result) - off, ".%s", event.tool_action);
            if (off + 4 <= sizeof(result) && event.tool_value[0])
               snprintf(result + off, sizeof(result) - off, "(%.*s)",
                        (int)((sizeof(result) - off) > 3 ? (sizeof(result) - off) - 3 : 0),
                        event.tool_value);
         }
         /* Surface any per-briefing summarization steering so the model can
          * relay it verbatim when the user asks how a briefing is configured. */
         if (off < sizeof(result) && event.instructions[0])
            snprintf(result + off, sizeof(result) - off, " Summarization instructions: \"%s\"",
                     event.instructions);
      } else if (written > 0 && (size_t)written < sizeof(result) &&
                 event.event_type == SCHED_EVENT_TASK && event.tool_name[0]) {
         size_t off = (size_t)written;
         off += snprintf(result + off, sizeof(result) - off, " Runs %s", event.tool_name);
         if (off < sizeof(result) && event.tool_action[0])
            off += snprintf(result + off, sizeof(result) - off, ".%s", event.tool_action);
         if (off + 4 <= sizeof(result) && event.tool_value[0])
            snprintf(result + off, sizeof(result) - off, "(%s)", event.tool_value);
      }
   }

   return strdup(result);
}

/* Append a " Runs t1.a1(v1) → t2.a2(v2)" summary of a briefing's steps to sb,
 * falling back to the legacy single-tool fields for a pre-v50 row.  Reads the
 * steps fresh from the DB (per-event) — used only on the cold update-confirmation
 * path, so the batched read that handle_list uses is unnecessary here. */
static void append_run_summary(strbuf_t *sb, const sched_event_t *e) {
   sched_briefing_step_t steps[SCHED_BRIEFING_STEPS_MAX];
   int step_count = 0;
   scheduler_db_briefing_steps_list(e->id, steps, SCHED_BRIEFING_STEPS_MAX, &step_count);
   if (step_count > 0) {
      strbuf_append(sb, " Runs ");
      for (int s = 0; s < step_count; s++) {
         if (s > 0)
            strbuf_append(sb, " → ");
         strbuf_appendf(sb, "%s", steps[s].tool_name);
         if (steps[s].tool_action[0])
            strbuf_appendf(sb, ".%s", steps[s].tool_action);
         if (steps[s].tool_value[0])
            strbuf_appendf(sb, "(%s)", steps[s].tool_value);
      }
   } else if (e->tool_name[0]) {
      strbuf_appendf(sb, " Runs %s", e->tool_name);
      if (e->tool_action[0])
         strbuf_appendf(sb, ".%s", e->tool_action);
      if (e->tool_value[0])
         strbuf_appendf(sb, "(%s)", e->tool_value);
   }
}

/* Map a briefing-steps-update return code to a user-facing error string, or NULL
 * on success.  event_name is used only for the message text. */
static char *steps_update_error(int rc, const char *event_name) {
   char result[RESULT_BUF_SIZE];
   if (rc == SCHED_DB_NOT_EDITABLE) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK
               "Error: '%s' can no longer be edited (it already fired, is ringing, or was "
               "cancelled). If it recurs, the next occurrence exists now — edit that.",
               event_name);
      return strdup(result);
   }
   snprintf(result, sizeof(result),
            TOOL_RESULT_ERROR_MARK
            "Error: could not update '%s' (a step list can hold at most %d steps).",
            event_name, SCHED_BRIEFING_STEPS_MAX);
   return strdup(result);
}

/* update — edit an existing scheduled event in place, preserving its event_id
 * and recurrence-chain identity.  Two independent edit surfaces, either or both:
 *   • scalar fields (new_name, message, fire_at, recurrence, recurrence_days,
 *     deliver_to, say_aloud) — any event type;
 *   • briefing steps (add_steps append / steps replace) — briefings only.
 * The ownership + editable-status guard is authoritative in the DB primitives
 * (single txn each); the checks here are a friendlier pre-flight that cannot, by
 * itself, cause a false success.  `name`/`event_id` resolve the target; renaming
 * uses `new_name` so the lookup key and the new value don't collide. */
static char *handle_update(struct json_object *details, int user_id) {
   char result[RESULT_BUF_SIZE];

   /* Resolve the target event by id (preferred) or name — mirrors handle_cancel. */
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);
   const char *name = json_get_string(details, "name");
   sched_event_t event;

   if (event_id > 0) {
      if (scheduler_db_get(event_id, &event) != 0 || event.user_id != user_id) {
         snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Error: event not found");
         return strdup(result);
      }
   } else if (name) {
      if (scheduler_db_find_by_name(user_id, name, &event) != 0) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "No active event named '%s' found.", name);
         return strdup(result);
      }
      event_id = event.id;
   } else {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK "Error: 'event_id' or 'name' required to update");
      return strdup(result);
   }

   /* --- Parse scalar-field edits into a masked struct (validated up front). --- */
   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   uint32_t mask = 0;

   const char *new_name = json_get_string(details, "new_name");
   if (new_name && new_name[0]) {
      size_t cap = focus_utf8_safe_cap(new_name, SCHED_NAME_MAX - 1);
      memcpy(fields.name, new_name, cap);
      mask |= SCHED_FIELD_NAME;
   }

   struct json_object *jmsg = NULL;
   if (json_object_object_get_ex(details, "message", &jmsg) &&
       json_object_is_type(jmsg, json_type_string)) {
      const char *msg = json_object_get_string(jmsg);
      /* Cap on a UTF-8 boundary (fields is memset, so the tail stays NUL). */
      size_t msg_cap = focus_utf8_safe_cap(msg, SCHED_MESSAGE_MAX - 1);
      memcpy(fields.message, msg, msg_cap);
      mask |= SCHED_FIELD_MESSAGE;
   }

   const char *fire_at_str = json_get_string(details, "fire_at");
   if (fire_at_str) {
      time_t fire_time = iso8601_parse(fire_at_str);
      if (fire_time <= 0) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: invalid fire_at format '%s'", fire_at_str);
         return strdup(result);
      }
      if (fire_time <= time(NULL)) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: fire_at must be in the future");
         return strdup(result);
      }
      if (fire_time > time(NULL) + 365 * 86400) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK "Error: fire_at must be within 1 year");
         return strdup(result);
      }
      fields.fire_at = fire_time;
      mask |= SCHED_FIELD_FIRE_AT;
      /* Derive original_time (HH:MM local) from the new fire_at so a recurring
       * event's time-of-day follows the edit — the recurrence engine reads
       * original_time, not fire_at (see calculate_next_recurrence). */
      struct tm fire_tm;
      localtime_r(&fire_time, &fire_tm);
      snprintf(fields.original_time, SCHED_ORIGINAL_TIME_MAX, "%02d:%02d", fire_tm.tm_hour,
               fire_tm.tm_min);
      mask |= SCHED_FIELD_ORIGINAL_TIME;
   }

   const char *recur = json_get_string(details, "recurrence");
   if (recur) {
      sched_recurrence_t r = sched_recurrence_from_str(recur);
      /* sched_recurrence_from_str returns ONCE for ANY unrecognized token, so an
       * out-of-vocabulary value ("biweekly", "monthly") would silently CLEAR an
       * existing recurrence on the edit surface.  Reject unless the input really
       * is "once" (the only valid token that maps to ONCE). */
      if (r == SCHED_RECUR_ONCE && strcasecmp(recur, "once") != 0) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: invalid recurrence '%s'. Use one of: once, daily, weekdays, weekends, "
                  "weekly, custom",
                  recur);
         return strdup(result);
      }
      fields.recurrence = r;
      mask |= SCHED_FIELD_RECURRENCE;
   }

   const char *recur_days = json_get_string(details, "recurrence_days");
   if (recur_days) {
      if (!valid_recurrence_days_csv(recur_days)) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: invalid recurrence_days '%s'. Use CSV of: sun,mon,tue,wed,thu,fri,sat",
                  recur_days);
         return strdup(result);
      }
      strncpy(fields.recurrence_days, recur_days, SCHED_RECURRENCE_DAYS_MAX - 1);
      mask |= SCHED_FIELD_RECURRENCE_DAYS;
   }

   struct json_object *jdt = NULL;
   if (json_object_object_get_ex(details, "deliver_to", &jdt) &&
       json_object_is_type(jdt, json_type_string)) {
      strncpy(fields.deliver_to, json_object_get_string(jdt), SCHED_DELIVER_TO_MAX - 1);
      mask |= SCHED_FIELD_DELIVER_TO; /* empty string clears fan-out */
   }

   struct json_object *jsa = NULL;
   if (json_object_object_get_ex(details, "say_aloud", &jsa) &&
       json_object_is_type(jsa, json_type_boolean)) {
      fields.say_aloud = json_object_get_boolean(jsa) ? SCHED_SAY_ALOUD_ALWAYS
                                                      : SCHED_SAY_ALOUD_NEVER;
      mask |= SCHED_FIELD_SAY_ALOUD;
   }

   /* Per-briefing summarization instructions (schema v84).  An empty string
    * CLEARS the field (reverts to the legacy fixed prompt); a non-empty string
    * REPLACES it wholesale (this is not an append — the model is told to query
    * first if it means to amend).  Briefing-only: unlike create (which silently
    * ignores it off-type), update REFUSES on a non-briefing so a mistargeted
    * edit surfaces rather than no-ops.  Truncated on a UTF-8 boundary. */
   struct json_object *jinstr = NULL;
   if (json_object_object_get_ex(details, "instructions", &jinstr) &&
       json_object_is_type(jinstr, json_type_string)) {
      if (event.event_type != SCHED_EVENT_BRIEFING) {
         snprintf(result, sizeof(result),
                  TOOL_RESULT_ERROR_MARK
                  "Error: instructions can only be set on a briefing (this is a %s)",
                  sched_event_type_to_str(event.event_type));
         return strdup(result);
      }
      const char *instr = json_object_get_string(jinstr);
      size_t cap = focus_utf8_safe_cap(instr, SCHED_INSTRUCTIONS_MAX - 1);
      memcpy(fields.instructions, instr, cap);
      mask |= SCHED_FIELD_INSTRUCTIONS; /* empty string clears the steering */
   }

   /* --- Parse step edits (briefings only). --- */
   struct json_object *replace_arr = NULL, *append_arr = NULL;
   json_object_object_get_ex(details, "steps", &replace_arr);
   json_object_object_get_ex(details, "add_steps", &append_arr);
   bool has_replace = replace_arr && json_object_is_type(replace_arr, json_type_array);
   bool has_append = append_arr && json_object_is_type(append_arr, json_type_array);

   if (has_replace && has_append) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK
               "Error: pass either 'add_steps' (append) or 'steps' (replace all), not both");
      return strdup(result);
   }
   bool has_steps = has_replace || has_append;
   if (has_steps && event.event_type != SCHED_EVENT_BRIEFING) {
      snprintf(result, sizeof(result),
               TOOL_RESULT_ERROR_MARK
               "Error: steps can only be edited on a briefing (this is a %s)",
               sched_event_type_to_str(event.event_type));
      return strdup(result);
   }
   if (mask == 0 && !has_steps) {
      snprintf(result, sizeof(result),
               "Error: nothing to update — pass add_steps/steps (briefings) or a field such as "
               "new_name, fire_at, recurrence, deliver_to, say_aloud, or instructions");
      return strdup(result);
   }

   sched_briefing_step_t parsed_steps[SCHED_BRIEFING_STEPS_MAX];
   int parsed_count = 0;
   if (has_steps) {
      char err[RESULT_BUF_SIZE];
      if (parse_validate_steps(has_append ? append_arr : replace_arr, parsed_steps, &parsed_count,
                               err, sizeof(err)) != SUCCESS) {
         char marked_err[RESULT_BUF_SIZE + 1];
         snprintf(marked_err, sizeof(marked_err), TOOL_RESULT_ERROR_MARK "%s", err);
         return strdup(marked_err);
      }
   }

   /* --- Apply.  STEPS FIRST: the step edit has more ways to fail (notably the
    * 8-step cap), so doing it first means a rejected step edit leaves the scalar
    * fields untouched too — a combined "rename + add a step" that busts the cap
    * changes NOTHING and reports the cap error, instead of committing the rename
    * and then claiming total failure.  The two edits are still separate
    * transactions (each individually guarded + atomic), so a fire-thread flip in
    * the sub-millisecond gap between them can commit the first and reject the
    * second; that residual race is surfaced honestly as a partial success below,
    * never as a flat failure hiding a committed change. --- */
   bool steps_committed = false;
   if (has_steps) {
      int rc = scheduler_db_briefing_steps_update(event_id, user_id, parsed_steps, parsed_count,
                                                  has_append);
      if (rc != SCHED_DB_SUCCESS) {
         return steps_update_error(rc, event.name); /* nothing applied yet */
      }
      steps_committed = true;
   }
   if (mask != 0) {
      int rc = scheduler_db_update_fields(event_id, user_id, &fields, mask);
      if (rc != SCHED_DB_SUCCESS) {
         if (steps_committed) {
            /* Rare: the row changed editability between the two transactions, or
             * a DB error hit the scalar UPDATE.  The step edit already committed —
             * refresh listeners and report the partial state honestly rather than
             * claim total failure.  Attribute the cause precisely (don't blame a
             * state change on a genuine DB error). */
            scheduler_broadcast_events_changed(user_id);
            scheduler_notify_new_event();
            const char *why = (rc == SCHED_DB_NOT_EDITABLE)
                                  ? "it just fired, started ringing, or was cancelled"
                                  : "of an unexpected error";
            snprintf(result, sizeof(result),
                     "Updated the steps of '%s', but its other settings could not be changed "
                     "(%s).",
                     event.name, why);
            return strdup(result);
         }
         if (rc == SCHED_DB_NOT_EDITABLE) {
            snprintf(result, sizeof(result),
                     TOOL_RESULT_ERROR_MARK
                     "Error: '%s' can no longer be edited (it already fired, is ringing, or was "
                     "cancelled).",
                     event.name);
            return strdup(result);
         }
         snprintf(result, sizeof(result), TOOL_RESULT_ERROR_MARK "Error: could not update '%s'.",
                  event.name);
         return strdup(result);
      }
   }

   scheduler_broadcast_events_changed(user_id);
   scheduler_notify_new_event(); /* fire_at may have moved → recompute next wakeup */

   /* Read-back confirmation from the freshly-persisted row so the model relays
    * the new configuration exactly (name/time/steps all reflect the edit). */
   sched_event_t updated;
   if (scheduler_db_get(event_id, &updated) != 0)
      updated = event; /* fall back to the pre-edit copy for the label */

   strbuf_t sb;
   strbuf_init(&sb, 256);
   struct tm fire_tm;
   localtime_r(&updated.fire_at, &fire_tm);
   char time_str[48];
   strftime(time_str, sizeof(time_str), "%I:%M %p on %b %d", &fire_tm);
   strbuf_appendf(&sb, "Updated %s '%s' (fires %s", sched_event_type_to_str(updated.event_type),
                  updated.name, time_str);
   if (updated.recurrence != SCHED_RECUR_ONCE)
      strbuf_appendf(&sb, ", %s", sched_recurrence_to_str(updated.recurrence));
   strbuf_append(&sb, ").");
   if (mask & SCHED_FIELD_INSTRUCTIONS) {
      /* Echo from `fields` (what we just wrote), not `updated` — the write
       * already succeeded above, and `updated` falls back to the PRE-edit copy
       * if the confirmation read-back happens to fail. */
      if (fields.instructions[0])
         strbuf_appendf(&sb, " Summarization instructions set to: \"%s\"", fields.instructions);
      else
         strbuf_append(&sb, " Summarization instructions cleared (back to the default format).");
   }
   if (updated.event_type == SCHED_EVENT_BRIEFING)
      append_run_summary(&sb, &updated);
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Updated event.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Updated event.");
}

static char *handle_snooze(struct json_object *details) {
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);
   int snooze_min = json_get_int(details, "snooze_minutes", 0);

   if (snooze_min < 0 || snooze_min > MAX_SNOOZE_MINUTES)
      snooze_min = 0;

   int result = scheduler_snooze(event_id, snooze_min);
   if (result == 0) {
      int actual_min = snooze_min > 0 ? snooze_min : g_config.scheduler.default_snooze_minutes;
      char buf[128];
      snprintf(buf, sizeof(buf), "Snoozed for %d minute%s.", actual_min,
               actual_min == 1 ? "" : "s");
      return strdup(buf);
   }

   return strdup("No alarm is currently ringing to snooze.");
}

static char *handle_dismiss(struct json_object *details) {
   int64_t event_id = (int64_t)json_get_int(details, "event_id", 0);

   int result = scheduler_dismiss(event_id);
   if (result == 0)
      return strdup("Alarm dismissed.");

   return strdup("No alarm is currently ringing to dismiss.");
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *scheduler_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!action || !action[0])
      return strdup(TOOL_RESULT_ERROR_MARK "Error: action is required");

   /* No scheduler action tolerates a non-JSON `arguments` value.  Even `list`
    * reads an optional `type` filter, so degrading prose to an empty object
    * would silently list everything instead of the requested subset; better to
    * error and let the model re-emit proper JSON (the schema now tells it to
    * omit `arguments` for a no-arg action rather than narrate). */
   struct json_object *details = tool_parse_details(value, false);
   if (!details)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: invalid JSON in details parameter");

   /* Get user context */
   int user_id = 1; /* Default */
   const char *source_uuid = NULL;
   const char *source_location = NULL;
   sched_source_type_t source_client_type = SCHED_SOURCE_LOCAL;

#ifdef ENABLE_MULTI_CLIENT
   session_t *ctx = session_get_command_context();
   if (ctx) {
      user_id = ctx->metrics.user_id > 0 ? ctx->metrics.user_id : 1;
      if (ctx->type == SESSION_TYPE_DAP2) {
         source_uuid = ctx->identity.uuid;
         source_location = ctx->identity.location;
         source_client_type = SCHED_SOURCE_DAP2;
      } else if (ctx->type == SESSION_TYPE_WEBUI) {
         source_client_type = SCHED_SOURCE_WEBUI;
      }
   }

   /* Schedule-mutating actions act on a specific user's schedule.  A caller with
    * no session context is an unauthenticated MQTT publish (no session_id), which
    * resolves to user_id 1 — refuse it, mirroring job_tool's starts_work guard.
    * Every legitimate path (LLM tool call incl. local voice, WebUI, authenticated
    * MQTT-with-session) sets the context.  snooze/dismiss are deliberately left
    * reachable: they act only on an already-ringing alarm, so a physical MQTT
    * button can still silence one. */
   const bool mutates_schedule = (strcmp(action, "create") == 0 || strcmp(action, "cancel") == 0 ||
                                  strcmp(action, "update") == 0);
   if (mutates_schedule && ctx == NULL) {
      json_object_put(details);
      OLOG_WARNING("scheduler_tool: refused '%s' from a caller with no session context", action);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: changing a schedule requires a user session.");
   }
#endif

   char *result = NULL;

   if (strcmp(action, "create") == 0) {
      result = handle_create(details, user_id, source_uuid, source_location, source_client_type);
   } else if (strcmp(action, "list") == 0) {
      result = handle_list(details, user_id);
   } else if (strcmp(action, "cancel") == 0) {
      result = handle_cancel(details, user_id);
   } else if (strcmp(action, "query") == 0) {
      result = handle_query(details, user_id);
   } else if (strcmp(action, "update") == 0) {
      result = handle_update(details, user_id);
   } else if (strcmp(action, "snooze") == 0) {
      result = handle_snooze(details);
   } else if (strcmp(action, "dismiss") == 0) {
      result = handle_dismiss(details);
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               TOOL_RESULT_ERROR_MARK "Error: unknown action '%s'. Valid: create, list, cancel, "
                                      "query, update, snooze, dismiss",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Tool Lifecycle
 * ============================================================================= */

static int scheduler_tool_init(void) {
   return scheduler_init();
}

static void scheduler_tool_cleanup(void) {
   scheduler_shutdown();
}

/* =============================================================================
 * Tool Parameter Definition
 * ============================================================================= */

static const treg_param_t scheduler_params[] = {
   {
       .name = "action",
       .description = "The scheduler action: 'create' (new event), 'list' (show active events), "
                      "'cancel' (cancel by name/id), 'query' (check status/time remaining), "
                      "'update' (edit an existing briefing's steps in place, keeping its id and "
                      "recurrence — use this to add/change what a briefing reports, NOT a memory "
                      "note), 'snooze' (snooze ringing alarm), 'dismiss' (dismiss ringing alarm)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "create", "list", "cancel", "query", "update", "snooze", "dismiss" },
       .enum_count = 7,
   },
   {
       .name = "arguments",
       .description =
           "JSON object of the action's arguments, passed as a JSON-encoded string.  "
           "Omit entirely for an action that takes no arguments; never fill it with a "
           "description or rationale.\n"
           "create: {type (timer|alarm|reminder|task|briefing), name (optional), "
           "fire_at, duration_minutes (1-43200, MINUTES UNTIL FIRE — use ONLY for "
           "'in X minutes' / 'X-minute timer' shapes; this is NOT briefing duration / "
           "task duration / how-long-the-thing-runs.  Use EITHER fire_at OR "
           "duration_minutes, never both — they conflict and duration_minutes wins, "
           "which causes a briefing fire_at=10min-from-now + duration_minutes=1 to fire "
           "in 1 minute instead of 10), message (reminders, ≤512 chars), recurrence "
           "(once|daily|weekdays|weekends|weekly|custom), recurrence_days "
           "(csv: mon,tue,...), announce_all (bool — multi-user fan-out, NOT "
           "audio control), say_aloud (briefing-only bool — TTS override; see "
           "AUDIO section), deliver_to (optional string — messaging channel "
           "display_name; see DELIVERY section), instructions (briefing-only "
           "optional string, ≤1024 chars — free-text steering for HOW the "
           "briefing is summarized: content emphasis, structure, length, tone; "
           "omit to use the default briefing format)}.\n"
           "  fire_at: ISO 8601. No timezone suffix = the user's LOCAL timezone "
           "('2026-03-19T07:00:00' = 7 AM local). 'Z' suffix = UTC. '+05:00' = explicit "
           "offset. For user-said times like 'set an alarm for 7am', use NO suffix.\n"
           "task/briefing also require tool_name + tool_action + tool_value (or "
           "briefing-only `steps` array — see the tool's top-level description for "
           "multi-step shape).\n"
           "  tool_action: the action you would pass when calling that tool DIRECTLY — "
           "the value of the tool's own action selector (the enum `action` parameter in "
           "its schema), e.g. weather->today/tomorrow/week, search->web/news/social/..., "
           "music->play/stop, calendar->today/range/add, home_assistant->on/off. Use one "
           "of the tool's own action values; NEVER put the tool's name in tool_action.\n"
           "  tool_value: the literal arguments the tool receives. SHAPE depends on the "
           "target tool:\n"
           "    - For tools whose value is plain text (search, weather, url_fetch): pass "
           "the RAW string (e.g. tool_value: 'today's news').\n"
           "    - For tools whose value is a JSON object (calendar, email, scheduler, "
           "memory): pass a JSON STRING with escaped quotes "
           "(e.g. tool_value: '{\\\"action\\\":\\\"today\\\"}').\n"
           "  search/url_fetch reject empty tool_value at create time.\n"
           "list: {type (optional filter)}.\n"
           "cancel/query: {name or event_id (event_id is the integer shown in 'list' "
           "output as '[id=N]')}.\n"
           "update: {name or event_id to identify the event, plus any of: new_name (rename), "
           "fire_at (reschedule, ISO 8601), recurrence, recurrence_days, message (reminders), "
           "deliver_to (channel; empty string clears fan-out), say_aloud (briefings), "
           "instructions (briefing-only summarization steering — REPLACES the current "
           "instructions wholesale, so 'query' the briefing first if you mean to amend "
           "rather than overwrite; empty string clears it back to the default format).  For "
           "BRIEFINGS also: add_steps (append one or more steps — use this to ADD to a "
           "briefing, e.g. add stock news to a morning briefing; send only the NEW steps) OR "
           "steps (replace the WHOLE step list — you must resend every step you want kept). "
           "Each step is {tool_name, tool_action, tool_value}, same shape as create.  Prefer "
           "add_steps for additive requests so you can't accidentally drop existing steps.  "
           "NOTE: 'name' identifies the event; to RENAME it, use new_name.  Editing a "
           "recurring event's fire_at also moves its time-of-day for all future occurrences.  "
           "'list' or 'query' the event first to get its name/id.\n"
           "snooze: {event_id (optional), snooze_minutes (1-120, optional)}.\n"
           "dismiss: {event_id (optional)}.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const tool_metadata_t scheduler_metadata = {
   .name = "scheduler",
   .device_string = "scheduler",
   .topic = "dawn",
   .aliases = { "timer", "alarm", "reminder", "schedule" },
   .alias_count = 4,

   .description =
       "Manage timers, alarms, reminders, scheduled tasks, and briefings. "
       "Set timers with duration ('set a 10 minute timer'), "
       "alarms at specific times ('set an alarm for 7 AM'), "
       "reminders with messages ('remind me to call Mom at 3pm'), "
       "schedule tool execution ('turn off lights at midnight'), "
       "or briefings that summarize tool output via LLM ('weather briefing at 7am'). "
       "Query time remaining, list active events, cancel, snooze, or dismiss.\n\n"
       "TASK vs BRIEFING: use `briefing` whenever the user wants to RECEIVE the tool's "
       "output (weather, search, news, web fetches, calendar readouts) — briefings "
       "summarize and deliver the result.  Use `task` ONLY for action tools whose "
       "result is just a status the user doesn't need read back (lights, locks, "
       "media control, sending a message).  A `task` discards its tool output; if you "
       "schedule an informational/read-only tool as a `task` the scheduler "
       "auto-corrects it to a single-step briefing so the result isn't lost, but "
       "picking `briefing` yourself is clearer.\n\n"
       "CRITICAL: see the TOOL-CALL DISCIPLINE block in your system prompt — it applies "
       "doubly hard here.  A schedule that doesn't fire is the worst failure mode: the user "
       "trusts your confirmation and finds out hours later that nothing happened.  When the "
       "user asks you to schedule ANY event, the `scheduler` create call MUST be in the same "
       "turn as your acknowledgement.  Never say \"locked in\" / \"firing at\" / \"got it "
       "scheduled\" without actually emitting the tool call first.\n\n"
       "Briefings can be SINGLE-STEP (one tool, one summary) or MULTI-STEP (run several "
       "tools, summarize the combined output).  For multi-step, pass a `steps` array "
       "inside `details` and omit top-level tool_name; each step is "
       "{tool_name, tool_action, tool_value} — use those exact key names, NOT the short "
       "forms tool/action/value.  Each step's tool_value MUST be the literal "
       "arguments the tool receives (for `search`, that's the query string).  Empty "
       "tool_value for tools that require it (search, url_fetch) is rejected at create "
       "time.  Maximum 8 steps per briefing.\n\n"
       "EDITING A BRIEFING: when the user asks to CHANGE, MODIFY, or ADD TO an existing "
       "briefing ('add my stock holdings to the morning briefing', 'also include traffic'), "
       "use action='update' — do NOT store a memory note and do NOT create a second "
       "briefing.  A memory note does nothing to a schedule; only editing its steps changes "
       "what it reports.  First 'list' to find the briefing (the id is shown as [id=N]), then "
       "'update' with add_steps to append the new step(s).  Editing the pending occurrence "
       "propagates to all future occurrences of a recurring briefing automatically.\n\n"
       "AUDIO (briefings only): controlled by `say_aloud` in `details` — NOT "
       "`announce_all` (which is multi-user fan-out, unrelated to TTS).  "
       "Default behavior: briefings created via voice (local mic / satellite) speak "
       "their summary aloud when they fire; briefings created via text in the WebUI "
       "are SILENT.  Override with `say_aloud`: set to `true` when the user asks for "
       "audio explicitly ('read it to me out loud' from a WebUI-text user), set to "
       "`false` when the user asks for silence ('don't say this one aloud', 'keep it "
       "quiet', 'silent briefing' — even from a voice user).  When in doubt — if the "
       "user mentioned audio/quiet/silent/aloud at all — set `say_aloud` explicitly "
       "rather than relying on the default.  Omit the field only when the user "
       "didn't reference audio at all.\n\n"
       "DELIVERY (any event type): set `deliver_to` to a messaging channel "
       "display_name (e.g. \"telegram_main\", \"slack_D0B6SQGB31C\") to fan the "
       "announcement out to that chat surface IN ADDITION TO the local TTS / "
       "WebUI banner — additive, not exclusive.  Applies to timers, alarms, "
       "reminders, tasks, AND briefings (no exception).  Use "
       "`messaging.list_channels` to discover names the user has linked.\n"
       "  CURRENT-SURFACE DEFAULT: if the system prompt contains a "
       "`MessagingChannel=[NAME]` line (the user is asking through a chat app: "
       "Slack/Telegram/Discord/SMS), DEFAULT `deliver_to` to NAME for EVERY "
       "event type — including briefings — UNLESS the user explicitly says "
       "otherwise.  Reasoning: the user is paying attention to the chat surface "
       "(that's where they typed the request); a local-only event fires where "
       "they aren't listening.  This applies to both one-shot ('briefing on X "
       "in two minutes') and recurring ('morning briefing every weekday') "
       "events — recurring briefings set from Slack continue to land on Slack "
       "daily until the user says otherwise.  Override when the user says "
       "\"set it locally\", \"don't send to Slack\", \"just on the speaker\", "
       "\"silent\" (audio cue, not messaging — keep deliver_to), etc.\n"
       "  EXPLICIT TARGET: when the user names a different channel than the "
       "current one ('send it to my Discord', 'ping family group'), set "
       "`deliver_to` to THAT channel — don't second-guess.\n"
       "  NO MESSAGING IN PROMPT: when the system prompt has no "
       "`MessagingChannel=[...]` line (WebUI / local mic / satellite), only set "
       "`deliver_to` when the user explicitly asks for a chat-surface delivery.\n"
       "  RESPONSE COHERENCE: whatever delivery you set in `deliver_to`, REFLECT "
       "IT in your reply to the user.  If you set `deliver_to=slack_main`, say "
       "\"...delivered to Slack\" or similar.  If you omitted `deliver_to`, do "
       "NOT claim delivery to a chat surface.  Telling the user you'll send to "
       "Discord when you didn't set `deliver_to` is a hallucination — surface "
       "the actual tool-call shape in the reply.\n"
       "  The engine verifies the channel belongs to the current user at "
       "delivery time, so a guessed or stale name fails safely (logged, no "
       "leak).\n\n"
       "Example with explicit silent override (voice user said 'don't say this aloud'): "
       "{\"type\":\"briefing\",\"name\":\"Iran War News\","
       "\"duration_minutes\":2,\"say_aloud\":false,"
       "\"steps\":[{\"tool_name\":\"search\",\"tool_action\":\"query\","
       "\"tool_value\":\"Iran War news today\"}]}\n"
       "Example with default audio (recurring morning briefing, no explicit override): "
       "{\"type\":\"briefing\",\"name\":\"Morning Briefing\","
       "\"fire_at\":\"2026-05-22T07:00:00\",\"recurrence\":\"weekdays\","
       "\"steps\":[{\"tool_name\":\"weather\",\"tool_action\":\"get\","
       "\"tool_value\":\"Atlanta\"},{\"tool_name\":\"search\",\"tool_action\":\"search\","
       "\"tool_value\":\"top tech news today\"}]}\n"
       "Example with messaging fan-out (user said 'send the morning briefing to Slack'): "
       "{\"type\":\"briefing\",\"name\":\"Morning Briefing\","
       "\"fire_at\":\"2026-05-22T07:00:00\",\"recurrence\":\"weekdays\","
       "\"deliver_to\":\"slack_D0B6SQGB31C\","
       "\"steps\":[{\"tool_name\":\"weather\",\"tool_action\":\"get\","
       "\"tool_value\":\"Atlanta\"}]}\n"
       "Example editing a briefing (user said 'add my stock holdings news to the morning "
       "briefing' — action='update', append only the new step): "
       "{\"name\":\"Morning Briefing\",\"add_steps\":[{\"tool_name\":\"search\","
       "\"tool_action\":\"news\",\"tool_value\":\"AAPL NVDA TSLA stock news today\"}]}",
   .params = scheduler_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,

   .init = scheduler_tool_init,
   .cleanup = scheduler_tool_cleanup,
   .callback = scheduler_tool_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int scheduler_tool_register(void) {
   return tool_registry_register(&scheduler_metadata);
}
