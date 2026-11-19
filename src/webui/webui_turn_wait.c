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
 * A turn waiting for another on its conversation (one turn per conversation
 * at a time, conv_turn_lease.h): what the waiting client is told, and its
 * request to stop the reply it waits for.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/conv_turn_lease.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "logging.h"
#include "webui/webui_handlers.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"
#include "webui/webui_turn_ref.h"

/* What a client that knows no turn_waiting frame shows meanwhile. */
#define TURN_WAIT_DETAIL "Finishing the reply already in progress…"
#define TURN_WAIT_DETAIL_JOB "Finishing the background job's turn…"

/* What kind of turn the session @p holder_id runs: "turn", "job" or "channel". */
static const char *holder_kind(uint32_t holder_id) {
   if (holder_id == CONV_TURN_LEASE_POST_SESSION) {
      return "turn"; /* a post being written: gone in a moment */
   }
   session_t *holder = session_get_for_reconnect(holder_id);
   if (!holder) {
      return "turn";
   }
   const char *kind = holder->type == SESSION_TYPE_JOB         ? "job"
                      : holder->type == SESSION_TYPE_MESSAGING ? "channel"
                                                               : "turn";
   session_release(holder);
   return kind;
}

/* Strong override of the session unit's weak hook: the waiting session's own
 * client, on the turn's thread (its client_ref is this thread's). */
void session_turn_waiting_notice(session_t *session, int64_t conv_id, int what, uint32_t holder) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return; /* a job or a channel has no client to tell */
   }
   const char *kind = what == SESSION_TURN_WAIT_BEGIN ? holder_kind(holder) : NULL;
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   const char *ref = webui_turn_ref_get();
   if (ref && ref[0]) {
      json_object_object_add(payload, "client_ref", json_object_new_string(ref));
   }
   if (what == SESSION_TURN_WAIT_UNSENT) {
      json_object_object_add(payload, "text_saved", json_object_new_boolean(0));
   } else {
      json_object_object_add(payload, "waiting",
                             json_object_new_boolean(what == SESSION_TURN_WAIT_BEGIN));
      if (what == SESSION_TURN_WAIT_BEGIN) {
         json_object_object_add(payload, "holder", json_object_new_string(kind));
         json_object_object_add(payload, "stop_after_ms",
                                json_object_new_int(CONV_TURN_WAIT_STOP_OFFER_MS));
      }
   }
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type",
                          json_object_new_string(
                              what == SESSION_TURN_WAIT_UNSENT ? "turn_unsent" : "turn_waiting"));
   json_object_object_add(root, "payload", payload);
   webui_send_session_json(session, json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
   json_object_put(root);

   if (what == SESSION_TURN_WAIT_BEGIN) {
      webui_send_state_for_conversation(session, "thinking",
                                        strcmp(kind, "job") == 0 ? TURN_WAIT_DETAIL_JOB
                                                                 : TURN_WAIT_DETAIL,
                                        conv_id);
   } else if (what == SESSION_TURN_WAIT_UNSENT) {
      webui_send_state_for_conversation(session, "idle", NULL, conv_id);
   }
}

static void send_stop_result(ws_connection_t *conn, int64_t conv_id, const char *error) {
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(payload, "success", json_object_new_boolean(error == NULL));
   if (error) {
      json_object_object_add(payload, "error", json_object_new_string(error));
   }
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("stop_other_reply_response"));
   json_object_object_add(root, "payload", payload);
   send_json_response(conn, root);
   json_object_put(root);
}

const char *webui_stop_other_reply_core(uint32_t requester, int user_id, int64_t conv_id) {
   uint32_t holder_id = 0;
   uint64_t token = 0;
   if (conv_id <= 0 || !conv_turn_lease_holder(conv_id, &holder_id, &token, NULL) ||
       holder_id == CONV_TURN_LEASE_POST_SESSION) {
      return "NOT_RUNNING";
   }
   if (holder_id == requester) {
      /* Its own turn: the ordinary Stop is for that. */
      return "FORBIDDEN";
   }
   session_t *holder = session_get_for_reconnect(holder_id);
   int owner = 0;
   if (!holder || !session_turn_holds_lease(holder, conv_id, token, &owner) || owner != user_id) {
      /* Another user's turn answers as no turn at all: no word on what runs where. */
      if (holder) {
         session_release(holder);
      }
      return "NOT_RUNNING";
   }
   /* A job's own turn is cancelled as the job, so it is recorded as cancelled
    * rather than failed; anything else (a job session re-engaging a
    * conversation, a channel, a browser) is the turn's ordinary Stop, and only
    * while that same turn still holds the conversation. */
   const job_cancel_result_t jr = holder->type == SESSION_TYPE_JOB
                                      ? job_manager_cancel_or_retire(conv_id, owner, NULL, 0)
                                      : JOB_CANCEL_NOT_FOUND;
   const bool stopped = jr == JOB_CANCEL_SIGNALLED || jr == JOB_CANCEL_RETIRED ||
                        session_turn_cancel_if_holds(holder, conv_id, token);
   session_release(holder);
   if (!stopped) {
      return "NOT_RUNNING"; /* it ended meanwhile */
   }
   OLOG_INFO("WebUI: session %u stopped session %u's turn on conversation %lld", requester,
             holder_id, (long long)conv_id);
   return NULL;
}

void webui_handle_stop_other_reply(ws_connection_t *conn, json_object *payload) {
   if (!conn || !conn->session || !conn->authenticated || conn->is_satellite ||
       conn->auth_user_id <= 0) {
      return;
   }
   json_object *o = NULL;
   const int64_t conv_id = (payload && json_object_object_get_ex(payload, "conversation_id", &o))
                               ? json_object_get_int64(o)
                               : 0;
   send_stop_result(conn, conv_id,
                    webui_stop_other_reply_core(conn->session->session_id, conn->auth_user_id,
                                                conv_id));
}
