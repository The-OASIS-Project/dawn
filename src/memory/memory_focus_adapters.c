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
 * Memory focus-source adapters — Phase 1c of Dynamic Context Injection.
 *
 * Four adapters land here:
 *   - memory_fact      (requires_embedding=true) — hybrid keyword+vector via
 *                      memory_fact_search_hybrid()
 *   - memory_entity    (requires_embedding=true) — keyword + vector hybrid
 *                      with merge by entity_id; importance boosts on photo
 *                      ownership and name-keyword match
 *   - memory_relation  (requires_embedding=true) — top-3 query-similar
 *                      entities → currently-valid relations (bitemporal
 *                      filter at as_of=now), deterministic round-robin
 *                      allocation across subjects
 *   - memory_summary   (requires_embedding=false) — keyword search over
 *                      summaries created within the last 30 days
 *
 * Filter-on-retrieval is FRAMEWORK-OWNED + trust-tier-gated.  Adapters
 * do NOT call `memory_filter_check()` — `focus_compose()` decides based
 * on the adapter's `source_type` whether to filter.  Memory adapters
 * here are FOCUS_SOURCE_INTERNAL → skipped at retrieval (filtered at
 * extraction-time ingestion gate is the model).  See trust-tier comment
 * in src/core/focus/focus_source.c::focus_compose for the full rationale.
 *
 * Memory ownership: each adapter mallocs the candidate array AND each
 * candidate's `text` + `item_id`.  Framework owns on SUCCESS.  On
 * FAILURE adapters MUST clean up partial allocations and zero out the
 * out parameters (per `focus_source.h` contract).
 */

#include "memory/memory_focus_adapters.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_recency.h"
#include "core/focus/focus_source.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_fact_search.h"
#include "memory/memory_types.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 *
 * All file-static so 1j tuning lands here without touching configs.  See
 * `docs/DYNAMIC_CONTEXT_INJECTION_DESIGN.md` §"Phase 1 — Per-Turn Focus"
 * for the bench-driven tuning plan.
 *
 * Phase 1d extracted the previously-local recency / candidate-helper
 * primitives to `focus_recency.{c,h}` + `focus_candidate_helpers.{c,h}`
 * so L3 external adapters can share them without depending on the
 * memory subsystem.  The constants below are memory-adapter-only.
 * ============================================================================= */

/* Importance-score formula constants (1c-author choices, NOT
 * design-doc-mandated): photo indicates user-curation; keyword
 * name-match indicates the user referred by name in this turn;
 * consolidated summaries have passed the multi-conversation aggregation
 * step.  Documented here for tuning in 1j after bench evidence; will
 * NOT become config knobs without bench justification. */
#define ENTITY_IMPORTANCE_BASE 0.5f
#define ENTITY_IMPORTANCE_PHOTO_BOOST 0.2f
#define ENTITY_IMPORTANCE_NAMEMATCH 0.1f

#define SUMMARY_IMPORTANCE_CONSOLIDATED 1.0f
#define SUMMARY_IMPORTANCE_NORMAL 0.7f

/* Summary lookback matches `memory_context.c` recent-summaries default. */
#define SUMMARY_LOOKBACK_SECONDS (30 * 86400)

/* Relation adapter — top-K subjects to seed the per-subject relation
 * fetch with.  Larger values dilute round-robin weighting; 3 keeps the
 * most-relevant entity dominant while still surfacing supporting
 * subjects. */
#define RELATION_TOP_SUBJECTS 3

/* Per-entity buffer cap when loading embeddings for cosine.  Same
 * order-of-magnitude as the production `memory_embeddings_entity_search`
 * cap; bounded so the stack/heap footprint stays predictable. */
#define ENTITY_EMBED_BUF_CAP 256

/* Hard ceiling on `max_candidates` accepted by the relation adapter,
 * tied to the upper bound enforced by `config_validate.c` for
 * `memory.focus_injection.top_k` (1..64).  Preallocated stack arrays
 * for produced_relation_ids[] / conv_ids[] / starts[] / ends[] use this
 * cap; the static_assert below makes the contract a compile-time
 * invariant rather than a runtime comment.  If config_validate's range
 * ever bumps past 64 without updating this cap, the build breaks here. */
#define RELATION_ADAPTER_MAX_CANDIDATES_CAP 64
_Static_assert(RELATION_ADAPTER_MAX_CANDIDATES_CAP >= 64,
               "relation adapter cap must cover the focus_injection top_k validate range");

/* =============================================================================
 * Entity-embedding load workspace
 *
 * Both the entity adapter (cosine vs all entities) and the relation
 * adapter (top-K subject seeding) load the same four buffers from
 * `memory_db_entity_get_embeddings`.  Promoting the buffers to a single
 * file-static scratch struct guarded by a mutex avoids ~820 KB / call of
 * heap churn at production dims=384 (4 buffers × 256 entries × 4-byte
 * floats / id+name/type strings).  The mutex is uncontended in the
 * single-session compose path; if a future multi-user concurrent compose
 * lands, the cost is at most one extra adapter call serialized.
 * ============================================================================= */
typedef struct {
   int64_t ids[ENTITY_EMBED_BUF_CAP];
   char names[ENTITY_EMBED_BUF_CAP][MEMORY_ENTITY_NAME_MAX];
   char types[ENTITY_EMBED_BUF_CAP][MEMORY_ENTITY_TYPE_MAX];
   /* embeddings + norms allocated lazily on first use because their size
    * depends on the engine's `dims` (384 for bge-small, 1536 for
    * OpenAI ada).  Resized in place if the engine reconfigures. */
   float *embeddings;
   float *norms;
   int embeddings_dims; /* 0 = not yet allocated */
} entity_embed_scratch_t;

static entity_embed_scratch_t s_entity_scratch;
static pthread_mutex_t s_entity_scratch_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Acquire the scratch buffer for `dims` floats per entity.  Caller MUST
 * call `release_entity_scratch()` before returning.  Returns SUCCESS on
 * acquisition (mutex held), FAILURE on OOM (mutex released). */
static int acquire_entity_scratch(int dims, entity_embed_scratch_t **out_scratch) {
   pthread_mutex_lock(&s_entity_scratch_mutex);
   if (s_entity_scratch.embeddings_dims != dims) {
      free(s_entity_scratch.embeddings);
      free(s_entity_scratch.norms);
      s_entity_scratch.embeddings = calloc(ENTITY_EMBED_BUF_CAP * (size_t)dims, sizeof(float));
      s_entity_scratch.norms = calloc(ENTITY_EMBED_BUF_CAP, sizeof(float));
      if (s_entity_scratch.embeddings == NULL || s_entity_scratch.norms == NULL) {
         free(s_entity_scratch.embeddings);
         free(s_entity_scratch.norms);
         s_entity_scratch.embeddings = NULL;
         s_entity_scratch.norms = NULL;
         s_entity_scratch.embeddings_dims = 0;
         pthread_mutex_unlock(&s_entity_scratch_mutex);
         OLOG_ERROR("focus_adapter: OOM allocating entity scratch (dims=%d)", dims);
         return FAILURE;
      }
      s_entity_scratch.embeddings_dims = dims;
   }
   *out_scratch = &s_entity_scratch;
   return SUCCESS;
}

static void release_entity_scratch(void) {
   pthread_mutex_unlock(&s_entity_scratch_mutex);
}

/* =============================================================================
 * Fact adapter
 *
 * source_id          = "memory_fact"
 * source_type        = FOCUS_SOURCE_INTERNAL
 * requires_embedding = true
 *
 * Pipeline: memory_fact_search_hybrid → defense-in-depth user_id
 * post-check on each fact → batch provenance via
 * memory_db_facts_get_sources.
 *
 * include_private (1f gap): the parameter is accepted from the
 * framework but is a no-op in v1.  hybrid_search currently scopes by
 * user_id; the conversation-private boundary is enforced at conv_db_*
 * level only, not at memory_facts.  Wired in 1f.
 * ============================================================================= */
static int fact_adapter_query(int user_id,
                              bool include_private,
                              const char *query_text,
                              const float *query_embedding,
                              size_t embed_dim,
                              time_t now,
                              int max_candidates,
                              focus_candidate_t **out_candidates,
                              int *out_count) {
   (void)include_private; /* 1f gap — see header comment above */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_text == NULL || query_text[0] == '\0')
      return SUCCESS;

   memory_fact_t facts[10];
   float scores[10];
   int n = 0;
   const int cap = (max_candidates > 10) ? 10 : max_candidates;
   if (memory_fact_search_hybrid(user_id, query_text, query_embedding, embed_dim, /*since_ts*/ 0,
                                 facts, scores, cap, &n) != SUCCESS) {
      return FAILURE;
   }
   if (n <= 0)
      return SUCCESS;

   /* Defense-in-depth: drop any fact whose user_id doesn't match.  The
    * helper itself already filters at SQL boundary AND post-checks the
    * vector-only fetch path; this is a third gate that catches a
    * hypothetical regression in either of the upstream layers. */
   int kept = 0;
   for (int i = 0; i < n; i++) {
      if (facts[i].user_id != user_id) {
         OLOG_ERROR("fact_adapter: fact_id=%lld owned by user_id=%d (expected %d) — skipping",
                    (long long)facts[i].id, facts[i].user_id, user_id);
         continue;
      }
      if (kept != i) {
         facts[kept] = facts[i];
         scores[kept] = scores[i];
      }
      kept++;
   }

   /* "I don't remember" gate — same configurable score floor that gates the
    * memory tool's `search` action.  Focus-block facts are prepended to the
    * system prompt verbatim, so the marginal-cosine fabrication surface is
    * identical to the tool path.  Single knob (g_config.memory.search_score_floor)
    * keeps tool-time and injection-time semantics aligned. */
   kept = memory_search_apply_score_floor(facts, scores, kept, g_config.memory.search_score_floor);
   if (kept <= 0)
      return SUCCESS;

   /* Batch provenance lookup. */
   int64_t fact_ids[10];
   int64_t conv_ids[10] = { 0 }, starts[10] = { 0 }, ends[10] = { 0 };
   for (int i = 0; i < kept; i++)
      fact_ids[i] = facts[i].id;
   memory_db_facts_get_sources(user_id, fact_ids, kept, conv_ids, starts, ends);

   focus_candidate_t *out = calloc((size_t)kept, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("fact_adapter: OOM allocating candidate array (n=%d)", kept);
      return FAILURE;
   }

   bool truncated_warned = false;
   int produced = 0;
   for (int i = 0; i < kept; i++) {
      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "fact", facts[i].id) !=
          SUCCESS) {
         OLOG_ERROR("fact_adapter: item_id formatting failed (fact_id=%lld)",
                    (long long)facts[i].id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         return FAILURE;
      }
      const float recency = focus_recency_decay_uniform(facts[i].created_at, now);
      if (focus_candidate_init(&out[produced], "memory_fact", FOCUS_SOURCE_INTERNAL,
                               facts[i].fact_text, item_id, facts[i].created_at, scores[i], recency,
                               facts[i].confidence, &truncated_warned) != SUCCESS) {
         OLOG_ERROR("fact_adapter: focus_candidate_init failed (fact_id=%lld)",
                    (long long)facts[i].id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         return FAILURE;
      }
      out[produced].provenance.conv_id = conv_ids[i];
      out[produced].provenance.msg_id_start = starts[i];
      out[produced].provenance.msg_id_end = ends[i];
      produced++;
   }

   *out_candidates = out;
   *out_count = produced;
   return SUCCESS;
}

/* =============================================================================
 * Entity adapter (keyword + vector hybrid)
 *
 * source_id          = "memory_entity"
 * source_type        = FOCUS_SOURCE_INTERNAL
 * requires_embedding = true
 *
 * Provenance: ENTITIES HAVE NO SOURCE LINKAGE — each entity is the
 * aggregate of N facts.  candidate.provenance stays {0,0,0} (the
 * sentinel `memory_provenance_t` zero value).  This is a deliberate
 * design decision, not an oversight.
 * ============================================================================= */

/* Render "Pepper Potts (person)" — name + type only.  Relations are
 * surfaced by the relation adapter to keep this O(N) across the merged
 * set. */
static int render_entity_text(const memory_entity_t *e, char *buf, size_t buflen) {
   if (e == NULL || buf == NULL || buflen == 0)
      return FAILURE;
   const char *type = (e->entity_type[0] != '\0') ? e->entity_type : "thing";
   const int n = snprintf(buf, buflen, "%s (%s)", e->name, type);
   if (n < 0 || (size_t)n >= buflen)
      return FAILURE;
   return SUCCESS;
}

/* Embedding-dimension mismatch is an operational signal: the engine
 * was swapped but stored entity vectors haven't been recomputed yet.
 * Log ONCE per session via this file-static flag — the recompute
 * worker will eventually catch up; flooding the log buys nothing. */
static bool s_entity_dim_mismatch_warned = false;

/* Cosine-rank entries used internally by the entity adapter. */
typedef struct {
   int64_t entity_id;
   memory_entity_t entity;
   float semantic_score;       /* Cosine when vec path; FOCUS_SCORE_NA on keyword-only */
   float importance_increment; /* Accumulated boosts (photo, name-match) */
   bool name_matched;
} entity_rank_entry_t;

/* Linear scan for an existing entry with `entity_id`.  Returns the
 * index in [0, n) on hit, or `n` on miss (callers test `slot < n`).
 * Out-of-range "n" miss-sentinel preserves the project-wide rule
 * banning negative returns from non-public functions. */
static int entity_rank_find(entity_rank_entry_t *rows, int n, int64_t entity_id) {
   for (int i = 0; i < n; i++)
      if (rows[i].entity_id == entity_id)
         return i;
   return n;
}

static int entity_adapter_query(int user_id,
                                bool include_private,
                                const char *query_text,
                                const float *query_embedding,
                                size_t embed_dim,
                                time_t now,
                                int max_candidates,
                                focus_candidate_t **out_candidates,
                                int *out_count) {
   (void)include_private; /* Same 1f gap as fact adapter */
   (void)embed_dim;       /* Engine reports its own dim */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_text == NULL || query_text[0] == '\0')
      return SUCCESS;

   const int cap = (max_candidates > ENTITY_EMBED_BUF_CAP) ? ENTITY_EMBED_BUF_CAP : max_candidates;
   /* Working set bounded by `2 * cap` (worst case: keyword and vector
    * paths return disjoint top-cap sets). */
   const int work_cap = cap * 2;
   entity_rank_entry_t *rows = calloc((size_t)work_cap, sizeof(*rows));
   if (rows == NULL) {
      OLOG_ERROR("entity_adapter: OOM allocating rank workspace");
      return FAILURE;
   }
   int row_count = 0;

   /* Path A — keyword.  Each match contributes ENTITY_IMPORTANCE_NAMEMATCH. */
   memory_entity_t kw_buf[ENTITY_EMBED_BUF_CAP];
   int kw_n = 0;
   if (memory_db_entity_search(user_id, query_text, kw_buf, cap, &kw_n) == MEMORY_DB_SUCCESS) {
      for (int i = 0; i < kw_n && row_count < work_cap; i++) {
         /* Defense-in-depth user_id post-check (entity_search scopes
          * already; this guards against future regression). */
         if (kw_buf[i].user_id != user_id) {
            OLOG_ERROR(
                "entity_adapter: entity_id=%lld owned by user_id=%d (expected %d) — skipping",
                (long long)kw_buf[i].id, kw_buf[i].user_id, user_id);
            continue;
         }
         rows[row_count].entity_id = kw_buf[i].id;
         rows[row_count].entity = kw_buf[i];
         rows[row_count].semantic_score = FOCUS_SCORE_NA;
         rows[row_count].importance_increment = ENTITY_IMPORTANCE_NAMEMATCH;
         rows[row_count].name_matched = true;
         row_count++;
      }
   }

   /* Path B — vector.  Skip if no query_embedding or engine unavailable. */
   if (query_embedding != NULL && memory_embeddings_available()) {
      const int dims = memory_embeddings_dims();
      if (dims > 0) {
         entity_embed_scratch_t *scratch = NULL;
         if (acquire_entity_scratch(dims, &scratch) != SUCCESS) {
            free(rows);
            return FAILURE;
         }
         int loaded = 0;
         /* Focus-adapter scratch is canonical-only (v43) — soft aliases are
          * absent from the cosine pool so duplicate surface forms don't
          * compete for the focus-block budget. */
         if (memory_db_entity_get_embeddings(user_id, /* include_aliases */ false, dims,
                                             scratch->ids, scratch->names, scratch->types,
                                             scratch->embeddings, scratch->norms,
                                             ENTITY_EMBED_BUF_CAP, &loaded) == MEMORY_DB_SUCCESS) {
            if (loaded == 0 && !s_entity_dim_mismatch_warned) {
               s_entity_dim_mismatch_warned = true;
               OLOG_WARNING("entity_adapter: 0 entity embeddings loaded at dim=%d — possible "
                            "model swap; recompute worker will catch up (logged once per session)",
                            dims);
            }

            const float q_norm = memory_embeddings_l2_norm(query_embedding, dims);
            /* Score every loaded entity, then pick top-cap.  Bounded N
             * (≤ENTITY_EMBED_BUF_CAP) so an O(N log N) sort would be
             * overkill — a small-K selection by repeated scan is
             * cheaper for cap≤20 typical. */
            for (int i = 0; i < loaded && row_count < work_cap; i++) {
               const float cosine = memory_embeddings_cosine_with_norms(
                   query_embedding, &scratch->embeddings[i * dims], dims, q_norm,
                   scratch->norms[i]);
               int slot = entity_rank_find(rows, row_count, scratch->ids[i]);
               if (slot < row_count) {
                  /* Merge: keep max(semantic_score). */
                  if (rows[slot].semantic_score == FOCUS_SCORE_NA ||
                      cosine > rows[slot].semantic_score) {
                     rows[slot].semantic_score = cosine;
                  }
               } else if (row_count < work_cap) {
                  /* Vector-only candidate; need to fetch the full
                   * entity record to populate canonical fields the
                   * keyword path provides. */
                  memory_entity_t e;
                  if (memory_db_entity_get_by_name(user_id, scratch->names[i], &e) ==
                      MEMORY_DB_SUCCESS) {
                     if (e.user_id != user_id) {
                        OLOG_ERROR("entity_adapter: vector-only entity_id=%lld owned by "
                                   "user_id=%d (expected %d) — skipping",
                                   (long long)scratch->ids[i], e.user_id, user_id);
                        continue;
                     }
                     /* Use `e.id` (post-lookup canonical) so id and
                      * displayed record content stay paired even if
                      * a future merge artifact ever produced two
                      * entities sharing a canonical name. */
                     rows[row_count].entity_id = e.id;
                     rows[row_count].entity = e;
                     rows[row_count].semantic_score = cosine;
                     rows[row_count].importance_increment = 0.0f;
                     rows[row_count].name_matched = false;
                     row_count++;
                  }
               }
            }
         }
         release_entity_scratch();
      }
   }

   if (row_count == 0) {
      free(rows);
      return SUCCESS;
   }

   /* Photo boost (lookup happens once per surviving entity).  Clamp
    * importance to 1.0 — boosts can stack across name-match + photo. */
   for (int i = 0; i < row_count; i++) {
      char photo_id[64] = { 0 };
      if (memory_db_entity_get_photo(user_id, rows[i].entity_id, photo_id, sizeof(photo_id)) ==
              MEMORY_DB_SUCCESS &&
          photo_id[0] != '\0') {
         rows[i].importance_increment += ENTITY_IMPORTANCE_PHOTO_BOOST;
      }
   }

   /* Order by semantic_score desc (NA treated as 0 for ranking only),
    * then trim to cap.  Stable insertion sort — N is small.
    *
    * Intentional design: this sort key DOES NOT incorporate
    * `importance_increment` (photo + name-match boosts).  The framework's
    * `compute_final_score` re-mixes semantic + recency + importance +
    * source weight at compose time, which is where cross-dimension
    * trade-offs belong.  The adapter's job here is ONLY to surface the
    * top semantic-relevance candidates within its per-source cap — the
    * boosts ride along on each candidate and feed the final ranking.
    * Trade-off: a high-importance keyword-only entity (semantic=NA→0)
    * can be trimmed at the cap by a low-cosine vector-only entity.  In
    * practice `cap` ≥ kw_count + a few, so trimming pressure is low. */
   for (int i = 1; i < row_count; i++) {
      entity_rank_entry_t tmp = rows[i];
      const float a = (tmp.semantic_score == FOCUS_SCORE_NA) ? 0.0f : tmp.semantic_score;
      int j = i - 1;
      while (j >= 0) {
         const float b = (rows[j].semantic_score == FOCUS_SCORE_NA) ? 0.0f : rows[j].semantic_score;
         if (b >= a)
            break;
         rows[j + 1] = rows[j];
         j--;
      }
      rows[j + 1] = tmp;
   }
   const int kept = (row_count > cap) ? cap : row_count;

   focus_candidate_t *out = calloc((size_t)kept, sizeof(*out));
   if (out == NULL) {
      free(rows);
      OLOG_ERROR("entity_adapter: OOM allocating candidate array (n=%d)", kept);
      return FAILURE;
   }

   bool truncated_warned = false;
   int produced = 0;
   for (int i = 0; i < kept; i++) {
      char text_buf[256];
      if (render_entity_text(&rows[i].entity, text_buf, sizeof(text_buf)) != SUCCESS)
         continue;
      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "entity", rows[i].entity_id) !=
          SUCCESS)
         continue;

      const time_t ts = (rows[i].entity.last_seen != 0) ? rows[i].entity.last_seen
                                                        : rows[i].entity.first_seen;
      const float recency = focus_recency_decay_uniform(ts, now);
      float importance = ENTITY_IMPORTANCE_BASE + rows[i].importance_increment;
      if (importance > 1.0f)
         importance = 1.0f;

      if (focus_candidate_init(&out[produced], "memory_entity", FOCUS_SOURCE_INTERNAL, text_buf,
                               item_id, ts, rows[i].semantic_score, recency, importance,
                               &truncated_warned) != SUCCESS) {
         OLOG_ERROR("entity_adapter: focus_candidate_init failed (entity_id=%lld)",
                    (long long)rows[i].entity_id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         free(rows);
         return FAILURE;
      }
      /* Provenance intentionally zeroed — entities aggregate from many
       * facts, no single source range applies. */
      produced++;
   }

   free(rows);
   *out_candidates = out;
   *out_count = produced;
   return SUCCESS;
}

/* =============================================================================
 * Relation adapter (subject-driven)
 *
 * source_id          = "memory_relation"
 * source_type        = FOCUS_SOURCE_INTERNAL
 * requires_embedding = true
 *
 * Pipeline:
 *   1. Find top-RELATION_TOP_SUBJECTS query-similar entity IDs via
 *      cosine over `memory_db_entity_get_embeddings`.
 *   2. Sort subjects by cosine desc (similarity-desc tiebreak rule).
 *   3. Round-robin allocate `max_candidates` slots across subjects,
 *      with the first `max_candidates % n_subjects` subjects (in
 *      similarity-desc order) getting one extra slot.
 *   4. For each subject, call `memory_db_relation_list_by_subject_at`
 *      with `as_of_ts = now` so only currently-valid relations
 *      surface (bitemporal filter handled by memory_db).
 *   5. Render "Subject Verb Object[ since YYYY-MM]".
 *   6. Batch provenance via `memory_db_relations_get_sources`.
 * ============================================================================= */

typedef struct {
   int64_t entity_id;
   memory_entity_t entity;
   float cosine;
} rel_subject_t;

static int format_yyyymm(time_t ts, char *out, size_t outlen) {
   if (ts <= 0)
      return FAILURE;
   struct tm tm;
   if (gmtime_r(&ts, &tm) == NULL)
      return FAILURE;
   const int n = snprintf(out, outlen, "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
   if (n < 0 || (size_t)n >= outlen)
      return FAILURE;
   return SUCCESS;
}

/* TODO(v49-followup, cross-alias aggregation): when the deferred LLM-context
 * "× N" emit work lands (see memory_callback.c TODO marker at the entity-
 * recall site), this helper needs symmetric cross-alias aggregation matching
 * www/js/ui/memory.js::aggregateRelationsForDisplay.  The partial UNIQUE
 * invariant in idx_memory_relations_unique_open is scoped to literal
 * entity_id, not canonical class — a canonical entity with N soft-aliased
 * members can have N open (alias_i, relation, X) rows each with their own
 * mention_count.  Per-row rendering here would show "Kris working_on DAWN ×3"
 * once per alias instead of "Kris working_on DAWN ×N" rolled up.  Display-
 * side (JS) already does the rollup; LLM-side needs to match before the
 * mention_count gets injected into the prompt. */
static int render_relation_text(const memory_entity_t *subj,
                                const memory_relation_t *r,
                                char *buf,
                                size_t buflen) {
   const char *subject = (subj->name[0] != '\0') ? subj->name : "(entity)";
   const char *object_disp = (r->object_name[0] != '\0') ? r->object_name : "(unknown)";
   if (r->valid_from > 0) {
      char yyyymm[16];
      if (format_yyyymm(r->valid_from, yyyymm, sizeof(yyyymm)) == SUCCESS) {
         const int n = snprintf(buf, buflen, "%s %s %s since %s", subject, r->relation, object_disp,
                                yyyymm);
         if (n < 0 || (size_t)n >= buflen)
            return FAILURE;
         return SUCCESS;
      }
   }
   const int n = snprintf(buf, buflen, "%s %s %s", subject, r->relation, object_disp);
   if (n < 0 || (size_t)n >= buflen)
      return FAILURE;
   return SUCCESS;
}

static int relation_adapter_query(int user_id,
                                  bool include_private,
                                  const char *query_text,
                                  const float *query_embedding,
                                  size_t embed_dim,
                                  time_t now,
                                  int max_candidates,
                                  focus_candidate_t **out_candidates,
                                  int *out_count) {
   (void)include_private;
   (void)embed_dim;
   (void)query_text; /* Subject seeding is purely vector-based in v1 */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_embedding == NULL || !memory_embeddings_available())
      return SUCCESS;

   const int dims = memory_embeddings_dims();
   if (dims <= 0)
      return SUCCESS;

   /* Load entity embeddings; pick top-RELATION_TOP_SUBJECTS by cosine.
    * Shared file-static scratch with the entity adapter — see
    * `acquire_entity_scratch` rationale at the top of this file. */
   entity_embed_scratch_t *scratch = NULL;
   if (acquire_entity_scratch(dims, &scratch) != SUCCESS)
      return FAILURE;

   int loaded = 0;
   /* Canonical-only (v43) — see entity adapter above for rationale. */
   memory_db_entity_get_embeddings(user_id, /* include_aliases */ false, dims, scratch->ids,
                                   scratch->names, scratch->types, scratch->embeddings,
                                   scratch->norms, ENTITY_EMBED_BUF_CAP, &loaded);
   if (loaded <= 0) {
      release_entity_scratch();
      return SUCCESS;
   }

   const float q_norm = memory_embeddings_l2_norm(query_embedding, dims);
   rel_subject_t subjects[RELATION_TOP_SUBJECTS] = { 0 };
   int subject_count = 0;
   for (int i = 0; i < loaded; i++) {
      const float cosine = memory_embeddings_cosine_with_norms(query_embedding,
                                                               &scratch->embeddings[i * dims], dims,
                                                               q_norm, scratch->norms[i]);
      if (subject_count < RELATION_TOP_SUBJECTS) {
         subjects[subject_count].entity_id = scratch->ids[i];
         safe_strscpy(subjects[subject_count].entity.name, scratch->names[i]);
         subjects[subject_count].cosine = cosine;
         subject_count++;
      } else {
         /* Replace the lowest-scoring subject if this one beats it. */
         int worst = 0;
         for (int k = 1; k < RELATION_TOP_SUBJECTS; k++)
            if (subjects[k].cosine < subjects[worst].cosine)
               worst = k;
         if (cosine > subjects[worst].cosine) {
            subjects[worst].entity_id = scratch->ids[i];
            safe_strscpy(subjects[worst].entity.name, scratch->names[i]);
            subjects[worst].cosine = cosine;
         }
      }
   }
   release_entity_scratch();

   if (subject_count == 0)
      return SUCCESS;

   /* Sort subjects by cosine desc (deterministic input to round-robin). */
   for (int i = 1; i < subject_count; i++) {
      rel_subject_t tmp = subjects[i];
      int j = i - 1;
      while (j >= 0 && subjects[j].cosine < tmp.cosine) {
         subjects[j + 1] = subjects[j];
         j--;
      }
      subjects[j + 1] = tmp;
   }

   /* Round-robin allocation: floor + remainder distributed in
    * similarity-desc order.  Determinism matters for tests + bench. */
   const int floor_per = max_candidates / subject_count;
   const int remainder = max_candidates % subject_count;
   int per_subject_quota[RELATION_TOP_SUBJECTS] = { 0 };
   for (int i = 0; i < subject_count; i++)
      per_subject_quota[i] = floor_per + (i < remainder ? 1 : 0);

   /* Working buffer sized to max_candidates. */
   focus_candidate_t *out = calloc((size_t)max_candidates, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("relation_adapter: OOM allocating candidate array");
      return FAILURE;
   }

   /* Hard refuse work above the static array cap — keeps the stack
    * arrays defensible without a runtime allocation.  Today the
    * upstream config_validate.c clamps top_k at 64, so this branch is
    * unreachable; the static_assert above pins the relationship. */
   if (max_candidates > RELATION_ADAPTER_MAX_CANDIDATES_CAP) {
      OLOG_ERROR("relation_adapter: max_candidates=%d exceeds cap %d — refusing", max_candidates,
                 RELATION_ADAPTER_MAX_CANDIDATES_CAP);
      return FAILURE;
   }

   int produced = 0;
   bool truncated_warned = false;
   /* Per-relation provenance lookup is batched once at the end. */
   int64_t produced_relation_ids[RELATION_ADAPTER_MAX_CANDIDATES_CAP];
   memset(produced_relation_ids, 0, sizeof(produced_relation_ids));
   const int prod_id_cap = (int)(sizeof(produced_relation_ids) / sizeof(produced_relation_ids[0]));

   for (int s = 0; s < subject_count && produced < max_candidates; s++) {
      const int quota = per_subject_quota[s];
      if (quota <= 0)
         continue;

      memory_relation_t rels[16];
      const int fetch_cap = quota > 16 ? 16 : quota;
      int n = 0;
      if (memory_db_relation_list_by_subject_at(user_id, subjects[s].entity_id, /*as_of*/ now, rels,
                                                fetch_cap, &n) != MEMORY_DB_SUCCESS) {
         continue;
      }
      for (int i = 0; i < n && produced < max_candidates; i++) {
         char text_buf[256];
         if (render_relation_text(&subjects[s].entity, &rels[i], text_buf, sizeof(text_buf)) !=
             SUCCESS) {
            continue;
         }
         char item_id[FOCUS_ITEM_ID_BUFLEN];
         if (focus_candidate_format_item_id(item_id, sizeof(item_id), "relation", rels[i].id) !=
             SUCCESS)
            continue;
         const float recency = focus_recency_decay_uniform(rels[i].valid_from, now);
         if (focus_candidate_init(&out[produced], "memory_relation", FOCUS_SOURCE_INTERNAL,
                                  text_buf, item_id, rels[i].valid_from, subjects[s].cosine,
                                  recency, rels[i].confidence, &truncated_warned) != SUCCESS) {
            focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
            return FAILURE;
         }
         if (produced < prod_id_cap)
            produced_relation_ids[produced] = rels[i].id;
         produced++;
      }
   }

   /* Batch provenance over the produced relation IDs. */
   if (produced > 0) {
      int64_t conv_ids[RELATION_ADAPTER_MAX_CANDIDATES_CAP] = { 0 };
      int64_t starts[RELATION_ADAPTER_MAX_CANDIDATES_CAP] = { 0 };
      int64_t ends[RELATION_ADAPTER_MAX_CANDIDATES_CAP] = { 0 };
      const int prov_n = produced > prod_id_cap ? prod_id_cap : produced;
      memory_db_relations_get_sources(user_id, produced_relation_ids, prov_n, conv_ids, starts,
                                      ends);
      for (int i = 0; i < prov_n; i++) {
         out[i].provenance.conv_id = conv_ids[i];
         out[i].provenance.msg_id_start = starts[i];
         out[i].provenance.msg_id_end = ends[i];
      }
   }

   *out_candidates = out;
   *out_count = produced;
   return SUCCESS;
}

/* =============================================================================
 * Summary adapter — hybrid keyword + semantic
 *
 * source_id          = "memory_summary"
 * source_type        = FOCUS_SOURCE_INTERNAL
 * requires_embedding = false  (semantic path is best-effort; keyword still
 *                              fires when no embedding is available)
 *
 * Strategy: always run keyword search; additionally run semantic search
 * when a query_embedding is available.  Merge results by summary id and
 * score with semantic-dominant arithmetic (cosine wins when present;
 * keyword presence supplies a floor so a keyword hit without an embedded
 * row still surfaces).
 *
 * Why hybrid: keyword catches exact-token matches that low-dim embeddings
 * sometimes miss; semantic catches paraphrase, summarized-topic-by-name,
 * and "do I have a summary discussing X" intent that keyword cannot.
 * The live conversation transcript captured before Step 3 showed zero
 * summary surfacings across 8 turns under keyword-only, validating the
 * upgrade.
 * ============================================================================= */

/* Score floor for a pure keyword-only match (no embedding row).  Picked to
 * sit below typical genuine-match cosine (~0.55+) while staying above noise
 * floor — keeps keyword as the fallback signal when semantic fails.
 *
 * TODO(phase-1j-style bench tune): this is a first-cut value with no bench
 * evidence behind it.  Once the summary adapter is exercised by the focus-
 * injection bench harness, treat 0.4 the same way ENTITY_IMPORTANCE_BASE /
 * RELATION_TOP_SUBJECTS are treated above — bench-driven tuning, possible
 * promotion to g_config if a per-deployment knob proves useful. */
#define SUMMARY_KEYWORD_FLOOR_SCORE 0.4f

/* Max merged-result buffer.  Comfortably exceeds the 10-cap per-source
 * pull so merging 10 keyword + 10 semantic cannot overflow. */
#define SUMMARY_MERGE_BUFLEN 32

/* Internal merged-result row. */
typedef struct {
   memory_summary_t summary;
   float score;
} summary_merge_entry_t;

static int summary_adapter_query(int user_id,
                                 bool include_private,
                                 const char *query_text,
                                 const float *query_embedding,
                                 size_t embed_dim,
                                 time_t now,
                                 int max_candidates,
                                 focus_candidate_t **out_candidates,
                                 int *out_count) {
   (void)include_private; /* Same 1f gap as fact adapter */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_text == NULL || query_text[0] == '\0')
      return SUCCESS;

   const int cap = (max_candidates > 10) ? 10 : max_candidates;
   const time_t since_ts = now - SUMMARY_LOOKBACK_SECONDS;

   /* Keyword path — existing search-since helper. */
   memory_summary_t kw_summaries[10];
   int kw_n = 0;
   if (memory_db_summary_search_since(user_id, query_text, since_ts, kw_summaries, cap, &kw_n) !=
       MEMORY_DB_SUCCESS) {
      return FAILURE;
   }

   /* Semantic path — only runs when caller supplied an embedded query.
    * Failure is non-fatal: warn and fall back to keyword-only results.
    *
    * summary_max_scan: cosine ranking pre-filter cap.  Original
    * hardcoded 256 broke when a full reextract clustered every summary's
    * created_at into a narrow window and the
    * ORDER BY created_at DESC LIMIT 256 in the scan SQL chopped off
    * oldest-extracted rows.  Now config-driven (default 4096); operators
    * can bump above 4096 if their corpus grows past that. */
   memory_summary_t sem_summaries[10];
   float sem_scores[10] = { 0 };
   int sem_n = 0;
   if (query_embedding != NULL && embed_dim > 0) {
      const int scan_cap = (g_config.memory.focus_injection.summary_max_scan > 0)
                               ? g_config.memory.focus_injection.summary_max_scan
                               : MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT;
      int rc = memory_db_summary_search_semantic(user_id, query_embedding, (int)embed_dim, since_ts,
                                                 cap, scan_cap, sem_summaries, sem_scores, &sem_n);
      if (rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("summary_adapter: semantic search failed for user %d; keyword-only this turn",
                      user_id);
         sem_n = 0;
      }
   }

   if (kw_n <= 0 && sem_n <= 0)
      return SUCCESS;

   /* Merge by summary id.  O(kw_n * sem_n) is fine at N <= 10 each. */
   summary_merge_entry_t merged[SUMMARY_MERGE_BUFLEN];
   int m = 0;

   for (int i = 0; i < kw_n && m < SUMMARY_MERGE_BUFLEN; i++) {
      if (kw_summaries[i].user_id != user_id) {
         OLOG_ERROR("summary_adapter: keyword summary_id=%lld owned by user_id=%d (expected %d) — "
                    "skipping",
                    (long long)kw_summaries[i].id, kw_summaries[i].user_id, user_id);
         continue;
      }
      merged[m].summary = kw_summaries[i];
      merged[m].score = SUMMARY_KEYWORD_FLOOR_SCORE;
      m++;
   }

   for (int i = 0; i < sem_n && m < SUMMARY_MERGE_BUFLEN; i++) {
      if (sem_summaries[i].user_id != user_id) {
         OLOG_ERROR("summary_adapter: semantic summary_id=%lld owned by user_id=%d (expected %d) — "
                    "skipping",
                    (long long)sem_summaries[i].id, sem_summaries[i].user_id, user_id);
         continue;
      }
      /* Already present from keyword? Promote score to the higher of the
       * two signals (cosine almost always wins on genuine matches). */
      int existing = -1;
      for (int j = 0; j < m; j++) {
         if (merged[j].summary.id == sem_summaries[i].id) {
            existing = j;
            break;
         }
      }
      if (existing >= 0) {
         if (sem_scores[i] > merged[existing].score)
            merged[existing].score = sem_scores[i];
         continue;
      }
      merged[m].summary = sem_summaries[i];
      merged[m].score = sem_scores[i];
      m++;
   }

   if (m <= 0)
      return SUCCESS;

   /* Sort by score descending — insertion sort at N <= 32. */
   for (int i = 1; i < m; i++) {
      summary_merge_entry_t tmp = merged[i];
      int j = i - 1;
      while (j >= 0 && merged[j].score < tmp.score) {
         merged[j + 1] = merged[j];
         j--;
      }
      merged[j + 1] = tmp;
   }

   /* Truncate to requested cap. */
   const int kept = (m > cap) ? cap : m;

   /* Batch provenance lookup. */
   int64_t summary_ids[SUMMARY_MERGE_BUFLEN];
   int64_t conv_ids[SUMMARY_MERGE_BUFLEN] = { 0 };
   int64_t starts[SUMMARY_MERGE_BUFLEN] = { 0 };
   int64_t ends[SUMMARY_MERGE_BUFLEN] = { 0 };
   for (int i = 0; i < kept; i++)
      summary_ids[i] = merged[i].summary.id;
   memory_db_summaries_get_sources(user_id, summary_ids, kept, conv_ids, starts, ends);

   focus_candidate_t *out = calloc((size_t)kept, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("summary_adapter: OOM allocating candidate array (n=%d)", kept);
      return FAILURE;
   }

   bool truncated_warned = false;
   int produced = 0;
   for (int i = 0; i < kept; i++) {
      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "summary",
                                         merged[i].summary.id) != SUCCESS) {
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         return FAILURE;
      }
      const float recency = focus_recency_decay_uniform(merged[i].summary.created_at, now);
      const float importance = merged[i].summary.consolidated ? SUMMARY_IMPORTANCE_CONSOLIDATED
                                                              : SUMMARY_IMPORTANCE_NORMAL;
      if (focus_candidate_init(&out[produced], "memory_summary", FOCUS_SOURCE_INTERNAL,
                               merged[i].summary.summary, item_id, merged[i].summary.created_at,
                               merged[i].score, recency, importance,
                               &truncated_warned) != SUCCESS) {
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         return FAILURE;
      }
      out[produced].provenance.conv_id = conv_ids[i];
      out[produced].provenance.msg_id_start = starts[i];
      out[produced].provenance.msg_id_end = ends[i];
      produced++;
   }

   *out_candidates = out;
   *out_count = produced;
   return SUCCESS;
}

/* =============================================================================
 * Adapter registration
 * ============================================================================= */

static const focus_source_adapter_t k_fact_adapter = {
   .source_id = "memory_fact",
   .source_type = FOCUS_SOURCE_INTERNAL,
   .requires_embedding = true,
   .query = fact_adapter_query,
};

static const focus_source_adapter_t k_entity_adapter = {
   .source_id = "memory_entity",
   .source_type = FOCUS_SOURCE_INTERNAL,
   .requires_embedding = true,
   .query = entity_adapter_query,
};

static const focus_source_adapter_t k_relation_adapter = {
   .source_id = "memory_relation",
   .source_type = FOCUS_SOURCE_INTERNAL,
   .requires_embedding = true,
   .query = relation_adapter_query,
};

static const focus_source_adapter_t k_summary_adapter = {
   .source_id = "memory_summary",
   .source_type = FOCUS_SOURCE_INTERNAL,
   .requires_embedding = false,
   .query = summary_adapter_query,
};

int memory_focus_adapters_register_all(void) {
   if (focus_register_source(&k_fact_adapter) != SUCCESS)
      return FAILURE;
   if (focus_register_source(&k_entity_adapter) != SUCCESS)
      return FAILURE;
   if (focus_register_source(&k_relation_adapter) != SUCCESS)
      return FAILURE;
   if (focus_register_source(&k_summary_adapter) != SUCCESS)
      return FAILURE;
   return SUCCESS;
}
