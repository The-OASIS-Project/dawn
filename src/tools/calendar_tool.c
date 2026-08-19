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
 * Calendar LLM tool — voice-controlled CalDAV calendar access.
 * Actions: calendars, today, range, next, search, add, update, delete
 */

#include "tools/calendar_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/iso8601.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "logging.h"
#include "tools/calendar_service.h"
#include "tools/oauth_client.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MAX_EVENTS 20

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *calendar_tool_callback(const char *action, char *value, int *should_respond);
static int calendar_tool_init(void);
static void calendar_tool_cleanup(void);
static bool calendar_tool_available(void);

/* =============================================================================
 * JSON Helpers
 * ============================================================================= */

static const char *json_get_str(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_int(val);
}

static bool json_get_bool(struct json_object *obj, const char *key, bool def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_boolean(val);
}

/* =============================================================================
 * Occurrence Formatter
 * ============================================================================= */

/** Format time for natural speech: "10 AM", "2:30 PM" (not "10:00 AM", "02:30 PM") */
static void format_time_speech(const struct tm *t, char *buf, size_t buf_len) {
   int hour = t->tm_hour % 12;
   if (hour == 0)
      hour = 12;
   const char *ampm = t->tm_hour >= 12 ? "PM" : "AM";

   if (t->tm_min == 0)
      snprintf(buf, buf_len, "%d %s", hour, ampm);
   else
      snprintf(buf, buf_len, "%d:%02d %s", hour, t->tm_min, ampm);
}

/* If the most recent oauth refresh on this thread detected invalid_grant,
 * return a heap-allocated "re-link" message naming the revoked account.
 * Caller checks for non-NULL and returns it instead of a generic failure.
 * Returns NULL when no revocation has been detected on this thread. */
static char *check_oauth_revoked(void) {
   char revoked_account[128] = { 0 };
   if (!oauth_was_last_refresh_revoked(revoked_account, sizeof(revoked_account))) {
      return NULL;
   }
   char *msg = malloc(384);
   if (!msg)
      return NULL;
   snprintf(msg, 384,
            "Error: OAuth tokens for '%s' have been revoked at the provider. Tell the user "
            "to re-link this calendar account in WebUI Settings -> Calendar.  Do NOT retry — "
            "retrying with the same tokens will keep failing until re-authorized.",
            revoked_account[0] ? revoked_account : "this account");
   return msg;
}

/* Append a single occurrence line to the strbuf.  Returns 0 on success or
 * STRBUF_E_OOM if the buffer cap was reached (caller should bail). */
static int format_occurrence_sb(const calendar_occurrence_t *occ, strbuf_t *sb) {
   int rc;
   if (occ->all_day) {
      rc = strbuf_appendf(sb, "- %s (all day: %s)%s%s", occ->summary, occ->dtstart_date,
                          occ->location[0] ? " @ " : "", occ->location);
   } else {
      struct tm start_tm, end_tm;
      localtime_r(&occ->dtstart, &start_tm);
      localtime_r(&occ->dtend, &end_tm);

      char start_str[32], end_str[32];
      format_time_speech(&start_tm, start_str, sizeof(start_str));
      format_time_speech(&end_tm, end_str, sizeof(end_str));

      char date_str[32];
      strftime(date_str, sizeof(date_str), "%a %b %d", &start_tm);

      if (start_tm.tm_yday != end_tm.tm_yday || start_tm.tm_year != end_tm.tm_year) {
         char date_end_str[32];
         strftime(date_end_str, sizeof(date_end_str), "%a %b %d", &end_tm);
         rc = strbuf_appendf(sb, "- %s (%s %s - %s %s)%s%s", occ->summary, date_str, start_str,
                             date_end_str, end_str, occ->location[0] ? " @ " : "", occ->location);
      } else {
         rc = strbuf_appendf(sb, "- %s (%s, %s - %s)%s%s", occ->summary, date_str, start_str,
                             end_str, occ->location[0] ? " @ " : "", occ->location);
      }
   }
   if (rc < 0)
      return STRBUF_E_OOM;

   if (occ->event_uid[0])
      rc = strbuf_appendf(sb, " [UID: %s]", occ->event_uid);
   return (rc < 0) ? STRBUF_E_OOM : 0;
}

/* =============================================================================
 * Access Summary Footer (appended to query results when read-only accounts exist)
 * ============================================================================= */

static void append_access_summary_sb(strbuf_t *sb, int user_id) {
   char writable[512] = { 0 }, ro[512] = { 0 };
   if (calendar_service_get_access_summary(user_id, writable, sizeof(writable), ro, sizeof(ro)) >
       0) {
      strbuf_appendf(sb, "\n\nWritable calendars: %s\nRead-only calendars (no AI edits): %s",
                     writable[0] ? writable : "(none)", ro);
   }
}

/* Helper: finalize a calendar tool response buffer.  Mirrors the pattern in
 * memory_callback — return strbuf contents via strbuf_steal, falling back to
 * a friendly error string on OOM.  Caller must free the returned heap pointer. */
static char *finalize_calendar_response(strbuf_t *sb) {
   if (strbuf_oom(sb)) {
      strbuf_free(sb);
      return strdup("Error: response buffer exceeded safety cap.");
   }
   char *out = strbuf_steal(sb);
   return out ? out : strdup("Error: out of memory.");
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_calendars(int user_id) {
   calendar_account_t accounts[CALENDAR_MAX_ACCOUNTS];
   int acct_count = 0;
   calendar_db_account_list(user_id, accounts, 16, &acct_count);

   if (acct_count <= 0)
      return strdup("No calendar accounts configured.");

   /* Use strbuf so a deeply-nested account/calendar list cannot silently
    * truncate mid-row the way the prior fixed 4KB buffer did. */
   strbuf_t sb;
   strbuf_init(&sb, 2048);
   strbuf_appendf(&sb, "Calendar accounts (%d):\n", acct_count);

   for (int a = 0; a < acct_count; a++) {
      const char *status = accounts[a].enabled ? "" : " [DISABLED]";
      const char *ro = accounts[a].read_only ? " [READ-ONLY]" : "";
      if (strbuf_appendf(&sb, "\n%s%s%s:\n", accounts[a].name, ro, status) < 0)
         break;

      calendar_calendar_t cals[16];
      int cal_count = 0;
      calendar_db_calendar_list(accounts[a].id, cals, 16, &cal_count);
      if (cal_count <= 0) {
         strbuf_append(&sb, "  (no calendars synced yet)\n");
      } else {
         for (int c = 0; c < cal_count; c++) {
            const char *active = cals[c].is_active ? "" : " [disabled]";
            if (strbuf_appendf(&sb, "  - %s%s\n", cals[c].display_name, active) < 0)
               break;
         }
      }
   }

   return finalize_calendar_response(&sb);
}

static char *handle_today(struct json_object *details, int user_id) {
   const char *tz = g_config.localization.timezone;
   const char *calendar_name = json_get_str(details, "calendar");

   calendar_occurrence_t events[MAX_EVENTS];
   int count = calendar_service_today(user_id, tz, calendar_name, events, MAX_EVENTS);

   strbuf_t sb;
   strbuf_init(&sb, 2048);

   if (count <= 0) {
      if (calendar_name)
         strbuf_appendf(&sb,
                        "No events scheduled for today on calendar '%s'. "
                        "Use the 'calendars' action to list available calendars.",
                        calendar_name);
      else
         strbuf_append(&sb, "No events scheduled for today.");
   } else {
      strbuf_appendf(&sb, "Today's events (%d):\n", count);
      for (int i = 0; i < count; i++) {
         if (format_occurrence_sb(&events[i], &sb) != 0)
            break;
         if (strbuf_append(&sb, "\n") < 0)
            break;
      }
   }
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_range(struct json_object *details, int user_id) {
   const char *start_str = json_get_str(details, "start");
   const char *end_str = json_get_str(details, "end");
   const char *calendar_name = json_get_str(details, "calendar");

   if (!start_str) {
      return strdup("Error: 'start' is required for range query (ISO 8601 datetime)");
   }

   time_t start = iso8601_parse(start_str);
   if (start == (time_t)-1)
      return strdup("Error: invalid 'start' datetime format. Use ISO 8601 — e.g. "
                    "'2026-05-27T14:00:00' (local time), '2026-05-27T14:00:00Z' (UTC), or "
                    "'2026-05-27' (date only).");

   time_t end;
   if (end_str) {
      end = iso8601_parse(end_str);
      if (end == (time_t)-1)
         return strdup("Error: invalid 'end' datetime format. Use ISO 8601 — e.g. "
                       "'2026-05-27T15:00:00' (local time) or '2026-05-27T15:00:00Z' (UTC).");
   } else {
      end = start + 86400; /* Default: 24 hours */
   }

   calendar_occurrence_t events[MAX_EVENTS];
   int count = calendar_service_range(user_id, start, end, calendar_name, events, MAX_EVENTS);

   strbuf_t sb;
   strbuf_init(&sb, 2048);

   if (count <= 0) {
      if (calendar_name)
         strbuf_appendf(&sb,
                        "No events in the specified range on calendar '%s'. "
                        "Use the 'calendars' action to list available calendars.",
                        calendar_name);
      else
         strbuf_append(&sb, "No events in the specified range.");
   } else {
      strbuf_appendf(&sb, "Events in range (%d):\n", count);
      for (int i = 0; i < count; i++) {
         if (format_occurrence_sb(&events[i], &sb) != 0)
            break;
         if (strbuf_append(&sb, "\n") < 0)
            break;
      }
   }
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_next(struct json_object *details, int user_id) {
   const char *calendar_name = json_get_str(details, "calendar");

   calendar_occurrence_t occ;
   int rc = calendar_service_next(user_id, calendar_name, &occ);

   if (rc != 0) {
      if (calendar_name) {
         char msg[256];
         snprintf(msg, sizeof(msg),
                  "No upcoming events found on calendar '%s'. "
                  "Use the 'calendars' action to list available calendars.",
                  calendar_name);
         return strdup(msg);
      }
      return strdup("No upcoming events found.");
   }

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_append(&sb, "Next event:\n");
   format_occurrence_sb(&occ, &sb);
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_search(struct json_object *details, int user_id) {
   const char *query = json_get_str(details, "query");
   if (!query || !query[0])
      return strdup("Error: 'query' text is required for search");

   const char *calendar_name = json_get_str(details, "calendar");

   calendar_occurrence_t events[MAX_EVENTS];
   int count = calendar_service_search(user_id, query, calendar_name, events, 20);

   strbuf_t sb;
   strbuf_init(&sb, 2048);

   if (count <= 0) {
      if (calendar_name)
         strbuf_appendf(&sb,
                        "No events matching '%s' on calendar '%s'. "
                        "Use the 'calendars' action to list available calendars.",
                        query, calendar_name);
      else
         strbuf_appendf(&sb, "No events matching '%s'.", query);
   } else {
      strbuf_appendf(&sb, "Search results for '%s' (%d):\n", query, count);
      for (int i = 0; i < count; i++) {
         if (format_occurrence_sb(&events[i], &sb) != 0)
            break;
         if (strbuf_append(&sb, "\n") < 0)
            break;
      }
   }
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_add(struct json_object *details, int user_id) {
   const char *summary = json_get_str(details, "summary");
   if (!summary || !summary[0])
      return strdup("Error: 'summary' (event title) is required");

   const char *start_str = json_get_str(details, "start");
   if (!start_str)
      return strdup("Error: 'start' datetime is required (ISO 8601)");

   time_t start = iso8601_parse(start_str);
   if (start == (time_t)-1)
      return strdup("Error: invalid 'start' datetime format. Use ISO 8601 — e.g. "
                    "'2026-05-27T14:00:00' (local time), '2026-05-27T14:00:00Z' (UTC), or "
                    "'2026-05-27' (date only).");

   time_t end = 0;
   const char *end_str = json_get_str(details, "end");
   if (end_str) {
      end = iso8601_parse(end_str);
      if (end == (time_t)-1)
         return strdup("Error: invalid 'end' datetime format. Use ISO 8601 — e.g. "
                       "'2026-05-27T15:00:00' (local time) or '2026-05-27T15:00:00Z' (UTC).");
   }

   const char *location = json_get_str(details, "location");
   const char *description = json_get_str(details, "description");
   bool all_day = json_get_bool(details, "all_day", false);
   const char *calendar_name = json_get_str(details, "calendar");
   const char *rrule = json_get_str(details, "rrule");
   const char *tz = g_config.localization.timezone;

   char uid[256] = { 0 };
   int rc = calendar_service_add(user_id, summary, start, end, location, description, all_day,
                                 calendar_name, rrule, tz, uid, sizeof(uid));
   if (rc == 2)
      return strdup("Error: the target calendar belongs to a read-only account. "
                    "The user has restricted this account from AI modifications. "
                    "Try specifying a different writable calendar.");
   if (rc != 0) {
      char *revoked = check_oauth_revoked();
      if (revoked)
         return revoked;
      return strdup("Error: failed to create event on calendar server. Either no "
                    "calendars are synced yet for any account (run action='calendars' to "
                    "confirm) or the CalDAV server rejected the request — retry once.");
   }

   char *buf = malloc(512);
   if (!buf)
      return strdup("Event created successfully.");

   struct tm st;
   localtime_r(&start, &st);
   char time_str[80];
   if (all_day) {
      strftime(time_str, sizeof(time_str), "%A, %B %d, %Y", &st);
   } else {
      char t_str[16];
      format_time_speech(&st, t_str, sizeof(t_str));
      char date_str[48];
      strftime(date_str, sizeof(date_str), "%A, %B %d", &st);
      snprintf(time_str, sizeof(time_str), "%s at %s", date_str, t_str);
   }

   int pos = snprintf(buf, 512, "Event created: '%s' on %s.%s%s", summary, time_str,
                      location ? " Location: " : "", location ? location : "");
   if (uid[0])
      snprintf(buf + pos, 512 - pos, "\nUID: %s", uid);
   return buf;
}

static char *handle_update(struct json_object *details, int user_id) {
   const char *uid = json_get_str(details, "uid");
   if (!uid || !uid[0])
      return strdup("Error: 'uid' of the event to update is required");

   const char *summary = json_get_str(details, "summary");
   const char *start_str = json_get_str(details, "start");
   const char *end_str = json_get_str(details, "end");
   const char *location = json_get_str(details, "location");
   const char *description = json_get_str(details, "description");

   time_t start = start_str ? iso8601_parse(start_str) : 0;
   time_t end = end_str ? iso8601_parse(end_str) : 0;

   int rc = calendar_service_update(user_id, uid, summary, start, end, location, description);
   if (rc == 2)
      return strdup("Error: the event belongs to a read-only account. "
                    "The user has restricted this account from AI modifications.");
   if (rc != 0) {
      char *revoked = check_oauth_revoked();
      if (revoked)
         return revoked;
      return strdup("Error: failed to update event. Either the UID does not exist on any "
                    "writable calendar (re-run 'today'/'range'/'search' to get current "
                    "UIDs) or the CalDAV server rejected the change. Retry once.");
   }

   return strdup("Event updated successfully.");
}

static char *handle_delete(struct json_object *details, int user_id) {
   const char *uid = json_get_str(details, "uid");
   if (!uid || !uid[0])
      return strdup("Error: 'uid' of the event to delete is required");

   int rc = calendar_service_delete(user_id, uid);
   if (rc == 2)
      return strdup("Error: the event belongs to a read-only account. "
                    "The user has restricted this account from AI modifications.");
   if (rc != 0) {
      char *revoked = check_oauth_revoked();
      if (revoked)
         return revoked;
      return strdup("Error: failed to delete event. Either the UID does not exist on any "
                    "writable calendar (re-run 'today'/'range'/'search' to get current "
                    "UIDs) or the CalDAV server rejected the request.");
   }

   return strdup("Event deleted successfully.");
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *calendar_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!action || !action[0])
      return strdup("Error: action is required");

   /* Parse details JSON */
   struct json_object *details = NULL;
   if (value && value[0]) {
      details = json_tokener_parse(value);
      if (!details)
         return strdup("Error: invalid JSON in details parameter");
   } else {
      details = json_object_new_object();
   }

   int user_id = tool_get_current_user_id();

   char *result = NULL;

   if (strcmp(action, "calendars") == 0) {
      result = handle_calendars(user_id);
   } else if (strcmp(action, "today") == 0) {
      result = handle_today(details, user_id);
   } else if (strcmp(action, "range") == 0) {
      result = handle_range(details, user_id);
   } else if (strcmp(action, "next") == 0) {
      result = handle_next(details, user_id);
   } else if (strcmp(action, "search") == 0) {
      result = handle_search(details, user_id);
   } else if (strcmp(action, "add") == 0) {
      result = handle_add(details, user_id);
   } else if (strcmp(action, "update") == 0) {
      result = handle_update(details, user_id);
   } else if (strcmp(action, "delete") == 0) {
      result = handle_delete(details, user_id);
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Error: unknown action '%s'. Valid: calendars, today, range, next, search, add, "
               "update, delete",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Tool Lifecycle
 * ============================================================================= */

static int calendar_tool_init(void) {
   return calendar_service_init(&g_config.calendar);
}

static void calendar_tool_cleanup(void) {
   calendar_service_shutdown();
}

static bool calendar_tool_available(void) {
   return calendar_service_available();
}

/* =============================================================================
 * Tool Parameter Definition
 * ============================================================================= */

static const treg_param_t calendar_params[] = {
   {
       .name = "action",
       .description = "The calendar action: 'calendars' (list all calendar accounts and their "
                      "calendars), 'today' (today's events), 'range' (events in date range), "
                      "'next' (next upcoming event), 'search' (find events by text), "
                      "'add' (create new event), 'update' (modify event), 'delete' (remove event)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "calendars", "today", "range", "next", "search", "add", "update",
                        "delete" },
       .enum_count = 8,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields (pass as JSON-encoded string).\n"
           "For 'calendars': no fields needed.\n"
           "For 'today': {calendar?}.\n"
           "For 'range': {start (ISO 8601, required), end (ISO 8601, optional, default "
           "+24h), calendar?}.\n"
           "For 'next': {calendar?}.\n"
           "For 'search': {query (text to search in event titles/locations), calendar?}.\n"
           "For 'add': {summary (required), start (ISO 8601, required), end (ISO 8601, "
           "optional), location (optional), description (optional), all_day (bool, "
           "optional), calendar?, rrule (RFC 5545 recurrence, optional — e.g. "
           "'FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=10')}.\n"
           "For 'update': {uid (required, from previous query — emitted as '[UID: ...]' on "
           "each result row), summary, start, end, location, description — only specified "
           "fields are changed}.\n"
           "For 'delete': {uid (required)}.\n"
           "  calendar?: the calendar DISPLAY NAME as returned by action='calendars' (e.g. "
           "'Family', 'Work'). NOT an account name and NOT an arbitrary guess; if "
           "uncertain, run action='calendars' first to enumerate. Omit to search across all "
           "calendars on all accounts.\n"
           "  start/end ISO 8601 format: no timezone suffix = LOCAL time "
           "('2026-05-27T14:00:00'); 'Z' = UTC; '+05:00' = explicit offset; bare date "
           "'2026-05-27' is accepted too.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const tool_metadata_t calendar_metadata = {
   .name = "calendar",
   .device_string = "calendar",
   .topic = "dawn",
   .aliases = { "cal", "schedule", "events", "appointment" },
   .alias_count = 4,

   .description = "Access and manage calendar events via CalDAV. "
                  "List available calendars ('which calendars are available'), "
                  "check today's schedule ('what's on my calendar today'), "
                  "find upcoming events ('when is my next meeting'), "
                  "search events ('do I have anything with dentist'), "
                  "create events ('add a meeting tomorrow at 2pm'), "
                  "update or delete events by UID.",
   .params = calendar_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SECRETS | TOOL_CAP_SCHEDULABLE,
   .default_local = true,
   .default_remote = true,

   .init = calendar_tool_init,
   .cleanup = calendar_tool_cleanup,
   .is_available = calendar_tool_available,
   .callback = calendar_tool_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int calendar_tool_register(void) {
   return tool_registry_register(&calendar_metadata);
}
