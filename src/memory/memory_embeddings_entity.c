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
 * Entity embeddings: the in-memory copy every entity lookup ranks against.
 *
 * Each user's canonical entities (all of them, up to a memory budget, not a
 * most-mentioned subset) are held with their embeddings, a running sum of
 * their unit vectors (so a query's mean cosine over the pool is one dot
 * product) and their names' stemmed content words (so "is this entity named in
 * the query" costs no stemming per turn).  A few users' copies are kept, least
 * recently used going first, so a household taking turns doesn't reload on
 * every turn.
 *
 * Staleness: a change to a user's entities bumps that user's generation, a
 * change to everything bumps the global one.  Both are atomics, so writers
 * invalidate without a lock (they often hold the database lock, which this
 * cache's lock must never be taken under).  A copy records the generations
 * read BEFORE it loads, so a change that lands during a load leaves it stale.
 *
 * LOCKING: s_ent.mutex guards the slots and is held only to look up, score
 * and install; a copy is read from the database and indexed with no cache
 * lock held.  Callers get copied-out results, never pointers into the cache.
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_terms.h"
#include "utils/string_utils.h"

/* Users whose entity copies are kept at once. */
#define ENTITY_CACHE_SLOTS 4
/* Memory one user's copy may use.  At 384 dimensions an entity takes about
 * 1.7 KB, so this holds about 9,500; at 1,536 about 2,500.  Past it the most
 * mentioned are kept, and it's logged once. */
#define ENTITY_CACHE_SLOT_BYTES (16u * 1024u * 1024u)
/* Buckets of per-user generations (users sharing a bucket just reload a
 * little more often). */
#define ENTITY_GEN_BUCKETS 64

typedef struct {
   int user_id;
   bool used;
   int dims;
   uint64_t gen_all;
   uint64_t gen_user;
   uint64_t last_used;
   int count;
   int64_t *ids;
   char (*names)[MEMORY_ENTITY_NAME_MAX];
   char (*types)[MEMORY_ENTITY_TYPE_MAX];
   char (*name_stems)[MEMORY_ENTITY_NAME_MAX]; /* the name's content words */
   uint8_t *name_terms;                        /* distinct content words (0: never named) */
   uint8_t *name_words;                        /* words in the name, function words included */
   float *embeddings;                          /* count * dims */
   float *norms;
   double *unit_sum; /* dims */
} entity_slot_t;

static struct {
   pthread_mutex_t mutex;
   entity_slot_t slot[ENTITY_CACHE_SLOTS];
   uint64_t clock;
} s_ent = { .mutex = PTHREAD_MUTEX_INITIALIZER };

static atomic_uint_fast64_t s_gen_all = 1;
static atomic_uint_fast64_t s_gen_user[ENTITY_GEN_BUCKETS];
static atomic_bool s_budget_warned = false;
static atomic_bool s_empty_warned = false;

static atomic_uint_fast64_t *user_gen(int user_id) {
   return &s_gen_user[(unsigned)user_id % ENTITY_GEN_BUCKETS];
}

static size_t row_bytes(int dims) {
   return (size_t)dims * sizeof(float) + sizeof(int64_t) + 2 * MEMORY_ENTITY_NAME_MAX +
          MEMORY_ENTITY_TYPE_MAX + 2 * sizeof(uint8_t) + sizeof(float);
}

static void slot_free(entity_slot_t *s) {
   free(s->ids);
   free(s->names);
   free(s->types);
   free(s->name_stems);
   free(s->name_terms);
   free(s->name_words);
   free(s->embeddings);
   free(s->norms);
   free(s->unit_sum);
   memset(s, 0, sizeof(*s));
}

static int slot_alloc(entity_slot_t *s, int cap, int dims) {
   const size_t n = cap > 0 ? (size_t)cap : 1;
   s->ids = malloc(n * sizeof(*s->ids));
   s->names = malloc(n * sizeof(*s->names));
   s->types = malloc(n * sizeof(*s->types));
   s->name_stems = malloc(n * sizeof(*s->name_stems));
   s->name_terms = malloc(n * sizeof(*s->name_terms));
   s->name_words = malloc(n * sizeof(*s->name_words));
   s->embeddings = malloc(n * (size_t)dims * sizeof(*s->embeddings));
   s->norms = malloc(n * sizeof(*s->norms));
   s->unit_sum = calloc((size_t)dims, sizeof(*s->unit_sum));
   return (s->ids && s->names && s->types && s->name_stems && s->name_terms && s->name_words &&
           s->embeddings && s->norms && s->unit_sum)
              ? SUCCESS
              : FAILURE;
}

/* Read the user's canonical entities into @p s (not in the cache yet).
 * Aliases are left out so duplicate surface forms don't compete. */
static int slot_read(entity_slot_t *s, int user_id, int dims) {
   int total = 0;
   if (memory_db_entity_embedding_count(user_id, dims, &total) != MEMORY_DB_SUCCESS) {
      return FAILURE;
   }
   const int budget = (int)(ENTITY_CACHE_SLOT_BYTES / row_bytes(dims));
   int cap = total;
   if (cap > budget) {
      cap = budget;
      if (!atomic_exchange(&s_budget_warned, true)) {
         OLOG_WARNING("memory_embeddings: user %d has %d entities; lookups rank the %d most "
                      "mentioned (cache budget %u MB)",
                      user_id, total, budget, ENTITY_CACHE_SLOT_BYTES / (1024u * 1024u));
      }
   }
   if (slot_alloc(s, cap, dims) != SUCCESS) {
      return FAILURE;
   }
   if (cap == 0) {
      return SUCCESS;
   }
   int loaded = 0;
   if (memory_db_entity_get_embeddings(user_id, false, dims, s->ids, s->names, s->types,
                                       s->embeddings, s->norms, cap,
                                       &loaded) != MEMORY_DB_SUCCESS) {
      return FAILURE;
   }
   s->count = loaded;
   return SUCCESS;
}

/* Name stems and the unit-vector sum, once per load. */
static void slot_index(entity_slot_t *s, int dims) {
   for (int i = 0; i < s->count; i++) {
      /* The name's content words (matched against the query) and its full
       * word count (name_match needs both). */
      const int c = memory_terms_stem_line(s->names[i], true, s->name_stems[i],
                                           sizeof(s->name_stems[i]));
      const int w = memory_terms_word_count(s->names[i]);
      s->name_terms[i] = (uint8_t)(c > UINT8_MAX ? UINT8_MAX : c);
      s->name_words[i] = (uint8_t)(w > UINT8_MAX ? UINT8_MAX : w);
      if (s->norms[i] > 1e-9f) {
         const float *e = s->embeddings + (size_t)i * (size_t)dims;
         for (int d = 0; d < dims; d++) {
            s->unit_sum[d] += (double)e[d] / (double)s->norms[i];
         }
      }
   }
}

/* Caller holds s_ent.mutex.  The user's current copy, or NULL. */
static entity_slot_t *slot_find_locked(int user_id, int dims, uint64_t gen_all, uint64_t gen_user) {
   for (int i = 0; i < ENTITY_CACHE_SLOTS; i++) {
      entity_slot_t *s = &s_ent.slot[i];
      if (s->used && s->user_id == user_id && s->dims == dims && s->gen_all == gen_all &&
          s->gen_user == gen_user) {
         s->last_used = ++s_ent.clock;
         return s;
      }
   }
   return NULL;
}

/* Caller holds s_ent.mutex.  Install @p built (ownership taken) in place of
 * this user's old copy, an empty slot, or the least recently used one. */
static entity_slot_t *slot_install_locked(entity_slot_t *built) {
   entity_slot_t *pick = NULL;
   for (int i = 0; i < ENTITY_CACHE_SLOTS && !pick; i++) {
      if (s_ent.slot[i].used && s_ent.slot[i].user_id == built->user_id) {
         pick = &s_ent.slot[i]; /* this user's stale copy */
      }
   }
   for (int i = 0; i < ENTITY_CACHE_SLOTS && !pick; i++) {
      if (!s_ent.slot[i].used) {
         pick = &s_ent.slot[i];
      }
   }
   if (!pick) {
      pick = &s_ent.slot[0];
      for (int i = 1; i < ENTITY_CACHE_SLOTS; i++) {
         if (s_ent.slot[i].last_used < pick->last_used) {
            pick = &s_ent.slot[i];
         }
      }
   }
   slot_free(pick);
   *pick = *built;
   pick->last_used = ++s_ent.clock;
   return pick;
}

/* The user's current copy with s_ent.mutex HELD (callers unlock), loading it
 * if needed; NULL on failure, still holding the mutex. */
static entity_slot_t *slot_acquire(int user_id) {
   const int dims = embedding_engine_dims();
   pthread_mutex_lock(&s_ent.mutex);
   if (dims <= 0) {
      return NULL;
   }
   uint64_t gen_all = atomic_load(&s_gen_all);
   uint64_t gen_user = atomic_load(user_gen(user_id));
   entity_slot_t *s = slot_find_locked(user_id, dims, gen_all, gen_user);
   if (s) {
      return s;
   }
   pthread_mutex_unlock(&s_ent.mutex);

   /* Read and index with no cache lock held; the generations read first make
    * a change during the load leave this copy stale. */
   entity_slot_t built = { .user_id = user_id, .used = true, .dims = dims };
   built.gen_all = atomic_load(&s_gen_all);
   built.gen_user = atomic_load(user_gen(user_id));
   const int rc = slot_read(&built, user_id, dims);
   if (rc == SUCCESS) {
      slot_index(&built, dims);
      if (built.count == 0 && !atomic_exchange(&s_empty_warned, true)) {
         OLOG_WARNING("memory_embeddings: no entity embeddings at %d dimensions for user %d "
                      "(new user, or a model change the recompute worker hasn't caught up with)",
                      dims, user_id);
      }
   }

   pthread_mutex_lock(&s_ent.mutex);
   gen_all = atomic_load(&s_gen_all);
   gen_user = atomic_load(user_gen(user_id));
   s = slot_find_locked(user_id, dims, gen_all, gen_user); /* another thread's load */
   if (s || rc != SUCCESS) {
      slot_free(&built);
      return s;
   }
   s = slot_install_locked(&built);
   OLOG_INFO("memory_embeddings: loaded %d entity embeddings for user %d", s->count, user_id);
   return s;
}

void memory_embeddings_invalidate_entity_cache(void) {
   atomic_fetch_add(&s_gen_all, 1);
}

void memory_embeddings_invalidate_entity_cache_for_user(int user_id) {
   atomic_fetch_add(user_gen(user_id), 1);
}

void memory_embeddings_entity_cleanup(void) {
   pthread_mutex_lock(&s_ent.mutex);
   for (int i = 0; i < ENTITY_CACHE_SLOTS; i++) {
      slot_free(&s_ent.slot[i]);
   }
   pthread_mutex_unlock(&s_ent.mutex);
}

int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text) {
   if (!embedding_engine_available() || !text) {
      return FAILURE;
   }
   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;
   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0) {
      return rc;
   }
   const float norm = memory_embeddings_l2_norm(embedding, dims);
   rc = memory_db_entity_update_embedding(entity_id, user_id, embedding, dims, norm);
   if (rc == MEMORY_DB_SUCCESS) {
      memory_embeddings_invalidate_entity_cache_for_user(user_id);
   }
   return rc;
}

/* How a selection treats similarity where no baseline exists. */
typedef struct {
   const float *qvec;              /* NULL: names only */
   const memory_terms_t *terms;    /* NULL: similarity only */
   const char *query;              /* the text @p terms came from */
   float min_relevance;            /* <= 0: gate off */
   const char *type_filter;        /* NULL or "": any type */
   bool ungated_similarity_counts; /* a pool too small to gate: similar counts */
} select_opts_t;

/* Insert @p m into @p out (best first by key, at most @p max). */
static void top_insert(memory_entity_match_t *out,
                       float *keys,
                       int *n,
                       int max,
                       const memory_entity_match_t *m,
                       float key) {
   if (*n == max) {
      (*n)--; /* the caller checked key beats the last */
   }
   int j = *n - 1;
   while (j >= 0 && keys[j] < key) {
      out[j + 1] = out[j];
      keys[j + 1] = keys[j];
      j--;
   }
   out[j + 1] = *m;
   keys[j + 1] = key;
   (*n)++;
}

/* The query's mean cosine over the pool (one dot product with unit_sum). */
static double pool_mean(const entity_slot_t *s, const float *qvec, float qnorm) {
   if (s->count == 0) {
      return 0.0;
   }
   double dot = 0.0;
   for (int d = 0; d < s->dims; d++) {
      dot += (double)qvec[d] * s->unit_sum[d];
   }
   return dot / ((double)qnorm * (double)s->count);
}

typedef struct {
   int idx;     /* into the slot */
   int matched; /* the name's content words the query contains */
} named_t;

/* Whether stem line @p line contains word @p w (@p len bytes). */
static bool line_has(const char *line, const char *w, size_t len) {
   for (const char *p = line; *p;) {
      const char *end = strchr(p, ' ');
      const size_t n = end ? (size_t)(end - p) : strlen(p);
      if (n == len && memcmp(p, w, len) == 0) {
         return true;
      }
      if (!end) {
         return false;
      }
      p = end + 1;
   }
   return false;
}

/* Whether every query word in name @p a is also in name @p b. */
static bool query_words_covered(const memory_terms_t *q, const char *a, const char *b) {
   char word[MEMORY_TERM_LEN];
   for (const char *p = a; *p;) {
      const char *end = strchr(p, ' ');
      const size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len > 0 && len < sizeof(word)) {
         memcpy(word, p, len);
         word[len] = '\0';
         if (memory_terms_has(q, word) && !line_has(b, p, len)) {
            return false;
         }
      }
      if (!end) {
         break;
      }
      p = end + 1;
   }
   return true;
}

/* Whether @p name appears in @p query as a phrase: its words, case-folded,
 * back to back and in order. */
static bool phrase_in(const char *query, const char *name) {
   const char *qs = NULL;
   size_t ql = 0;
   for (const char *q = query; q && (q = memory_terms_next_word(q, &qs, &ql)) != NULL;) {
      /* Try the name starting at this query word. */
      const char *qw = qs;
      size_t qwl = ql;
      const char *qnext = q;
      const char *ns = NULL;
      size_t nl = 0;
      bool all = true;
      for (const char *n = name; (n = memory_terms_next_word(n, &ns, &nl)) != NULL;) {
         if (!qw || nl != qwl) {
            all = false;
            break;
         }
         for (size_t k = 0; k < nl && all; k++) {
            all = memory_terms_fold_at(ns, k) == memory_terms_fold_at(qw, k);
         }
         if (!all) {
            break;
         }
         qnext = qnext ? memory_terms_next_word(qnext, &qw, &qwl) : NULL;
         if (!qnext) {
            qw = NULL;
         }
      }
      if (all) {
         return true;
      }
   }
   return false;
}

/* Whether the query names entity @p i, and how many of its content words it
 * matched (0: not named):
 *   - two or more content words ("Harbor Lane Relocation"): two must match;
 *   - a one-word name ("Quillon"): that word must;
 *   - one content word among other words ("The Quillmen", "Why We Build",
 *     "X Corp", "Borra Borra"): the name must appear as a phrase, since its
 *     other words turn up scattered through any long message. */
static int name_match(const entity_slot_t *s, const memory_terms_t *q, const char *query, int i) {
   const int c = s->name_terms[i];
   if (c == 0) {
      return 0;
   }
   const int matched = memory_terms_count_in(q, s->name_stems[i]);
   if (c >= 2) {
      return matched >= 2 ? matched : 0;
   }
   if (matched == 0) {
      return 0;
   }
   return (s->name_words[i] <= 1 || phrase_in(query, s->names[i])) ? 1 : 0;
}

/* Whether named entry @p b covers @p a: A's matched content words all belong
 * to B, and B matched more of them, or as many with a shorter name (the exact
 * "Morning Report" over "Morning Report Test" for "morning report"). */
static bool covers(const entity_slot_t *s,
                   const memory_terms_t *q,
                   const named_t *b,
                   const named_t *a) {
   const bool longer = b->matched > a->matched ||
                       (b->matched == a->matched && s->name_words[b->idx] < s->name_words[a->idx]);
   return longer && query_words_covered(q, s->name_stems[a->idx], s->name_stems[b->idx]);
}

/* The entities @p o's query names, in slot order, into @p named (capacity
 * s->count).  Longest match wins: one whose matched words all belong to a
 * longer named match ("AI" by "ai" when the query says "Marigold AI Planner") isn't
 * named; the query meant the longer one.  It can still count as similar. */
static int find_named(const entity_slot_t *s, const select_opts_t *o, named_t *named) {
   if (!o->terms || o->terms->count == 0) {
      return 0;
   }
   int n = 0;
   for (int i = 0; i < s->count; i++) {
      if (o->type_filter && o->type_filter[0] && strcmp(s->types[i], o->type_filter) != 0) {
         continue;
      }
      const int matched = name_match(s, o->terms, o->query, i);
      if (matched > 0) {
         named[n].idx = i;
         named[n].matched = matched;
         n++;
      }
   }
   int kept = 0;
   for (int a = 0; a < n; a++) {
      bool subsumed = false;
      for (int b = 0; b < n && !subsumed; b++) {
         subsumed = b != a && covers(s, o->terms, &named[b], &named[a]);
      }
      if (!subsumed) {
         named[kept++] = named[a];
      }
   }
   return kept;
}

/* Caller holds s_ent.mutex.  See memory_embeddings_entity_matches().  Writes
 * the count to @p n_out; FAILURE only on allocation failure. */
static int select_locked(const entity_slot_t *s,
                         const select_opts_t *o,
                         memory_entity_match_t *out,
                         int max,
                         int *n_out) {
   const int dims = s->dims;
   const float *qvec = o->qvec;
   const float qnorm = qvec ? memory_embeddings_l2_norm(qvec, dims) : 0.0f;
   if (qnorm < 1e-9f) {
      qvec = NULL;
   }
   const double mean = qvec ? pool_mean(s, qvec, qnorm) : 0.0;
   /* Similarity counts when: the gate is off (min_relevance <= 0, "any
    * similar entity"); the pool is big enough to measure relevance and it
    * clears the bar; or, in a pool too small for a baseline, as the caller
    * chose (see select_opts_t). */
   const bool gate_off = o->min_relevance <= 0.0f;
   const bool measurable = s->count >= EMBEDDING_RELEVANCE_MIN_POOL;
   *n_out = 0;
   named_t *named = NULL;
   if (s->count > 0 && o->terms && o->terms->count > 0) {
      named = malloc((size_t)s->count * sizeof(*named));
      if (!named) {
         return FAILURE;
      }
   }
   const int n_named = named ? find_named(s, o, named) : 0;
   int next_named = 0; /* named[] is in slot order */
   float keys[MEMORY_ENTITY_MATCH_MAX];
   int n = 0;
   for (int i = 0; i < s->count; i++) {
      if (o->type_filter && o->type_filter[0] && strcmp(s->types[i], o->type_filter) != 0) {
         continue;
      }
      memory_entity_match_t m = { .id = s->ids[i] };
      if (qvec) {
         m.has_cosine = true;
         m.cosine = memory_embeddings_cosine_with_norms(qvec,
                                                        s->embeddings + (size_t)i * (size_t)dims,
                                                        dims, qnorm, s->norms[i]);
         m.relevance = embedding_corpus_relevance(m.cosine, mean * s->count, s->count);
      }
      while (next_named < n_named && named[next_named].idx < i) {
         next_named++;
      }
      m.named = next_named < n_named && named[next_named].idx == i;
      const bool similar = m.has_cosine &&
                           (gate_off || (measurable ? m.relevance >= o->min_relevance
                                                    : o->ungated_similarity_counts));
      if (!similar && !m.named) {
         continue;
      }
      /* An entity the query names ranks ahead of any it only resembles. */
      const float key = (m.named ? 2.0f : 0.0f) + (m.has_cosine ? m.cosine : 0.0f);
      if (n == max && key <= keys[n - 1]) {
         continue;
      }
      safe_strscpy(m.name, s->names[i]);
      safe_strscpy(m.type, s->types[i]);
      top_insert(out, keys, &n, max, &m, key);
   }
   free(named);
   *n_out = n;
   return SUCCESS;
}

/* Select for @p user_id: the shared body of the two public lookups. */
static int entity_select(int user_id,
                         const char *query,
                         const float *qvec,
                         int dims,
                         select_opts_t o,
                         memory_entity_match_t *out,
                         int max,
                         int *n_out) {
   memory_terms_t *terms = NULL;
   if (query) {
      terms = malloc(sizeof(*terms));
      if (!terms) {
         return FAILURE;
      }
      memory_terms_from_text(query, true, terms); /* before the lock: stemming */
   }
   o.terms = terms;
   o.query = query;
   entity_slot_t *s = slot_acquire(user_id);
   if (!s) {
      pthread_mutex_unlock(&s_ent.mutex);
      free(terms);
      return FAILURE;
   }
   o.qvec = (qvec && dims == s->dims) ? qvec : NULL;
   const int rc = select_locked(s, &o, out, max, n_out);
   pthread_mutex_unlock(&s_ent.mutex);
   free(terms);
   return rc;
}

int memory_embeddings_entity_matches(int user_id,
                                     const char *query,
                                     const float *qvec,
                                     int dims,
                                     float min_relevance,
                                     memory_entity_match_t *out,
                                     int max,
                                     int *n_out) {
   if (n_out) {
      *n_out = 0;
   }
   if (!out || !n_out || max <= 0 || max > MEMORY_ENTITY_MATCH_MAX || (!query && !qvec)) {
      return FAILURE;
   }
   /* Context injection: a pool too small to gate keeps its most similar. */
   const select_opts_t o = { .min_relevance = min_relevance, .ungated_similarity_counts = true };
   return entity_select(user_id, query, qvec, dims, o, out, max, n_out);
}

int memory_embeddings_entity_search(int user_id,
                                    const char *query,
                                    const char *type_filter,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_scores,
                                    int max_results) {
   if (!memory_embeddings_available() || !query || !out_ids || max_results <= 0) {
      return 0;
   }
   if (max_results > MEMORY_ENTITY_MATCH_MAX) {
      max_results = MEMORY_ENTITY_MATCH_MAX;
   }
   float qvec[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(query, qvec, &dims) != 0) {
      dims = 0; /* names still match */
   }
   /* A search answers "which entities is this about": where no baseline tells
    * relevant from typical, similarity alone says nothing, and the caller
    * falls back to its keyword search. */
   const select_opts_t o = {
      .min_relevance = g_config.memory.focus_injection.entity_min_relevance,
      .type_filter = type_filter,
      .ungated_similarity_counts = false,
   };
   memory_entity_match_t matches[MEMORY_ENTITY_MATCH_MAX];
   int n = 0;
   if (entity_select(user_id, query, dims > 0 ? qvec : NULL, dims, o, matches, max_results, &n) !=
       SUCCESS) {
      return 0;
   }
   for (int i = 0; i < n; i++) {
      out_ids[i] = matches[i].id;
      if (out_names) {
         safe_strscpy(out_names[i], matches[i].name);
      }
      if (out_types) {
         safe_strscpy(out_types[i], matches[i].type);
      }
      if (out_scores) {
         out_scores[i] = matches[i].cosine;
      }
   }
   return n;
}

int memory_embeddings_entity_cosine(int user_id,
                                    int64_t entity_id,
                                    const float *query_embedding,
                                    int query_dims,
                                    float query_norm,
                                    float *out_cosine) {
   if (!query_embedding || query_dims <= 0 || !out_cosine || entity_id <= 0 ||
       !memory_embeddings_available()) {
      return FAILURE;
   }
   const entity_slot_t *s = slot_acquire(user_id);
   /* A dimension mismatch means the caller's embedding came from another
    * model: the cosine would be meaningless. */
   int rc = FAILURE;
   if (s && s->dims == query_dims) {
      /* Linear scan: the alias resolver runs at extraction time, off the
       * conversational path, and scores a handful of candidates per call. */
      for (int i = 0; i < s->count; i++) {
         if (s->ids[i] == entity_id) {
            *out_cosine = memory_embeddings_cosine_with_norms(
                query_embedding, s->embeddings + (size_t)i * (size_t)query_dims, query_dims,
                query_norm, s->norms[i]);
            rc = SUCCESS;
            break;
         }
      }
   }
   pthread_mutex_unlock(&s_ent.mutex);
   return rc;
}
