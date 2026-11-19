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
#include <string.h>

#include "llm/llm_history_kind.h"

/* Caller holds history_mutex.  The index of @p token among the stopped turns,
 * or -1. */
static int stopped_index_locked(const session_t *session, uint64_t token) {
   for (int i = 0; i < TURN_ORIGIN_STOPPED_MAX; i++) {
      if (session->stopped_turn_tokens[i] == token) {
         return i;
      }
   }
   return -1;
}

void session_stop_turn_by_user(session_t *session) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   session_stop_record_locked(session);
   pthread_mutex_unlock(&session->history_mutex);
   session_cancel_turn(session);
}

void session_turn_stop_forget(session_t *session) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   const int i = session->turn_owner_token != 0
                     ? stopped_index_locked(session, session->turn_owner_token)
                     : -1;
   if (i >= 0) {
      memmove(&session->stopped_turn_tokens[i], &session->stopped_turn_tokens[i + 1],
              (size_t)(TURN_ORIGIN_STOPPED_MAX - 1 - i) * sizeof(session->stopped_turn_tokens[0]));
      session->stopped_turn_tokens[TURN_ORIGIN_STOPPED_MAX - 1] = 0;
   }
   pthread_mutex_unlock(&session->history_mutex);
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
