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
 * model-change re-index / after an embed failure, and the one-shot fact
 * category pass that runs once a user's facts are embedded.
 */

#include "memory/memory_embed_backfill.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
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
/* Category centroids for the backfill worker, built once per embedding model.
 * Only the (single) backfill worker reads or writes these; reindex_begin marks
 * them stale via s_centroids_stale, since a model change changes the space. */
static float *s_centroids;
static int s_centroid_dims;
static atomic_bool s_centroids_stale;

#define BACKFILL_QUEUE_MAX 32
#define BACKFILL_BATCH 50                  /* facts listed per page */
#define BACKFILL_MAX_CONSEC_FAIL 3         /* consecutive failures that trigger an engine probe */
#define BACKFILL_RETRY_DELAY_SEC 300       /* first re-sweep after the engine failed */
#define BACKFILL_RETRY_DELAY_MAX_SEC 3600  /* backoff ceiling while it stays down */
#define BACKFILL_CACHE_REFRESH_SEC 30      /* newly embedded facts reach search this often */
#define BACKFILL_EMBED_THROTTLE_USEC 50000 /* between embeddings, to share the CPU */
#define BACKFILL_CATEGORY_THROTTLE_USEC 50000 /* between category pages */
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

/* =============================================================================
 * Category Centroid Backfill (v34)
 *
 * One-shot per-user pass that classifies existing facts using their already-cached
 * embeddings.  Runs after the embedding backfill completes (centroids are useless
 * without populated fact embeddings).  Gated by users.categories_backfilled_at:
 * non-zero = already done, skip.
 * ============================================================================= */

/* Category backfill threshold is read from g_config.memory.category_threshold
 * (default 0.25, calibrated for MiniLM-L6-v2-int8). */
#define CATEGORY_BACKFILL_FETCH 200

/* Seed phrases per category.  Used to compute one centroid per category at
 * backfill time — embed each seed, average per category, store as the
 * comparison vector for cosine classification.  "general" is intentionally
 * absent: it's the fallback when no other centroid scores above the threshold. */
static const struct {
   const char *category;
   const char *seeds[6]; /* NULL-terminated, ~5 phrases each */
} CATEGORY_SEEDS[] = {
   { "personal",
     { "I was born in 1985", "my full name is Alex Smith", "I grew up in Texas",
       "I am 38 years old", "my middle name is Lee", NULL } },
   { "professional",
     { "I work as a software engineer", "I graduated from MIT", "my company is Acme Corp",
       "I am a senior developer", "I have a Python certification", NULL } },
   { "relationships",
     { "my wife's name is Jane", "my son is named Sam", "my best friend is Bob",
       "my mother lives in Ohio", "I have two siblings", NULL } },
   { "health",
     { "I am allergic to peanuts", "I take metformin daily", "I follow a vegetarian diet",
       "I have asthma", "I work out three times a week", NULL } },
   { "interests",
     { "I love science fiction novels", "I play guitar in a band", "I enjoy hiking on weekends",
       "I follow the Lakers", "I am learning Spanish", NULL } },
   { "practical",
     { "my home address is 123 Main St", "my car is a Honda Civic",
       "my router is in the office closet", "my office is on the third floor",
       "I have an Amazon Prime account", NULL } },
   { "preferences",
     { "I prefer dark mode", "I like concise responses", "I prefer Celsius over Fahrenheit",
       "I dislike interruptions", "I like formal language", NULL } },
};
#define CATEGORY_SEEDS_COUNT ((int)(sizeof(CATEGORY_SEEDS) / sizeof(CATEGORY_SEEDS[0])))

/* Build per-category centroid embeddings by averaging the seed phrase embeddings.
 * Returns malloc'd buffer of CATEGORY_SEEDS_COUNT * dims floats; caller frees.
 * Returns NULL on failure. */
static float *build_category_centroids(int *out_dims) {
   if (!embedding_engine_available())
      return NULL;

   int dims = 0;
   /* Probe dimensions with the first seed of the first category. */
   float probe[MAX_EMBEDDING_DIMS];
   if (embedding_engine_embed(CATEGORY_SEEDS[0].seeds[0], probe, MAX_EMBEDDING_DIMS, &dims) != 0 ||
       dims <= 0) {
      OLOG_ERROR("memory_embeddings: centroid build failed (probe embed)");
      return NULL;
   }

   float *centroids = calloc((size_t)CATEGORY_SEEDS_COUNT * dims, sizeof(float));
   if (!centroids) {
      OLOG_ERROR("memory_embeddings: centroid alloc failed");
      return NULL;
   }

   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      int seed_count = 0;
      float *cent = centroids + (size_t)c * dims;
      for (int s = 0; CATEGORY_SEEDS[c].seeds[s]; s++) {
         float emb[MAX_EMBEDDING_DIMS];
         int sd = 0;
         if (embedding_engine_embed(CATEGORY_SEEDS[c].seeds[s], emb, MAX_EMBEDDING_DIMS, &sd) ==
                 0 &&
             sd == dims) {
            for (int d = 0; d < dims; d++)
               cent[d] += emb[d];
            seed_count++;
         }
      }
      if (seed_count == 0) {
         OLOG_WARNING("memory_embeddings: no seeds embedded for category '%s'",
                      CATEGORY_SEEDS[c].category);
         /* Leave as zero vector — cosine will produce 0, no false matches. */
         continue;
      }
      /* Average then re-normalize so cosine is well-defined. */
      float norm_sq = 0.0f;
      for (int d = 0; d < dims; d++) {
         cent[d] /= (float)seed_count;
         norm_sq += cent[d] * cent[d];
      }
      float n = sqrtf(norm_sq);
      if (n > 1e-6f) {
         for (int d = 0; d < dims; d++)
            cent[d] /= n;
      }
      /* Health check: log the final centroid norm.  A healthy unit vector prints
       * ~1.0; zeros or near-zero mean all embeds failed for this category. */
      float check_sq = 0.0f;
      for (int d = 0; d < dims; d++)
         check_sq += cent[d] * cent[d];
      OLOG_INFO("memory_embeddings: centroid[%s] seeds=%d norm=%.4f", CATEGORY_SEEDS[c].category,
                seed_count, sqrtf(check_sq));
   }

   *out_dims = dims;
   return centroids;
}

/* Classify a single fact embedding against the centroids.  Returns the winning
 * category name (one of MEMORY_FACT_CATEGORIES) or "general" when no centroid
 * scores above the threshold. */
static const char *classify_fact_embedding(const float *fact_emb,
                                           const float *centroids,
                                           int dims,
                                           float threshold) {
   if (!fact_emb || !centroids || dims <= 0)
      return "general";

   float best_score = -1.0f;
   int best_idx = -1;

   /* Re-normalize the fact embedding once for cosine comparison.  We don't
    * trust the cached norm here because we want pure cosine = dot product
    * of unit vectors, and centroids are unit vectors. */
   float fact_norm_sq = 0.0f;
   for (int d = 0; d < dims; d++)
      fact_norm_sq += fact_emb[d] * fact_emb[d];
   float fact_norm = sqrtf(fact_norm_sq);
   if (fact_norm < 1e-6f)
      return "general";

   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      const float *cent = centroids + (size_t)c * dims;
      float dot = 0.0f;
      for (int d = 0; d < dims; d++)
         dot += fact_emb[d] * cent[d];
      float cosine = dot / fact_norm;
      if (cosine > best_score) {
         best_score = cosine;
         best_idx = c;
      }
   }

   if (best_idx < 0 || best_score < threshold)
      return "general";
   return CATEGORY_SEEDS[best_idx].category;
}

/* One-time diagnostic on a pass's first fact: raw scores against each centroid,
 * so the threshold can be tuned from real data. */
static void log_category_sample(int64_t fact_id,
                                const float *emb,
                                const float *centroids,
                                int dims,
                                float threshold) {
   float best = -1.0f;
   int best_idx = -1;
   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      const float *cent = centroids + (size_t)c * dims;
      float dot = 0.0f;
      for (int d = 0; d < dims; d++)
         dot += emb[d] * cent[d];
      OLOG_INFO("memory_embeddings: sample fact_id=%lld vs %s = %.4f", (long long)fact_id,
                CATEGORY_SEEDS[c].category, dot);
      if (dot > best) {
         best = dot;
         best_idx = c;
      }
   }
   OLOG_INFO("memory_embeddings: sample best=%s @ %.4f (threshold=%.2f)",
             best_idx >= 0 ? CATEGORY_SEEDS[best_idx].category : "(none)", best, threshold);
}

/* Iterate user's facts with embeddings + general category, classify, batch-UPDATE
 * the assigned category.  Sets *classified_out to the count of facts classified
 * (assigned non-general).  Returns 0 on success, 1 on hard error.
 * Caller already verified embedding engine + flag state. */
static int categorize_user_facts(int user_id,
                                 const float *centroids,
                                 int dims,
                                 int *classified_out) {
   if (classified_out)
      *classified_out = 0;

   if (!centroids || dims <= 0)
      return 1;

   int classified = 0;
   int touched = 0;
   int loops = 0;
   int64_t cursor_id = 0;      /* id-based pagination (prevents infinite loop) */
   bool sample_logged = false; /* one-time cosine-score sample for tuning */
   int per_cat_count[CATEGORY_SEEDS_COUNT] = { 0 }; /* assignment distribution */

   /* Per-page scratch, reused across pages. */
   int64_t ids[CATEGORY_BACKFILL_FETCH];
   const char *assigned[CATEGORY_BACKFILL_FETCH];
   float *embs = malloc((size_t)CATEGORY_BACKFILL_FETCH * (size_t)dims * sizeof(float));
   if (!embs)
      return 1;

   const float threshold = g_config.memory.category_threshold;
   while (!atomic_load(&s_backfill_shutdown)) {
      /* Page by id > cursor: if every fact classifies as 'general', nothing is
       * updated and a "category = 'general'" query alone would return the same
       * rows forever. */
      int batch_count = 0;
      if (memory_db_fact_list_general_embedded(user_id, cursor_id, dims, CATEGORY_BACKFILL_FETCH,
                                               ids, embs, &batch_count,
                                               &cursor_id) != MEMORY_DB_SUCCESS) {
         free(embs);
         return 1;
      }
      if (batch_count == 0)
         break;

      /* Classify with no lock held: pure CPU on the copied embeddings. */
      for (int i = 0; i < batch_count && !atomic_load(&s_backfill_shutdown); i++) {
         const float *emb = embs + (size_t)i * (size_t)dims;
         if (!sample_logged) {
            log_category_sample(ids[i], emb, centroids, dims, threshold);
            sample_logged = true;
         }
         const char *cat = classify_fact_embedding(emb, centroids, dims, threshold);
         /* 'general' is already the column's value: nothing to write. */
         assigned[i] = strcmp(cat, "general") == 0 ? NULL : cat;
         touched++;
      }

      int written = 0;
      if (memory_db_fact_set_categories(user_id, ids, assigned, batch_count, &written) !=
          MEMORY_DB_SUCCESS) {
         free(embs);
         return 1;
      }
      classified += written;
      for (int i = 0; i < batch_count; i++) {
         for (int c = 0; assigned[i] && c < CATEGORY_SEEDS_COUNT; c++) {
            if (strcmp(assigned[i], CATEGORY_SEEDS[c].category) == 0) {
               per_cat_count[c]++;
               break;
            }
         }
      }

      /* If we got fewer than the fetch limit, no more unclassified facts remain. */
      if (batch_count < CATEGORY_BACKFILL_FETCH)
         break;

      usleep(BACKFILL_CATEGORY_THROTTLE_USEC);

      if (++loops > 1000) {
         OLOG_WARNING("memory_embeddings: category backfill loop guard tripped");
         break;
      }
   }

   free(embs);

   OLOG_INFO("memory_embeddings: category backfill user=%d touched=%d assigned=%d (general=%d)",
             user_id, touched, classified, touched - classified);
   for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
      if (per_cat_count[c] > 0) {
         OLOG_INFO("memory_embeddings:   %s: %d", CATEGORY_SEEDS[c].category, per_cat_count[c]);
      }
   }
   if (classified_out)
      *classified_out = classified;
   return 0;
}


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
   free(s_centroids); /* worker joined: no other reader */
   s_centroids = NULL;
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

static const float *backfill_centroids(int *dims_out) {
   if (s_centroids &&
       (atomic_exchange(&s_centroids_stale, false) || s_centroid_dims != embedding_engine_dims())) {
      free(s_centroids);
      s_centroids = NULL;
   }
   if (!s_centroids) {
      s_centroids = build_category_centroids(&s_centroid_dims);
   }
   *dims_out = s_centroid_dims;
   return s_centroids;
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
 * dimension), then run the one-shot category pass for that user.
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
   bool reached_end = false;
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
      if (count <= 0) {
         reached_end = true;
         break;
      }

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

   /* v34: one-shot category classification for this user.  Only after a pass that
    * reached the end: centroids score facts by their embeddings, and the flag it
    * sets is permanent, so running it after an engine outage would leave the
    * un-embedded remainder unclassified for good.  Facts that failed individually
    * (the engine answered the probe) would fail again next time too, so they don't
    * hold the pass back.  Idempotent via users.categories_backfilled_at. */
   if (reached_end && !atomic_load(&s_backfill_shutdown) && embedding_engine_available()) {
      int64_t flag = 0;
      bool first_pass = (auth_db_user_get_categories_backfilled_at(user_id, &flag) ==
                             AUTH_DB_SUCCESS &&
                         flag == 0);
      /* Also classify after any pass that embedded facts: those were created
       * without an embedding (imports, or a failed inline embed), so they could
       * not be classified when created and are still 'general'. */
      if (first_pass || total_embedded > 0) {
         int cdims = 0;
         const float *centroids = backfill_centroids(&cdims);
         if (centroids) {
            int classified = 0;
            if (categorize_user_facts(user_id, centroids, cdims, &classified) == 0 && first_pass) {
               auth_db_user_set_categories_backfilled_at(user_id, (int64_t)time(NULL));
            }
         } else {
            OLOG_WARNING("memory_embeddings: category centroid build failed, skipping backfill");
         }
      }
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
   atomic_store(&s_centroids_stale, true);
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
