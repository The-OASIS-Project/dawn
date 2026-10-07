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
 * Where the email executor's replies go: the session that asked, by id (never
 * a connection pointer), and only while it is still the same user's.
 */

#include <pthread.h>
#include <stdlib.h>

#include "core/session_manager.h"
#include "webui/webui_email_exec.h"
#include "webui/webui_server.h"

void *webui_email_exec_session_open(uint32_t session_id, int user_id) {
   session_t *session = session_get(session_id); /* NULL once disconnected */
   if (!session)
      return NULL;
   pthread_mutex_lock(&session->metrics_mutex);
   const int owner = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   /* A tab that logged in as someone else since doesn't get this user's mail. */
   if (owner != user_id) {
      session_release(session);
      return NULL;
   }
   return session;
}

void webui_email_exec_session_send(void *session, char *json) {
   webui_send_session_json_take((session_t *)session, json);
}

void webui_email_exec_session_close(void *session) {
   if (session)
      session_release((session_t *)session);
}
