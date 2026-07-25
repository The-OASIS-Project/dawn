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
 * WebUI conversation-event replay (background jobs Phase 2 — the observe half).
 *
 * This is the READ side of the durable event log that src/auth/auth_db_events.c
 * writes.  It exists so a client that was not connected while a background job
 * ran can still reconstruct what happened, and so a client that reconnects
 * mid-job can resume without gaps or duplicates.
 *
 * Attach ordering (§6) — messages, then events, then the in-memory ring, then
 * the live tail.  That sequence is deliberate: messages and events interleave by
 * insertion order, and the ring holds only the not-yet-persisted tail of the
 * CURRENT turn, so replaying it before the events it follows would render the
 * conversation out of order for a dumb consumer (a line-printer / TUI) that
 * appends what it receives.
 *
 * Lives outside webui_history.c because that file is already past the 1,500-line
 * soft limit, and this is a distinct surface with its own frame contract.
 */

#include <json-c/json.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "logging.h"
#include "webui/webui_internal.h"

/* Events per replay frame.  A long-running job accumulates a few dozen events,
 * so this covers the overwhelming majority in one round-trip while bounding the
 * frame size for the pathological case; `has_more` tells the client to ask again
 * with the last seq it received. */
#define CONV_EVENT_REPLAY_MAX 200

void webui_send_conversation_events(ws_connection_t *conn, int64_t conv_id, int64_t last_seq) {
   if (!conn || conv_id <= 0 || conn->auth_user_id <= 0) {
      return;
   }

   conv_event_t rows[CONV_EVENT_REPLAY_MAX];
   int n = 0;
   /* Ownership is the SQL JOIN inside conv_db_event_list (§8.3) — a conversation
    * this user does not own comes back empty rather than as a distinct error, so
    * there is nothing to probe with. */
   if (conv_db_event_list(conv_id, conn->auth_user_id, last_seq, CONV_EVENT_REPLAY_MAX, rows, &n) !=
       AUTH_DB_SUCCESS) {
      OLOG_WARNING("webui_events: replay read failed for conv %lld", (long long)conv_id);
      return;
   }
   if (n == 0) {
      return; /* nothing to replay — don't spend a frame saying so */
   }

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("conversation_events"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));

   json_object *arr = json_object_new_array();
   for (int i = 0; i < n; i++) {
      json_object *e = json_object_new_object();
      json_object_object_add(e, "seq", json_object_new_int64(rows[i].seq));
      json_object_object_add(e, "kind", json_object_new_string(rows[i].kind));
      json_object_object_add(e, "created_at", json_object_new_int64((int64_t)rows[i].created_at));
      /* A pruned row keeps its place in the sequence but has no body: send the
       * field as null rather than "" so the renderer can say "payload expired"
       * instead of showing a step that looks like it did nothing (§8.8). */
      if (rows[i].payload) {
         json_object_object_add(e, "payload", json_object_new_string(rows[i].payload));
      }
      json_object_array_add(arr, e);
   }
   json_object_object_add(payload, "events", arr);

   /* A full batch means there may be more; the client re-attaches with the last
    * seq it saw.  Exactly-full-and-no-more costs one empty follow-up request. */
   json_object_object_add(payload, "has_more", json_object_new_boolean(n == CONV_EVENT_REPLAY_MAX));
   json_object_object_add(payload, "last_seq", json_object_new_int64(rows[n - 1].seq));
   json_object_object_add(root, "payload", payload);

   send_json_response(conn, root);
   json_object_put(root);

   conv_db_event_rows_free(rows, n);

   OLOG_INFO("WebUI: replayed %d event(s) for conv %lld (from seq %lld)", n, (long long)conv_id,
             (long long)last_seq);
}
