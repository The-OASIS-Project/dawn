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
 * Conversation-event emit: persist + live fan-out as one operation.
 * See conv_event.h for why the pairing lives here rather than at the call sites.
 */

#include "core/conv_event.h"

#include <stdlib.h>

#include "auth/auth_db.h"
#include "logging.h"

/* Weak default: no WebUI linked (WEBUI-off builds, unit tests).  Strong
 * override in src/webui/webui_broadcasts.c. */
__attribute__((weak)) void webui_broadcast_conversation_event(int user_id,
                                                              int64_t conv_id,
                                                              int64_t seq,
                                                              const char *kind,
                                                              const char *payload) {
   (void)user_id;
   (void)conv_id;
   (void)seq;
   (void)kind;
   (void)payload;
}

void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   if (conv_id <= 0 || kind == NULL) {
      free(payload_owned);
      return;
   }

   int64_t seq = 0;
   int rc = conv_db_event_append(conv_id, kind, payload_owned, &seq);
   if (rc != AUTH_DB_SUCCESS) {
      /* Swallow: an observe-side event must never break the turn it describes.
       * The turn still runs, still persists its messages, and still completes —
       * the client just loses one step of visibility. */
      OLOG_WARNING("conv_event: append '%s' for conv %lld failed (rc=%d)", kind, (long long)conv_id,
                   rc);
      free(payload_owned);
      return;
   }

   /* Fan out only after a successful append, and with the DB-assigned seq —
    * a client that receives a live frame during its attach replay dedups on
    * exactly this value. */
   if (user_id > 0) {
      webui_broadcast_conversation_event(user_id, conv_id, seq, kind, payload_owned);
   }
   free(payload_owned);
}

/* Weak default: no WebUI linked.  Strong override in webui_broadcasts.c. */
__attribute__((weak)) void webui_broadcast_message_appended(int user_id,
                                                            int64_t conv_id,
                                                            int64_t msg_id,
                                                            const char *role,
                                                            const char *text) {
   (void)user_id;
   (void)conv_id;
   (void)msg_id;
   (void)role;
   (void)text;
}

void conv_event_notify_message_appended(int64_t conv_id,
                                        int user_id,
                                        int64_t msg_id,
                                        const char *role,
                                        const char *text) {
   if (conv_id <= 0 || user_id <= 0 || text == NULL || text[0] == '\0') {
      return;
   }
   webui_broadcast_message_appended(user_id, conv_id, msg_id, role ? role : "assistant", text);
}
