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
 * Semantic ranking over a user's accessible document chunks, from an in-memory
 * copy of their embeddings.
 *
 * Per-turn context injection and the document_search tool both need every
 * accessible chunk scored.  Reading the embeddings from SQLite each time holds
 * the global database lock for the scan, and each row's text sits in front of
 * its embedding, so the scan walks the text too.  A few users' copies are kept
 * (least recently used goes first), keyed on (user, dims, chunk generation); the
 * generation is a pair of counters (the user's own documents, the shared ones)
 * the database's own triggers bump on any change to chunk visibility (see the
 * v89 migration), so a stale copy is detected with one small read, no writer
 * has to remember to invalidate it, and one user's upload leaves the others'
 * copies alone.
 *
 * Copies are built from pages (each its own short database-lock hold) outside
 * the cache lock, scored while they are read, then installed if nothing changed
 * meanwhile; a corpus past the size one user may keep is scored page by page
 * without keeping it.  Scoring runs under s_cache.mutex only.
 *
 * LOCKING: s_cache.mutex is never held across a database call.
 */

#include "tools/document_embed_cache.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"

/* All copies together stay under this. */
#define DOC_EMBED_CACHE_MAX_BYTES (64u * 1024u * 1024u)
/* One user's copy stays under this, so one large corpus can't evict every
 * other user's on each turn; a corpus past it is scored straight from the
 * database. */
#define DOC_EMBED_SLOT_MAX_BYTES (DOC_EMBED_CACHE_MAX_BYTES / 2u)
/* Users whose copies are kept at once. */
#define DOC_EMBED_CACHE_SLOTS 4
/* Chunks read per database page. */
#define DOC_EMBED_PAGE 256

typedef struct {
   int user_id; /* 0 = empty slot */
   int dims;
   document_chunk_gen_t gen;
   int n;
   int64_t *ids;
   float *norms;
   float *embs; /* n * dims */
   size_t bytes;
   uint64_t last_used;
} cache_slot_t;

static struct {
   pthread_mutex_t mutex;
   cache_slot_t slot[DOC_EMBED_CACHE_SLOTS];
   size_t total_bytes;
   uint64_t clock;
} s_cache = { .mutex = PTHREAD_MUTEX_INITIALIZER };

/* A requested chunk id and where its cosine goes (sorted by id for lookup). */
typedef struct {
   int64_t id;
   int slot;
} want_t;

/* Top-K accumulator shared by the cached and streaming paths. */
typedef struct {
   const float *query;
   float query_norm;
   int dims;
   int keep;
   document_chunk_score_t *top; /* sorted, best first */
   int n;
   document_rank_stats_t stats;
   const want_t *want; /* sorted by id; NULL when none requested */
   int n_want;
   float *want_cos;
} rank_acc_t;

static void acc_record_wanted(rank_acc_t *acc, int64_t chunk_id, float cos) {
   if (chunk_id < acc->want[0].id || chunk_id > acc->want[acc->n_want - 1].id) {
      return; /* most chunks: outside the requested range */
   }
   int lo = 0;
   int hi = acc->n_want - 1;
   while (lo <= hi) {
      const int mid = lo + (hi - lo) / 2;
      if (acc->want[mid].id == chunk_id) {
         /* Duplicates in the request sit next to each other after sorting. */
         for (int i = mid; i >= 0 && acc->want[i].id == chunk_id; i--) {
            acc->want_cos[acc->want[i].slot] = cos;
         }
         for (int i = mid + 1; i < acc->n_want && acc->want[i].id == chunk_id; i++) {
            acc->want_cos[acc->want[i].slot] = cos;
         }
         return;
      }
      if (acc->want[mid].id < chunk_id) {
         lo = mid + 1;
      } else {
         hi = mid - 1;
      }
   }
}

static void acc_add(rank_acc_t *acc, int64_t chunk_id, const float *emb, float norm) {
   const float cos = embedding_engine_cosine_with_norms(acc->query, emb, acc->dims, acc->query_norm,
                                                        norm);
   acc->stats.pool++;
   acc->stats.cosine_sum += cos;
   if (acc->n_want > 0) {
      acc_record_wanted(acc, chunk_id, cos);
   }
   if (acc->n == acc->keep) {
      if (cos <= acc->top[acc->n - 1].cosine) {
         return;
      }
      acc->n--; /* drop the current lowest */
   }
   int j = acc->n - 1;
   while (j >= 0 && acc->top[j].cosine < cos) {
      acc->top[j + 1] = acc->top[j];
      j--;
   }
   acc->top[j + 1].chunk_id = chunk_id;
   acc->top[j + 1].cosine = cos;
   acc->n++;
}

static void slot_free(cache_slot_t *slot) {
   free(slot->ids);
   free(slot->norms);
   free(slot->embs);
   memset(slot, 0, sizeof(*slot));
}

/* Caller holds s_cache.mutex. */
static void slot_evict_locked(cache_slot_t *slot) {
   s_cache.total_bytes -= slot->bytes;
   slot_free(slot);
}

static bool gen_equal(document_chunk_gen_t a, document_chunk_gen_t b) {
   return a.own == b.own && a.shared == b.shared;
}

/* Caller holds s_cache.mutex.  The current copy for (user, dims, gen), or NULL.
 * Stale copies go: this user's of another generation, and every user's built
 * before the shared documents last changed (everyone sees those). */
static cache_slot_t *slot_find_locked(int user_id, int dims, document_chunk_gen_t gen) {
   cache_slot_t *found = NULL;
   for (int i = 0; i < DOC_EMBED_CACHE_SLOTS; i++) {
      cache_slot_t *slot = &s_cache.slot[i];
      if (slot->user_id == 0) {
         continue;
      }
      const bool mine = slot->user_id == user_id;
      if (slot->gen.shared != gen.shared || (mine && slot->gen.own != gen.own)) {
         slot_evict_locked(slot);
      } else if (mine && slot->dims == dims) {
         slot->last_used = ++s_cache.clock;
         found = slot;
      }
   }
   return found;
}

/* Caller holds s_cache.mutex.  The largest copy one user may keep: the whole
 * cache when no other user's copy is held, else half of it, so one large
 * corpus can't evict everyone else's copy on every turn. */
static size_t slot_max_bytes_locked(int user_id) {
   for (int i = 0; i < DOC_EMBED_CACHE_SLOTS; i++) {
      if (s_cache.slot[i].user_id != 0 && s_cache.slot[i].user_id != user_id) {
         return DOC_EMBED_SLOT_MAX_BYTES;
      }
   }
   return DOC_EMBED_CACHE_MAX_BYTES;
}

/* Page through the user's chunks; each page is passed to @p visit (outside any
 * cache lock).  SUCCESS or FAILURE. */
typedef bool (
    *page_visit_fn)(const int64_t *ids, const float *norms, const float *embs, int n, void *ctx);

static int for_each_page(int user_id, int dims, page_visit_fn visit, void *ctx) {
   int64_t *ids = malloc(DOC_EMBED_PAGE * sizeof(*ids));
   float *norms = malloc(DOC_EMBED_PAGE * sizeof(*norms));
   float *embs = malloc((size_t)DOC_EMBED_PAGE * (size_t)dims * sizeof(*embs));
   int rc = (ids && norms && embs) ? SUCCESS : FAILURE;
   document_chunk_cursor_t cursor = { 0 };
   while (rc == SUCCESS && !cursor.done) {
      int n = 0;
      if (document_db_chunk_embeddings_page(user_id, dims, &cursor, DOC_EMBED_PAGE, ids, norms,
                                            embs, &n) != SUCCESS) {
         rc = FAILURE;
         break;
      }
      if (n > 0 && !visit(ids, norms, embs, n, ctx)) {
         rc = FAILURE;
         break;
      }
   }
   free(ids);
   free(norms);
   free(embs);
   return rc;
}

static bool score_page(const int64_t *ids,
                       const float *norms,
                       const float *embs,
                       int n,
                       void *ctx) {
   rank_acc_t *acc = (rank_acc_t *)ctx;
   for (int i = 0; i < n; i++) {
      acc_add(acc, ids[i], embs + (size_t)i * (size_t)acc->dims, norms[i]);
   }
   return true;
}

static size_t row_bytes(int dims) {
   return (size_t)dims * sizeof(float) + sizeof(int64_t) + sizeof(float);
}

/* A copy under construction (not yet in the cache), scored as it is read so a
 * corpus that turns out too large for the cache costs one walk, not two. */
typedef struct {
   cache_slot_t slot;
   int cap;
   bool overflow;    /* past max_bytes: scoring only */
   size_t max_bytes; /* the largest copy this user may keep */
   rank_acc_t *acc;
} build_t;

static void build_drop(build_t *b) {
   free(b->slot.ids);
   free(b->slot.norms);
   free(b->slot.embs);
   b->slot.ids = NULL;
   b->slot.norms = NULL;
   b->slot.embs = NULL;
   b->slot.n = 0;
   b->cap = 0;
}

static bool build_page(const int64_t *ids,
                       const float *norms,
                       const float *embs,
                       int n,
                       void *ctx) {
   build_t *b = (build_t *)ctx;
   (void)score_page(ids, norms, embs, n, b->acc);
   if (b->overflow) {
      return true;
   }
   const int dims = b->slot.dims;
   if ((size_t)(b->slot.n + n) * row_bytes(dims) > b->max_bytes) {
      b->overflow = true; /* chunks were added since the count */
      build_drop(b);
      return true;
   }
   if (b->slot.n + n > b->cap) {
      /* Chunks were added since the count: grow by what is needed plus a page. */
      int ncap = b->slot.n + n + DOC_EMBED_PAGE;
      int64_t *nids = realloc(b->slot.ids, (size_t)ncap * sizeof(*nids));
      if (nids) {
         b->slot.ids = nids;
      }
      float *nnorms = realloc(b->slot.norms, (size_t)ncap * sizeof(*nnorms));
      if (nnorms) {
         b->slot.norms = nnorms;
      }
      float *nembs = realloc(b->slot.embs, (size_t)ncap * (size_t)dims * sizeof(*nembs));
      if (nembs) {
         b->slot.embs = nembs;
      }
      if (!nids || !nnorms || !nembs) {
         b->overflow = true; /* keep scoring; just don't cache */
         build_drop(b);
         return true;
      }
      b->cap = ncap;
   }
   memcpy(b->slot.ids + b->slot.n, ids, (size_t)n * sizeof(*ids));
   memcpy(b->slot.norms + b->slot.n, norms, (size_t)n * sizeof(*norms));
   memcpy(b->slot.embs + (size_t)b->slot.n * (size_t)dims, embs,
          (size_t)n * (size_t)dims * sizeof(*embs));
   b->slot.n += n;
   return true;
}

/* Walk the user's chunks once, scoring each into @p acc and building a copy of
 * them.  Returns the copy (exactly sized; NULL when it would pass the per-user
 * cap or on allocation failure); FAILURE only when the walk itself failed. */
static int build_and_score(int user_id,
                           int dims,
                           document_chunk_gen_t gen,
                           int expected,
                           size_t max_bytes,
                           rank_acc_t *acc,
                           cache_slot_t **built_out) {
   *built_out = NULL;
   build_t b = { .slot = { .user_id = user_id, .dims = dims, .gen = gen },
                 .cap = expected > 0 ? expected : 1,
                 .max_bytes = max_bytes,
                 .acc = acc };
   b.slot.ids = malloc((size_t)b.cap * sizeof(*b.slot.ids));
   b.slot.norms = malloc((size_t)b.cap * sizeof(*b.slot.norms));
   b.slot.embs = malloc((size_t)b.cap * (size_t)dims * sizeof(*b.slot.embs));
   if (!b.slot.ids || !b.slot.norms || !b.slot.embs) {
      build_drop(&b);
      b.overflow = true;
   }
   if (for_each_page(user_id, dims, build_page, &b) != SUCCESS) {
      build_drop(&b);
      return FAILURE;
   }
   if (b.overflow || b.slot.n == 0) {
      build_drop(&b);
      return SUCCESS;
   }
   if (b.cap > b.slot.n) {
      /* Exact size, so the cache accounts what it holds. */
      int64_t *ids = realloc(b.slot.ids, (size_t)b.slot.n * sizeof(*ids));
      float *norms = realloc(b.slot.norms, (size_t)b.slot.n * sizeof(*norms));
      float *embs = realloc(b.slot.embs, (size_t)b.slot.n * (size_t)dims * sizeof(*embs));
      b.slot.ids = ids ? ids : b.slot.ids;
      b.slot.norms = norms ? norms : b.slot.norms;
      b.slot.embs = embs ? embs : b.slot.embs;
   }
   b.slot.bytes = (size_t)b.slot.n * row_bytes(dims);
   cache_slot_t *out = malloc(sizeof(*out));
   if (out) {
      *out = b.slot;
   } else {
      build_drop(&b);
   }
   *built_out = out;
   return SUCCESS;
}

/* Caller holds s_cache.mutex.  Install @p built (ownership taken), evicting the
 * least recently used copies to stay under the cap.  A copy of the same user
 * already there of the same or a newer generation is kept instead. */
static void install_locked(cache_slot_t *built) {
   for (int i = 0; i < DOC_EMBED_CACHE_SLOTS; i++) {
      cache_slot_t *slot = &s_cache.slot[i];
      if (slot->user_id == built->user_id && slot->dims == built->dims) {
         if (slot->gen.own >= built->gen.own && slot->gen.shared >= built->gen.shared) {
            /* A concurrent build installed the same or a newer copy. */
            slot_free(built);
            free(built);
            return;
         }
         slot_evict_locked(slot);
      }
   }
   for (;;) {
      int free_idx = -1;
      int lru = -1;
      for (int i = 0; i < DOC_EMBED_CACHE_SLOTS; i++) {
         if (s_cache.slot[i].user_id == 0) {
            if (free_idx < 0) {
               free_idx = i;
            }
         } else if (lru < 0 || s_cache.slot[i].last_used < s_cache.slot[lru].last_used) {
            lru = i;
         }
      }
      if (free_idx >= 0 && s_cache.total_bytes + built->bytes <= DOC_EMBED_CACHE_MAX_BYTES) {
         cache_slot_t *slot = &s_cache.slot[free_idx];
         *slot = *built;
         free(built);
         slot->last_used = ++s_cache.clock;
         s_cache.total_bytes += slot->bytes;
         return;
      }
      if (lru < 0) {
         /* Nothing to evict yet no room: can't happen (built <= cap). */
         slot_free(built);
         free(built);
         return;
      }
      slot_evict_locked(&s_cache.slot[lru]);
   }
}

static int want_cmp(const void *a, const void *b) {
   const int64_t x = ((const want_t *)a)->id;
   const int64_t y = ((const want_t *)b)->id;
   return (x > y) - (x < y);
}

int document_embed_rank(int user_id,
                        const float *query,
                        int dims,
                        int keep,
                        document_chunk_score_t *top,
                        int *n_out,
                        document_rank_stats_t *stats) {
   return document_embed_rank_with(user_id, query, dims, keep, top, n_out, stats, NULL, 0, NULL);
}

int document_embed_rank_with(int user_id,
                             const float *query,
                             int dims,
                             int keep,
                             document_chunk_score_t *top,
                             int *n_out,
                             document_rank_stats_t *stats,
                             const int64_t *want_ids,
                             int n_want,
                             float *want_cos) {
   if (n_out) {
      *n_out = 0;
   }
   if (stats) {
      memset(stats, 0, sizeof(*stats));
   }
   if (!query || dims <= 0 || keep <= 0 || keep > DOCUMENT_RANK_MAX || !top || !n_out ||
       n_want < 0 || n_want > DOCUMENT_RANK_WANT_MAX || (n_want > 0 && (!want_ids || !want_cos))) {
      return FAILURE;
   }
   want_t want[DOCUMENT_RANK_WANT_MAX];
   for (int i = 0; i < n_want; i++) {
      want[i].id = want_ids[i];
      want[i].slot = i;
      want_cos[i] = NAN;
   }
   if (n_want > 1) {
      qsort(want, (size_t)n_want, sizeof(want[0]), want_cmp);
   }
   rank_acc_t acc = { .query = query,
                      .query_norm = embedding_engine_l2_norm(query, dims),
                      .dims = dims,
                      .keep = keep,
                      .top = top,
                      .want = n_want > 0 ? want : NULL,
                      .n_want = n_want,
                      .want_cos = want_cos };

   document_chunk_gen_t gen = { 0 };
   const bool have_gen = document_db_chunk_generation(user_id, &gen) == SUCCESS;
   bool scored = false;
   size_t max_bytes = DOC_EMBED_SLOT_MAX_BYTES;
   if (have_gen) {
      pthread_mutex_lock(&s_cache.mutex);
      cache_slot_t *slot = slot_find_locked(user_id, dims, gen);
      if (slot) {
         score_page(slot->ids, slot->norms, slot->embs, slot->n, &acc);
         scored = true;
      }
      max_bytes = slot_max_bytes_locked(user_id);
      pthread_mutex_unlock(&s_cache.mutex);
   }

   if (!scored) {
      int expected = 0;
      const bool fits = have_gen && document_db_chunk_count(user_id, &expected) == SUCCESS &&
                        (size_t)expected * row_bytes(dims) <= max_bytes;
      if (fits) {
         cache_slot_t *built = NULL;
         if (build_and_score(user_id, dims, gen, expected, max_bytes, &acc, &built) != SUCCESS) {
            return FAILURE;
         }
         /* Keep it only if nothing changed while it was read: a copy of a
          * generation already gone would just be evicted by the next lookup
          * (after pushing a valid one out). */
         document_chunk_gen_t now = { 0 };
         if (built &&
             (document_db_chunk_generation(user_id, &now) != SUCCESS || !gen_equal(now, gen))) {
            slot_free(built);
            free(built);
            built = NULL;
         }
         if (built) {
            pthread_mutex_lock(&s_cache.mutex);
            install_locked(built);
            pthread_mutex_unlock(&s_cache.mutex);
         }
      } else {
         if (have_gen) {
            OLOG_DEBUG("document_embed_cache: user %d's %d chunks exceed the %zu MB this user "
                       "may cache; ranking page by page",
                       user_id, expected, max_bytes / (1024u * 1024u));
         }
         if (for_each_page(user_id, dims, score_page, &acc) != SUCCESS) {
            return FAILURE;
         }
      }
   }
   *n_out = acc.n;
   if (stats) {
      *stats = acc.stats;
   }
   return SUCCESS;
}

void document_embed_cache_shutdown(void) {
   pthread_mutex_lock(&s_cache.mutex);
   for (int i = 0; i < DOC_EMBED_CACHE_SLOTS; i++) {
      if (s_cache.slot[i].user_id != 0) {
         slot_evict_locked(&s_cache.slot[i]);
      }
   }
   pthread_mutex_unlock(&s_cache.mutex);
}
