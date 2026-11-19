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
 * The user's Stop on a session's running turn (session_stop.h).
 */

#include "core/session_stop.h"

#include <pthread.h>

#include "llm/llm_history_kind.h"

void session_stop_turn_by_user(session_t *session) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active) {
      session->stopped_turn_token = session->turn_owner_token;
   }
   pthread_mutex_unlock(&session->history_mutex);
   session_cancel_turn(session);
}

bool session_turn_stopped_by_user(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool stopped = session->turn_active && session->turn_owner_token != 0 &&
                        session->stopped_turn_token == session->turn_owner_token;
   pthread_mutex_unlock(&session->history_mutex);
   return stopped;
}

bool session_turn_reply_stopped(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool stopped = session->turn_active && llm_history_is_stopped(session->turn_reply);
   pthread_mutex_unlock(&session->history_mutex);
   return stopped;
}

bool session_claimed_reply_stopped(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool stopped = llm_history_is_stopped(session->claimed_reply_msg);
   pthread_mutex_unlock(&session->history_mutex);
   return stopped;
}
