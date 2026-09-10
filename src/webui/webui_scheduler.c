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
 * the project author(s).
 *
 * WebUI Scheduler Queue Handlers
 *
 * Surfaces the scheduled_events table to the WebUI panel: list (active +
 * missed), cancel (series), cancel_occurrence (this one only, chain preserved).
 *
 * Authorization shape:
 * - list: satellite-allowed, scoped by conn->auth_user_id (empty for unassigned)
 * - cancel / cancel_occurrence: enforces ev.user_id == conn->auth_user_id with
 *   NO satellite carve-out.  Cancel is not a live-alarm operation — the
 *   carve-out that exists for dismiss/snooze in webui_message_dispatch.c does
 *   not apply.
 *
 * NOT_FOUND is returned for both "doesn't exist" and "not yours" to avoid
 * ownership-existence leakage.
 */

#include <string.h>

#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "logging.h"
#include "webui/webui_handlers.h"
#include "webui/webui_internal.h"

/* tool_value can be up to 2048 bytes per scheduler_db.h.  Truncating in the
 * list payload reduces XSS payload room and JSON bloat — the LLM tool path
 * still serves the full value when needed. */
#define SCHED_TOOL_VALUE_DISPLAY_MAX 200

/* Best-effort UTF-8 truncation: back up to the start of a multi-byte sequence
 * so we don't leave a half-emitted codepoint at the cut point. */
static void truncate_to_display(const char *src, char *dst, size_t dst_size) {
   if (!src || !dst || dst_size == 0)
      return;
   size_t len = strlen(src);
   if (len + 1 <= dst_size) {
      memcpy(dst, src, len + 1);
      return;
   }
   size_t cut = dst_size - 4; /* room for '...' + NUL */
   if (cut > 0) {
      /* Walk backwards past UTF-8 continuation bytes (10xxxxxx). */
      while (cut > 0 && (((unsigned char)src[cut]) & 0xC0) == 0x80)
         cut--;
   }
   memcpy(dst, src, cut);
   dst[cut] = '\0';
   /* Append ellipsis marker */
   if (cut + 3 < dst_size) {
      dst[cut] = '.';
      dst[cut + 1] = '.';
      dst[cut + 2] = '.';
      dst[cut + 3] = '\0';
   }
}

/* Per-event payload builder.  prebatched_steps + prebatched_count may be
 * NULL/0 for non-briefing rows or when the caller didn't pre-batch the
 * steps table (in which case we fall back to the legacy per-row lookup).
 * Passing pre-batched values from the caller eliminates the per-event
 * auth_db_mutex acquisition the old code paid for every briefing row. */
static json_object *serialize_event(const sched_event_t *e,
                                    const sched_briefing_step_t *prebatched_steps,
                                    int prebatched_count) {
   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "id", json_object_new_int64(e->id));
   json_object_object_add(obj, "event_type",
                          json_object_new_string(sched_event_type_to_str(e->event_type)));
   json_object_object_add(obj, "status", json_object_new_string(sched_status_to_str(e->status)));
   json_object_object_add(obj, "name", json_object_new_string(e->name));
   json_object_object_add(obj, "message", json_object_new_string(e->message));
   json_object_object_add(obj, "fire_at", json_object_new_int64((int64_t)e->fire_at));
   json_object_object_add(obj, "created_at", json_object_new_int64((int64_t)e->created_at));
   json_object_object_add(obj, "duration_sec", json_object_new_int(e->duration_sec));
   json_object_object_add(obj, "snoozed_until", json_object_new_int64((int64_t)e->snoozed_until));
   json_object_object_add(obj, "snooze_count", json_object_new_int(e->snooze_count));
   json_object_object_add(obj, "say_aloud", json_object_new_int((int)e->say_aloud));
   json_object_object_add(obj, "deliver_to", json_object_new_string(e->deliver_to));
   json_object_object_add(obj, "instructions", json_object_new_string(e->instructions));
   json_object_object_add(obj, "recurrence",
                          json_object_new_string(sched_recurrence_to_str(e->recurrence)));
   json_object_object_add(obj, "recurrence_days", json_object_new_string(e->recurrence_days));
   json_object_object_add(obj, "original_time", json_object_new_string(e->original_time));
   json_object_object_add(obj, "source_location", json_object_new_string(e->source_location));
   json_object_object_add(obj, "tool_name", json_object_new_string(e->tool_name));
   json_object_object_add(obj, "tool_action", json_object_new_string(e->tool_action));

   char tool_value_disp[SCHED_TOOL_VALUE_DISPLAY_MAX + 4];
   truncate_to_display(e->tool_value, tool_value_disp, sizeof(tool_value_disp));
   json_object_object_add(obj, "tool_value", json_object_new_string(tool_value_disp));

   /* Multi-step briefings: emit the steps array so the panel can render the
    * full chain on demand (click-to-expand).  Each step's tool_value gets the
    * same display truncation as the top-level field — caps payload size and
    * limits XSS surface in the panel.  Tasks/timers/alarms/reminders skip
    * this query path. */
   if (e->event_type == SCHED_EVENT_BRIEFING) {
      sched_briefing_step_t local_steps[SCHED_BRIEFING_STEPS_MAX];
      const sched_briefing_step_t *steps;
      int step_count;
      if (prebatched_steps) {
         steps = prebatched_steps;
         step_count = prebatched_count;
      } else {
         step_count = 0;
         scheduler_db_briefing_steps_list(e->id, local_steps, SCHED_BRIEFING_STEPS_MAX,
                                          &step_count);
         steps = local_steps;
      }
      if (step_count > 0) {
         json_object *arr = json_object_new_array();
         for (int i = 0; i < step_count; i++) {
            json_object *step = json_object_new_object();
            json_object_object_add(step, "tool_name", json_object_new_string(steps[i].tool_name));
            json_object_object_add(step, "tool_action",
                                   json_object_new_string(steps[i].tool_action));
            char step_value_disp[SCHED_TOOL_VALUE_DISPLAY_MAX + 4];
            truncate_to_display(steps[i].tool_value, step_value_disp, sizeof(step_value_disp));
            json_object_object_add(step, "tool_value", json_object_new_string(step_value_disp));
            json_object_array_add(arr, step);
         }
         json_object_object_add(obj, "steps", arr);
      }
   }

   /* Convenience: client uses effective_fire_at to sort/render the active
    * group regardless of whether the row is pending or snoozed. */
   time_t eff = (e->snoozed_until > 0) ? e->snoozed_until : e->fire_at;
   json_object_object_add(obj, "effective_fire_at", json_object_new_int64((int64_t)eff));
   return obj;
}

void handle_scheduler_list_events(ws_connection_t *conn) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("scheduler_events_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *events_array = json_object_new_array();

   if (conn->auth_user_id > 0) {
      /* Stack arithmetic: sched_event_t is ~4 KB (tool_value[2048] +
       * instructions[1024] dominate).  Two SCHED_MAX_RESULTS=50 arrays =
       * ~400 KB on this stack frame.  Safe on the libwebsockets service thread
       * (8 MB glibc pthread stack), and deliberately inside the
       * `auth_user_id > 0` gate so unassigned satellites don't pay the cost.
       * (The scheduler_tool `handle_list` path runs on the 512 KB parallel-tool
       * thread and therefore heap-allocates its equivalent array.)  If
       * SCHED_MAX_RESULTS grows past ~400 or the libws thread later hosts deeper
       * call chains, switch these to calloc/free. */
      sched_event_t active[SCHED_MAX_RESULTS];
      sched_event_t missed[SCHED_MAX_RESULTS];
      int active_count = scheduler_db_list_user_events(conn->auth_user_id, -1, active,
                                                       SCHED_MAX_RESULTS);
      int missed_count = scheduler_db_list_user_missed(conn->auth_user_id, missed,
                                                       SCHED_MAX_RESULTS);

      /* Batch-load briefing steps for both arrays under a single lock
       * acquisition each — old path acquired the auth_db mutex per briefing
       * row inside serialize_event (up to 2 × 50 = 100 cycles per panel
       * open).  Heap-alloc the step tables; ~870 KB per side at
       * SCHED_MAX_RESULTS=50 doesn't fit comfortably on the libws stack
       * alongside the existing 294 KB of event arrays.  Falls back to
       * per-row lookup on OOM (correctness preserved). */
      sched_briefing_step_t *active_steps = NULL;
      int *active_counts = NULL;
      sched_briefing_step_t *missed_steps = NULL;
      int *missed_counts = NULL;
      if (active_count > 0) {
         active_steps = calloc((size_t)active_count * SCHED_BRIEFING_STEPS_MAX,
                               sizeof(sched_briefing_step_t));
         active_counts = calloc((size_t)active_count, sizeof(int));
         if (active_steps && active_counts) {
            int64_t ids[SCHED_MAX_RESULTS];
            for (int i = 0; i < active_count; i++)
               ids[i] = active[i].id;
            scheduler_db_briefing_steps_list_many(ids, active_count, active_steps, active_counts);
         } else {
            free(active_steps);
            free(active_counts);
            active_steps = NULL;
            active_counts = NULL;
         }
      }
      if (missed_count > 0) {
         missed_steps = calloc((size_t)missed_count * SCHED_BRIEFING_STEPS_MAX,
                               sizeof(sched_briefing_step_t));
         missed_counts = calloc((size_t)missed_count, sizeof(int));
         if (missed_steps && missed_counts) {
            int64_t ids[SCHED_MAX_RESULTS];
            for (int i = 0; i < missed_count; i++)
               ids[i] = missed[i].id;
            scheduler_db_briefing_steps_list_many(ids, missed_count, missed_steps, missed_counts);
         } else {
            free(missed_steps);
            free(missed_counts);
            missed_steps = NULL;
            missed_counts = NULL;
         }
      }

      for (int i = 0; i < active_count; i++) {
         const sched_briefing_step_t *ev_steps =
             active_steps ? &active_steps[(size_t)i * SCHED_BRIEFING_STEPS_MAX] : NULL;
         int ev_count = active_counts ? active_counts[i] : 0;
         json_object_array_add(events_array, serialize_event(&active[i], ev_steps, ev_count));
      }
      for (int i = 0; i < missed_count; i++) {
         const sched_briefing_step_t *ev_steps =
             missed_steps ? &missed_steps[(size_t)i * SCHED_BRIEFING_STEPS_MAX] : NULL;
         int ev_count = missed_counts ? missed_counts[i] : 0;
         json_object_array_add(events_array, serialize_event(&missed[i], ev_steps, ev_count));
      }
      free(active_steps);
      free(active_counts);
      free(missed_steps);
      free(missed_counts);
   }
   /* For unassigned satellites (auth_user_id <= 0) the events array stays
    * empty — quieter than an error frame, and matches how the dispatcher
    * already lets unauthenticated satellites through to scheduler_action. */

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "events", events_array);
   json_object_object_add(response, "payload", resp_payload);

   send_json_response(conn, response);
   json_object_put(response);
}

/* Shared authorization gate for the write paths.  Loads the event, enforces
 * ev.user_id == conn->auth_user_id (no satellite carve-out — cancel is not a
 * live-alarm operation), and returns SUCCESS only when the caller may touch
 * the row.  On failure, emits the appropriate error frame and returns
 * FAILURE so the caller bails out cleanly. */
static int authorize_event_for_user(ws_connection_t *conn,
                                    int64_t event_id,
                                    sched_event_t *ev_out) {
   if (event_id <= 0) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing or invalid event_id");
      return FAILURE;
   }
   if (conn->auth_user_id <= 0) {
      /* Same wording as the not-found branch to avoid leaking whether the
       * row exists.  Unassigned satellites end up here for cancel ops. */
      send_error_impl(conn->wsi, "NOT_FOUND", "Event not found");
      return FAILURE;
   }
   int get_rc = scheduler_db_get(event_id, ev_out);
   if (get_rc != SUCCESS) {
      send_error_impl(conn->wsi, "NOT_FOUND", "Event not found");
      return FAILURE;
   }
   if (ev_out->user_id != conn->auth_user_id) {
      send_error_impl(conn->wsi, "NOT_FOUND", "Event not found");
      return FAILURE;
   }
   return SUCCESS;
}

void handle_scheduler_cancel_event(ws_connection_t *conn, int64_t event_id) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn))
      return;

   sched_event_t ev;
   if (authorize_event_for_user(conn, event_id, &ev) != SUCCESS)
      return;

   int rc = scheduler_cancel_and_broadcast(event_id, ev.user_id);
   if (rc != SUCCESS) {
      send_error_impl(conn->wsi, "ALREADY_RESOLVED", "Event already fired or cancelled");
   }
}

void handle_scheduler_cancel_occurrence(ws_connection_t *conn, int64_t event_id) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn))
      return;

   sched_event_t ev;
   if (authorize_event_for_user(conn, event_id, &ev) != SUCCESS)
      return;

   int rc = scheduler_cancel_occurrence_and_broadcast(event_id, ev.user_id);
   if (rc != SUCCESS) {
      send_error_impl(conn->wsi, "ALREADY_RESOLVED", "Event already fired or cancelled");
   }
}

void handle_scheduler_clear_missed(ws_connection_t *conn, int64_t event_id) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn))
      return;

   sched_event_t ev;
   if (authorize_event_for_user(conn, event_id, &ev) != SUCCESS)
      return;

   int rc = scheduler_db_clear_missed(event_id, ev.user_id);
   if (rc == SUCCESS) {
      scheduler_broadcast_events_changed(ev.user_id);
   } else {
      send_error_impl(conn->wsi, "ALREADY_RESOLVED", "Event was not in 'missed' state");
   }
}
