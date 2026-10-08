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
 *   - memory_entity    (requires_embedding=false) — the entities the query
 *                      names or clearly resembles, over the user's whole
 *                      entity pool, merged by entity_id with a whole-query
 *                      name search; importance boosts on photo ownership
 *                      and being named
 *   - memory_relation  (requires_embedding=false) — the top-3 relevant
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

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
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
 * include_private: the parameter is accepted from the framework but is
 * a no-op.  hybrid_search scopes by user_id; the conversation-private
 * boundary is enforced at the conv_db_* level only, not at memory_facts.
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
   (void)include_private; /* No-op — see header comment above */
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
    * memory tool's `search` action.  Focus facts go verbatim into the turn's
    * context (just before the user's message), so the marginal-cosine
    * fabrication surface is identical to the tool path.  Single knob
    * (g_config.memory.search_score_floor) keeps tool-time and injection-time semantics aligned. */
   kept = memory_search_apply_score_floor(facts, scores, kept, g_config.memory.search_score_floor);
   if (kept <= 0)
      return SUCCESS;

   /* Relevance gate for injection only (the memory tool's search is unaffected).
    * The search floor above sits near the embedding model's unrelated-text
    * baseline, so it lets through facts that don't relate to the turn (an
    * arithmetic question pulling in an anniversary).  Keep facts that stand out
    * from the user's typical fact for this query; facts with no embedding yet
    * matched on keywords and are kept.  Needs enough facts for a baseline. */
   const float min_rel = g_config.memory.focus_injection.fact_min_relevance;
   if (min_rel > 0.0f && query_embedding != NULL && embed_dim > 0) {
      int64_t ids[10];
      float rel[10];
      int pool = 0;
      for (int i = 0; i < kept; i++)
         ids[i] = facts[i].id;
      if (memory_embeddings_fact_relevance(user_id, query_embedding, ids, kept, rel, &pool) ==
              SUCCESS &&
          pool >= EMBEDDING_RELEVANCE_MIN_POOL) {
         int k2 = 0;
         for (int i = 0; i < kept; i++) {
            if (isnan(rel[i]) || rel[i] >= min_rel) {
               facts[k2] = facts[i];
               scores[k2] = scores[i];
               k2++;
            }
         }
         kept = k2;
         if (kept <= 0)
            return SUCCESS;
      }
   }

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
 * Entity adapter (named or clearly similar entities)
 *
 * source_id          = "memory_entity"
 * source_type        = FOCUS_SOURCE_INTERNAL
 * requires_embedding = false (entities the query names are found without one)
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

/* Path A — the whole query as a substring of a name (aliases included).
 * Each match contributes ENTITY_IMPORTANCE_NAMEMATCH. */
static void add_keyword_entities(int user_id,
                                 const char *query_text,
                                 int cap,
                                 entity_rank_entry_t *rows,
                                 int *row_count,
                                 int work_cap) {
   memory_entity_t *kw = calloc((size_t)cap, sizeof(*kw));
   int kw_n = 0;
   if (!kw || memory_db_entity_search(user_id, query_text, kw, cap, &kw_n) != MEMORY_DB_SUCCESS) {
      free(kw);
      return;
   }
   for (int i = 0; i < kw_n && *row_count < work_cap; i++) {
      /* Defense-in-depth user_id post-check (entity_search scopes already;
       * this guards against future regression). */
      if (kw[i].user_id != user_id) {
         OLOG_ERROR("entity_adapter: entity_id=%lld owned by user_id=%d (expected %d) — skipping",
                    (long long)kw[i].id, kw[i].user_id, user_id);
         continue;
      }
      entity_rank_entry_t *r = &rows[(*row_count)++];
      r->entity_id = kw[i].id;
      r->entity = kw[i];
      r->semantic_score = FOCUS_SCORE_NA;
      r->importance_increment = ENTITY_IMPORTANCE_NAMEMATCH;
      r->name_matched = true;
   }
   free(kw);
}

/* Path B — every entity the query names or clearly resembles
 * (memory_embeddings_entity_matches: the user's whole entity pool, gated by
 * entity_min_relevance), merged with path A by id.  The full records come
 * from one batched lookup by id. */
static void add_matched_entities(int user_id,
                                 const char *query_text,
                                 const float *query_embedding,
                                 int cap,
                                 entity_rank_entry_t *rows,
                                 int *row_count,
                                 int work_cap) {
   memory_entity_match_t matches[MEMORY_ENTITY_MATCH_MAX];
   int n = 0;
   const bool semantic = query_embedding != NULL && memory_embeddings_available();
   if (memory_embeddings_entity_matches(user_id, query_text, semantic ? query_embedding : NULL,
                                        semantic ? memory_embeddings_dims() : 0,
                                        g_config.memory.focus_injection.entity_min_relevance,
                                        matches, cap, &n) != SUCCESS ||
       n == 0) {
      return; /* the keyword path still stands */
   }
   int64_t fetch_ids[MEMORY_ENTITY_MATCH_MAX];
   int n_fetch = 0;
   for (int i = 0; i < n; i++) {
      /* Scores are 0..1; FOCUS_SCORE_NA is a negative sentinel. */
      const float cosine = matches[i].has_cosine ? fmaxf(matches[i].cosine, 0.0f) : FOCUS_SCORE_NA;
      const int slot = entity_rank_find(rows, *row_count, matches[i].id);
      if (slot < *row_count) {
         /* Found by path A too: keep max(semantic_score). */
         if (rows[slot].semantic_score == FOCUS_SCORE_NA || cosine > rows[slot].semantic_score) {
            rows[slot].semantic_score = cosine;
         }
      } else {
         fetch_ids[n_fetch++] = matches[i].id;
      }
   }
   if (n_fetch == 0) {
      return;
   }
   memory_entity_t *found = calloc((size_t)n_fetch, sizeof(*found));
   int n_found = 0;
   if (!found || memory_db_entities_get_by_ids(user_id, fetch_ids, n_fetch, found, &n_found) !=
                     MEMORY_DB_SUCCESS) {
      free(found);
      return;
   }
   /* In match order (most relevant first), so a full work set drops the
    * least relevant. */
   for (int i = 0; i < n && *row_count < work_cap; i++) {
      const memory_entity_match_t *m = &matches[i];
      const memory_entity_t *e = NULL;
      for (int j = 0; j < n_found; j++) {
         if (found[j].id == m->id) {
            e = &found[j];
            break;
         }
      }
      if (!e || entity_rank_find(rows, *row_count, m->id) < *row_count) {
         continue; /* deleted since it was cached, or merged above */
      }
      entity_rank_entry_t *r = &rows[(*row_count)++];
      r->entity_id = e->id;
      r->entity = *e;
      r->semantic_score = m->has_cosine ? fmaxf(m->cosine, 0.0f) : FOCUS_SCORE_NA;
      r->importance_increment = m->named ? ENTITY_IMPORTANCE_NAMEMATCH : 0.0f;
      r->name_matched = m->named;
   }
   free(found);
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
   (void)include_private; /* No-op, as in the fact adapter */
   (void)embed_dim;       /* Engine reports its own dim */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_text == NULL || query_text[0] == '\0')
      return SUCCESS;

   const int cap = (max_candidates > MEMORY_ENTITY_MATCH_MAX) ? MEMORY_ENTITY_MATCH_MAX
                                                              : max_candidates;
   /* Working set bounded by `2 * cap` (worst case: keyword and vector
    * paths return disjoint top-cap sets). */
   const int work_cap = cap * 2;
   entity_rank_entry_t *rows = calloc((size_t)work_cap, sizeof(*rows));
   if (rows == NULL) {
      OLOG_ERROR("entity_adapter: OOM allocating rank workspace");
      return FAILURE;
   }
   int row_count = 0;

   add_keyword_entities(user_id, query_text, cap, rows, &row_count, work_cap);
   add_matched_entities(user_id, query_text, query_embedding, cap, rows, &row_count, work_cap);

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
 * requires_embedding = false (a named entity is a subject without one)
 *
 * Pipeline:
 *   1. Take the top-RELATION_TOP_SUBJECTS relevant entities
 *      (memory_embeddings_entity_matches: named first, then by cosine,
 *      gated by entity_min_relevance).  None relevant, no relations.
 *   2. Round-robin allocate `max_candidates` slots across subjects,
 *      with the first `max_candidates % n_subjects` subjects (in
 *      similarity-desc order) getting one extra slot.
 *   3. For each subject, call `memory_db_relation_list_by_subject_at`
 *      with `as_of_ts = now` so only currently-valid relations
 *      surface (bitemporal filter handled by memory_db).
 *   4. Render "Subject Verb Object[ since YYYY-MM]".
 *   5. Batch provenance via `memory_db_relations_get_sources`.
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
 * mention_count.  Per-row rendering here would show "Alex working_on Acme ×3"
 * once per alias instead of "Alex working_on Acme ×N" rolled up.  Display-
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
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_text == NULL || query_text[0] == '\0')
      return SUCCESS;

   /* Subjects: the entities the query names or clearly resembles, most
    * relevant first (memory_embeddings_entity_matches).  None relevant, no
    * relations. */
   memory_entity_match_t matches[RELATION_TOP_SUBJECTS];
   int n_matches = 0;
   const bool semantic = query_embedding != NULL && memory_embeddings_available();
   if (memory_embeddings_entity_matches(user_id, query_text, semantic ? query_embedding : NULL,
                                        semantic ? memory_embeddings_dims() : 0,
                                        g_config.memory.focus_injection.entity_min_relevance,
                                        matches, RELATION_TOP_SUBJECTS, &n_matches) != SUCCESS ||
       n_matches == 0) {
      return SUCCESS;
   }
   rel_subject_t subjects[RELATION_TOP_SUBJECTS] = { 0 };
   const int subject_count = n_matches;
   for (int i = 0; i < n_matches; i++) {
      subjects[i].entity_id = matches[i].id;
      safe_strscpy(subjects[i].entity.name, matches[i].name);
      subjects[i].cosine = matches[i].has_cosine ? fmaxf(matches[i].cosine, 0.0f) : FOCUS_SCORE_NA;
   }

   /* Round-robin allocation: floor + remainder distributed in
    * similarity-desc order.  Determinism matters for tests + bench. */
   const int floor_per = max_candidates / subject_count;
   const int remainder = max_candidates % subject_count;
   int per_subject_quota[RELATION_TOP_SUBJECTS] = { 0 };
   for (int i = 0; i < subject_count; i++)
      per_subject_quota[i] = floor_per + (i < remainder ? 1 : 0);

   /* Hard refuse work above the static array cap — keeps the stack
    * arrays defensible without a runtime allocation.  Today the
    * upstream config_validate.c clamps top_k at 64, so this branch is
    * unreachable; the static_assert above pins the relationship. */
   if (max_candidates > RELATION_ADAPTER_MAX_CANDIDATES_CAP) {
      OLOG_ERROR("relation_adapter: max_candidates=%d exceeds cap %d — refusing", max_candidates,
                 RELATION_ADAPTER_MAX_CANDIDATES_CAP);
      return FAILURE;
   }

   /* Working buffer sized to max_candidates. */
   focus_candidate_t *out = calloc((size_t)max_candidates, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("relation_adapter: OOM allocating candidate array");
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
   (void)include_private; /* No-op, as in the fact adapter */
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
   memory_summary_pool_t pool = { 0 };
   if (query_embedding != NULL && embed_dim > 0) {
      const int scan_cap = (g_config.memory.focus_injection.summary_max_scan > 0)
                               ? g_config.memory.focus_injection.summary_max_scan
                               : MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT;
      int rc = memory_db_summary_search_semantic(user_id, query_embedding, (int)embed_dim, since_ts,
                                                 cap, scan_cap, sem_summaries, sem_scores, &sem_n,
                                                 &pool);
      if (rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("summary_adapter: semantic search failed for user %d; keyword-only this turn",
                      user_id);
         sem_n = 0;
      }
   }

   if (kw_n <= 0 && sem_n <= 0)
      return SUCCESS;

   /* Relevance gate for summaries found only by meaning.  The semantic search
    * always returns its top ten, related or not; like facts, entities and
    * documents, a summary must stand out from the user's typical summary for
    * this query: relevance = (cos - pool_mean) / (1 - pool_mean), measured over
    * the summaries actually scored.  A keyword match keeps its floor.  A pool
    * too small for a baseline isn't gated. */
   const float min_rel = g_config.memory.focus_injection.summary_min_relevance;
   const bool gated = min_rel > 0.0f && pool.scored >= EMBEDDING_RELEVANCE_MIN_POOL;

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
      if (gated &&
          embedding_corpus_relevance(sem_scores[i], pool.cosine_sum, pool.scored) < min_rel) {
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
   .requires_embedding = false,
   .query = entity_adapter_query,
};

static const focus_source_adapter_t k_relation_adapter = {
   .source_id = "memory_relation",
   .source_type = FOCUS_SOURCE_INTERNAL,
   .requires_embedding = false,
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
