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
 * Searching every account of a user at once (email_fanout.h).
 */

#define _GNU_SOURCE
#include "tools/email_fanout.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tools/email_transfer.h"

typedef struct {
   pthread_t thread;
   bool started;
   int index;
   int max;
   email_fanout_fn fn;
   void *ctx;
   const atomic_bool *cancel;
   email_fanout_slot_t *slot;
} fanout_job_t;

static int64_t now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void run_job(fanout_job_t *job) {
   const atomic_bool *prev = email_transfer_scope_cancel(job->cancel);
   const int64_t start = now_ms();
   job->fn(job->ctx, job->index, job->slot, job->max);
   job->slot->ms = now_ms() - start;
   email_transfer_scope_cancel(prev);
}

static void *job_thread(void *arg) {
   run_job(arg);
   return NULL;
}

int email_fanout_run(int n, int max, email_fanout_fn fn, void *ctx, email_fanout_slot_t *slots) {
   if (n <= 0)
      return 0;
   memset(slots, 0, sizeof(*slots) * (size_t)n);
   fanout_job_t *jobs = calloc((size_t)n, sizeof(*jobs));
   if (!jobs)
      return 1;
   for (int i = 0; i < n; i++) {
      slots[i].rows = calloc((size_t)max, sizeof(email_summary_t));
      if (!slots[i].rows) {
         email_fanout_free(slots, n);
         free(jobs);
         return 1;
      }
      slots[i].cap = max;
   }

   pthread_attr_t attr;
   const bool attr_ok = pthread_attr_init(&attr) == 0;
   if (attr_ok)
      pthread_attr_setstacksize(&attr, EMAIL_WORKER_STACK_BYTES);
   const atomic_bool *cancel = email_transfer_thread_cancel();
   for (int i = 0; i < n; i++) {
      jobs[i] = (fanout_job_t){ .index = i,
                                .max = max,
                                .fn = fn,
                                .ctx = ctx,
                                .cancel = cancel,
                                .slot = &slots[i] };
      jobs[i].started = pthread_create(&jobs[i].thread, attr_ok ? &attr : NULL, job_thread,
                                       &jobs[i]) == 0;
   }
   if (attr_ok)
      pthread_attr_destroy(&attr);

   /* An account whose thread couldn't start runs here, after the others began. */
   for (int i = 0; i < n; i++) {
      if (!jobs[i].started)
         run_job(&jobs[i]);
   }
   for (int i = 0; i < n; i++) {
      if (jobs[i].started)
         pthread_join(jobs[i].thread, NULL);
   }
   free(jobs);
   return 0;
}

/* One row of one account, for the merge's sort. */
typedef struct {
   time_t date;
   int slot;
   int row;
} merge_key_t;

/* Newest first; a tie keeps account, then row, order (qsort isn't stable). */
static int cmp_merge_key(const void *a, const void *b) {
   const merge_key_t *x = a, *y = b;
   if (x->date != y->date)
      return x->date < y->date ? 1 : -1;
   if (x->slot != y->slot)
      return x->slot < y->slot ? -1 : 1;
   return x->row < y->row ? -1 : (x->row > y->row);
}

bool email_fanout_merge(const email_fanout_slot_t *slots,
                        int n,
                        int max,
                        email_summary_t *out,
                        int *total_out,
                        email_fanout_fail_fn on_fail,
                        void *ctx) {
   bool whole = true;
   size_t rows = 0;
   for (int i = 0; i < n; i++) {
      const email_fanout_slot_t *s = &slots[i];
      if (s->rc == 0) {
         rows += (size_t)s->count;
      } else if (s->err == EMAIL_ERR_CANCELLED) {
         whole = false;
      } else if (on_fail) {
         on_fail(ctx, i, s);
      }
   }

   /* Sorted by date alone, not by each account's own order: an old message
    * re-filed into a folder (a new UID, its old date) mustn't hold back that
    * account's newer mail. */
   int total = 0;
   merge_key_t *keys = rows ? malloc(rows * sizeof(*keys)) : NULL;
   if (keys) {
      size_t k = 0;
      for (int i = 0; i < n; i++) {
         if (slots[i].rc != 0)
            continue;
         for (int r = 0; r < slots[i].count; r++)
            keys[k++] = (merge_key_t){ .date = slots[i].rows[r].date, .slot = i, .row = r };
      }
      qsort(keys, rows, sizeof(*keys), cmp_merge_key);
      for (size_t k2 = 0; k2 < rows && total < max; k2++)
         out[total++] = slots[keys[k2].slot].rows[keys[k2].row];
      free(keys);
   } else {
      /* Out of memory: each account's rows in turn, still within max. */
      for (int i = 0; i < n && total < max; i++) {
         for (int r = 0; slots[i].rc == 0 && r < slots[i].count && total < max; r++)
            out[total++] = slots[i].rows[r];
      }
   }
   *total_out = total;
   return whole;
}

void email_fanout_free(email_fanout_slot_t *slots, int n) {
   for (int i = 0; i < n; i++) {
      if (slots[i].rows) {
         explicit_bzero(slots[i].rows, sizeof(email_summary_t) * (size_t)slots[i].cap);
         free(slots[i].rows);
         slots[i].rows = NULL;
      }
   }
}
