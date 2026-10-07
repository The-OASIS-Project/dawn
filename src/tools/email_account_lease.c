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
 * One IMAP connection per account at a time: a lease per account, handed FIFO
 * from holder to waiter.  See email_account_lease.h.
 */

#include "tools/email_account_lease.h"

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "logging.h"

/* Accounts that can be in use (held or waited on) at once, across all users. */
#define EMAIL_LEASE_SLOTS 64

typedef struct {
   bool in_use;
   int64_t id;
   bool held;
   bool holder_is_thread;
   pthread_t holder_thread;
   email_lease_ticket_t *holder_ticket; /* the ticket holding it, reset to IDLE on release */
   email_lease_ticket_t *head;
   email_lease_ticket_t *tail;
} lease_slot_t;

/* A blocking acquirer's place in the FIFO; the ticket comes first so the
 * FIFO's ticket pointer is the waiter's address. */
typedef struct {
   email_lease_ticket_t t;
   pthread_cond_t cond;
   pthread_t thread;
} blocking_waiter_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_hook_idle = PTHREAD_COND_INITIALIZER;
static lease_slot_t s_slots[EMAIL_LEASE_SLOTS];
static email_lease_hook_t s_hook;
static int s_hook_running;

/* Invariant: a slot that isn't held has no waiter it could be handed to (a
 * blocking one, or a ticket while a hook is set), so a free lease is taken at
 * once and never ahead of anyone who could have had it. */

static lease_slot_t *slot_find_locked(int64_t id) {
   for (int i = 0; i < EMAIL_LEASE_SLOTS; i++) {
      if (s_slots[i].in_use && s_slots[i].id == id)
         return &s_slots[i];
   }
   return NULL;
}

static lease_slot_t *slot_get_locked(int64_t id) {
   lease_slot_t *s = slot_find_locked(id);
   if (s)
      return s;
   for (int i = 0; i < EMAIL_LEASE_SLOTS; i++) {
      if (!s_slots[i].in_use) {
         memset(&s_slots[i], 0, sizeof(s_slots[i]));
         s_slots[i].in_use = true;
         s_slots[i].id = id;
         return &s_slots[i];
      }
   }
   return NULL;
}

static void slot_free_if_idle_locked(lease_slot_t *s) {
   if (s && !s->held && !s->head)
      s->in_use = false;
}

static void fifo_push_locked(lease_slot_t *s, email_lease_ticket_t *t) {
   t->next = NULL;
   if (s->tail)
      s->tail->next = t;
   else
      s->head = t;
   s->tail = t;
}

static bool fifo_remove_locked(lease_slot_t *s, email_lease_ticket_t *t) {
   email_lease_ticket_t *prev = NULL;
   for (email_lease_ticket_t *cur = s->head; cur; prev = cur, cur = cur->next) {
      if (cur != t)
         continue;
      if (prev)
         prev->next = cur->next;
      else
         s->head = cur->next;
      if (s->tail == cur)
         s->tail = prev;
      cur->next = NULL;
      return true;
   }
   return false;
}

/* Hand a free slot to its first waiter that can take it.  A ticket granted here
 * is returned for the caller to pass to the hook once the lock is dropped (the
 * hook call is already counted in s_hook_running). */
static email_lease_ticket_t *grant_next_locked(lease_slot_t *s, email_lease_hook_t *hook_out) {
   for (email_lease_ticket_t *t = s->head; t; t = t->next) {
      if (!t->blocking && !s_hook)
         continue; /* nobody to tell yet */
      fifo_remove_locked(s, t);
      t->state = EMAIL_LEASE_TICKET_GRANTED;
      s->held = true;
      if (t->blocking) {
         blocking_waiter_t *w = (blocking_waiter_t *)t;
         s->holder_is_thread = true;
         s->holder_thread = w->thread;
         s->holder_ticket = NULL;
         pthread_cond_signal(&w->cond);
         return NULL;
      }
      s->holder_is_thread = false;
      s->holder_ticket = t;
      s_hook_running++;
      *hook_out = s_hook;
      return t;
   }
   s->held = false;
   return NULL;
}

static void run_hook(email_lease_hook_t hook, email_lease_ticket_t *t) {
   if (!t)
      return;
   hook(t);
   pthread_mutex_lock(&s_mutex);
   if (--s_hook_running == 0)
      pthread_cond_broadcast(&s_hook_idle);
   pthread_mutex_unlock(&s_mutex);
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

static bool ts_before(const struct timespec *a, const struct timespec *b) {
   return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

int email_lease_acquire(int64_t account_id, const atomic_bool *cancel, int timeout_s) {
   pthread_mutex_lock(&s_mutex);
   lease_slot_t *s = slot_get_locked(account_id);
   if (!s) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("email lease: no free slot for account %lld", (long long)account_id);
      return EMAIL_LEASE_FULL;
   }
   if (s->held && s->holder_is_thread && pthread_equal(s->holder_thread, pthread_self())) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("email lease: account %lld taken again by the thread holding it",
                 (long long)account_id);
      assert(!"email lease re-taken by its holder");
      return EMAIL_LEASE_SELF;
   }
   if (!s->held) {
      s->held = true;
      s->holder_is_thread = true;
      s->holder_thread = pthread_self();
      s->holder_ticket = NULL;
      pthread_mutex_unlock(&s_mutex);
      return EMAIL_LEASE_OK;
   }
   if (timeout_s <= 0) {
      pthread_mutex_unlock(&s_mutex);
      return EMAIL_LEASE_TIMEOUT;
   }

   blocking_waiter_t w;
   memset(&w, 0, sizeof(w));
   w.t.account_id = account_id;
   w.t.blocking = true;
   w.t.state = EMAIL_LEASE_TICKET_WAITING;
   w.thread = pthread_self();
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&w.cond, &attr);
   pthread_condattr_destroy(&attr);
   fifo_push_locked(s, &w.t);

   struct timespec deadline;
   deadline_after(&deadline, (long)timeout_s * 1000L);
   int rc = EMAIL_LEASE_OK;
   for (;;) {
      if (w.t.state == EMAIL_LEASE_TICKET_GRANTED)
         break; /* a grant wins over a cancel or deadline noticed at the same time */
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (cancel && atomic_load(cancel))
         rc = EMAIL_LEASE_CANCELLED;
      else if (!ts_before(&now, &deadline))
         rc = EMAIL_LEASE_TIMEOUT;
      if (rc != EMAIL_LEASE_OK) {
         /* Still the account's slot: the slot can't be reused while we wait in it. */
         lease_slot_t *mine = slot_find_locked(account_id);
         if (mine) {
            fifo_remove_locked(mine, &w.t);
            slot_free_if_idle_locked(mine);
         }
         break;
      }
      struct timespec wake;
      deadline_after(&wake, EMAIL_LEASE_WAIT_SLICE_MS);
      if (ts_before(&deadline, &wake))
         wake = deadline;
      pthread_cond_timedwait(&w.cond, &s_mutex, &wake);
   }
   pthread_mutex_unlock(&s_mutex);
   pthread_cond_destroy(&w.cond);
   return rc;
}

int email_lease_acquire_async(int64_t account_id, email_lease_ticket_t *ticket, bool *got) {
   if (!ticket || !got)
      return EMAIL_LEASE_FAILURE;
   *got = false;
   pthread_mutex_lock(&s_mutex);
   if (ticket->state != EMAIL_LEASE_TICKET_IDLE) {
      pthread_mutex_unlock(&s_mutex);
      return EMAIL_LEASE_FAILURE;
   }
   lease_slot_t *s = slot_get_locked(account_id);
   if (!s) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("email lease: no free slot for account %lld", (long long)account_id);
      return EMAIL_LEASE_FULL;
   }
   ticket->account_id = account_id;
   ticket->blocking = false;
   if (!s->held) {
      s->held = true;
      s->holder_is_thread = false;
      s->holder_ticket = ticket;
      ticket->state = EMAIL_LEASE_TICKET_GRANTED;
      *got = true;
   } else {
      ticket->state = EMAIL_LEASE_TICKET_WAITING;
      fifo_push_locked(s, ticket);
   }
   pthread_mutex_unlock(&s_mutex);
   return EMAIL_LEASE_OK;
}

bool email_lease_cancel_ticket(email_lease_ticket_t *ticket) {
   if (!ticket)
      return false;
   bool removed = false;
   pthread_mutex_lock(&s_mutex);
   if (ticket->state == EMAIL_LEASE_TICKET_WAITING) {
      lease_slot_t *s = slot_find_locked(ticket->account_id);
      if (s && fifo_remove_locked(s, ticket)) {
         ticket->state = EMAIL_LEASE_TICKET_IDLE;
         removed = true;
         slot_free_if_idle_locked(s);
      }
   }
   pthread_mutex_unlock(&s_mutex);
   return removed;
}

int email_lease_release(int64_t account_id) {
   pthread_mutex_lock(&s_mutex);
   lease_slot_t *s = slot_find_locked(account_id);
   if (!s || !s->held) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("email lease: release of account %lld, which isn't held", (long long)account_id);
      return EMAIL_LEASE_FAILURE;
   }
   /* A ticket's lease may be released from any thread; a thread's only by it. */
   if (s->holder_is_thread && !pthread_equal(s->holder_thread, pthread_self())) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("email lease: account %lld released by a thread that doesn't hold it",
                 (long long)account_id);
      return EMAIL_LEASE_FAILURE;
   }
   /* The ticket that held it may be used again (state belongs to the lease). */
   if (s->holder_ticket) {
      s->holder_ticket->state = EMAIL_LEASE_TICKET_IDLE;
      s->holder_ticket = NULL;
   }
   email_lease_hook_t hook = NULL;
   email_lease_ticket_t *granted = grant_next_locked(s, &hook);
   slot_free_if_idle_locked(s);
   pthread_mutex_unlock(&s_mutex);
   run_hook(hook, granted);
   return EMAIL_LEASE_OK;
}

bool email_lease_is_held(int64_t account_id) {
   pthread_mutex_lock(&s_mutex);
   const lease_slot_t *s = slot_find_locked(account_id);
   const bool held = s && s->held;
   pthread_mutex_unlock(&s_mutex);
   return held;
}

void email_lease_set_hook(email_lease_hook_t hook) {
   email_lease_ticket_t *granted[EMAIL_LEASE_SLOTS];
   int n = 0;
   pthread_mutex_lock(&s_mutex);
   s_hook = hook;
   /* Tickets queued while no hook was set may now be handed a free lease. */
   for (int i = 0; hook && i < EMAIL_LEASE_SLOTS; i++) {
      lease_slot_t *s = &s_slots[i];
      if (!s->in_use || s->held || !s->head)
         continue;
      email_lease_hook_t h = NULL;
      email_lease_ticket_t *t = grant_next_locked(s, &h);
      if (t)
         granted[n++] = t;
   }
   pthread_mutex_unlock(&s_mutex);
   for (int i = 0; i < n; i++)
      run_hook(hook, granted[i]);
}

void email_lease_clear_hook(void) {
   pthread_mutex_lock(&s_mutex);
   s_hook = NULL;
   while (s_hook_running > 0)
      pthread_cond_wait(&s_hook_idle, &s_mutex);
   pthread_mutex_unlock(&s_mutex);
}
