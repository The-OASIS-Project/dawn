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
 * A turn and its conversation's lease (conv_turn_lease.h): one turn runs on a
 * conversation at a time.  The turn's wait for the lease, what its client is
 * told meanwhile, taking the lease when the turn learns its conversation late,
 * and what others may ask about the turn holding a lease.  Part of the session
 * unit (session_history.c).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "core/conv_turn_lease.h"
#include "core/session_manager.h"
#include "core/session_stop.h"
#include "logging.h"

/* No client to tell in a build without the WebUI. */
__attribute__((weak)) void session_turn_waiting_notice(session_t *session,
                                                       int64_t conv_id,
                                                       int what,
                                                       uint32_t holder) {
   (void)session;
   (void)conv_id;
   (void)what;
   (void)holder;
}

/* Caller holds history_mutex.  The running turn's own code is the caller. */
static bool turn_is_caller_locked(const session_t *session) {
   return session->turn_active && !session_turn_reads_elsewhere_locked(session);
}

/* A turn's wait for its conversation's lease. */
typedef struct {
   session_t *session;
   int64_t conv;
   bool told; /* the client was told it waits */
   struct timespec since;
} lease_wait_t;

/* A turn whose client is gone waits no longer than the lease calls a holder
 * stale: it keeps the session from being reaped while it waits. */
static bool disconnected_too_long(const lease_wait_t *w) {
   const int stale = conv_turn_lease_stale_after();
   if (stale <= 0 || !atomic_load(&w->session->disconnected)) {
      return false;
   }
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   if ((long)(now.tv_sec - w->since.tv_sec) < stale) {
      return false;
   }
   OLOG_WARNING("Session %u: its client is gone and conversation %lld is still held after %ds; "
                "the turn stops waiting",
                w->session->session_id, (long long)w->conv, stale);
   return true;
}

/* The lease's stop check, called with no lock held each time the wait goes
 * on: the first call is the turn queuing behind the holder, so it tells the
 * client.  A Stop or the session's teardown ends the wait. */
static bool lease_wait_should_stop(void *ctx) {
   lease_wait_t *w = ctx;
   if (!w->told) {
      w->told = true;
      clock_gettime(CLOCK_MONOTONIC, &w->since);
      uint32_t holder = 0;
      (void)conv_turn_lease_holder(w->conv, &holder, NULL, NULL);
      OLOG_INFO("Session %u: turn waits for conversation %lld (held by session %u)",
                w->session->session_id, (long long)w->conv, holder);
      session_turn_waiting_notice(w->session, w->conv, SESSION_TURN_WAIT_BEGIN, holder);
   }
   return atomic_load(&w->session->cancel_requested) || atomic_load(&w->session->being_destroyed) ||
          disconnected_too_long(w);
}

int session_turn_lease_wait(session_t *session, int64_t conv, uint64_t token, bool background) {
   lease_wait_t w = { .session = session, .conv = conv };
   const int rc = conv_turn_lease_acquire(conv, session->session_id, token, lease_wait_should_stop,
                                          &w);
   if (w.told) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long waited_ms = (long)(now.tv_sec - w.since.tv_sec) * 1000L +
                             (now.tv_nsec - w.since.tv_nsec) / 1000000L;
      OLOG_INFO("Session %u: %s conversation %lld after %ld ms", session->session_id,
                rc == CONV_TURN_LEASE_GRANTED ? "turn got" : "turn stopped waiting for",
                (long long)conv, waited_ms);
      if (rc == CONV_TURN_LEASE_GRANTED) {
         session_turn_waiting_notice(session, conv, SESSION_TURN_WAIT_GRANTED, 0);
      }
   }
   /* A background turn's client sent nothing: there is no text to give back. */
   if (rc == CONV_TURN_LEASE_STOPPED && !background) {
      session_turn_waiting_notice(session, conv, SESSION_TURN_WAIT_UNSENT, 0);
   }
   return rc;
}

void session_release_stray_lease(session_t *session) {
   pthread_mutex_lock(&session->history_mutex);
   const int64_t conv = session->turn_active ? session->turn_lease_conv : 0;
   const uint64_t token = session->turn_lease_token;
   if (conv > 0) {
      session->turn_lease_conv = 0;
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (conv > 0) {
      OLOG_ERROR("Session %u: releasing conversation %lld held by a turn still running",
                 session->session_id, (long long)conv);
      (void)conv_turn_lease_release(conv, token);
   }
}

bool session_bind_lease(session_t *session, int64_t conv_id, bool *took) {
   *took = false;
   pthread_mutex_lock(&session->history_mutex);
   const bool mine = turn_is_caller_locked(session);
   const bool unsent = session->turn_unsent;
   const bool background = session->turn_background;
   const int64_t held = session->turn_lease_conv;
   const uint64_t token = session->turn_lease_token;
   pthread_mutex_unlock(&session->history_mutex);
   if (!mine || unsent || held == conv_id) {
      return !(mine && unsent);
   }
   if (held != 0) {
      /* A turn holds one conversation; none of today's callers moves it. */
      OLOG_ERROR("Session %u: turn holding conversation %lld binds to %lld without its lease",
                 session->session_id, (long long)held, (long long)conv_id);
      return true;
   }
   uint64_t holder_token = 0;
   const bool claimed = conv_turn_lease_holder(conv_id, NULL, &holder_token, NULL) &&
                        holder_token == token;
   const int rc = claimed ? CONV_TURN_LEASE_GRANTED
                          : session_turn_lease_wait(session, conv_id, token, background);
   pthread_mutex_lock(&session->history_mutex);
   if (rc == CONV_TURN_LEASE_GRANTED) {
      session->turn_lease_conv = conv_id;
      *took = true;
   } else if (rc == CONV_TURN_LEASE_STOPPED) {
      session->turn_unsent = true;
   }
   pthread_mutex_unlock(&session->history_mutex);
   return rc != CONV_TURN_LEASE_STOPPED;
}

bool session_turn_unsent(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool unsent = session->turn_active && session->turn_unsent;
   pthread_mutex_unlock(&session->history_mutex);
   return unsent;
}

uint64_t session_turn_claim_token(session_t *session, bool own_thread) {
   if (!session) {
      return 0;
   }
   pthread_mutex_lock(&session->history_mutex);
   /* From another thread, only a turn that adopts the conversation created for
    * it (session_bind_created_conversation) can take the claim over. */
   const bool ok = session->turn_active && session->turn_lease_conv == 0 && !session->turn_unsent &&
                   (own_thread ? turn_is_caller_locked(session)
                               : session->turn_awaits_conversation);
   const uint64_t token = ok ? session->turn_lease_token : 0;
   pthread_mutex_unlock(&session->history_mutex);
   return token;
}

bool session_turn_keep_created_lease(session_t *session, int64_t conv_id, uint64_t token) {
   if (!session || conv_id <= 0 || token == 0) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool mine = session->turn_active && session->turn_lease_token == token;
   /* Already recorded (the turn took it over itself): kept, never released twice. */
   const bool keep = mine &&
                     (session->turn_lease_conv == conv_id ||
                      (session->turn_lease_conv == 0 && session->turn_history_conv == conv_id));
   if (keep) {
      session->turn_lease_conv = conv_id;
   }
   pthread_mutex_unlock(&session->history_mutex);
   return keep;
}

bool session_turn_holds_lease(session_t *session, int64_t conv_id, uint64_t token, int *user_out) {
   if (!session || conv_id <= 0 || token == 0) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool holds = session->turn_active && session->turn_lease_conv == conv_id &&
                      session->turn_lease_token == token;
   if (holds && user_out) {
      *user_out = session->turn_user_id;
   }
   pthread_mutex_unlock(&session->history_mutex);
   return holds;
}

bool session_turn_cancel_if_holds(session_t *session, int64_t conv_id, uint64_t token) {
   if (!session || conv_id <= 0 || token == 0) {
      return false;
   }
   /* Under the lock that guards the token: a turn ending clears it under the
    * same lock, so the cancel can't fall on the session's next turn. */
   pthread_mutex_lock(&session->history_mutex);
   const bool holds = session->turn_active && session->turn_lease_conv == conv_id &&
                      session->turn_lease_token == token;
   if (holds) {
      /* The user's Stop, pressed from another of their clients. */
      session_stop_record_locked(session);
      session_cancel_turn(session);
   }
   pthread_mutex_unlock(&session->history_mutex);
   return holds;
}
