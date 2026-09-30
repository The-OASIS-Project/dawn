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
 * The session reaper (see session_reaper.h).  session_destroy() only ends a
 * session, so no caller ever waits on a reference another thread must drop:
 * the WebUI service thread once destroyed a session whose music socket held a
 * reference only that thread's next turn could release, and stalled 30 s.
 */

#include "core/session_reaper.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "core/session_compaction.h"
#include "logging.h"

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond;
static pthread_t s_thread;
static bool s_running;        /* the thread is up (s_mutex) */
static bool s_stop;           /* asked to stop (s_mutex) */
static bool s_woken;          /* a release reached zero, or a session arrived (s_mutex) */
static session_t *s_incoming; /* ended, not yet seen by the thread (s_mutex) */
static atomic_int s_pending;  /* ended and not yet finished */
static bool s_held;           /* finishing held off (shutdown, after the drain; s_mutex) */

/* Deadlines count in monotonic seconds: a wall-clock step (NTP on a board
 * with no RTC) must not leak a session it just ended. */
static time_t mono_sec(void) {
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return now.tv_sec;
}

static int ref_count_of(session_t *s) {
   pthread_mutex_lock(&s->ref_mutex);
   const int n = s->ref_count;
   pthread_mutex_unlock(&s->ref_mutex);
   return n;
}

static void finish(session_t *s) {
   session_manager_finalize(s);
   atomic_fetch_sub(&s_pending, 1);
}

/* Still referenced: leaked (recoverable) rather than freed under its holder
 * (a use-after-free).  @p why says when: its deadline passed, or shutdown. */
static void leak(session_t *s, int refs, const char *why) {
   OLOG_ERROR("Session %u: ref_count stuck at %d %s — leaking session instead of freeing "
              "(UAF guard)",
              s->session_id, refs, why);
   atomic_fetch_sub(&s_pending, 1);
}

/* Without the thread: finish on the caller's thread, waiting as a destroy
 * always did. */
static void reap_inline(session_t *s) {
   session_compaction_teardown(s);
   pthread_mutex_lock(&s->ref_mutex);
   int waited = 0;
   while (s->ref_count > 0 && waited < SESSION_DESTROY_REF_WAIT_MAX_SEC) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += 1;
      if (pthread_cond_timedwait(&s->ref_zero_cond, &s->ref_mutex, &until) == ETIMEDOUT) {
         waited++;
      }
   }
   const int refs = s->ref_count;
   pthread_mutex_unlock(&s->ref_mutex);
   if (refs > 0) {
      leak(s, refs, "after its wait");
   } else {
      finish(s);
   }
}

/* One pass over the sessions waiting for their last reference: finish those
 * at zero, leak those past their deadline.  Returns the list still waiting. */
static session_t *reap_waiting(session_t *waiting, bool held) {
   if (held) {
      return waiting;
   }
   const time_t now = mono_sec();
   session_t *keep = NULL;
   while (waiting) {
      session_t *s = waiting;
      waiting = s->reap_next;
      s->reap_next = NULL;
      const int refs = ref_count_of(s);
      if (refs <= 0) {
         finish(s);
      } else if (now >= s->reap_deadline) {
         leak(s, refs, "after its wait");
      } else {
         s->reap_next = keep;
         keep = s;
      }
   }
   return keep;
}

static void *reaper_thread(void *arg) {
   (void)arg;
   session_t *waiting = NULL;
   for (;;) {
      pthread_mutex_lock(&s_mutex);
      if (!s_stop && !s_incoming && !s_woken) {
         if (waiting) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += (long)SESSION_REAPER_POLL_MS * 1000000L;
            until.tv_sec += until.tv_nsec / 1000000000L;
            until.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&s_cond, &s_mutex, &until);
         } else {
            pthread_cond_wait(&s_cond, &s_mutex);
         }
      }
      session_t *arrived = s_incoming;
      s_incoming = NULL;
      s_woken = false;
      const bool stop = s_stop;
      const bool held = s_held;
      pthread_mutex_unlock(&s_mutex);

      /* A newly ended session's compaction worker is joined here, off the
       * caller's thread (the join can take as long as its transfer's abort). */
      while (arrived) {
         session_t *s = arrived;
         arrived = s->reap_next;
         session_compaction_teardown(s);
         s->reap_next = waiting;
         waiting = s;
      }
      waiting = reap_waiting(waiting, held);
      if (stop) {
         /* session_reaper_stop() finishes the rest. */
         pthread_mutex_lock(&s_mutex);
         while (waiting) {
            session_t *s = waiting;
            waiting = s->reap_next;
            s->reap_next = s_incoming;
            s_incoming = s;
         }
         pthread_mutex_unlock(&s_mutex);
         return NULL;
      }
   }
}

int session_reaper_start(void) {
   pthread_mutex_lock(&s_mutex);
   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      return 0;
   }
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&s_cond, &attr);
   pthread_condattr_destroy(&attr);
   s_stop = false;
   s_woken = false;
   if (pthread_create(&s_thread, NULL, reaper_thread, NULL) != 0) {
      pthread_cond_destroy(&s_cond);
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("Session reaper: failed to start; destroyed sessions finish on the caller");
      return 1;
   }
   s_running = true;
   pthread_mutex_unlock(&s_mutex);
   return 0;
}

void session_reaper_enqueue(session_t *session) {
   if (!session) {
      return;
   }
   atomic_fetch_add(&s_pending, 1);
   session->reap_deadline = mono_sec() + SESSION_DESTROY_REF_WAIT_MAX_SEC;
   pthread_mutex_lock(&s_mutex);
   if (!s_running || s_stop) {
      pthread_mutex_unlock(&s_mutex);
      reap_inline(session);
      return;
   }
   session->reap_next = s_incoming;
   s_incoming = session;
   pthread_cond_signal(&s_cond);
   pthread_mutex_unlock(&s_mutex);
}

void session_reaper_wake(void) {
   pthread_mutex_lock(&s_mutex);
   if (s_running) {
      s_woken = true;
      pthread_cond_signal(&s_cond);
   }
   pthread_mutex_unlock(&s_mutex);
}

bool session_reaper_drain(int timeout_ms) {
   struct timespec step = { .tv_sec = 0, .tv_nsec = 20L * 1000000L };
   for (int waited = 0; atomic_load(&s_pending) > 0 && waited < timeout_ms; waited += 20) {
      session_reaper_wake();
      nanosleep(&step, NULL);
   }
   return atomic_load(&s_pending) == 0;
}

void session_reaper_stop(void) {
   pthread_mutex_lock(&s_mutex);
   if (!s_running) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }
   s_stop = true;
   pthread_cond_signal(&s_cond);
   pthread_mutex_unlock(&s_mutex);
   pthread_join(s_thread, NULL);

   pthread_mutex_lock(&s_mutex);
   session_t *left = s_incoming;
   s_incoming = NULL;
   s_running = false;
   pthread_cond_destroy(&s_cond);
   pthread_mutex_unlock(&s_mutex);

   while (left) {
      session_t *s = left;
      left = s->reap_next;
      s->reap_next = NULL;
      session_compaction_teardown(s);
      const int refs = ref_count_of(s);
      if (refs <= 0) {
         finish(s);
      } else {
         leak(s, refs, "at shutdown");
      }
   }
}

void session_reaper_hold(void) {
   pthread_mutex_lock(&s_mutex);
   s_held = true;
   pthread_mutex_unlock(&s_mutex);
}

int session_reaper_pending(void) {
   return atomic_load(&s_pending);
}
