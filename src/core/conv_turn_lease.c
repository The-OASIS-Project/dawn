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
 * One turn per conversation at a time: a lease per conversation, handed FIFO
 * from holder to waiter, with posts queued behind the holder.  See
 * conv_turn_lease.h.
 */

#include "core/conv_turn_lease.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"

#define WAITER_WAITING 0
#define WAITER_GRANTED 1
#define WAITER_STOPPED 2

/* A waiting turn's place in the FIFO, on its own stack. */
typedef struct lease_waiter {
   struct lease_waiter *next;
   uint32_t session;
   uint64_t token;
   int state;
   bool woken; /* conv_turn_lease_wake_session(): check should_stop again now */
   pthread_cond_t cond;
} lease_waiter_t;

typedef struct {
   bool in_use;
   int64_t conv;
   bool held;
   uint32_t holder_session;
   uint64_t holder_token;
   time_t holder_since;
   int depth;
   /* The holder's last release is writing queued posts with the mutex
    * dropped: no other release or hand-over may run on the entry meanwhile. */
   bool releasing;
   lease_waiter_t *head;
   lease_waiter_t *tail;
   conv_lease_item_t *posts_head;
   conv_lease_item_t *posts_tail;
} lease_entry_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static lease_entry_t s_entries[CONV_TURN_LEASE_SLOTS];
static conv_lease_write_fn s_write;
static conv_lease_drop_fn s_drop;
static bool s_shutdown;
static atomic_int s_stale_after;
static atomic_uint_fast64_t s_next_token = 1;

/* Invariant: an entry that isn't held has no waiter and no queued post (a
 * release hands it on and writes posts before letting go), so a free lease is
 * taken at once and never ahead of anyone. */

static lease_entry_t *entry_find_locked(int64_t conv) {
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      if (s_entries[i].in_use && s_entries[i].conv == conv)
         return &s_entries[i];
   }
   return NULL;
}

static lease_entry_t *entry_get_locked(int64_t conv) {
   lease_entry_t *e = entry_find_locked(conv);
   if (e)
      return e;
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      if (!s_entries[i].in_use) {
         memset(&s_entries[i], 0, sizeof(s_entries[i]));
         s_entries[i].in_use = true;
         s_entries[i].conv = conv;
         return &s_entries[i];
      }
   }
   return NULL;
}

static void entry_free_if_idle_locked(lease_entry_t *e) {
   if (e && !e->held && !e->head && !e->posts_head)
      e->in_use = false;
}

static bool token_holds_other_locked(uint64_t token, int64_t conv) {
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      const lease_entry_t *e = &s_entries[i];
      if (e->in_use && e->held && e->holder_token == token && e->conv != conv)
         return true;
   }
   return false;
}

static void take_locked(lease_entry_t *e, uint32_t session, uint64_t token) {
   e->held = true;
   e->holder_session = session;
   e->holder_token = token;
   e->holder_since = time(NULL);
   e->depth = 1;
}

static void waiter_remove_locked(lease_entry_t *e, lease_waiter_t *w) {
   lease_waiter_t *prev = NULL;
   for (lease_waiter_t *cur = e->head; cur; prev = cur, cur = cur->next) {
      if (cur != w)
         continue;
      if (prev)
         prev->next = cur->next;
      else
         e->head = cur->next;
      if (e->tail == cur)
         e->tail = prev;
      cur->next = NULL;
      return;
   }
}

static conv_lease_item_t *post_pop_locked(lease_entry_t *e) {
   conv_lease_item_t *item = e->posts_head;
   if (item) {
      e->posts_head = item->next;
      if (!e->posts_head)
         e->posts_tail = NULL;
      item->next = NULL;
   }
   return item;
}

/* Write or dispose of one item with no lease lock held. */
static void write_item(conv_lease_write_fn write,
                       conv_lease_drop_fn drop,
                       conv_lease_item_t *item) {
   if (write)
      (void)write(item); /* a queued post's failure is the writer's to log */
   else if (drop)
      drop(item);
   else
      OLOG_ERROR("conv lease: a queued post was lost (no writer registered)");
}

/* The last release of @p e by its holder: write its queued posts (the lease
 * still held, so nothing interleaves), then hand it to the head waiter or free
 * it.  Called and returns with s_mutex held. */
static void finish_release_locked(lease_entry_t *e) {
   e->releasing = true;
   conv_lease_item_t *item;
   while ((item = post_pop_locked(e)) != NULL) {
      conv_lease_write_fn write = s_write;
      conv_lease_drop_fn drop = s_drop;
      pthread_mutex_unlock(&s_mutex);
      write_item(write, drop, item);
      pthread_mutex_lock(&s_mutex);
   }
   e->releasing = false;
   lease_waiter_t *w = e->head;
   if (w) {
      waiter_remove_locked(e, w);
      take_locked(e, w->session, w->token);
      w->state = WAITER_GRANTED;
      pthread_cond_signal(&w->cond);
      return;
   }
   e->held = false;
   e->depth = 0;
   e->holder_token = 0;
   entry_free_if_idle_locked(e);
}

static void deadline_after(struct timespec *ts, long ms) {
   clock_gettime(CLOCK_MONOTONIC, ts);
   ts->tv_sec += ms / 1000;
   ts->tv_nsec += (ms % 1000) * 1000000L;
   if (ts->tv_nsec >= 1000000000L) {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
   }
}

uint64_t conv_turn_lease_new_token(void) {
   return atomic_fetch_add(&s_next_token, 1);
}

/* Wait in @p e's FIFO until granted or stopped.  Called and returns with
 * s_mutex held; the waiter is off the FIFO on return. */
static int wait_locked(lease_entry_t *e,
                       uint32_t session,
                       uint64_t token,
                       conv_lease_stop_fn should_stop,
                       void *ctx) {
   const int64_t conv = e->conv;
   lease_waiter_t w;
   memset(&w, 0, sizeof(w));
   w.session = session;
   w.token = token;
   w.state = WAITER_WAITING;
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&w.cond, &attr);
   pthread_condattr_destroy(&attr);
   if (e->tail)
      e->tail->next = &w;
   else
      e->head = &w;
   e->tail = &w;

   struct timespec started;
   clock_gettime(CLOCK_MONOTONIC, &started);
   bool stale_logged = false;
   for (;;) {
      if (w.state != WAITER_WAITING)
         break;

      /* Copy what a stale-wait line needs; log it with the lock dropped. */
      bool log_stale = false;
      uint32_t holder_session = 0;
      uint64_t holder_token = 0;
      long holder_age = 0;
      long waited = 0;
      const int stale_after = atomic_load(&s_stale_after);
      if (!stale_logged && stale_after > 0) {
         struct timespec now;
         clock_gettime(CLOCK_MONOTONIC, &now);
         waited = (long)(now.tv_sec - started.tv_sec);
         if (waited >= stale_after) {
            const lease_entry_t *mine = entry_find_locked(conv);
            if (mine && mine->held) {
               holder_session = mine->holder_session;
               holder_token = mine->holder_token;
               holder_age = (long)(time(NULL) - mine->holder_since);
            }
            log_stale = true;
            stale_logged = true;
         }
      }

      pthread_mutex_unlock(&s_mutex);
      if (log_stale) {
         OLOG_ERROR("conv lease: session %u has waited %lds for conversation %lld, held by "
                    "session %u (token %llu) for %lds",
                    session, waited, (long long)conv, holder_session,
                    (unsigned long long)holder_token, holder_age);
      }
      const bool stop = should_stop && should_stop(ctx);
      pthread_mutex_lock(&s_mutex);

      if (w.state != WAITER_WAITING)
         break; /* a grant wins over a stop noticed at the same time */
      if (!stop && w.woken) {
         w.woken = false; /* woken while should_stop ran: ask it again */
         continue;
      }
      if (stop) {
         /* Still this conversation's entry: it can't be reused while we wait in it. */
         lease_entry_t *mine = entry_find_locked(conv);
         if (mine) {
            waiter_remove_locked(mine, &w);
            entry_free_if_idle_locked(mine);
         }
         w.state = WAITER_STOPPED;
         break;
      }
      struct timespec wake;
      deadline_after(&wake, CONV_TURN_LEASE_WAIT_SLICE_MS);
      pthread_cond_timedwait(&w.cond, &s_mutex, &wake);
      w.woken = false;
   }
   pthread_cond_destroy(&w.cond);
   return w.state == WAITER_GRANTED ? CONV_TURN_LEASE_GRANTED : CONV_TURN_LEASE_STOPPED;
}

int conv_turn_lease_acquire(int64_t conv,
                            uint32_t session,
                            uint64_t token,
                            conv_lease_stop_fn should_stop,
                            void *ctx) {
   if (conv <= 0 || token == 0)
      return CONV_TURN_LEASE_FAILURE;
   pthread_mutex_lock(&s_mutex);
   if (s_shutdown) {
      pthread_mutex_unlock(&s_mutex);
      return CONV_TURN_LEASE_STOPPED;
   }
   lease_entry_t *e = entry_get_locked(conv);
   if (!e) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("conv lease: no free entry for conversation %lld", (long long)conv);
      return CONV_TURN_LEASE_FULL;
   }
   if (e->held && e->holder_token == token && e->releasing) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("conv lease: session %u asked for conversation %lld while releasing it", session,
                 (long long)conv);
      return CONV_TURN_LEASE_FAILURE;
   }
   if (e->held && e->holder_token == token) {
      e->depth++;
      pthread_mutex_unlock(&s_mutex);
      return CONV_TURN_LEASE_GRANTED;
   }
   if (!e->held) {
      const bool other = token_holds_other_locked(token, conv);
      take_locked(e, session, token);
      pthread_mutex_unlock(&s_mutex);
      if (other)
         OLOG_ERROR("conv lease: session %u took conversation %lld while holding another", session,
                    (long long)conv);
      return CONV_TURN_LEASE_GRANTED;
   }
   if (token_holds_other_locked(token, conv)) {
      entry_free_if_idle_locked(e);
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("conv lease: session %u asked for held conversation %lld while holding "
                 "another; not waiting",
                 session, (long long)conv);
      return CONV_TURN_LEASE_BUSY;
   }
   const int rc = wait_locked(e, session, token, should_stop, ctx);
   pthread_mutex_unlock(&s_mutex);
   return rc;
}

int conv_turn_lease_try_acquire(int64_t conv, uint32_t session, uint64_t token) {
   if (conv <= 0 || token == 0)
      return CONV_TURN_LEASE_FAILURE;
   pthread_mutex_lock(&s_mutex);
   if (s_shutdown) {
      pthread_mutex_unlock(&s_mutex);
      return CONV_TURN_LEASE_STOPPED;
   }
   lease_entry_t *e = entry_get_locked(conv);
   if (!e) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("conv lease: no free entry for conversation %lld", (long long)conv);
      return CONV_TURN_LEASE_FULL;
   }
   /* A free entry has no waiter (see the invariant above), so taking it jumps no one. */
   const bool taken = !e->held;
   if (taken)
      take_locked(e, session, token);
   pthread_mutex_unlock(&s_mutex);
   return taken ? CONV_TURN_LEASE_GRANTED : CONV_TURN_LEASE_BUSY;
}

bool conv_turn_lease_claim_new(int64_t conv, uint32_t session, uint64_t token) {
   return conv_turn_lease_try_acquire(conv, session, token) == CONV_TURN_LEASE_GRANTED;
}

int conv_turn_lease_release(int64_t conv, uint64_t token) {
   pthread_mutex_lock(&s_mutex);
   lease_entry_t *e = entry_find_locked(conv);
   if (!e || !e->held || e->holder_token != token || token == 0 || e->releasing) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("conv lease: release of conversation %lld by a token that doesn't hold it",
                 (long long)conv);
      return CONV_TURN_LEASE_FAILURE;
   }
   if (--e->depth == 0)
      finish_release_locked(e);
   pthread_mutex_unlock(&s_mutex);
   return SUCCESS;
}

int conv_turn_lease_post_or_defer(int64_t conv, conv_lease_item_t *item) {
   if (conv <= 0 || !item)
      return CONV_TURN_LEASE_FAILURE;
   pthread_mutex_lock(&s_mutex);
   conv_lease_write_fn write = s_write;
   if (!write) {
      pthread_mutex_unlock(&s_mutex);
      return CONV_TURN_LEASE_FAILURE;
   }
   item->next = NULL;
   lease_entry_t *e = s_shutdown ? NULL : entry_get_locked(conv);
   if (e && e->held) {
      if (e->posts_tail)
         e->posts_tail->next = item;
      else
         e->posts_head = item;
      e->posts_tail = item;
      pthread_mutex_unlock(&s_mutex);
      return CONV_TURN_LEASE_DEFERRED;
   }
   if (!e) {
      /* Shut down, or no entry to hold it under: no turn can take this
       * conversation either, so writing now can't land between a turn's rows. */
      const bool full = !s_shutdown;
      pthread_mutex_unlock(&s_mutex);
      if (full)
         OLOG_ERROR("conv lease: no free entry for conversation %lld; post written unheld",
                    (long long)conv);
      return write(item) ? CONV_TURN_LEASE_WRITTEN : CONV_TURN_LEASE_WRITE_FAILED;
   }
   const uint64_t token = conv_turn_lease_new_token();
   take_locked(e, CONV_TURN_LEASE_POST_SESSION, token);
   pthread_mutex_unlock(&s_mutex);
   const bool ok = write(item);
   conv_turn_lease_release(conv, token);
   return ok ? CONV_TURN_LEASE_WRITTEN : CONV_TURN_LEASE_WRITE_FAILED;
}

void conv_turn_lease_set_writer(conv_lease_write_fn write, conv_lease_drop_fn drop) {
   pthread_mutex_lock(&s_mutex);
   s_write = write;
   s_drop = drop;
   pthread_mutex_unlock(&s_mutex);
}

void conv_turn_lease_set_stale_after(int seconds) {
   atomic_store(&s_stale_after, seconds > 0 ? seconds : 0);
}

int conv_turn_lease_stale_after(void) {
   return atomic_load(&s_stale_after);
}

bool conv_turn_lease_holder(int64_t conv, uint32_t *session, uint64_t *token, time_t *since) {
   pthread_mutex_lock(&s_mutex);
   const lease_entry_t *e = entry_find_locked(conv);
   const bool held = e && e->held;
   if (held) {
      if (session)
         *session = e->holder_session;
      if (token)
         *token = e->holder_token;
      if (since)
         *since = e->holder_since;
   }
   pthread_mutex_unlock(&s_mutex);
   return held;
}

int conv_turn_lease_waiters(int64_t conv) {
   int n = 0;
   pthread_mutex_lock(&s_mutex);
   const lease_entry_t *e = entry_find_locked(conv);
   for (const lease_waiter_t *w = e ? e->head : NULL; w; w = w->next)
      n++;
   pthread_mutex_unlock(&s_mutex);
   return n;
}

int conv_turn_lease_release_session(uint32_t session) {
   int released = 0;
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      lease_entry_t *e = &s_entries[i];
      /* One whose holder is releasing it is handed on by that release. */
      if (!e->in_use || !e->held || e->holder_session != session || e->releasing)
         continue;
      e->depth = 0;
      finish_release_locked(e);
      released++;
   }
   pthread_mutex_unlock(&s_mutex);
   return released;
}

void conv_turn_lease_wake_session(uint32_t session) {
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      const lease_entry_t *e = &s_entries[i];
      if (!e->in_use)
         continue;
      for (lease_waiter_t *w = e->head; w; w = w->next) {
         if (w->session == session) {
            w->woken = true;
            pthread_cond_signal(&w->cond);
         }
      }
   }
   pthread_mutex_unlock(&s_mutex);
}

void conv_turn_lease_shutdown(void) {
   pthread_mutex_lock(&s_mutex);
   s_shutdown = true;
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      lease_entry_t *e = &s_entries[i];
      if (!e->in_use)
         continue;
      lease_waiter_t *w;
      while ((w = e->head) != NULL) {
         waiter_remove_locked(e, w);
         w->state = WAITER_STOPPED;
         pthread_cond_signal(&w->cond);
      }
      entry_free_if_idle_locked(e);
   }
   /* Write every queued post; one whose holder still runs is written anyway. */
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      lease_entry_t *e = &s_entries[i];
      conv_lease_item_t *item;
      while (e->in_use && (item = post_pop_locked(e)) != NULL) {
         const bool held = e->held;
         const int64_t conv = e->conv;
         const uint32_t holder = e->holder_session;
         conv_lease_write_fn write = s_write;
         conv_lease_drop_fn drop = s_drop;
         pthread_mutex_unlock(&s_mutex);
         if (held)
            OLOG_WARNING("conv lease: shutdown writes a post into conversation %lld while "
                         "session %u still holds it",
                         (long long)conv, holder);
         write_item(write, drop, item);
         pthread_mutex_lock(&s_mutex);
      }
      entry_free_if_idle_locked(e);
   }
   pthread_mutex_unlock(&s_mutex);
}
