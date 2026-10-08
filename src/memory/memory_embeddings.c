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
#include "memory/memory_embed_backfill.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_types.h"

/* In-memory embedding cache for fast cosine search.
 * created_ats added in #3 — per-fact origin timestamps used by temporal-query
 * scoring.  Loaded together with embeddings so scoring stays single-pass.
 *
 * Sized to the user's facts when loaded, and grown as facts are appended, so
 * every current fact with an embedding takes part in semantic search. */
static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   float *embeddings; /* flat: count * dims */
   float *norms;
   int64_t *created_ats;  /* per-fact created_at, parallel to ids */
   int64_t *note_doc_ids; /* per-fact note_doc_id (v61): >0 = bridge gloss */
   double *unit_sum;      /* sum of embedding / norm over the cache (dims): a
                           * query's mean cosine over the pool in one dot product */
   int count;
   int capacity;
   int dims;
   bool valid;
   atomic_bool dirty; /* set by backfill after each store */
} s_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

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
   free(s_cache.unit_sum);
   s_cache.unit_sum = NULL;
   s_cache.ids = NULL;
   s_cache.embeddings = NULL;
   s_cache.norms = NULL;
   s_cache.created_ats = NULL;
   s_cache.note_doc_ids = NULL;
   s_cache.count = 0;
   s_cache.capacity = 0;
   s_cache.valid = false;
}

/* Add @p vec / @p norm to @p sum (a zero vector adds nothing; its cosine is 0). */
static void unit_sum_add(double *sum, const float *vec, int dims, float norm) {
   if (norm < 1e-6f) {
      return;
   }
   const double inv = 1.0 / (double)norm;
   for (int d = 0; d < dims; d++) {
      sum[d] += (double)vec[d] * inv;
   }
}

/* Facts the cache holds room for beyond what a load found (appends until the
 * next reload, which applies [memory] fact_cache_mb again). */
#define CACHE_HEADROOM 64

/* Caller holds s_cache.mutex.  Allocate the cache's arrays for @p cap facts. */
static int cache_alloc(int cap, int dims) {
   s_cache.ids = malloc((size_t)cap * sizeof(int64_t));
   s_cache.embeddings = malloc((size_t)cap * (size_t)dims * sizeof(float));
   s_cache.norms = malloc((size_t)cap * sizeof(float));
   s_cache.created_ats = malloc((size_t)cap * sizeof(int64_t));
   /* calloc (not malloc): a future append path that forgets to set this field
    * fails safe — a 0 slot is treated as a non-gloss, never the reverse. */
   s_cache.note_doc_ids = calloc((size_t)cap, sizeof(int64_t));
   if (!s_cache.ids || !s_cache.embeddings || !s_cache.norms || !s_cache.created_ats ||
       !s_cache.note_doc_ids) {
      return FAILURE;
   }
   s_cache.capacity = cap;
   return SUCCESS;
}

/* Caller holds s_cache.mutex.  Grow a loaded cache to hold @p cap facts, keeping
 * its contents; on failure it is unchanged. */
static int cache_grow_locked(int cap) {
   const size_t dims = (size_t)s_cache.dims;
   int64_t *ids = realloc(s_cache.ids, (size_t)cap * sizeof(int64_t));
   if (ids) {
      s_cache.ids = ids;
   }
   float *embeddings = realloc(s_cache.embeddings, (size_t)cap * dims * sizeof(float));
   if (embeddings) {
      s_cache.embeddings = embeddings;
   }
   float *norms = realloc(s_cache.norms, (size_t)cap * sizeof(float));
   if (norms) {
      s_cache.norms = norms;
   }
   int64_t *created_ats = realloc(s_cache.created_ats, (size_t)cap * sizeof(int64_t));
   if (created_ats) {
      s_cache.created_ats = created_ats;
   }
   int64_t *note_doc_ids = realloc(s_cache.note_doc_ids, (size_t)cap * sizeof(int64_t));
   if (note_doc_ids) {
      s_cache.note_doc_ids = note_doc_ids;
      memset(note_doc_ids + s_cache.capacity, 0,
             (size_t)(cap - s_cache.capacity) * sizeof(int64_t));
   }
   /* Each array that grew is kept (it's still valid at its old size); only when
    * all did does the capacity move. */
   if (!ids || !embeddings || !norms || !created_ats || !note_doc_ids) {
      return FAILURE;
   }
   s_cache.capacity = cap;
   return SUCCESS;
}

/* Bytes one cached fact takes: its embedding plus id, norm, created_at and
 * note_doc_id. */
#define CACHE_FACT_OVERHEAD (sizeof(int64_t) * 3 + sizeof(float))

/* How many facts [memory] fact_cache_mb holds at @p dims. */
static int cache_fact_limit(int dims) {
   const int mb = g_config.memory.fact_cache_mb > 0 ? g_config.memory.fact_cache_mb
                                                    : MEMORY_FACT_CACHE_MB_DEFAULT;
   const size_t per_fact = (size_t)dims * sizeof(float) + CACHE_FACT_OVERHEAD;
   const size_t n = ((size_t)mb << 20) / per_fact;
   return n > (size_t)(INT_MAX - CACHE_HEADROOM) ? INT_MAX - CACHE_HEADROOM : (int)n;
}

typedef struct {
   int dims;
   int limit; /* facts [memory] fact_cache_mb holds at this dimension */
   int total; /* facts the user has (before the limit) */
} cache_load_ctx_t;

/* Caller holds s_cache.mutex (memory_db_fact_foreach_embedding callback).  Store
 * one loaded fact; the first row says how many are coming, which sizes the cache. */
static int cache_load_row(const memory_fact_embedding_row_t *row, int total, void *arg) {
   cache_load_ctx_t *c = (cache_load_ctx_t *)arg;
   if (!s_cache.ids) {
      c->total = total;
      const int n = total < c->limit ? total : c->limit;
      if (cache_alloc(n + CACHE_HEADROOM, c->dims) != SUCCESS) {
         return FAILURE;
      }
   }
   if (s_cache.count >= s_cache.capacity) {
      return FAILURE; /* more rows than the limit the query was given */
   }
   const size_t i = (size_t)s_cache.count;
   const size_t dims = (size_t)c->dims;
   s_cache.ids[i] = row->id;
   memcpy(s_cache.embeddings + i * dims, row->embedding, dims * sizeof(float));
   s_cache.norms[i] = row->norm;
   s_cache.created_ats[i] = row->created_at;
   s_cache.note_doc_ids[i] = row->note_doc_id;
   s_cache.count++;
   return SUCCESS;
}

static int cache_load(int user_id) {
   /* Already valid for this user? */
   if (s_cache.valid && s_cache.user_id == user_id && !atomic_load(&s_cache.dirty))
      return 0;

   cache_free_data();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return FAILURE;

   /* One pass: the user's best facts up to the ceiling, the cache sized from
    * the first row (see memory_db_fact_foreach_embedding for the order). */
   /* Cleared before reading: a fact changed while the load runs dirties it
    * again, so the next search reloads rather than keeping what was read. */
   atomic_store(&s_cache.dirty, false);
   cache_load_ctx_t c = { .dims = dims, .limit = cache_fact_limit(dims) };
   if (memory_db_fact_foreach_embedding(user_id, dims, c.limit, cache_load_row, &c) !=
           MEMORY_DB_SUCCESS ||
       (!s_cache.ids && cache_alloc(CACHE_HEADROOM, dims) != SUCCESS)) {
      cache_free_data();
      return FAILURE;
   }

   s_cache.unit_sum = calloc((size_t)dims, sizeof(double));
   if (!s_cache.unit_sum) {
      cache_free_data();
      return FAILURE;
   }
   for (int i = 0; i < s_cache.count; i++) {
      /* count only grows after norms[i] is written */
      // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
      unit_sum_add(s_cache.unit_sum, s_cache.embeddings + (size_t)i * (size_t)dims, dims,
                   s_cache.norms[i]);
   }

   s_cache.dims = dims;
   s_cache.user_id = user_id;
   s_cache.valid = true;

   OLOG_INFO("memory_embeddings: loaded %d embeddings into cache for user %d", s_cache.count,
             user_id);
   if (c.total > s_cache.count) {
      OLOG_WARNING("memory_embeddings: user %d has %d facts; semantic search holds the %d most "
                   "used, confident and recent ([memory] fact_cache_mb), %d left to keyword "
                   "search",
                   user_id, c.total, s_cache.count, c.total - s_cache.count);
   }

   return 0;
}

void memory_embeddings_invalidate_cache(void) {
   atomic_store(&s_cache.dirty, true);
}

/* Mark the cache stale only if it currently holds @p user_id.  The cache holds one
 * user; another user's write can't make its vectors stale, and dirtying it would
 * force the active user's next query to reload every embedding from SQLite.
 * Unlike memory_embeddings_invalidate_cache() this takes s_cache.mutex, which is
 * held across a full cache_load, so a caller may briefly wait behind a reload. */
void memory_embeddings_invalidate_cache_for_user(int user_id) {
   pthread_mutex_lock(&s_cache.mutex);
   if (s_cache.valid && s_cache.user_id == user_id) {
      atomic_store(&s_cache.dirty, true);
   }
   pthread_mutex_unlock(&s_cache.mutex);
}

/* =============================================================================
 * Init / Cleanup — delegate provider management to shared embedding engine
 * ============================================================================= */

int memory_embeddings_init(void) {
   return embedding_engine_init();
}

void memory_embeddings_cleanup(void) {
   memory_embed_backfill_shutdown();

   /* Free caches */
   pthread_mutex_lock(&s_cache.mutex);
   cache_free_data();
   pthread_mutex_unlock(&s_cache.mutex);

   memory_embeddings_entity_cleanup();

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

/* Embed @p text and store it on the fact.  A provider can answer "success" with an
 * empty vector, or with a vector from a different model than the engine was probed
 * with (e.g. an endpoint repointed at runtime); storing either would leave the fact
 * un-searchable while looking done, so both are failures here. */
int memory_embeddings_embed_and_store_ex(int user_id,
                                         int64_t fact_id,
                                         const char *text,
                                         bool from_backfill) {
   if (!embedding_engine_available() || !text)
      return FAILURE;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   /* A failure outside the backfill worker leaves a fact to embed later: arm a
    * retry sweep.  The worker decides for itself (only when the engine is down;
    * a fact the provider rejects would otherwise re-arm it forever). */
   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0) {
      if (!from_backfill) {
         memory_embed_backfill_schedule_retry();
      }
      return rc;
   }
   if (dims <= 0 || dims != embedding_engine_dims()) {
      OLOG_WARNING("memory_embeddings: fact %lld embedded with %d dims (engine %d); not stored",
                   (long long)fact_id, dims, embedding_engine_dims());
      if (!from_backfill) {
         memory_embed_backfill_schedule_retry();
      }
      return FAILURE;
   }

   float norm = memory_embeddings_l2_norm(embedding, dims);

   rc = memory_db_fact_update_embedding(user_id, fact_id, embedding, dims, norm);
   if (rc == MEMORY_DB_SUCCESS && !from_backfill) {
      memory_embeddings_invalidate_cache_for_user(user_id);
   }
   return rc;
}

int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   return memory_embeddings_embed_and_store_ex(user_id, fact_id, text, false);
}

/* Internal: append a pre-embedded fact to the in-memory cache without
 * invalidating it, so an N-fact extraction loop does not pay N cache
 * reloads against SQLite.  Grows the cache when it is full.  Returns 0 on
 * append, non-zero on any reason the cache cannot accept the row (out of
 * memory / user mismatch / dims mismatch / cache invalid) — callers should treat non-zero as
 * "fall back to invalidate so the next access reloads fresh." */
static int cache_append_locked(int user_id,
                               int64_t fact_id,
                               const float *vec,
                               int dims,
                               int64_t created_at,
                               float norm) {
   if (!s_cache.valid || s_cache.user_id != user_id || s_cache.dims != dims)
      return FAILURE;
   /* At the [memory] fact_cache_mb budget: the caller marks the cache dirty,
    * and the reload picks which facts to keep. */
   if (s_cache.count >= cache_fact_limit(dims))
      return FAILURE;
   if (s_cache.count >= s_cache.capacity &&
       cache_grow_locked(s_cache.capacity + s_cache.capacity / 2 + CACHE_HEADROOM) != SUCCESS)
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
   if (s_cache.unit_sum)
      unit_sum_add(s_cache.unit_sum, vec, dims, norm);
   s_cache.count++;
   return SUCCESS;
}

int memory_embeddings_store_precomputed(int user_id, int64_t fact_id, const float *vec, int dims) {
   if (!vec || dims <= 0 || dims != embedding_engine_dims())
      return FAILURE;

   float norm = memory_embeddings_l2_norm(vec, dims);

   /* Store and append under the cache lock (taken before the auth_db lock, as
    * cache_load does), so a concurrent reload either reads the new row or
    * finishes before the append: never both, which would cache it twice. */
   pthread_mutex_lock(&s_cache.mutex);
   int rc = memory_db_fact_update_embedding(user_id, fact_id, vec, dims, norm);
   if (rc != MEMORY_DB_SUCCESS) {
      pthread_mutex_unlock(&s_cache.mutex);
      return rc;
   }

   /* Try to append directly into the warm cache so a multi-fact extraction
    * loop avoids the N cache-reload cycles that invalidate-then-reload would
    * cause.  On any reason the cache cannot accept the row (at its budget,
    * out of memory, another user warm, dim mismatch, cache cold), mark it
    * dirty so the next access reloads fresh.  Either path leaves the cache in
    * a correct state. */
   if (cache_append_locked(user_id, fact_id, vec, dims, time(NULL), norm) != SUCCESS &&
       s_cache.valid && s_cache.user_id == user_id) {
      atomic_store(&s_cache.dirty, true);
   }
   pthread_mutex_unlock(&s_cache.mutex);
   return MEMORY_DB_SUCCESS;
}

/* A requested fact id and where its result goes. */
typedef struct {
   int64_t id;
   int slot;
} relevance_want_t;

static int relevance_want_cmp(const void *a, const void *b) {
   const int64_t x = ((const relevance_want_t *)a)->id;
   const int64_t y = ((const relevance_want_t *)b)->id;
   return (x > y) - (x < y);
}

int memory_embeddings_fact_relevance(int user_id,
                                     const float *query_emb,
                                     const int64_t *ids,
                                     int n,
                                     float *out_rel,
                                     int *pool_out) {
   if (pool_out) {
      *pool_out = 0;
   }
   if (!query_emb || !ids || !out_rel || n <= 0 || n > MEMORY_RELEVANCE_MAX_IDS) {
      return FAILURE;
   }
   for (int k = 0; k < n; k++) {
      out_rel[k] = MEMORY_RELEVANCE_NA;
   }
   const int dims = embedding_engine_dims();
   const float qnorm = memory_embeddings_l2_norm(query_emb, dims);
   if (dims <= 0 || qnorm < 1e-6f) {
      return FAILURE;
   }

   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0 || s_cache.count <= 0 || s_cache.dims != dims ||
       !s_cache.unit_sum) {
      pthread_mutex_unlock(&s_cache.mutex);
      return FAILURE;
   }
   /* The pool's mean cosine from the running sum of unit embeddings (one dot
    * product); a cosine only for the requested facts, found by binary search
    * over the requested ids (sorted once). */
   double dot = 0.0;
   for (int d = 0; d < dims; d++) {
      dot += (double)query_emb[d] * s_cache.unit_sum[d];
   }
   const double sum = dot / (double)qnorm;
   relevance_want_t want[MEMORY_RELEVANCE_MAX_IDS];
   for (int k = 0; k < n; k++) {
      want[k].id = ids[k];
      want[k].slot = k;
   }
   qsort(want, (size_t)n, sizeof(want[0]), relevance_want_cmp);
   float cos_of[MEMORY_RELEVANCE_MAX_IDS];
   bool found[MEMORY_RELEVANCE_MAX_IDS] = { false };
   for (int i = 0; i < s_cache.count; i++) {
      const relevance_want_t key = { .id = s_cache.ids[i] };
      const relevance_want_t *hit = bsearch(&key, want, (size_t)n, sizeof(want[0]),
                                            relevance_want_cmp);
      if (!hit) {
         continue;
      }
      const float cos = memory_embeddings_cosine_with_norms(
          query_emb, s_cache.embeddings + (size_t)i * (size_t)dims, dims, qnorm, s_cache.norms[i]);
      /* A repeated requested id sits next to its twins: fill them all. */
      int lo = (int)(hit - want);
      while (lo > 0 && want[lo - 1].id == key.id) {
         lo--;
      }
      for (int k = lo; k < n && want[k].id == key.id; k++) {
         cos_of[want[k].slot] = cos;
         found[want[k].slot] = true;
      }
   }
   for (int k = 0; k < n; k++) {
      if (found[k]) {
         out_rel[k] = embedding_corpus_relevance(cos_of[k], sum, s_cache.count);
      }
   }
   if (pool_out) {
      *pool_out = s_cache.count;
   }
   pthread_mutex_unlock(&s_cache.mutex);
   return SUCCESS;
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
 * Summary embeddings and bulk invalidation (entity embeddings: see
 * memory_embeddings_entity.c)
 * ============================================================================= */

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

/* hybrid_search_ex keeps at most this many results (callers ask for <= 10) and
 * considers at most this many keyword candidates (callers pass <= 10). */
#define HYBRID_TOP_CAP 64
#define HYBRID_KW_CAP 256

/* Insert (id, score) into top[0..n) kept sorted by score descending, capped at
 * @p keep entries.  Returns the new count. */
static int hybrid_top_insert(embedding_search_result_t *top,
                             int n,
                             int keep,
                             int64_t id,
                             float score) {
   if (keep <= 0) {
      return n;
   }
   if (n == keep) {
      if (score <= top[n - 1].score) {
         return n;
      }
      n--; /* drop the current lowest */
   }
   int j = n - 1;
   while (j >= 0 && top[j].score < score) {
      top[j + 1] = top[j];
      j--;
   }
   top[j + 1].fact_id = id;
   top[j + 1].score = score;
   return n + 1;
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

   /* Score EVERY cached fact and keep only the best `keep` in a small sorted
    * buffer.  (Collecting into a fixed scratch array and sorting it afterwards
    * stopped scanning once the array filled, so for a user with more cached
    * facts than the array held, the lowest-confidence ones were never searched
    * semantically at all.) */
   int keep = max_results > HYBRID_TOP_CAP ? HYBRID_TOP_CAP : max_results;
   embedding_search_result_t top[HYBRID_TOP_CAP];
   int top_n = 0;
   bool kw_in_cache[HYBRID_KW_CAP] = { false };
   int kw_n = keyword_count > HYBRID_KW_CAP ? HYBRID_KW_CAP : keyword_count;

   for (int i = 0; i < s_cache.count; i++) {
      float cosine = memory_embeddings_cosine_with_norms(
          query_emb, s_cache.embeddings + (size_t)i * (size_t)dims, dims, query_norm,
          s_cache.norms[i]);

      /* Find keyword score for this fact (if any) */
      float kw_score = 0.0f;
      for (int k = 0; k < kw_n; k++) {
         if (keyword_facts[k] == s_cache.ids[i]) {
            kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
            kw_in_cache[k] = true;
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
         top_n = hybrid_top_insert(top, top_n, keep, s_cache.ids[i], hybrid);
      }
   }

   /* Keyword hits with NO embedding (just created, awaiting backfill, or one the
    * engine can't embed).  Their vector similarity is unknown, not zero: scoring
    * it as 0 capped them at kw_weight, below the default search_score_floor, so
    * they could never be found; scoring the keyword match alone let them outrank
    * facts that match on both keywords and meaning.  Take the unknown similarity
    * as the floor itself, so a strong keyword match clears it and ranks below an
    * equally strong match with real semantic support. */
   const float unknown_cosine = g_config.memory.search_score_floor;
   for (int k = 0; k < kw_n; k++) {
      if (!kw_in_cache[k]) {
         float kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
         top_n = hybrid_top_insert(top, top_n, keep, keyword_facts[k],
                                   kw_weight * kw_score + vec_weight * unknown_cosine);
      }
   }

   pthread_mutex_unlock(&s_cache.mutex);

   memcpy(out_results, top, (size_t)top_n * sizeof(embedding_search_result_t));
   return top_n;
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

/* Candidate pool bound: the two cache channels' top lists plus every keyword
 * hit.  Sizes the per-call scratch arrays (~20 KB of stack in all). */
#define RRF_POOL_CAP (2 * RRF_PER_CHANNEL_CAP + HYBRID_KW_CAP)

/* Per-candidate scratch entry (24 B). */
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

   /* Candidate pool.  RRF only credits each channel's top RRF_PER_CHANNEL_CAP
    * ranks, so a full scan keeps the top-N by cosine and by temporal proximity
    * (every cached fact is considered — scanning into a fixed pool and
    * stopping when it filled left the rest of a large cache unsearched), and
    * the pool is their union plus every keyword candidate. */
   rrf_cand_t pool[RRF_POOL_CAP];
   int pool_count = 0;

   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      return hybrid_fallback_keyword_only(keyword_facts, keyword_scores, keyword_count, token_count,
                                          out_results, max_results);
   }

   /* top_* hold cache indices (as fact_id) ranked by that channel's value. */
   embedding_search_result_t top_sem[RRF_PER_CHANNEL_CAP];
   embedding_search_result_t top_tmp[RRF_PER_CHANNEL_CAP];
   int n_sem = 0, n_tmp = 0;
   int kw_n = keyword_count > HYBRID_KW_CAP ? HYBRID_KW_CAP : keyword_count;
   int kw_cache_idx[HYBRID_KW_CAP];
   for (int k = 0; k < kw_n; k++)
      kw_cache_idx[k] = -1;

   for (int i = 0; i < s_cache.count; i++) {
      float cosine = memory_embeddings_cosine_with_norms(
          query_emb, s_cache.embeddings + (size_t)i * (size_t)dims, dims, query_norm,
          s_cache.norms[i]);
      if (cosine > 0.01f)
         n_sem = hybrid_top_insert(top_sem, n_sem, RRF_PER_CHANNEL_CAP, i, cosine);
      if (has_temporal && s_cache.created_ats[i] > 0) {
         float tprox = time_query_proximity(&tq, s_cache.created_ats[i]);
         if (tprox > 0.0f)
            n_tmp = hybrid_top_insert(top_tmp, n_tmp, RRF_PER_CHANNEL_CAP, i, tprox);
      }
      for (int k = 0; k < kw_n; k++) {
         if (keyword_facts[k] == s_cache.ids[i]) {
            kw_cache_idx[k] = i;
            break;
         }
      }
   }

   /* Assemble the pool from cache indices (deduplicated) + keyword candidates. */
   int picked[2 * RRF_PER_CHANNEL_CAP + HYBRID_KW_CAP];
   int n_picked = 0;
   for (int t = 0; t < n_sem; t++)
      picked[n_picked++] = (int)top_sem[t].fact_id;
   for (int t = 0; t < n_tmp; t++)
      picked[n_picked++] = (int)top_tmp[t].fact_id;
   for (int k = 0; k < kw_n; k++)
      if (kw_cache_idx[k] >= 0)
         picked[n_picked++] = kw_cache_idx[k];

   for (int x = 0; x < n_picked && pool_count < RRF_POOL_CAP; x++) {
      int i = picked[x];
      bool dup = false;
      for (int p = 0; p < pool_count; p++) {
         if (pool[p].fact_id == s_cache.ids[i]) {
            dup = true;
            break;
         }
      }
      if (dup)
         continue;
      float kw_score = 0.0f;
      for (int k = 0; k < kw_n; k++) {
         if (kw_cache_idx[k] == i) {
            kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
            break;
         }
      }
      pool[pool_count].fact_id = s_cache.ids[i];
      pool[pool_count].cosine = memory_embeddings_cosine_with_norms(
          query_emb, s_cache.embeddings + (size_t)i * (size_t)dims, dims, query_norm,
          s_cache.norms[i]);
      pool[pool_count].kw_score = kw_score;
      pool[pool_count].tprox = (has_temporal && s_cache.created_ats[i] > 0)
                                   ? time_query_proximity(&tq, s_cache.created_ats[i])
                                   : 0.0f;
      pool_count++;
   }

   /* Keyword-only facts (no embedding) — keyword channel only. */
   for (int k = 0; k < kw_n && pool_count < RRF_POOL_CAP; k++) {
      if (kw_cache_idx[k] >= 0)
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
   int idx_scratch[RRF_POOL_CAP];
   int rank_sem[RRF_POOL_CAP];
   int rank_kw[RRF_POOL_CAP];
   int rank_tmp[RRF_POOL_CAP];
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
   embedding_search_result_t scored[RRF_POOL_CAP];
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

   /* Sort by RRF score desc (pool_count <= RRF_POOL_CAP). */
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
