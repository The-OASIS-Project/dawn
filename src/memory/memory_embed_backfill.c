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
 * Embedding backfill: embeds facts that were stored without an embedding
 * (bulk imports, or an inline embed that failed), one per-user request queue
 * drained by a single worker, an all-users sweep at startup / after a
 * model-change re-index / after an embed failure.
 */

#include "memory/memory_embed_backfill.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_embeddings.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_types.h"

/* Backfill worker state.  Requests are queued per user (deduplicated) and drained
 * by at most one worker thread, so a request that arrives while a pass is running
 * is served after it instead of being dropped.  s_backfill_mutex guards the queue,
 * s_backfill_running and s_backfill_joinable; it is a leaf lock (never held across
 * an embed or a DB call). */
#define BACKFILL_QUEUE_MAX 32
#define BACKFILL_BATCH 50                  /* facts listed per page */
#define BACKFILL_MAX_CONSEC_FAIL 3         /* consecutive failures that trigger an engine probe */
#define BACKFILL_RETRY_DELAY_SEC 300       /* first re-sweep after the engine failed */
#define BACKFILL_RETRY_DELAY_MAX_SEC 3600  /* backoff ceiling while it stays down */
#define BACKFILL_CACHE_REFRESH_SEC 30      /* newly embedded facts reach search this often */
#define BACKFILL_EMBED_THROTTLE_USEC 50000 /* between embeddings, to share the CPU */
static pthread_mutex_t s_backfill_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_backfill_thread;
static bool s_backfill_running;  /* a worker is alive (guarded by s_backfill_mutex) */
static bool s_backfill_joinable; /* s_backfill_thread was created and not yet joined */
static atomic_bool s_backfill_shutdown;
static int s_backfill_queue[BACKFILL_QUEUE_MAX];
static int s_backfill_queue_len;

/* All guarded by s_backfill_mutex.
 *
 * s_reindex_active: memory_embed_recompute is re-indexing after a model change.
 * Backfill would list the very rows it is rewriting (their dims no longer match)
 * and embed them twice, so requests made meanwhile set s_sweep_deferred and are
 * served by one all-users sweep when the re-index completes.
 *
 * s_resweep_pending / s_resweep_cursor: a request or sweep that didn't fit in the
 * queue.  When the worker drains the queue it continues the sweep from the
 * cursor (users with id > cursor), so nothing waits for the next boot. */
static bool s_reindex_active;
static bool s_sweep_deferred;

/* When set (epoch seconds), memory_embeddings_tick() queues an all-users sweep at
 * or after this time.  Armed when the embedding engine fails, so a fact created
 * while the provider was briefly down is embedded minutes later, not at the next
 * boot.  The delay doubles each time it is armed (a provider that stays down is
 * not hammered) and resets once a sweep embeds something. */
static atomic_llong s_retry_sweep_at;
static atomic_int s_retry_delay_sec = BACKFILL_RETRY_DELAY_SEC;

void memory_embed_backfill_schedule_retry(void) {
   long long expected = 0;
   const int delay = atomic_load(&s_retry_delay_sec);
   if (atomic_compare_exchange_strong(&s_retry_sweep_at, &expected,
                                      (long long)time(NULL) + delay)) {
      atomic_store(&s_retry_delay_sec, delay * 2 > BACKFILL_RETRY_DELAY_MAX_SEC
                                           ? BACKFILL_RETRY_DELAY_MAX_SEC
                                           : delay * 2);
   }
}
static bool s_resweep_pending;
static int s_resweep_cursor;

void memory_embed_backfill_shutdown(void) {
   /* Shutdown is set first so a concurrent request can't spawn a new worker, and
    * the queue is cleared so the running one exits after its current fact. */
   atomic_store(&s_backfill_shutdown, true);
   pthread_mutex_lock(&s_backfill_mutex);
   s_backfill_queue_len = 0;
   bool join = s_backfill_joinable;
   s_backfill_joinable = false;
   pthread_mutex_unlock(&s_backfill_mutex);
   if (join) {
      pthread_join(s_backfill_thread, NULL);
   }
}

static void *backfill_thread_fn(void *arg);

/* Record that users with id > @p cursor may still need queuing.  Caller holds
 * s_backfill_mutex.  Pending requests merge to the lowest cursor. */
static void mark_resweep_locked(int cursor) {
   if (!s_resweep_pending || cursor < s_resweep_cursor) {
      s_resweep_cursor = cursor;
   }
   s_resweep_pending = true;
}

/* True if the engine embeds a known-good string with the expected dimensions.
 * Separates "the engine is down" from "these particular facts can't be embedded". */
static bool backfill_engine_probe_ok(void) {
   float vec[MAX_EMBEDDING_DIMS];
   int dims = 0;
   return embedding_engine_embed("embedding health check", vec, MAX_EMBEDDING_DIMS, &dims) == 0 &&
          dims > 0 && dims == embedding_engine_dims();
}

/**
 * Embed every fact of @p user_id that has no embedding (or one of the wrong
 * dimension).
 *
 * Pages by fact id, so a fact that can't be embedded is stepped over rather than
 * re-listed; the pass always terminates.  After BACKFILL_MAX_CONSEC_FAIL
 * consecutive failures the engine is probed with a known-good string: if the
 * probe fails too the engine is down and the pass ends (the rest waits for the
 * next request); if it succeeds the failures belong to those facts, which are
 * skipped, and the pass continues.  So a handful of bad facts can never block
 * the facts after them.  The fact cache is marked stale once per page.
 */
static void backfill_user(int user_id) {
   pthread_mutex_lock(&s_backfill_mutex);
   if (s_reindex_active) {
      /* A re-index started after this user was queued; its completion sweep
       * serves them. */
      s_sweep_deferred = true;
      pthread_mutex_unlock(&s_backfill_mutex);
      return;
   }
   pthread_mutex_unlock(&s_backfill_mutex);

   const int dims = embedding_engine_dims();
   int64_t cursor = 0;
   int total_embedded = 0;
   int total_failed = 0;
   int consec_fail = 0;
   bool engine_down = false;
   bool cache_stale = false; /* facts embedded since the cache last refreshed */
   time_t cache_refreshed = time(NULL);

   while (!atomic_load(&s_backfill_shutdown)) {
      int64_t ids[BACKFILL_BATCH];
      char texts[BACKFILL_BATCH][512];

      int count = 0;
      if (memory_db_fact_list_without_embedding(user_id, cursor, dims, ids, texts, BACKFILL_BATCH,
                                                &count) != MEMORY_DB_SUCCESS) {
         break;
      }
      if (count <= 0)
         break;

      int page_embedded = 0;
      for (int i = 0; i < count && !engine_down; i++) {
         if (atomic_load(&s_backfill_shutdown))
            break;
         cursor = ids[i];

         if (memory_embeddings_embed_and_store_ex(user_id, ids[i], texts[i], true) == 0) {
            page_embedded++;
            consec_fail = 0;
         } else {
            total_failed++;
            if (++consec_fail >= BACKFILL_MAX_CONSEC_FAIL) {
               if (backfill_engine_probe_ok()) {
                  consec_fail = 0; /* the facts are bad, not the engine */
               } else {
                  engine_down = true;
               }
            }
         }

         usleep(BACKFILL_EMBED_THROTTLE_USEC);
      }
      if (page_embedded > 0) {
         total_embedded += page_embedded;
         cache_stale = true;
         atomic_store(&s_retry_delay_sec, BACKFILL_RETRY_DELAY_SEC);
      }
      /* Newly embedded facts reach semantic search when the cache reloads; do
       * that periodically, not per page, so a long pass doesn't keep a warm
       * cache cold. */
      if (cache_stale && time(NULL) - cache_refreshed >= BACKFILL_CACHE_REFRESH_SEC) {
         memory_embeddings_invalidate_cache_for_user(user_id);
         cache_stale = false;
         cache_refreshed = time(NULL);
      }
      if (engine_down) {
         OLOG_WARNING("memory_embeddings: backfill user=%d stopped: embedding engine not "
                      "responding",
                      user_id);
         break;
      }
   }

   if (cache_stale) {
      memory_embeddings_invalidate_cache_for_user(user_id);
   }
   if (engine_down) {
      memory_embed_backfill_schedule_retry(); /* the rest wait for the engine */
   }
   if (total_embedded > 0 || total_failed > 0) {
      OLOG_INFO("memory_embeddings: backfill user=%d embedded %d facts, %d failed", user_id,
                total_embedded, total_failed);
   }
}

/* Make sure a backfill worker is running.  Caller holds s_backfill_mutex. */
static void ensure_worker_locked(void) {
   if (s_backfill_running) {
      return;
   }
   /* Reap the previous worker before reusing the handle.  It has already cleared
    * s_backfill_running under this lock and takes no lock after that, so the join
    * cannot block on us. */
   if (s_backfill_joinable) {
      pthread_join(s_backfill_thread, NULL);
      s_backfill_joinable = false;
   }
   if (pthread_create(&s_backfill_thread, NULL, backfill_thread_fn, NULL) == 0) {
      s_backfill_running = true;
      s_backfill_joinable = true;
   } else {
      /* Leave pending work intact: the next request retries the spawn and serves
       * everyone already waiting. */
      OLOG_ERROR("memory_embeddings: failed to create backfill thread");
   }
}

/* Queue @p user_id and make sure a worker is running.  Caller holds
 * s_backfill_mutex.  Returns false only when the queue is full; the caller
 * records a resweep so the user is picked up once the queue drains. */
static bool backfill_enqueue_locked(int user_id) {
   bool queued = false;
   for (int i = 0; i < s_backfill_queue_len; i++) {
      if (s_backfill_queue[i] == user_id) {
         queued = true;
         break;
      }
   }
   if (!queued) {
      if (s_backfill_queue_len >= BACKFILL_QUEUE_MAX) {
         return false;
      }
      s_backfill_queue[s_backfill_queue_len++] = user_id;
   }

   ensure_worker_locked();
   return true;
}

/* Queue every user with id > @p after_user_id that needs a backfill pass, up to
 * the queue's capacity; the remainder is recorded as a resweep that the worker
 * continues when the queue drains.  Runs one DB query on the caller's thread. */
static void backfill_sweep_from(int after_user_id) {
   pthread_mutex_lock(&s_backfill_mutex);
   if (s_reindex_active) {
      s_sweep_deferred = true;
      pthread_mutex_unlock(&s_backfill_mutex);
      return;
   }
   pthread_mutex_unlock(&s_backfill_mutex);

   int users[BACKFILL_QUEUE_MAX];
   int n = 0;
   if (memory_db_fact_users_needing_backfill(embedding_engine_dims(), after_user_id, users,
                                             BACKFILL_QUEUE_MAX, &n) != MEMORY_DB_SUCCESS) {
      /* Not re-armed: a persistent DB error would spin the worker.  The next
       * request or boot sweep retries. */
      OLOG_WARNING("memory_embeddings: backfill sweep query failed (after user %d)", after_user_id);
      return;
   }
   if (n > 0) {
      OLOG_INFO("memory_embeddings: backfill sweep queuing %d user(s) (after user %d)", n,
                after_user_id);
   }

   pthread_mutex_lock(&s_backfill_mutex);
   if (!atomic_load(&s_backfill_shutdown)) {
      int i = 0;
      for (; i < n; i++) {
         if (!backfill_enqueue_locked(users[i])) {
            mark_resweep_locked(users[i] - 1);
            break;
         }
      }
      /* A full page may have more users behind it. */
      if (i == n && n == BACKFILL_QUEUE_MAX) {
         mark_resweep_locked(users[n - 1]);
      }
   }
   pthread_mutex_unlock(&s_backfill_mutex);
}

static void *backfill_thread_fn(void *arg) {
   (void)arg;
   for (;;) {
      pthread_mutex_lock(&s_backfill_mutex);
      if (s_backfill_queue_len == 0 || atomic_load(&s_backfill_shutdown)) {
         if (s_resweep_pending && !s_reindex_active && !atomic_load(&s_backfill_shutdown)) {
            /* Continue an overflowed sweep.  This thread stays the running worker,
             * so the enqueues below don't spawn another. */
            int cursor = s_resweep_cursor;
            s_resweep_pending = false;
            pthread_mutex_unlock(&s_backfill_mutex);
            backfill_sweep_from(cursor);
            continue;
         }
         /* Clear running under the same lock a requester checks it under, so a
          * request either lands in the queue before this check or sees running ==
          * false and starts a new worker. */
         s_backfill_running = false;
         pthread_mutex_unlock(&s_backfill_mutex);
         return NULL;
      }
      int user_id = s_backfill_queue[0];
      s_backfill_queue_len--;
      memmove(&s_backfill_queue[0], &s_backfill_queue[1],
              (size_t)s_backfill_queue_len * sizeof(s_backfill_queue[0]));
      pthread_mutex_unlock(&s_backfill_mutex);

      backfill_user(user_id);
   }
}

void memory_embeddings_request_backfill(int user_id) {
   if (user_id <= 0 || !memory_embeddings_available())
      return;

   pthread_mutex_lock(&s_backfill_mutex);
   if (atomic_load(&s_backfill_shutdown)) {
      /* nothing: shutting down */
   } else if (s_reindex_active) {
      s_sweep_deferred = true;
   } else if (!backfill_enqueue_locked(user_id)) {
      /* Queue full: a full resweep from the start picks this user up once the
       * worker drains the queue. */
      mark_resweep_locked(0);
   }
   pthread_mutex_unlock(&s_backfill_mutex);
}

void memory_embeddings_request_backfill_all(void) {
   if (!memory_embeddings_available())
      return;
   backfill_sweep_from(0);
}

/* Queue an all-users sweep on the backfill worker (the DB query runs there, not
 * on the caller's thread). */
static void request_sweep_async(void) {
   pthread_mutex_lock(&s_backfill_mutex);
   if (!atomic_load(&s_backfill_shutdown)) {
      if (s_reindex_active) {
         s_sweep_deferred = true;
      } else {
         mark_resweep_locked(0);
         ensure_worker_locked();
      }
   }
   pthread_mutex_unlock(&s_backfill_mutex);
}

void memory_embeddings_tick(time_t now) {
   long long at = atomic_load(&s_retry_sweep_at);
   if (at == 0 || (long long)now < at) {
      return;
   }
   if (atomic_compare_exchange_strong(&s_retry_sweep_at, &at, 0) && memory_embeddings_available()) {
      OLOG_INFO("memory_embeddings: retrying backfill after an earlier embed failure");
      request_sweep_async();
   }
}

void memory_embeddings_reindex_begin(void) {
   pthread_mutex_lock(&s_backfill_mutex);
   s_reindex_active = true;
   pthread_mutex_unlock(&s_backfill_mutex);
}

void memory_embeddings_reindex_end(bool completed) {
   pthread_mutex_lock(&s_backfill_mutex);
   s_reindex_active = false;
   /* Anything parked during the re-index is covered by one sweep from the start.
    * An interrupted re-index (shutdown) drops the deferred sweep; the next boot's
    * sweep finds the same users.  A pending resweep is kept either way and is
    * continued by the next worker that drains the queue. */
   bool sweep = completed && (s_sweep_deferred || s_resweep_pending);
   s_sweep_deferred = false;
   if (sweep) {
      s_resweep_pending = false;
   }
   pthread_mutex_unlock(&s_backfill_mutex);

   if (sweep) {
      backfill_sweep_from(0);
   }
}
