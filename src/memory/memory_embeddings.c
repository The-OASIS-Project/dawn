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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory Embeddings Core
 *
 * Provider abstraction, math utilities, in-memory cache, hybrid search,
 * and background backfill for semantic memory search.
 */

#define _GNU_SOURCE /* qsort_r — GNU signature with thread-local arg */

#include "memory/memory_embeddings.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "utils/string_utils.h"

#define AUTH_DB_INTERNAL_ALLOWED /* needed for direct sqlite access in category backfill */

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "core/time_query_parser.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_types.h"

/* In-memory embedding cache for fast cosine search.
 * created_ats added in #3 — per-fact origin timestamps used by temporal-query
 * scoring.  Loaded together with embeddings so scoring stays single-pass.
 *
 * saturated_warned_user_id: when the cache loader fills to capacity it
 * triggers a one-shot OLOG_WARNING per (user_id) — this field stores the
 * user_id we have already warned for so we don't spam the log on every
 * subsequent reload.  Reset to 0 (no user, so harmless first-warn) on any
 * cache invalidation path. */
static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   float *embeddings; /* flat: count * dims */
   float *norms;
   int64_t *created_ats;  /* per-fact created_at, parallel to ids */
   int64_t *note_doc_ids; /* per-fact note_doc_id (v61): >0 = bridge gloss */
   int count;
   int capacity;
   int dims;
   bool valid;
   atomic_bool dirty; /* set by backfill after each store */
   int saturated_warned_user_id;
} s_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Entity embedding cache (separate from fact cache) */
#define ENTITY_CACHE_CAP 500

static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   char (*names)[MEMORY_ENTITY_NAME_MAX];
   char (*types)[MEMORY_ENTITY_TYPE_MAX];
   float *embeddings; /* flat: count * dims */
   float *norms;
   int count;
   int dims;
   bool valid;
   atomic_bool dirty;
} s_entity_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Forward declaration */
static void entity_cache_free(void);

/* Backfill thread state */
static pthread_t s_backfill_thread;
static atomic_bool s_backfill_running;
static atomic_bool s_backfill_shutdown;
static int s_backfill_user_id;

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
#define CATEGORY_BACKFILL_BATCH_SIZE 25
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

/* Read a user's flag.  Sets *ts_out to 0 if not yet backfilled, non-zero (timestamp)
 * if already done.  Returns 0 on success, 1 on error (caller should treat as
 * "skip and retry next time"). */
static int user_categories_backfilled_at(int user_id, int64_t *ts_out) {
   if (ts_out)
      *ts_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT categories_backfilled_at FROM users WHERE id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 1;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   if (sqlite3_step(stmt) == SQLITE_ROW && ts_out) {
      *ts_out = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return 0;
}

static void user_set_categories_backfilled(int user_id, int64_t ts) {
   AUTH_DB_LOCK_OR_RETURN_VOID();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE users SET categories_backfilled_at = ? WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, ts);
      sqlite3_bind_int(stmt, 2, user_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
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

   /* Hoist the per-batch scratch buffer (~1.6 MB for 200 rows × 2048-dim float
    * array) out of the loop to avoid repeated alloc+zero churn on the heap.
    * The buffer is reused across all batches; ids/dims overwritten each pass. */
   typedef struct {
      int64_t id;
      float emb[MAX_EMBEDDING_DIMS];
      int emb_dims;
   } row_t;
   row_t *rows = calloc(CATEGORY_BACKFILL_FETCH, sizeof(row_t));
   if (!rows)
      return 1;

   while (!atomic_load(&s_backfill_shutdown)) {
      /* Pull the next batch by id > cursor.  This is the fix for the infinite
       * loop that hit prod: if every fact classifies as 'general', the UPDATE
       * doesn't fire and WHERE category='general' keeps returning the same rows. */
      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return 1;
      }
      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "SELECT id, embedding, embedding_norm FROM memory_facts "
                                  "WHERE user_id = ? AND superseded_by IS NULL "
                                  "  AND embedding IS NOT NULL AND id > ? "
                                  "  AND category = 'general' "
                                  "ORDER BY id ASC LIMIT ?",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         AUTH_DB_UNLOCK();
         free(rows);
         return 1;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, cursor_id);
      sqlite3_bind_int(stmt, 3, CATEGORY_BACKFILL_FETCH);

      /* rows buffer is pre-allocated above the while loop — just reset count. */
      int batch_count = 0;
      int64_t batch_max_id = cursor_id;
      while (batch_count < CATEGORY_BACKFILL_FETCH && sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t row_id = sqlite3_column_int64(stmt, 0);
         if (row_id > batch_max_id)
            batch_max_id = row_id;
         const void *blob = sqlite3_column_blob(stmt, 1);
         int blob_bytes = sqlite3_column_bytes(stmt, 1);
         int row_dims = blob_bytes / (int)sizeof(float);
         if (blob && row_dims == dims && row_dims <= MAX_EMBEDDING_DIMS) {
            rows[batch_count].id = row_id;
            memcpy(rows[batch_count].emb, blob, (size_t)blob_bytes);
            rows[batch_count].emb_dims = row_dims;
            batch_count++;
         }
         /* Skip facts with mismatched dims (different embedding model than centroids). */
      }
      cursor_id = batch_max_id; /* advance past this batch regardless of UPDATE results */
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();

      if (batch_count == 0)
         break;

      /* Phase 1: classify this batch WITHOUT holding the DB lock.  Rows already
       * have embeddings copied into stack-local storage, so dot products are
       * pure CPU work with no DB access.  Holding the lock across these loops
       * would block unrelated DB traffic (session writes, fact updates, etc.)
       * for ~1ms × batch_size. */
      const char *assigned_cat[CATEGORY_BACKFILL_FETCH]; /* parallel to rows[] */
      const float threshold = g_config.memory.category_threshold;
      for (int i = 0; i < batch_count && !atomic_load(&s_backfill_shutdown); i++) {
         /* One-time diagnostic on the very first fact — logs raw cosines against
          * each centroid so the threshold can be tuned from real data.  Fires
          * once per user per backfill run. */
         if (!sample_logged) {
            float best = -1.0f;
            int best_idx = -1;
            for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
               const float *cent = centroids + (size_t)c * dims;
               float dot = 0.0f;
               for (int d = 0; d < dims; d++)
                  dot += rows[i].emb[d] * cent[d];
               OLOG_INFO("memory_embeddings: sample fact_id=%lld vs %s = %.4f",
                         (long long)rows[i].id, CATEGORY_SEEDS[c].category, dot);
               if (dot > best) {
                  best = dot;
                  best_idx = c;
               }
            }
            OLOG_INFO("memory_embeddings: sample best=%s @ %.4f (threshold=%.2f)",
                      best_idx >= 0 ? CATEGORY_SEEDS[best_idx].category : "(none)", best,
                      threshold);
            sample_logged = true;
         }

         assigned_cat[i] = classify_fact_embedding(rows[i].emb, centroids, dims, threshold);
         touched++;
      }

      /* Phase 2: apply the UPDATEs under one transaction.  Lock held only for
       * the SQLite work, not the cosine math. */
      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return 1;
      }
      char *errmsg = NULL;
      int begin_rc = sqlite3_exec(s_db.db, "BEGIN", NULL, NULL, &errmsg);
      if (begin_rc != SQLITE_OK) {
         OLOG_ERROR("memory_embeddings: BEGIN failed in category backfill: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return 1;
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      for (int i = 0; i < batch_count && !atomic_load(&s_backfill_shutdown); i++) {
         const char *cat = assigned_cat[i];
         if (strcmp(cat, "general") == 0)
            continue; /* Leave the column at its current 'general' value. */

         /* Inline UPDATE — already hold lock; can't call memory_db_fact_update_category. */
         sqlite3_stmt *upd_stmt = s_db.stmt_memory_fact_update_category;
         sqlite3_reset(upd_stmt);
         sqlite3_bind_text(upd_stmt, 1, cat, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(upd_stmt, 2, rows[i].id);
         if (sqlite3_step(upd_stmt) == SQLITE_DONE) {
            classified++;
            /* Tally for the final distribution log */
            for (int c = 0; c < CATEGORY_SEEDS_COUNT; c++) {
               if (strcmp(cat, CATEGORY_SEEDS[c].category) == 0) {
                  per_cat_count[c]++;
                  break;
               }
            }
         }
         sqlite3_reset(upd_stmt);
      }

      int commit_rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
      if (commit_rc != SQLITE_OK) {
         OLOG_ERROR("memory_embeddings: COMMIT failed in category backfill: %s — rolling back",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return 1;
      }
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();

      /* If we got fewer than the fetch limit, no more unclassified facts remain. */
      if (batch_count < CATEGORY_BACKFILL_FETCH)
         break;

      /* Cooperative throttle between batches. */
      usleep(50000);

      if (++loops > 1000) {
         OLOG_WARNING("memory_embeddings: category backfill loop guard tripped");
         break;
      }
   }

   free(rows);

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

/* =============================================================================
 * Math Utilities — delegate to shared embedding engine
 * ============================================================================= */

float memory_embeddings_l2_norm(const float *vec, int dims) {
   return embedding_engine_l2_norm(vec, dims);
}

float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b) {
   return embedding_engine_cosine_with_norms(a, b, dims, norm_a, norm_b);
}

float memory_embeddings_cosine(const float *a, const float *b, int dims) {
   return embedding_engine_cosine(a, b, dims);
}

/* =============================================================================
 * Cache Management
 * ============================================================================= */

static void cache_free_data(void) {
   free(s_cache.ids);
   free(s_cache.embeddings);
   free(s_cache.norms);
   free(s_cache.created_ats);
   free(s_cache.note_doc_ids);
   s_cache.ids = NULL;
   s_cache.embeddings = NULL;
   s_cache.norms = NULL;
   s_cache.created_ats = NULL;
   s_cache.note_doc_ids = NULL;
   s_cache.count = 0;
   s_cache.capacity = 0;
   s_cache.valid = false;
}

static int cache_load(int user_id) {
   /* Already valid for this user? */
   if (s_cache.valid && s_cache.user_id == user_id && !atomic_load(&s_cache.dirty))
      return 0;

   cache_free_data();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return FAILURE;

   /* Allocate for EMBEDDING_SEARCH_CAP entries */
   int cap = EMBEDDING_SEARCH_CAP;
   s_cache.ids = malloc(cap * sizeof(int64_t));
   s_cache.embeddings = malloc((size_t)cap * (size_t)dims * sizeof(float));
   s_cache.norms = malloc(cap * sizeof(float));
   s_cache.created_ats = malloc(cap * sizeof(int64_t));
   /* calloc (not malloc): a future append path that forgets to set this field
    * fails safe — a 0 slot is treated as a non-gloss, never the reverse. */
   s_cache.note_doc_ids = calloc((size_t)cap, sizeof(int64_t));

   if (!s_cache.ids || !s_cache.embeddings || !s_cache.norms || !s_cache.created_ats ||
       !s_cache.note_doc_ids) {
      cache_free_data();
      return FAILURE;
   }

   int loaded = 0;
   if (memory_db_fact_get_embeddings(user_id, dims, s_cache.ids, s_cache.embeddings, s_cache.norms,
                                     s_cache.created_ats, s_cache.note_doc_ids, cap,
                                     &loaded) != MEMORY_DB_SUCCESS) {
      cache_free_data();
      return FAILURE;
   }

   s_cache.count = loaded;
   s_cache.capacity = cap;
   s_cache.dims = dims;
   s_cache.user_id = user_id;
   s_cache.valid = true;
   atomic_store(&s_cache.dirty, false);

   OLOG_INFO("memory_embeddings: loaded %d embeddings into cache for user %d", loaded, user_id);

   /* Saturation warning (efficiency H1).  When the cache loader fills the
    * entire EMBEDDING_SEARCH_CAP slot allocation, the (cap+1)th+ rows for
    * this user silently never enter the cache and are excluded from
    * cosine ranking.  Emit a one-shot WARNING per user so the operator
    * notices before retrieval quality visibly degrades.  Subsequent
    * reloads for the same user keep the high-water mark and stay silent. */
   if (loaded >= cap && s_cache.saturated_warned_user_id != user_id) {
      OLOG_WARNING("memory_embeddings: cache saturated at %d facts for user_id=%d — additional "
                   "facts will not participate in cosine ranking (see EMBEDDING_SEARCH_CAP)",
                   loaded, user_id);
      s_cache.saturated_warned_user_id = user_id;
   }
   return 0;
}

void memory_embeddings_invalidate_cache(void) {
   atomic_store(&s_cache.dirty, true);
}

/* =============================================================================
 * Init / Cleanup — delegate provider management to shared embedding engine
 * ============================================================================= */

int memory_embeddings_init(void) {
   return embedding_engine_init();
}

void memory_embeddings_cleanup(void) {
   /* Stop backfill thread */
   if (atomic_load(&s_backfill_running)) {
      atomic_store(&s_backfill_shutdown, true);
      pthread_join(s_backfill_thread, NULL);
      atomic_store(&s_backfill_running, false);
   }

   /* Free caches */
   pthread_mutex_lock(&s_cache.mutex);
   cache_free_data();
   pthread_mutex_unlock(&s_cache.mutex);

   pthread_mutex_lock(&s_entity_cache.mutex);
   entity_cache_free();
   pthread_mutex_unlock(&s_entity_cache.mutex);

   /* Provider cleanup handled by embedding_engine_cleanup() in dawn.c shutdown */
}

bool memory_embeddings_available(void) {
   return embedding_engine_available();
}

int memory_embeddings_dims(void) {
   return embedding_engine_dims();
}

/* =============================================================================
 * Embedding Generation
 * ============================================================================= */

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   if (!text || !out || !out_dims)
      return FAILURE;

   return embedding_engine_embed(text, out, MAX_EMBEDDING_DIMS, out_dims);
}

int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   if (!embedding_engine_available() || !text)
      return FAILURE;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return rc;

   float norm = memory_embeddings_l2_norm(embedding, dims);

   rc = memory_db_fact_update_embedding(user_id, fact_id, embedding, dims, norm);
   if (rc == MEMORY_DB_SUCCESS) {
      memory_embeddings_invalidate_cache();
   }
   return rc;
}

/* Internal: append a pre-embedded fact to the in-memory cache without
 * invalidating it, so an N-fact extraction loop does not pay N cache
 * reloads against SQLite.  Returns 0 on append, non-zero on any reason
 * the cache cannot accept the row (capacity / user mismatch / dims
 * mismatch / cache invalid) — callers should treat non-zero as
 * "fall back to invalidate so the next access reloads fresh." */
static int cache_append_locked(int user_id,
                               int64_t fact_id,
                               const float *vec,
                               int dims,
                               int64_t created_at,
                               float norm) {
   if (!s_cache.valid || s_cache.user_id != user_id || s_cache.dims != dims)
      return FAILURE;
   if (s_cache.count >= s_cache.capacity)
      return FAILURE;

   int idx = s_cache.count;
   s_cache.ids[idx] = fact_id;
   memcpy(s_cache.embeddings + (size_t)idx * (size_t)dims, vec, (size_t)dims * sizeof(float));
   s_cache.norms[idx] = norm;
   s_cache.created_ats[idx] = created_at;
   /* A fact appended to the warm cache (store_precomputed) is by construction a
    * non-gloss — bridge glosses go through embed_and_store → invalidate → full
    * reload.  Must be set explicitly: the array is allocated uninitialized and
    * nearest_fact/find_duplicate_clusters skip entries with note_doc_id > 0, so a
    * garbage slot could silently drop a real fact from paraphrase-dedup. */
   if (s_cache.note_doc_ids)
      s_cache.note_doc_ids[idx] = 0;
   s_cache.count++;
   return SUCCESS;
}

int memory_embeddings_store_precomputed(int user_id, int64_t fact_id, const float *vec, int dims) {
   if (!vec || dims <= 0)
      return FAILURE;

   float norm = memory_embeddings_l2_norm(vec, dims);

   int rc = memory_db_fact_update_embedding(user_id, fact_id, vec, dims, norm);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Try to append directly into the warm cache so a multi-fact extraction
    * loop avoids the N cache-reload cycles that invalidate-then-reload would
    * cause.  On any reason the cache cannot accept the row (over capacity,
    * different user warm, dim mismatch, cache cold), fall back to invalidate
    * so the next access reloads fresh.  Either path leaves the cache in a
    * correct state. */
   pthread_mutex_lock(&s_cache.mutex);
   int append_rc = cache_append_locked(user_id, fact_id, vec, dims, time(NULL), norm);
   pthread_mutex_unlock(&s_cache.mutex);

   if (append_rc != SUCCESS) {
      memory_embeddings_invalidate_cache();
   }
   return MEMORY_DB_SUCCESS;
}

int memory_embeddings_warm_cache(int user_id) {
   pthread_mutex_lock(&s_cache.mutex);
   int rc = cache_load(user_id);
   pthread_mutex_unlock(&s_cache.mutex);
   return rc;
}

int memory_embeddings_nearest_fact(int user_id,
                                   const float *query_vec,
                                   int query_dims,
                                   float threshold,
                                   int64_t *matched_id_out,
                                   float *score_out) {
   if (matched_id_out)
      *matched_id_out = 0;
   if (score_out)
      *score_out = 0.0f;
   if (!query_vec || query_dims <= 0)
      return MEMORY_DB_FAILURE;

   float query_norm = memory_embeddings_l2_norm(query_vec, query_dims);
   if (query_norm < 1e-6f)
      return MEMORY_DB_SUCCESS; /* zero vector — no match, but not an error */

   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      return MEMORY_DB_FAILURE;
   }

   if (s_cache.dims != query_dims) {
      /* Dimension mismatch — provider swap mid-flight, or stale cache.
       * Treat as no-match rather than failing the gate; the embedding-
       * recompute worker will resync the cache shortly. */
      pthread_mutex_unlock(&s_cache.mutex);
      return MEMORY_DB_SUCCESS;
   }

   /* Walk the cache exiting on first match >= threshold.  We do not need
    * the absolute best match — the gate's purpose is to detect that ANY
    * existing fact paraphrases the new one.  Early-exit halves the
    * average critical section on the hit path. */
   int64_t best_id = 0;
   float best_score = 0.0f;
   for (int i = 0; i < s_cache.count; i++) {
      /* Skip facts without embeddings — pre-bge-small-swap rows that
       * have not yet been recompute-worker'd will have norm == 0. */
      if (s_cache.norms[i] < 1e-6f)
         continue;
      /* Skip memory→note bridge glosses (v61): they stay in the cache for
       * semantic retrieval (hybrid_search walks the same array) but must never
       * be a paraphrase-dedup merge target — a gloss is a pointer, not a fact. */
      if (s_cache.note_doc_ids && s_cache.note_doc_ids[i] > 0)
         continue;
      float cosine = memory_embeddings_cosine_with_norms(
          query_vec, s_cache.embeddings + (size_t)i * (size_t)query_dims, query_dims, query_norm,
          s_cache.norms[i]);
      if (cosine >= threshold) {
         best_id = s_cache.ids[i];
         best_score = cosine;
         break;
      }
   }
   pthread_mutex_unlock(&s_cache.mutex);

   if (best_id != 0) {
      if (matched_id_out)
         *matched_id_out = best_id;
      if (score_out)
         *score_out = best_score;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_embeddings_band_neighbors_scan(const int64_t *ids,
                                          const float *embs,
                                          const float *norms,
                                          int count,
                                          int dims,
                                          const float *query_vec,
                                          float query_norm,
                                          float low,
                                          float high,
                                          int64_t *out_ids,
                                          float *out_scores,
                                          int max,
                                          int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!ids || !embs || !norms || !query_vec || count < 0 || dims <= 0 || !out_ids || !out_scores ||
       max <= 0)
      return MEMORY_DB_FAILURE;
   if (query_norm < 1e-6f)
      return MEMORY_DB_SUCCESS; /* zero query vector — no neighbors, not an error */

   /* Single O(N) scan collecting band matches into a tiny insertion-sorted
    * top-@p max by descending score.  max <= MEMORY_BAND_NEIGHBORS_MAX keeps the
    * per-insert shift cheap (no heap, no full sort). */
   int n = 0;
   for (int i = 0; i < count; i++) {
      if (norms[i] < 1e-6f)
         continue; /* no embedding yet (pre-swap row) */
      float cosine = memory_embeddings_cosine_with_norms(query_vec, embs + (size_t)i * (size_t)dims,
                                                         dims, query_norm, norms[i]);
      if (cosine < low || cosine >= high)
         continue;
      if (n == max && cosine <= out_scores[n - 1])
         continue; /* window full and weaker than the current weakest — skip */
      /* Insertion-sort into the descending-score window, dropping the weakest. */
      int pos = (n < max) ? n : max - 1;
      while (pos > 0 && out_scores[pos - 1] < cosine) {
         out_scores[pos] = out_scores[pos - 1];
         out_ids[pos] = out_ids[pos - 1];
         pos--;
      }
      out_scores[pos] = cosine;
      out_ids[pos] = ids[i];
      if (n < max)
         n++;
   }

   if (out_count)
      *out_count = n;
   return MEMORY_DB_SUCCESS;
}

int memory_embeddings_band_neighbors(int user_id,
                                     const float *query_vec,
                                     int query_dims,
                                     float low,
                                     float high,
                                     int64_t *out_ids,
                                     float *out_scores,
                                     int max,
                                     int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!query_vec || query_dims <= 0 || !out_ids || !out_scores || max <= 0)
      return MEMORY_DB_FAILURE;

   float query_norm = memory_embeddings_l2_norm(query_vec, query_dims);
   if (query_norm < 1e-6f)
      return MEMORY_DB_SUCCESS; /* zero vector — no neighbors, but not an error */

   /* Lock-and-scan (NOT snapshot): this is a single O(N) pass like nearest_fact,
    * sub-ms at the dev's scale, so the brief lock hold is fine — no need for the
    * O(N^2) dup-finder's snapshot dance.  The pure scan reads the cache arrays. */
   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      return MEMORY_DB_FAILURE;
   }
   if (s_cache.dims != query_dims) {
      /* Dimension mismatch — provider swap mid-flight or stale cache.  Treat as
       * no-neighbors rather than failing; the recompute worker resyncs shortly. */
      pthread_mutex_unlock(&s_cache.mutex);
      return MEMORY_DB_SUCCESS;
   }
   int rc = memory_embeddings_band_neighbors_scan(s_cache.ids, s_cache.embeddings, s_cache.norms,
                                                  s_cache.count, s_cache.dims, query_vec,
                                                  query_norm, low, high, out_ids, out_scores, max,
                                                  out_count);
   pthread_mutex_unlock(&s_cache.mutex);
   return rc;
}

/* =============================================================================
 * Duplicate-fact clustering
 * ============================================================================= */

int memory_embeddings_cluster_by_cosine(const int64_t *ids,
                                        const float *embs,
                                        const float *norms,
                                        int count,
                                        int dims,
                                        float threshold,
                                        memory_dup_cluster_t *out,
                                        int max_clusters,
                                        int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!ids || !embs || !norms || !out || count <= 0 || dims <= 0 || max_clusters <= 0)
      return MEMORY_DB_FAILURE;

   bool *visited = calloc((size_t)count, sizeof(bool));
   if (!visited)
      return MEMORY_DB_FAILURE;

   int n_clusters = 0;
   for (int i = 0; i < count && n_clusters < max_clusters; i++) {
      if (visited[i] || norms[i] < 1e-6f)
         continue; /* already clustered, or no embedding */
      visited[i] = true;
      memory_dup_cluster_t cl;
      cl.count = 0;
      cl.min_similarity = 1.0f;
      cl.ids[cl.count++] = ids[i];

      const float *vi = embs + (size_t)i * (size_t)dims;
      for (int j = i + 1; j < count && cl.count < MEMORY_DUP_MAX_PER_CLUSTER; j++) {
         if (visited[j] || norms[j] < 1e-6f)
            continue;
         float cos = memory_embeddings_cosine_with_norms(vi, embs + (size_t)j * (size_t)dims, dims,
                                                         norms[i], norms[j]);
         if (cos >= threshold) {
            visited[j] = true;
            cl.ids[cl.count++] = ids[j];
            if (cos < cl.min_similarity)
               cl.min_similarity = cos;
         }
      }
      if (cl.count >= 2)
         out[n_clusters++] = cl;
   }

   free(visited);
   if (out_count)
      *out_count = n_clusters;
   return MEMORY_DB_SUCCESS;
}

int memory_embeddings_find_duplicate_clusters(int user_id,
                                              float threshold,
                                              memory_dup_cluster_t *out_clusters,
                                              int max_clusters,
                                              int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!out_clusters || max_clusters <= 0)
      return MEMORY_DB_FAILURE;
   if (threshold <= 0.0f || threshold > 1.0f)
      threshold = 0.85f;

   /* Snapshot the cache under the lock, then cluster on the copy: the O(N^2)
    * scan must NOT hold s_cache.mutex (it gates the per-fact paraphrase-dedup
    * gate and the recompute/backfill worker — a multi-hundred-ms hold would
    * stall extraction). */
   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      return MEMORY_DB_FAILURE;
   }
   int count = s_cache.count;
   int dims = s_cache.dims;
   if (count < 2 || dims <= 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      return MEMORY_DB_SUCCESS; /* nothing to cluster */
   }

   int64_t *ids = malloc((size_t)count * sizeof(int64_t));
   float *norms = malloc((size_t)count * sizeof(float));
   float *embs = malloc((size_t)count * (size_t)dims * sizeof(float));
   if (!ids || !norms || !embs) {
      pthread_mutex_unlock(&s_cache.mutex);
      free(ids);
      free(norms);
      free(embs);
      OLOG_ERROR("memory_embeddings: OOM snapshotting %d facts (%d dims) for dup-scan", count,
                 dims);
      return MEMORY_DB_FAILURE;
   }
   /* Exclude memory→note bridge glosses (v61) from the snapshot — a gloss must
    * never be offered to the operator as a duplicate-merge candidate.  The
    * buffers were sized for the full count, so the compacted copy fits. */
   int kept = 0;
   for (int i = 0; i < count; i++) {
      if (s_cache.note_doc_ids && s_cache.note_doc_ids[i] > 0)
         continue;
      ids[kept] = s_cache.ids[i];
      norms[kept] = s_cache.norms[i];
      memcpy(embs + (size_t)kept * (size_t)dims, s_cache.embeddings + (size_t)i * (size_t)dims,
             (size_t)dims * sizeof(float));
      kept++;
   }
   count = kept;
   pthread_mutex_unlock(&s_cache.mutex);

   if (count < 2) {
      free(ids);
      free(norms);
      free(embs);
      return MEMORY_DB_SUCCESS; /* nothing to cluster after excluding glosses */
   }

   int rc = memory_embeddings_cluster_by_cosine(ids, embs, norms, count, dims, threshold,
                                                out_clusters, max_clusters, out_count);
   free(ids);
   free(norms);
   free(embs);
   return rc;
}

/* =============================================================================
 * Entity Embedding Support
 * ============================================================================= */

static void entity_cache_free(void) {
   free(s_entity_cache.ids);
   free(s_entity_cache.names);
   free(s_entity_cache.types);
   free(s_entity_cache.embeddings);
   free(s_entity_cache.norms);
   s_entity_cache.ids = NULL;
   s_entity_cache.names = NULL;
   s_entity_cache.types = NULL;
   s_entity_cache.embeddings = NULL;
   s_entity_cache.norms = NULL;
   s_entity_cache.count = 0;
   s_entity_cache.valid = false;
}

static int entity_cache_load(int user_id) {
   if (s_entity_cache.valid && s_entity_cache.user_id == user_id &&
       !atomic_load(&s_entity_cache.dirty))
      return 0;

   entity_cache_free();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return FAILURE;

   s_entity_cache.ids = malloc(ENTITY_CACHE_CAP * sizeof(int64_t));
   s_entity_cache.names = malloc(ENTITY_CACHE_CAP * sizeof(*s_entity_cache.names));
   s_entity_cache.types = malloc(ENTITY_CACHE_CAP * sizeof(*s_entity_cache.types));
   s_entity_cache.embeddings = malloc((size_t)ENTITY_CACHE_CAP * (size_t)dims * sizeof(float));
   s_entity_cache.norms = malloc(ENTITY_CACHE_CAP * sizeof(float));

   if (!s_entity_cache.ids || !s_entity_cache.names || !s_entity_cache.types ||
       !s_entity_cache.embeddings || !s_entity_cache.norms) {
      entity_cache_free();
      return FAILURE;
   }

   int loaded = 0;
   /* Production cache is canonical-only — aliases (canonical_id IS NOT NULL)
    * are filtered out so the cosine pool doesn't surface duplicate
    * surface-form variants. */
   if (memory_db_entity_get_embeddings(user_id, /* include_aliases */ false, dims,
                                       s_entity_cache.ids, s_entity_cache.names,
                                       s_entity_cache.types, s_entity_cache.embeddings,
                                       s_entity_cache.norms, ENTITY_CACHE_CAP,
                                       &loaded) != MEMORY_DB_SUCCESS) {
      entity_cache_free();
      return FAILURE;
   }

   s_entity_cache.count = loaded;
   s_entity_cache.dims = dims;
   s_entity_cache.user_id = user_id;
   s_entity_cache.valid = true;
   atomic_store(&s_entity_cache.dirty, false);

   OLOG_INFO("memory_embeddings: loaded %d entity embeddings into cache for user %d", loaded,
             user_id);
   return 0;
}

int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text) {
   if (!embedding_engine_available() || !text)
      return FAILURE;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return rc;

   float norm = memory_embeddings_l2_norm(embedding, dims);

   rc = memory_db_entity_update_embedding(entity_id, user_id, embedding, dims, norm);
   if (rc == MEMORY_DB_SUCCESS) {
      memory_embeddings_invalidate_entity_cache();
   }
   return rc;
}

void memory_embeddings_invalidate_entity_cache(void) {
   atomic_store(&s_entity_cache.dirty, true);
}

int memory_embeddings_embed_and_store_summary(int user_id, int64_t summary_id, const char *text) {
   if (!embedding_engine_available() || !text || !text[0])
      return FAILURE;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return FAILURE;

   /* No cache to invalidate — memory_db_summary_search_semantic scans the
    * table directly each call (corpus is small).  Norm is recomputed
    * inside the scan, so we don't pass one. */
   return memory_db_summary_update_embedding(user_id, summary_id, embedding, dims);
}

void memory_embeddings_invalidate_all(void) {
   memory_embeddings_invalidate_cache();
   memory_embeddings_invalidate_entity_cache();
}

int memory_embeddings_entity_search(int user_id,
                                    const char *query,
                                    const char *type_filter,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_scores,
                                    int max_results) {
   if (!memory_embeddings_available() || !query || !out_ids || max_results <= 0)
      return 0;

   /* Embed the query */
   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(query, query_emb, &dims) != 0 || dims != embedding_engine_dims())
      return 0;

   float query_norm = memory_embeddings_l2_norm(query_emb, dims);

   pthread_mutex_lock(&s_entity_cache.mutex);
   if (entity_cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_entity_cache.mutex);
      return 0;
   }

   /* Score all cached entities by cosine similarity */
   typedef struct {
      int idx;
      float score;
   } scored_t;
   scored_t scored[ENTITY_CACHE_CAP];
   int scored_count = 0;

   for (int i = 0; i < s_entity_cache.count; i++) {
      /* Apply type filter if specified */
      if (type_filter && type_filter[0] != '\0' &&
          strcmp(s_entity_cache.types[i], type_filter) != 0) {
         continue;
      }

      float cosine = memory_embeddings_cosine_with_norms(query_emb,
                                                         s_entity_cache.embeddings + i * dims, dims,
                                                         query_norm, s_entity_cache.norms[i]);

      if (cosine > 0.4f) {
         scored[scored_count].idx = i;
         scored[scored_count].score = cosine;
         scored_count++;
      }
   }

   /* Sort by score descending (insertion sort — small N) */
   for (int i = 1; i < scored_count; i++) {
      scored_t tmp = scored[i];
      int j = i - 1;
      while (j >= 0 && scored[j].score < tmp.score) {
         scored[j + 1] = scored[j];
         j--;
      }
      scored[j + 1] = tmp;
   }

   /* Copy top results */
   int result_count = scored_count > max_results ? max_results : scored_count;
   for (int i = 0; i < result_count; i++) {
      int idx = scored[i].idx;
      out_ids[i] = s_entity_cache.ids[idx];
      if (out_names) {
         safe_strscpy(out_names[i], s_entity_cache.names[idx]);
      }
      if (out_types) {
         safe_strscpy(out_types[i], s_entity_cache.types[idx]);
      }
      if (out_scores)
         out_scores[i] = scored[i].score;
   }

   pthread_mutex_unlock(&s_entity_cache.mutex);
   return result_count;
}

int memory_embeddings_entity_cosine(int user_id,
                                    int64_t entity_id,
                                    const float *query_embedding,
                                    int query_dims,
                                    float query_norm,
                                    float *out_cosine) {
   if (!query_embedding || query_dims <= 0 || !out_cosine || entity_id <= 0) {
      return FAILURE;
   }
   if (!memory_embeddings_available()) {
      return FAILURE;
   }

   pthread_mutex_lock(&s_entity_cache.mutex);
   if (entity_cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_entity_cache.mutex);
      return FAILURE;
   }

   /* Dimension mismatch — caller's embedding came from a different model;
    * cosine would be meaningless. */
   if (s_entity_cache.dims != query_dims) {
      pthread_mutex_unlock(&s_entity_cache.mutex);
      return FAILURE;
   }

   /* Linear scan over the cache.  ENTITY_CACHE_CAP is 500; the alias
    * resolver runs at extraction time (off the conversational hot path)
    * and visits at most 8 candidates per call (Stage 4 cap), so the
    * outer-loop count of 500 is amortized across few invocations. */
   int found = -1;
   for (int i = 0; i < s_entity_cache.count; i++) {
      if (s_entity_cache.ids[i] == entity_id) {
         found = i;
         break;
      }
   }
   if (found < 0) {
      pthread_mutex_unlock(&s_entity_cache.mutex);
      return FAILURE;
   }

   *out_cosine = memory_embeddings_cosine_with_norms(
       query_embedding, s_entity_cache.embeddings + (size_t)found * query_dims, query_dims,
       query_norm, s_entity_cache.norms[found]);
   pthread_mutex_unlock(&s_entity_cache.mutex);
   return SUCCESS;
}

/* =============================================================================
 * Hybrid Search
 * ============================================================================= */

/* Helper: write keyword-only fallback (no embeddings available or embed failed). */
static int hybrid_fallback_keyword_only(const int64_t *keyword_facts,
                                        const int *keyword_scores,
                                        int keyword_count,
                                        int token_count,
                                        embedding_search_result_t *out_results,
                                        int max_results) {
   int count = keyword_count > max_results ? max_results : keyword_count;
   for (int i = 0; i < count; i++) {
      out_results[i].fact_id = keyword_facts[i];
      out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                               : 1.0f;
   }
   return count;
}

int memory_embeddings_hybrid_search_ex(int user_id,
                                       const char *query,
                                       const float *query_emb_in,
                                       float query_norm_in,
                                       const int64_t *keyword_facts,
                                       const int *keyword_scores,
                                       int keyword_count,
                                       int token_count,
                                       embedding_search_result_t *out_results,
                                       int max_results) {
   if (!out_results || max_results <= 0)
      return 0;

   float kw_weight = g_config.memory.embedding_keyword_weight;
   float vec_weight = g_config.memory.embedding_vector_weight;
   float temporal_weight = g_config.memory.temporal_weight;

   /* Parse temporal expression in the query (only when feature is enabled).
    * Cost is a single-pass strstr scan; skipped entirely when weight is 0. */
   time_query_t tq = { 0 };
   if (temporal_weight > 0.0f && query) {
      time_query_parse(query, (int64_t)time(NULL), &tq);
   }

   /* If no embeddings available, return keyword results directly */
   if (!memory_embeddings_available() || !query) {
      return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count, token_count,
                                          out_results, max_results);
   }

   /* Resolve the query embedding: caller-supplied if non-NULL with a usable
    * norm; otherwise embed internally (legacy behavior).  Efficiency M3 —
    * upstream callers that have already embedded the same query (rescore,
    * adapter framework) thread the pair through to skip a second ONNX
    * inference (~15 ms saved on bge-small INT8 / Jetson).
    *
    * Note on stack: query_emb_local[MAX_EMBEDDING_DIMS] is 8 KB and lives
    * at function scope.  When caller supplies a pre-embedded query, the
    * scratch goes unused but is still allocated.  Phase 9.5 reviewed
    * scoping it into an else-only block but query_emb aliases this buffer
    * across the rest of the function — restructure would require either
    * heap-alloc or a full function refactor, neither worth 8 KB on the
    * Jetson 8 GB system.  Accepted overhead. */
   float query_emb_local[MAX_EMBEDDING_DIMS];
   const float *query_emb;
   float query_norm;
   int dims = 0;
   if (query_emb_in != NULL && query_norm_in >= 1e-6f) {
      query_emb = query_emb_in;
      query_norm = query_norm_in;
      dims = embedding_engine_dims();
   } else {
      if (memory_embeddings_embed(query, query_emb_local, &dims) != 0 ||
          dims != embedding_engine_dims()) {
         /* Embedding failed — fall back to keyword only */
         return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count,
                                             token_count, out_results, max_results);
      }
      query_norm = memory_embeddings_l2_norm(query_emb_local, dims);
      query_emb = query_emb_local;
   }

   /* Load cache under lock */
   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      /* Cache load failed — keyword only */
      return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count, token_count,
                                          out_results, max_results);
   }

   /* Build result set: start with keyword facts, add vector matches.
    * Per-call scratch capped at MEMORY_HYBRID_SCRATCH_CAP (2000) — decoupled
    * from EMBEDDING_SEARCH_CAP so the cache can grow without inflating the
    * stack frame.  ~24 KB on the stack, safe on 256 KB pthread stacks. */
   int merged_cap = keyword_count + s_cache.count;
   if (merged_cap > MEMORY_HYBRID_SCRATCH_CAP)
      merged_cap = MEMORY_HYBRID_SCRATCH_CAP;

   embedding_search_result_t merged[MEMORY_HYBRID_SCRATCH_CAP];
   int merged_count = 0;

   /* Score all cached embeddings by vector similarity */
   for (int i = 0; i < s_cache.count && merged_count < merged_cap; i++) {
      float cosine = memory_embeddings_cosine_with_norms(query_emb, s_cache.embeddings + i * dims,
                                                         dims, query_norm, s_cache.norms[i]);

      /* Find keyword score for this fact (if any) */
      float kw_score = 0.0f;
      for (int k = 0; k < keyword_count; k++) {
         if (keyword_facts[k] == s_cache.ids[i]) {
            kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
            break;
         }
      }

      float hybrid = kw_weight * kw_score + vec_weight * cosine;

      /* Additive temporal boost.  Additive (not multiplicative) so undated facts
       * aren't penalized — they simply forfeit the bonus. */
      if (tq.found && s_cache.created_ats[i] > 0) {
         hybrid += temporal_weight * time_query_proximity(&tq, s_cache.created_ats[i]);
      }

      if (hybrid > 0.01f) { /* Skip near-zero results */
         merged[merged_count].fact_id = s_cache.ids[i];
         merged[merged_count].score = hybrid;
         merged_count++;
      }
   }

   /* Add keyword-only results (facts without embeddings) */
   for (int k = 0; k < keyword_count; k++) {
      bool found = false;
      for (int m = 0; m < merged_count; m++) {
         if (merged[m].fact_id == keyword_facts[k]) {
            found = true;
            break;
         }
      }
      if (!found && merged_count < merged_cap) {
         float kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
         merged[merged_count].fact_id = keyword_facts[k];
         merged[merged_count].score = kw_weight * kw_score; /* keyword only, no vector penalty */
         merged_count++;
      }
   }

   pthread_mutex_unlock(&s_cache.mutex);

   /* Sort by score descending (insertion sort — small N) */
   for (int i = 1; i < merged_count; i++) {
      embedding_search_result_t tmp = merged[i];
      int j = i - 1;
      while (j >= 0 && merged[j].score < tmp.score) {
         merged[j + 1] = merged[j];
         j--;
      }
      merged[j + 1] = tmp;
   }

   /* Copy top results */
   int result_count = merged_count > max_results ? max_results : merged_count;
   memcpy(out_results, merged, result_count * sizeof(embedding_search_result_t));

   return result_count;
}

int memory_embeddings_hybrid_search(int user_id,
                                    const char *query,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results) {
   /* Back-compat shim — NULL/0 pair forces internal embed (legacy behavior). */
   return memory_embeddings_hybrid_search_ex(user_id, query, NULL, 0.0f, keyword_facts,
                                             keyword_scores, keyword_count, token_count,
                                             out_results, max_results);
}

/* =============================================================================
 * Reciprocal Rank Fusion (RRF) Search
 *
 * Three-channel parallel-rank fusion alternative to the weighted-sum
 * composite in memory_embeddings_hybrid_search().  See header for citations
 * and the empirical basis.  Gate via g_config.memory.rrf_enabled.
 *
 * Algorithm:
 *   1. Build a candidate pool from the cache + keyword-only facts.  Each
 *      pool entry carries its raw score in three channels (cosine, kw,
 *      temporal-proximity).
 *   2. For each channel, compute per-candidate rank — facts with non-zero
 *      raw score in that channel get rank 1..N; the rest get INT_MAX
 *      (sentinel meaning "missing from this channel").
 *   3. RRF score = Σ 1/(60 + rank_i) over channels where rank ≠ INT_MAX.
 *   4. Sort by RRF score desc, return top-K.
 *
 * Rank computation is O(N²) per channel — fine for typical N ≤ 500.
 * ============================================================================= */

#define RRF_K_CONSTANT 60.0f

/* Per-channel candidate cap.  Facts ranked beyond this position in any
 * given channel get rank=INT_MAX (no contribution from that channel).
 *
 * Why: the semantic channel typically holds the entire user fact cache
 * (~300 facts), while the keyword channel holds 5-10.  Without a cap,
 * mid-rank semantic facts (positions 30-100) contribute 60/(60+rank) =
 * 0.375-0.667 and accumulate enough RRF score to outrank legitimately-
 * relevant facts, regressing top-K quality.  Cap matches canonical IR
 * practice (Mem0 v2 / Hindsight TEMPR cite top-N-per-channel before RRF).
 *
 * 50 is conservative for DAWN's scale — final top-K is typically 10, so
 * a cap of 50 still allows 5x oversampling per channel for stacking. */
#define RRF_PER_CHANNEL_CAP 50

/* Per-candidate scratch entry.  At 24 B × MEMORY_HYBRID_SCRATCH_CAP (2000) =
 * ~48 KB stack — fits within the smallest production pthread stack (256 KB,
 * llm_context.c). */
typedef struct {
   int64_t fact_id;
   float cosine;
   float kw_score;
   float tprox;
} rrf_cand_t;

/* qsort helpers (Fix H2): pre-sort each channel index array so rank
 * assignment is O(N log N) total instead of O(N²) per channel. */
struct rrf_rank_ctx {
   const rrf_cand_t *pool;
   int channel; /* 0=cosine, 1=kw, 2=tprox */
};

static int rrf_cmp_desc(const void *a, const void *b, void *ctx_v) {
   const struct rrf_rank_ctx *ctx = (const struct rrf_rank_ctx *)ctx_v;
   int ia = *(const int *)a;
   int ib = *(const int *)b;
   float sa, sb;
   switch (ctx->channel) {
      case 0:
         sa = ctx->pool[ia].cosine;
         sb = ctx->pool[ib].cosine;
         break;
      case 1:
         sa = ctx->pool[ia].kw_score;
         sb = ctx->pool[ib].kw_score;
         break;
      default:
         sa = ctx->pool[ia].tprox;
         sb = ctx->pool[ib].tprox;
         break;
   }
   /* qsort_r comparator contract: returns -1 / 0 / +1 (not SUCCESS/FAILURE).
    * Descending: higher score first.  Stable-ish on ties by pool index. */
   if (sa > sb)
      return -1;
   if (sa < sb)
      return 1;
   return ia - ib;
}

/* Comparator for sorting embedding_search_result_t by score DESC.
 * qsort contract — returns -1 / 0 / +1, not SUCCESS/FAILURE. */
static int score_cmp_desc(const void *a, const void *b) {
   float sa = ((const embedding_search_result_t *)a)->score;
   float sb = ((const embedding_search_result_t *)b)->score;
   if (sa > sb)
      return -1;
   if (sa < sb)
      return 1;
   return 0;
}

int memory_embeddings_rrf_search_ex(int user_id,
                                    const char *query,
                                    const float *query_emb_in,
                                    float query_norm_in,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results) {
   if (!out_results || max_results <= 0)
      return 0;

   /* Parse temporal expression once.  Only when the soft temporal_weight
    * is non-zero — same gate as the composite path so toggling rrf_enabled
    * doesn't silently start firing the parser. */
   time_query_t tq = { 0 };
   bool has_temporal = false;
   if (query && g_config.memory.temporal_weight > 0.0f) {
      time_query_parse(query, (int64_t)time(NULL), &tq);
      has_temporal = tq.found;
   }

   /* No embeddings or no query → keyword-only fallback (matches hybrid_search). */
   if (!memory_embeddings_available() || !query) {
      return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count, token_count,
                                          out_results, max_results);
   }

   /* Resolve the query embedding — caller-supplied if non-NULL, otherwise
    * embed internally.  Efficiency M3 — pairs with hybrid_search_ex. */
   float query_emb_local[MAX_EMBEDDING_DIMS];
   const float *query_emb;
   float query_norm;
   int dims = 0;
   if (query_emb_in != NULL && query_norm_in >= 1e-6f) {
      query_emb = query_emb_in;
      query_norm = query_norm_in;
      dims = embedding_engine_dims();
   } else {
      if (memory_embeddings_embed(query, query_emb_local, &dims) != 0 ||
          dims != embedding_engine_dims()) {
         return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count,
                                             token_count, out_results, max_results);
      }
      query_norm = memory_embeddings_l2_norm(query_emb_local, dims);
      query_emb = query_emb_local;
   }

   /* Per-candidate scratch — bounded by MEMORY_HYBRID_SCRATCH_CAP so cache
    * growth (Fix H1) doesn't inflate per-call stack. */
   rrf_cand_t pool[MEMORY_HYBRID_SCRATCH_CAP];
   int pool_count = 0;

   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count, token_count,
                                          out_results, max_results);
   }

   /* Cache pass: every cached fact becomes a candidate, scored across all
    * three channels. */
   for (int i = 0; i < s_cache.count && pool_count < MEMORY_HYBRID_SCRATCH_CAP; i++) {
      float cosine = memory_embeddings_cosine_with_norms(query_emb, s_cache.embeddings + i * dims,
                                                         dims, query_norm, s_cache.norms[i]);
      float kw_score = 0.0f;
      for (int k = 0; k < keyword_count; k++) {
         if (keyword_facts[k] == s_cache.ids[i]) {
            kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
            break;
         }
      }
      float tprox = 0.0f;
      if (has_temporal && s_cache.created_ats[i] > 0) {
         tprox = time_query_proximity(&tq, s_cache.created_ats[i]);
      }

      /* Drop candidates with no signal in any channel — they would contribute
       * 0 to RRF anyway. */
      if (cosine <= 0.01f && kw_score <= 0.0f && tprox <= 0.0f)
         continue;

      pool[pool_count].fact_id = s_cache.ids[i];
      pool[pool_count].cosine = cosine;
      pool[pool_count].kw_score = kw_score;
      pool[pool_count].tprox = tprox;
      pool_count++;
   }

   /* Keyword-only facts (not in cache) — keyword channel only. */
   for (int k = 0; k < keyword_count && pool_count < MEMORY_HYBRID_SCRATCH_CAP; k++) {
      bool already = false;
      for (int p = 0; p < pool_count; p++) {
         if (pool[p].fact_id == keyword_facts[k]) {
            already = true;
            break;
         }
      }
      if (already)
         continue;
      pool[pool_count].fact_id = keyword_facts[k];
      pool[pool_count].cosine = 0.0f;
      pool[pool_count].kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count
                                                    : 1.0f;
      pool[pool_count].tprox = 0.0f;
      pool_count++;
   }

   pthread_mutex_unlock(&s_cache.mutex);

   if (pool_count == 0)
      return 0;

   /* Efficiency H2: compute per-channel ranks in O(N log N) per channel via
    * qsort_r, not O(N²) per-channel as before.  At N=2000 that's ~7 µs per
    * channel × 3 channels = ~20 µs total vs ~24 ms per channel × 3 = ~70 ms
    * pre-fix.  At the new cache cap of 8192 (capped here at 2000), the
    * O(N²) cost would have grown to ~280 ms/query in the worst case —
    * fixing now keeps RRF correct + scalable when re-tested in Phase 7's
    * dead-letter decision (currently default-off via [memory] rrf_enabled).
    *
    * Phase 9.5 stack reduction: idx scratch is a single shared buffer reused
    * across the three channel sorts (was 3 × N int = 24 KB).  rank arrays
    * remain separate because they are read in the score-summation loop
    * below.  Total function stack at N=2000 was ~136 KB pre-9.5, ~120 KB
    * post-9.5.  Worker pthread stack is ~256 KB.  Still tight; further
    * reductions would require heap-allocating pool[]. */
   int idx_scratch[MEMORY_HYBRID_SCRATCH_CAP];
   int rank_sem[MEMORY_HYBRID_SCRATCH_CAP];
   int rank_kw[MEMORY_HYBRID_SCRATCH_CAP];
   int rank_tmp[MEMORY_HYBRID_SCRATCH_CAP];
   for (int i = 0; i < pool_count; i++) {
      rank_sem[i] = INT_MAX;
      rank_kw[i] = INT_MAX;
      rank_tmp[i] = INT_MAX;
   }

   /* Channel 0: semantic.  Fill scratch with [0..N), qsort, walk to fill rank. */
   for (int i = 0; i < pool_count; i++)
      idx_scratch[i] = i;
   struct rrf_rank_ctx ctx_sem = { .pool = pool, .channel = 0 };
   qsort_r(idx_scratch, (size_t)pool_count, sizeof(int), rrf_cmp_desc, &ctx_sem);
   for (int pos = 0; pos < pool_count; pos++) {
      int i = idx_scratch[pos];
      if (pool[i].cosine > 0.01f) {
         int r = pos + 1;
         rank_sem[i] = (r <= RRF_PER_CHANNEL_CAP) ? r : INT_MAX;
      }
   }

   /* Channel 1: keyword.  Reuse scratch. */
   for (int i = 0; i < pool_count; i++)
      idx_scratch[i] = i;
   struct rrf_rank_ctx ctx_kw = { .pool = pool, .channel = 1 };
   qsort_r(idx_scratch, (size_t)pool_count, sizeof(int), rrf_cmp_desc, &ctx_kw);
   for (int pos = 0; pos < pool_count; pos++) {
      int i = idx_scratch[pos];
      if (pool[i].kw_score > 0.0f) {
         int r = pos + 1;
         rank_kw[i] = (r <= RRF_PER_CHANNEL_CAP) ? r : INT_MAX;
      }
   }

   /* Channel 2: temporal proximity.  Only if temporal expression parsed. */
   if (has_temporal) {
      for (int i = 0; i < pool_count; i++)
         idx_scratch[i] = i;
      struct rrf_rank_ctx ctx_tmp = { .pool = pool, .channel = 2 };
      qsort_r(idx_scratch, (size_t)pool_count, sizeof(int), rrf_cmp_desc, &ctx_tmp);
      for (int pos = 0; pos < pool_count; pos++) {
         int i = idx_scratch[pos];
         if (pool[i].tprox > 0.0f) {
            int r = pos + 1;
            rank_tmp[i] = (r <= RRF_PER_CHANNEL_CAP) ? r : INT_MAX;
         }
      }
   }

   /* RRF score = Σ K/(K + rank_i) over channels with non-sentinel rank.
    *
    * Canonical RRF is `1/(K + rank)`; we scale by K so each channel's
    * rank-1 contribution = K/(K+1) ≈ 0.984 instead of 0.0164.  This puts
    * the output in the same [0, ~num_channels] range that
    * `g_config.memory.search_score_floor` (default 0.30) was calibrated
    * for — without it, every RRF result falls below the floor and gets
    * silently dropped.  Multi-channel facts can exceed 1.0 (max ≈ 2.95
    * for rank-1 in all three channels), which the floor passes cleanly. */
   embedding_search_result_t scored[MEMORY_HYBRID_SCRATCH_CAP];
   for (int i = 0; i < pool_count; i++) {
      float s = 0.0f;
      if (rank_sem[i] != INT_MAX)
         s += RRF_K_CONSTANT / (RRF_K_CONSTANT + (float)rank_sem[i]);
      if (rank_kw[i] != INT_MAX)
         s += RRF_K_CONSTANT / (RRF_K_CONSTANT + (float)rank_kw[i]);
      if (rank_tmp[i] != INT_MAX)
         s += RRF_K_CONSTANT / (RRF_K_CONSTANT + (float)rank_tmp[i]);
      scored[i].fact_id = pool[i].fact_id;
      scored[i].score = s;
   }

   /* Sort by RRF score desc.  pool_count can reach MEMORY_HYBRID_SCRATCH_CAP
    * (2000) — insertion sort here was ~4 ms at N=2000 (O(N²)); qsort is
    * ~70 µs (O(N log N)). */
   qsort(scored, (size_t)pool_count, sizeof(embedding_search_result_t), score_cmp_desc);

   int result_count = pool_count > max_results ? max_results : pool_count;
   memcpy(out_results, scored, result_count * sizeof(embedding_search_result_t));
   return result_count;
}

int memory_embeddings_rrf_search(int user_id,
                                 const char *query,
                                 const int64_t *keyword_facts,
                                 const int *keyword_scores,
                                 int keyword_count,
                                 int token_count,
                                 embedding_search_result_t *out_results,
                                 int max_results) {
   /* Back-compat shim — NULL/0 pair forces internal embed (legacy behavior). */
   return memory_embeddings_rrf_search_ex(user_id, query, NULL, 0.0f, keyword_facts, keyword_scores,
                                          keyword_count, token_count, out_results, max_results);
}

int memory_embeddings_rescore_against_query(int user_id,
                                            const float *query_emb,
                                            float query_norm,
                                            float entity_bonus,
                                            const int64_t *fact_ids,
                                            int fact_count,
                                            float *out_scores) {
   if (out_scores == NULL || fact_count < 0)
      return FAILURE;
   if (fact_count == 0)
      return SUCCESS;
   if (fact_ids == NULL)
      return FAILURE;

   /* Initialize every score to the un-scoreable sentinel.  Only facts
    * found in the per-user embedding cache get a real score below.
    * Callers MUST treat negative scores as "this fact could not be
    * query-scored" and skip them — DO NOT merge sentinel-scored facts
    * into the LLM-facing pool.
    *
    * Prior implementations fell back to `entity_bonus` for un-scoreable
    * facts, which silently dumped low-quality candidates into the merge.
    * Architecture-review finding (May 14, 2026): that fallback conflated
    * "I have no evidence" with "I have evidence this is irrelevant" and
    * was load-bearing on the Step 1 regression. */
   for (int i = 0; i < fact_count; i++)
      out_scores[i] = MEMORY_EMBEDDINGS_RESCORE_SENTINEL;

   if (query_emb == NULL || query_norm < 1e-6f || !memory_embeddings_available())
      return SUCCESS;

   const int dims = embedding_engine_dims();
   const float vec_weight = g_config.memory.embedding_vector_weight;

   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      OLOG_DEBUG("memory_embeddings_rescore: cache_load failed, all facts left unscored");
      return SUCCESS;
   }

   /* For each caller-supplied fact_id, find it in the cache and compute
    * cosine.  Linear scan of s_cache.ids[] per candidate — O(N * cache_size)
    * total.  At typical scale (~30 graph candidates × ~2000 cached facts =
    * ~60k integer comparisons) this is sub-ms.  Facts not in the cache
    * (no stored embedding) KEEP the sentinel — the caller drops them. */
   for (int i = 0; i < fact_count; i++) {
      int64_t fid = fact_ids[i];
      if (fid <= 0)
         continue;
      int found_idx = -1;
      for (int c = 0; c < s_cache.count; c++) {
         if (s_cache.ids[c] == fid) {
            found_idx = c;
            break;
         }
      }
      if (found_idx < 0)
         continue; /* keep sentinel — caller drops */
      const float cosine = memory_embeddings_cosine_with_norms(
          query_emb, s_cache.embeddings + (size_t)found_idx * (size_t)dims, dims, query_norm,
          s_cache.norms[found_idx]);
      out_scores[i] = vec_weight * cosine + entity_bonus;
   }

   pthread_mutex_unlock(&s_cache.mutex);
   return SUCCESS;
}

/* =============================================================================
 * Background Backfill
 * ============================================================================= */

static void *backfill_thread_fn(void *arg) {
   (void)arg;
   int user_id = s_backfill_user_id;
   int dims = embedding_engine_dims();

   OLOG_INFO("memory_embeddings: backfill started for user %d", user_id);

   int total_embedded = 0;
   int batch_size = 50;

   while (!atomic_load(&s_backfill_shutdown)) {
      int64_t ids[50];
      char texts[50][512];

      int count = 0;
      memory_db_fact_list_without_embedding(user_id, dims, ids, texts, batch_size, &count);
      if (count <= 0)
         break;

      for (int i = 0; i < count && !atomic_load(&s_backfill_shutdown); i++) {
         if (texts[i][0] == '\0')
            continue;

         if (memory_embeddings_embed_and_store(user_id, ids[i], texts[i]) == 0) {
            total_embedded++;
         }

         /* Throttle: 50ms between embeddings to avoid hogging CPU */
         usleep(50000);
      }
   }

   OLOG_INFO("memory_embeddings: backfill complete, embedded %d facts", total_embedded);

   /* v34: after embedding backfill completes, run a one-shot category-classification
    * pass for this user if it hasn't been done yet.  Centroids are only useful
    * once fact embeddings are populated — that's why this lives at the tail of
    * the same thread rather than as a separate scheduled job.  Idempotent across
    * reboots via users.categories_backfilled_at timestamp. */
   if (!atomic_load(&s_backfill_shutdown) && embedding_engine_available()) {
      int64_t flag = 0;
      if (user_categories_backfilled_at(user_id, &flag) == 0 && flag == 0) {
         OLOG_INFO("memory_embeddings: starting category backfill for user %d", user_id);
         int dims = 0;
         float *centroids = build_category_centroids(&dims);
         if (centroids && dims > 0) {
            int classified = 0;
            if (categorize_user_facts(user_id, centroids, dims, &classified) == 0) {
               user_set_categories_backfilled(user_id, (int64_t)time(NULL));
            }
            free(centroids);
         } else {
            OLOG_WARNING("memory_embeddings: category centroid build failed, skipping backfill");
         }
      }
   }

   atomic_store(&s_backfill_running, false);
   return NULL;
}

void memory_embeddings_start_backfill(int user_id) {
   if (!memory_embeddings_available())
      return;

   if (atomic_load(&s_backfill_running)) {
      OLOG_INFO("memory_embeddings: backfill already running");
      return;
   }

   s_backfill_user_id = user_id;
   atomic_store(&s_backfill_shutdown, false);
   atomic_store(&s_backfill_running, true);

   if (pthread_create(&s_backfill_thread, NULL, backfill_thread_fn, NULL) != 0) {
      OLOG_ERROR("memory_embeddings: failed to create backfill thread");
      atomic_store(&s_backfill_running, false);
   }
}
