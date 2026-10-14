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
 * @file memory_graph_retrieval.h
 * @brief Entity-graph candidate retrieval for memory.search and per-turn
 *        context (via memory_search_graph_candidates).
 *
 * Parallel candidate source to `memory_fact_search_hybrid()` for cat-3
 * style entity-specific queries — "What country did Jolene buy snake
 * Seraphim?", "Which meat does Audrey prefer?" — where cosine over-
 * broadens on the noun token ("country", "meat") and the gold answer
 * sits at rank 50+.  Closes the LoCoMo cat-3 retrieval gap that
 * remained after the May 2026 shipped retrieval surface.
 *
 * Phase 1A walks fact-linked relations only — relations with non-NULL
 * `fact_id`.  Phase 0 (May 13, 2026) paired-output schema guarantees
 * ~100% fact↔relation linkage on new extractions, so the fact-linked
 * subset is now effectively the full graph for fresh memory.
 *
 * Phase 2 (multi-hop traversal) was scoped, scaffolded, and reverted
 * during the May 14, 2026 session: the 43.8% additional reach measured
 * at hop-2 in the diagnostic turned out to be a measurement artifact
 * of extraction linking facts to peripheral entities, not genuine
 * graph-distance multi-hop questions on LoCoMo.  Production scaffolding
 * was stripped; the design (and the rationale for the strip) is captured
 * in docs/PHASE_2_GRAPH_RETRIEVAL_DESIGN.md.  Phase 2 production code
 * will re-land if a benchmark surfaces genuine multi-hop test cases.
 *
 * Pipeline:
 *   1. Seeds: the entities the query names by any of their names
 *      (memory_embeddings_entity_matches), then its capitalized words that
 *      are an entity's exact name; each as its class (an alias's canonical).
 *   2. For each seed, the facts about it (memory_db_fact_ids_for_subject,
 *      any name in its class) and the facts its relations came from
 *      (`memory_db_relation_fact_ids_for_entity()`, any name too), in turn.  Cap fan-out
 *      per query at `[memory.graph_retrieval].max_facts_per_query`; a seed
 *      that finds little passes its share on.
 *   3. Dedupe fact_ids and fetch the facts.  The caller scores them
 *      against the query (memory_embeddings_rescore_against_query).
 *
 * Layer: 2 (alongside memory_fact_search.h).  Pulls memory_types,
 * memory_db_entities, memory_embeddings (named entities) for query-time
 * entity resolution.  No LLM, no logging beyond OLOG.
 *
 * Thread safety: stateless module functions; each DB call acquires
 * the auth_db mutex internally.  Safe to call from any worker.
 */

#ifndef MEMORY_GRAPH_RETRIEVAL_H
#define MEMORY_GRAPH_RETRIEVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum proper-noun candidates extracted from a single query.  Bounds
 * stack usage in the extractor (each candidate is ~64 bytes of name +
 * scratch).  LoCoMo cat-3 questions average ~1.5 proper nouns; 8 is
 * comfortable headroom. */
#define MEMORY_GRAPH_MAX_SEED_CANDIDATES 8

/** Intermediate graph-candidate pool size used by `memory_action_search`,
 * the parallel bench path and per-turn context (the fact focus adapter).
 * Caller's working buffer must be at least this large.  Query-aware scoring
 * widens the candidate pool past the 10-slot LLM-facing cap so
 * topically-relevant graph candidates can compete with hybrid hits before the
 * final truncate.  30 matches `[memory.graph_retrieval] max_facts_per_query`
 * default; bump together if the per-query cap rises. */
#define MEMORY_GRAPH_INTERMEDIATE_CAP 30

/**
 * @brief The seed entities for @p query: those it names, then proper-noun
 *        candidates resolved against the user's `memory_entities` table.
 *
 * Named first: every entity the query names by any of its names, multi-word
 * names and aliases included (memory_embeddings_entity_matches, names only);
 * these are class canonicals.  Needs the entity cache (embeddings on).
 *
 * Then heuristic candidate extraction:
 *   - Capitalized 3+ letter tokens (alphanumeric, apostrophe and hyphen
 *     allowed inside).
 *   - First-token question-starter filter (What/Why/How/Who/When/Where/
 *     Which/Would/Could/Should/Is/Are/Was/Were/Did/Does/Do/Has/Have/Had/
 *     Will) — skipped because they're sentence-initial and never proper
 *     nouns in question context.
 *   - 100% coverage on LoCoMo cat-3 questions per the May 2026 profile.
 *
 * Resolution: each candidate goes through `memory_db_entity_root_by_name`
 * (exact canonical match, as its class: an alias seeds its canonical, so a
 * class is one seed).  Candidates that match no entity are dropped.
 *
 * @param user_id User ID
 * @param query Raw query text (NULL or empty → SUCCESS with count=0)
 * @param out_entity_ids Caller-allocated array of size @p max
 * @param max Cap (typically MEMORY_GRAPH_MAX_SEED_CANDIDATES)
 * @param out_count Receives actual seed count, in [0, @p max]
 * @return SUCCESS or FAILURE.  An empty seed set is SUCCESS, not failure.
 */
int memory_graph_extract_seed_entities(int user_id,
                                       const char *query,
                                       int64_t *out_entity_ids,
                                       int max,
                                       int *out_count);

/**
 * @brief Expand 1 hop from seed entities: the facts about each and the facts
 *        its relations came from.
 *
 * For each seed, memory_db_fact_ids_for_subject (any name in its class) and
 * `memory_db_relation_fact_ids_for_entity`, taken in turn within the seed's
 * share of @p max (what is left, split over the seeds still to go).  Dedupes
 * fact_ids across seeds, fetches each fact via `memory_db_fact_get`
 * (user-scoped in SQL) and drops expired and superseded ones.
 * Unscored: the caller scores them against the query.
 *
 * @param user_id User ID (defense-in-depth)
 * @param seed_entity_ids Array of seed entity IDs from
 *                        memory_graph_extract_seed_entities
 * @param seed_count Number of seeds (0 → SUCCESS with count=0)
 * @param out_facts Caller-allocated array of size @p max
 * @param max Total fan-out cap across all seeds
 * @param out_count Receives actual returned fact count, in [0, @p max]
 * @return SUCCESS or FAILURE
 */
int memory_graph_expand_fact_linked(int user_id,
                                    const int64_t *seed_entity_ids,
                                    int seed_count,
                                    memory_fact_t *out_facts,
                                    int max,
                                    int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_GRAPH_RETRIEVAL_H */
