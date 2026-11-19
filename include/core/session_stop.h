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
 * The Stop the user presses on a session's running turn: the only stop whose
 * reply is saved as far as it streamed.  Every other cancel (a superseding
 * turn, a timeout, a teardown, the wake word) stops the turn as before.
 */

#ifndef SESSION_STOP_H
#define SESSION_STOP_H

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Record the running turn as stopped by the user, while its reply hasn't
 * arrived: newest first among the session's last TURN_ORIGIN_STOPPED_MAX, the
 * oldest falling off.  Caller holds history_mutex.  Inline: the lease's
 * "stop the other reply" records too.
 */
static inline void session_stop_record_locked(session_t *session) {
   const uint64_t token = session->turn_owner_token;
   if (!session->turn_active || token == 0 || session->turn_reply) {
      return;
   }
   for (int i = 0; i < TURN_ORIGIN_STOPPED_MAX; i++) {
      if (session->stopped_turn_tokens[i] == token) {
         return;
      }
   }
   memmove(&session->stopped_turn_tokens[1], &session->stopped_turn_tokens[0],
           (TURN_ORIGIN_STOPPED_MAX - 1) * sizeof(session->stopped_turn_tokens[0]));
   session->stopped_turn_tokens[0] = token;
}

/**
 * @brief Stop the running turn at the user's request
 *
 * Records the turn as stopped by the user while its reply hasn't arrived
 * (among the session's last TURN_ORIGIN_STOPPED_MAX), then cancels it
 * (session_cancel_turn).  With no turn running, or one whose reply is in, it
 * only cancels.
 */
void session_stop_turn_by_user(session_t *session);

/**
 * @brief The running turn's reply arrived whole: it isn't a stopped turn
 *
 * Takes the running turn out of the stopped turns (what it staged can be
 * confirmed: the user saw the whole read-back).
 */
void session_turn_stop_forget(session_t *session);

/** Whether the running turn is one the user stopped (session_stop_turn_by_user).
 *  Inline: the tool loop asks too, and links no session unit. */
static inline bool session_turn_stopped_by_user(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   bool stopped = false;
   if (session->turn_active && session->turn_owner_token != 0) {
      for (int i = 0; i < TURN_ORIGIN_STOPPED_MAX && !stopped; i++) {
         stopped = session->stopped_turn_tokens[i] == session->turn_owner_token;
      }
   }
   pthread_mutex_unlock(&session->history_mutex);
   return stopped;
}

/** Whether the running turn's reply in its history is one the user stopped. */
bool session_turn_reply_stopped(session_t *session);

/**
 * Whether the reply of the ended turn's exchange being written now
 * (session_bind_created_conversation, session_turn_take_prior) is one the user
 * stopped.  Read before its row is written; the claim ends at
 * session_stamp_claimed().
 */
bool session_claimed_reply_stopped(session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_STOP_H */
