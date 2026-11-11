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
 * The message_appended frame (webui_message_frame.h).
 */

#include "webui/webui_message_frame.h"

#include <stdlib.h>
#include <string.h>

#include "webui/webui_email_ref.h"

const session_t *webui_message_origin(const session_t *session) {
   return (session != NULL && session->type != SESSION_TYPE_JOB) ? session : NULL;
}

json_object *webui_message_appended_frame(int64_t conv_id,
                                          int64_t msg_id,
                                          const char *role,
                                          const char *text,
                                          const char *reasoning,
                                          unsigned stream_id,
                                          const session_t *origin,
                                          const char *email_ref) {
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("message_appended"));

   json_object *p = json_object_new_object();
   json_object_object_add(p, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(p, "message_id", json_object_new_int64(msg_id));
   json_object_object_add(p, "role", json_object_new_string(role ? role : "assistant"));
   json_object_object_add(p, "text", json_object_new_string(text));
   /* Phase-0 cross-viewer fan-out fields (SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN §6a):
    * reasoning lets a non-origin viewer render the E3 panel; stream_id lets the
    * origin correlate + adopt instead of re-rendering. */
   if (reasoning && reasoning[0]) {
      json_object_object_add(p, "reasoning", json_object_new_string(reasoning));
   }
   json_object_object_add(p, "stream_id", json_object_new_int64((int64_t)stream_id));
   /* stream_id counts per session: the session lets a client tell its own stream N
    * from another session's stream N on the same conversation. */
   if (origin != NULL) {
      json_object_object_add(p, "session_id", json_object_new_int64(origin->session_id));
   }
   webui_row_add_email_ref(p, role, email_ref);
   json_object_object_add(root, "payload", p);
   return root;
}

char *webui_message_appended_origin_json(json_object *frame,
                                         const session_t *origin,
                                         const char *client_ref) {
   json_object *p = NULL;
   if (!origin || !client_ref || !client_ref[0] ||
       !json_object_object_get_ex(frame, "payload", &p)) {
      return NULL;
   }
   json_object_object_add(p, "client_ref", json_object_new_string(client_ref));
   const char *with_ref = json_object_to_json_string_ext(frame, JSON_C_TO_STRING_PLAIN);
   char *origin_json = with_ref ? strdup(with_ref) : NULL;
   json_object_object_del(p, "client_ref");
   return origin_json;
}
