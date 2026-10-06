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
 * Memory Embeddings API
 *
 * Provider-agnostic embedding generation and semantic search for the
 * memory system. Supports ONNX local inference (default), Ollama, and
 * OpenAI-compatible HTTP endpoints.
 */

#ifndef MEMORY_EMBEDDINGS_H
#define MEMORY_EMBEDDINGS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "core/embedding_engine.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum embedding dimensions (all-MiniLM = 384, OpenAI ada = 1536) */
#define MAX_EMBEDDING_DIMS 2048

_Static_assert(MAX_EMBEDDING_DIMS * sizeof(float) <= 8192, "Embedding stack buffer exceeds 8KB");

/* Hybrid search result */
typedef struct {
   int64_t fact_id;
   float score; /* Combined hybrid score */
} embedding_search_result_t;

/**
 * @brief Initialize the embedding system
 *
 * Selects and initializes the configured provider. Call once at startup.
 *
 * @return 0 on success, non-zero on failure (non-fatal — search falls back to keyword)
 */
int memory_embeddings_init(void);

/**
 * @brief Shut down the embedding system
 *
 * Joins the backfill thread and releases provider resources.
 */
void memory_embeddings_cleanup(void);

/**
 * @brief Check if embeddings are available
 *
 * @return true if a provider is initialized and ready
 */
bool memory_embeddings_available(void);

/**
 * @brief Get the embedding dimension for the current provider
 *
 * @return Number of dimensions, or 0 if not initialized
 */
int memory_embeddings_dims(void);

/**
 * @brief Generate an embedding for text
 *
 * @param text Input text to embed
 * @param out Output float array (must hold at least MAX_EMBEDDING_DIMS)
 * @param out_dims Output: actual dimensions written
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed(const char *text, float *out, int *out_dims);

/**
 * @brief Generate embedding and store it for a fact
 *
 * Convenience function: embeds text and writes to DB in one call.
 *
 * @param user_id User who owns the fact (for ownership check)
 * @param fact_id Fact ID to update
 * @param text Fact text to embed
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text);

/**
 * @brief Store a pre-embedded vector for a fact, then update the in-memory
 * cache directly (instead of invalidating it).
 *
 * Companion to memory_embeddings_nearest_fact() in the extraction-time
 * paraphrase-dedup loop.  The caller embeds the fact text once for both
 * dedup scoring and storage, avoiding the second embed call that
 * memory_embeddings_embed_and_store() would otherwise pay.  This variant
 * also appends to the per-user fact cache instead of invalidating it, so
 * an N-fact extraction loop does not pay N cache-reload cycles against
 * SQLite.
 *
 * If cache append fails (out of memory growing it, or a user mismatch), the
 * function falls back to invalidating the cache so the next access reloads
 * fresh.
 *
 * @param user_id User who owns the fact (for ownership check on the DB write)
 * @param fact_id Fact ID to update — the row must already exist
 * @param vec Pre-computed embedding vector
 * @param dims Vector dimensions (must match memory_embeddings_dims())
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_store_precomputed(int user_id, int64_t fact_id, const float *vec, int dims);

/**
 * @brief Pre-warm the per-user fact embedding cache.
 *
 * Loads (or refreshes if dirty) the cache for @p user_id.  The dedup loop
 * in memory_extraction.c calls this once at the top of
 * process_extraction_response(), so the first nearest_fact() lookup hits
 * a warm cache rather than paying one-time DB-load latency on the
 * extraction worker thread.  Idempotent — already-valid same-user cache
 * is a no-op.
 *
 * @param user_id User ID whose fact cache to load
 * @return 0 on success (cache is now warm), non-zero on failure
 */
int memory_embeddings_warm_cache(int user_id);

/**
 * @brief Find the highest-scoring fact-cache match against a precomputed
 * embedding, returning early on the first match >= @p threshold.
 *
 * Designed for the extraction-time paraphrase-dedup gate: the gate just
 * needs ANY match above threshold (not the absolute best), so we exit on
 * first hit instead of walking the entire cache.  The caller embeds the
 * fact text OUTSIDE any lock; this function acquires the cache mutex
 * internally for the cosine scan only, keeping the critical section
 * sub-millisecond at the dev's ~1200-fact scale.
 *
 * Treats facts without embeddings (norm == 0) as no-match — silently
 * skipped rather than failing the call.
 *
 * @param user_id User ID whose fact cache to scan
 * @param query_vec Pre-computed query embedding
 * @param query_dims Query embedding dimensions (must match cache dims)
 * @param threshold Minimum cosine score to count as a match (e.g. 0.92)
 * @param matched_id_out Output: fact_id of the match, or 0 if no match
 * @param score_out Output: cosine score of the match, or 0.0f if no match
 * @return MEMORY_DB_SUCCESS (with @p matched_id_out=0 if no match), or
 *         MEMORY_DB_FAILURE on cache load error / dim mismatch
 */
int memory_embeddings_nearest_fact(int user_id,
                                   const float *query_vec,
                                   int query_dims,
                                   float threshold,
                                   int64_t *matched_id_out,
                                   float *score_out);

/* Max advisory neighbors the write-time 'remember' band lookup returns.  Sized
 * to span a full duplicate cluster (>= the find_duplicates per-cluster cap) so a
 * fresh save next to an existing dup pile surfaces all of them, not a truncation. */
#define MEMORY_BAND_NEIGHBORS_MAX 12

/**
 * @brief Find existing facts whose embedding falls in a cosine BAND against a
 * precomputed query vector: @p low <= cosine < @p high.  Top-@p max by score.
 *
 * Backs the interactive 'remember' write-time dedup advisory.  The lower bound
 * filters unrelated facts; the upper bound excludes near-identical matches that
 * the extraction-time paraphrase gate (paraphrase_dedup_threshold) already
 * auto-merges — pass that threshold as @p high so the band stays in sync.  Same
 * lock-and-scan shape as memory_embeddings_nearest_fact() (single O(N) scan, lock
 * held only for the scan); facts without embeddings (norm < 1e-6) are skipped.
 *
 * Results are returned in descending-score order.  @p out_ids / @p out_scores are
 * CALLER-ALLOCATED parallel arrays of at least @p max entries — no free contract.
 * Unlike memory_embeddings_find_duplicate_clusters(), this does NOT clamp @p low /
 * @p high — the caller owns bound validation (e.g. defaulting @p high to
 * MEMORY_PARAPHRASE_DEDUP_DEFAULT when the config threshold is out of range).
 *
 * @param user_id    User whose fact cache to scan.
 * @param query_vec  Pre-computed query embedding (caller embeds outside any lock).
 * @param query_dims Query embedding dimensions (must match cache dims).
 * @param low        Inclusive lower cosine bound (e.g. 0.80).
 * @param high       Exclusive upper cosine bound (e.g. paraphrase_dedup_threshold).
 * @param out_ids    Caller-allocated, >= @p max entries; matched fact IDs.
 * @param out_scores Caller-allocated, >= @p max entries; matched cosine scores.
 * @param max        Capacity of the out arrays (<= MEMORY_BAND_NEIGHBORS_MAX).
 * @param out_count  Set to the number of neighbors returned (>= 0); zeroed on error.
 * @return MEMORY_DB_SUCCESS (including the 0-neighbor case), or MEMORY_DB_FAILURE
 *         on cache load error / dim mismatch.
 */
int memory_embeddings_band_neighbors(int user_id,
                                     const float *query_vec,
                                     int query_dims,
                                     float low,
                                     float high,
                                     int64_t *out_ids,
                                     float *out_scores,
                                     int max,
                                     int *out_count);

/**
 * @brief Pure top-K band filter over caller-supplied embeddings.
 *
 * No cache/DB access — exposed for unit testing.  Collects facts with
 * @p low <= cosine < @p high against @p query_vec, keeping the top-@p max by
 * descending score (insertion-sorted window, drops the weakest).  Facts with
 * @p norms[i] < 1e-6 are skipped.  @p out_ids / @p out_scores are caller-
 * allocated parallel arrays of at least @p max entries — no free contract.
 *
 * @param ids        Fact IDs, parallel to @p embs / @p norms (length @p count).
 * @param embs       Flat row-major embeddings (@p count * @p dims floats).
 * @param norms      Pre-computed L2 norms, parallel to @p ids.
 * @param count      Number of facts.
 * @param dims       Embedding dimensionality.
 * @param query_vec  Query embedding (length @p dims).
 * @param query_norm Pre-computed L2 norm of @p query_vec.
 * @param low        Inclusive lower cosine bound.
 * @param high       Exclusive upper cosine bound.
 * @param out_ids    Caller-allocated, >= @p max entries; matched fact IDs.
 * @param out_scores Caller-allocated, >= @p max entries; matched cosine scores.
 * @param max        Capacity of the out arrays.
 * @param out_count  Set to the number of neighbors returned; zeroed on error.
 * @return MEMORY_DB_SUCCESS (including the 0-neighbor case), or MEMORY_DB_FAILURE
 *         on bad args.
 */
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
                                          int *out_count);

/**
 * @brief Perform hybrid keyword + vector search
 *
 * Combines keyword search scores with vector cosine similarity.
 * Falls back to keyword-only if embeddings are unavailable.
 *
 * @param user_id User ID
 * @param query Search query text
 * @param keyword_facts Pre-searched keyword results (fact IDs)
 * @param keyword_scores Keyword scores per fact (from multi_token_fact_search)
 * @param keyword_count Number of keyword results
 * @param token_count Number of search tokens (for score normalization)
 * @param out_results Output: sorted hybrid results
 * @param max_results Maximum results to return
 * @return Number of results
 */
int memory_embeddings_hybrid_search(int user_id,
                                    const char *query,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results);

/**
 * @brief Variant of memory_embeddings_hybrid_search that accepts an optional
 *        pre-computed query embedding to avoid re-embedding on the hot path.
 *
 * Efficiency M3 (May 2026): callers that have already embedded the query for
 * another retrieval step (e.g., entity rescore, graph re-rank) can thread the
 * (vec, norm) pair through to skip the second ONNX inference (~15 ms saved on
 * bge-small INT8 / Jetson per call).
 *
 * If @p query_emb is NULL or @p query_norm < 1e-6f, the function falls back to
 * embedding the query string internally — preserving the exact behavior of
 * memory_embeddings_hybrid_search().  Dim is taken from embedding_engine_dims().
 *
 * @param user_id User ID
 * @param query Search query text (still used for temporal-expression parsing)
 * @param query_emb Optional pre-computed query embedding (NULL → compute internally)
 * @param query_norm Optional pre-computed L2 norm (paired with @p query_emb)
 * @param keyword_facts Pre-searched keyword results (fact IDs)
 * @param keyword_scores Keyword scores per fact (from multi_token_fact_search)
 * @param keyword_count Number of keyword results
 * @param token_count Number of search tokens (for score normalization)
 * @param out_results Output: sorted hybrid results
 * @param max_results Maximum results to return
 * @return Number of results
 */
int memory_embeddings_hybrid_search_ex(int user_id,
                                       const char *query,
                                       const float *query_emb,
                                       float query_norm,
                                       const int64_t *keyword_facts,
                                       const int *keyword_scores,
                                       int keyword_count,
                                       int token_count,
                                       embedding_search_result_t *out_results,
                                       int max_results);

/**
 * @brief Reciprocal Rank Fusion search — parallel-channel alternative to
 *        `memory_embeddings_hybrid_search`.
 *
 * Builds three independent rank lists from the same candidate pool:
 *   - **Semantic**: cached embedding cosine vs. query embedding
 *   - **Keyword**: caller-supplied multi-token match scores
 *   - **Temporal**: gaussian proximity from `time_query_parse` (only when
 *     the query carries a parseable temporal expression)
 *
 * Each fact's final score is `Σ K/(K + rank_i)` (K=60) over channels in
 * which it has a non-zero raw score — canonical RRF `1/(K+rank)` scaled
 * by K so per-channel max ≈ 0.984, matching the [0, ~1] scale that
 * `search_score_floor` (default 0.30) expects.  Multi-channel hits can
 * stack above 1.0.  Facts ranked in no channel get score 0 and are
 * dropped.  Top-`max_results` returned, sorted by RRF score desc.
 *
 * Drop-in replacement for `memory_embeddings_hybrid_search` — same
 * signature; callers gate at `g_config.memory.rrf_enabled`.
 *
 * Empirical basis: Cormack/Clarke/Buettcher 2009 (k=60 canonical).
 * Mem0 v2 and Hindsight TEMPR both cite RRF over parallel channels as
 * their primary retrieval lever.
 *
 * @param user_id User ID
 * @param query Search query text
 * @param keyword_facts Pre-searched keyword results (fact IDs)
 * @param keyword_scores Keyword scores per fact (multi-token match counts)
 * @param keyword_count Number of keyword results
 * @param token_count Number of search tokens (for score normalization)
 * @param out_results Output: sorted RRF results
 * @param max_results Maximum results to return
 * @return Number of results
 */
int memory_embeddings_rrf_search(int user_id,
                                 const char *query,
                                 const int64_t *keyword_facts,
                                 const int *keyword_scores,
                                 int keyword_count,
                                 int token_count,
                                 embedding_search_result_t *out_results,
                                 int max_results);

/**
 * @brief Variant of memory_embeddings_rrf_search that accepts an optional
 *        pre-computed query embedding — same contract as
 *        memory_embeddings_hybrid_search_ex.
 *
 * @param query_emb Optional pre-computed query embedding (NULL → compute internally)
 * @param query_norm Optional pre-computed L2 norm (paired with @p query_emb)
 */
int memory_embeddings_rrf_search_ex(int user_id,
                                    const char *query,
                                    const float *query_emb,
                                    float query_norm,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results);

/** Sentinel score returned by `memory_embeddings_rescore_against_query`
 * for facts that could not be scored (no embedding in the per-user cache,
 * embedding engine unavailable, query embedding failed).  Callers MUST
 * treat any score equal to this sentinel as "skip this fact" — do NOT
 * merge into the LLM-facing pool.  Negative-infinity (per IEEE 754
 * behavior with insertion sort) is sufficient and distinct from any
 * legitimate score value, but `-1.0f` is human-readable in logs and
 * works with `score < 0.0f` checks. */
#define MEMORY_EMBEDDINGS_RESCORE_SENTINEL (-1.0f)

/**
 * @brief Phase 2 Step 1: re-score a caller-supplied list of facts against a
 *        pre-computed query embedding using cosine similarity + an optional
 *        additive bonus.
 *
 * Used by graph retrieval to apply query-relevance scoring to entity-bounded
 * candidate facts, replacing the legacy flat `entity_grounding_bonus` that
 * was disconnected from query content.
 *
 * Scoring formula (per fact):
 *
 *   if fact_id is in the per-user embedding cache:
 *     score = vec_weight * cosine(query_emb, fact_emb) + entity_bonus
 *   else:
 *     score = MEMORY_EMBEDDINGS_RESCORE_SENTINEL  // caller MUST drop
 *
 * `vec_weight` is read from `g_config.memory.embedding_vector_weight` so the
 * scale stays consistent with `memory_embeddings_hybrid_search`.
 *
 * Taking a pre-computed embedding (rather than re-embedding the query)
 * avoids a duplicate ONNX inference per `memory_action_search` call —
 * `memory_fact_search_hybrid` already embedded the same query string
 * upstream; passing the embedding through saves 5-30 ms on bge-small
 * INT8 / Jetson.
 *
 * The keyword term from the hybrid formula is deliberately omitted: graph
 * candidates are entity-grounded by construction, so the kw signal is
 * largely redundant with the entity-graph membership.  Cosine carries the
 * topical-relevance signal cleanly.
 *
 * **Caller contract**: facts whose returned score equals
 * `MEMORY_EMBEDDINGS_RESCORE_SENTINEL` MUST be dropped from the merged
 * pool.  Earlier versions silently substituted `entity_bonus` for such
 * facts, which produced low-quality LLM-facing candidates and caused a
 * -9pp Phase 2 Step 1 regression (May 14, 2026 architecture audit).
 *
 * If @p query_emb is NULL or the embedding engine is unavailable, EVERY
 * fact gets the sentinel — the caller should treat this as "graph
 * retrieval is silently disabled this turn" and rely on hybrid alone.
 *
 * Thread-safe: takes the embedding cache lock internally.
 *
 * @param user_id Owner user_id (for cache scoping)
 * @param query_emb Caller-supplied query embedding (size must be
 *                  `embedding_engine_dims()`).  NULL → all sentinels.
 * @param query_norm Pre-computed L2 norm of @p query_emb (saves recomputation).
 * @param entity_bonus Caller-supplied additive bonus (typically 0.0)
 * @param fact_ids Caller-allocated array of fact IDs to score (in)
 * @param fact_count Number of facts
 * @param out_scores Caller-allocated parallel array (out) — overwritten;
 *                   sentinel means "drop this fact"
 * @return SUCCESS or FAILURE (FAILURE only on NULL out_scores / NULL fact_ids).
 */
int memory_embeddings_rescore_against_query(int user_id,
                                            const float *query_emb,
                                            float query_norm,
                                            float entity_bonus,
                                            const int64_t *fact_ids,
                                            int fact_count,
                                            float *out_scores);

/**
 * @brief Invalidate the fact embedding cache (e.g., after store/delete)
 */
void memory_embeddings_invalidate_cache(void);

/**
 * @brief Generate embedding and store it for an entity
 *
 * @param entity_id Entity ID to update
 * @param user_id User ID (for ownership check)
 * @param text Entity name text to embed
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text);

/**
 * @brief Generate embedding and store it for a summary row.
 *
 * Convenience wrapper paralleling memory_embeddings_embed_and_store for
 * facts.  Used by the live extractor (immediately after a successful
 * memory_db_summary_create) and the summarize-missing backfill worker so
 * every summary row lands with an embedding without a deferred recompute
 * pass.  No-op if the embedding engine is unavailable — the recompute
 * worker will pick it up on next boot.
 *
 * @param user_id     owning user ID
 * @param summary_id  summary row ID
 * @param text        summary text to embed
 * @return SUCCESS on store, FAILURE on embed or DB error
 */
int memory_embeddings_embed_and_store_summary(int user_id, int64_t summary_id, const char *text);

/**
 * @brief Invalidate every user's entity embedding cache
 *
 * Lock-free: safe to call while holding the database lock.
 */
void memory_embeddings_invalidate_entity_cache(void);

/**
 * @brief Invalidate one user's entity embedding cache (after changing their
 *        entities); other users' copies stay.  Lock-free.
 */
void memory_embeddings_invalidate_entity_cache_for_user(int user_id);

/** Most matches memory_embeddings_entity_matches() returns. */
#define MEMORY_ENTITY_MATCH_MAX 64

/** An entity relevant to a query. */
typedef struct {
   int64_t id;
   char name[MEMORY_ENTITY_NAME_MAX];
   char type[MEMORY_ENTITY_TYPE_MAX];
   bool has_cosine; /**< false without a usable query embedding */
   float cosine;
   float relevance; /**< cosine measured from the pool's typical level (see
                         embedding_corpus_relevance) */
   bool named;      /**< the query contains the entity's name */
} memory_entity_match_t;

/**
 * @brief The user's entities relevant to a query, most relevant first
 *
 * Every canonical entity is considered.  One is relevant when either:
 *   - the query names it: two of its name's content words appear in @p query,
 *     or the one word of a one-word name (a long message's embedding is
 *     diluted, but the name is still there).  Longest match wins: an entity
 *     whose matched words all belong to a longer named match isn't named
 *     (the query meant the longer one);
 *   - its relevance reaches @p min_relevance.  Embedding models put
 *     unrelated text at a model-specific baseline similarity, so a raw cosine
 *     floor doesn't transfer; relevance is measured from the pool's mean.
 *     A pool smaller than EMBEDDING_RELEVANCE_MIN_POOL isn't gated, and
 *     @p min_relevance <= 0 turns the gate off.
 * Named entities rank first, then by cosine.  Only entities with an embedding
 * from the current model are held, so one still awaiting its embedding
 * (after a model change, until the recompute worker reaches it) isn't found.
 *
 * @param user_id        Whose entities
 * @param query          Query text (for names), or NULL
 * @param qvec           Query embedding, or NULL (names only)
 * @param dims           Its dimension; a mismatch with the stored embeddings
 *                       is treated as no embedding
 * @param min_relevance  Relevance an unnamed entity needs (entity_min_relevance)
 * @param out            [out] The matches
 * @param max            Capacity of @p out (<= MEMORY_ENTITY_MATCH_MAX)
 * @param n_out          [out] Matches written
 * @return SUCCESS or FAILURE
 */
int memory_embeddings_entity_matches(int user_id,
                                     const char *query,
                                     const float *qvec,
                                     int dims,
                                     float min_relevance,
                                     memory_entity_match_t *out,
                                     int max,
                                     int *n_out);

/**
 * @brief Invalidate both fact and entity embedding caches in one call.
 *
 * Helper for paths that drop or replace memory state in bulk
 * (delete_user_memories, snapshot reload, bench reset).  Safer than calling
 * the two cache-specific invalidators separately — adding a third cache later
 * only requires updating this helper, not every call site.
 */
void memory_embeddings_invalidate_all(void);

/**
 * @brief Search entities by semantic similarity and name
 *
 * The entities memory_embeddings_entity_matches() finds for @p query, gated
 * at the configured entity_min_relevance, optionally of one type.  Unlike
 * context injection, a pool too small to gate returns only named entities:
 * similarity with no baseline says nothing, and the caller falls back to a
 * keyword search.
 *
 * @param user_id User ID
 * @param query Search query
 * @param type_filter Optional entity type filter (NULL for all)
 * @param out_ids Output: entity IDs
 * @param out_names Output: entity names
 * @param out_types Output: entity types
 * @param out_scores Output: cosine similarity scores
 * @param max_results Maximum results
 * @return Number of results
 */
int memory_embeddings_entity_search(int user_id,
                                    const char *query,
                                    const char *type_filter,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_scores,
                                    int max_results);

/**
 * @brief Score a precomputed query embedding against a single entity's cached
 * embedding.
 *
 * Loads the per-user entity cache on demand (same RAM-resident pool used by
 * memory_embeddings_entity_search) and returns the cosine in @p out_cosine
 * if the entity is found.  Returns FAILURE if the entity is not in the cache
 * (no embedding stored, or — once Ckpt 3's loader filter ships — an alias
 * row with canonical_id IS NOT NULL).  Caller treats FAILURE as "no signal"
 * and contributes 0 to the embedding-cosine term in the alias-merge composite.
 *
 * Used by the v43 alias resolver Stage 4 (see memory_db_alias.c).  Operates
 * on the same cache as memory_embeddings_entity_search() so the resolver does
 * not pay a DB hit per candidate.
 *
 * @param user_id User ID (selects which cache to use)
 * @param entity_id Entity to score
 * @param query_embedding Pre-computed query embedding
 * @param query_dims Dimensions of query embedding (must match cache dims)
 * @param query_norm Pre-computed L2 norm of query embedding
 * @param out_cosine Output: cosine similarity in [-1, 1]; populated only on SUCCESS
 * @return SUCCESS, or FAILURE if entity_id is not in the cache or dims mismatch
 */
int memory_embeddings_entity_cosine(int user_id,
                                    int64_t entity_id,
                                    const float *query_embedding,
                                    int query_dims,
                                    float query_norm,
                                    float *out_cosine);

/** out_rel value for a fact that has no cached embedding (NaN: no relevance
 *  value can equal it; test with isnan()). */
#define MEMORY_RELEVANCE_NA NAN

/** Most ids memory_embeddings_fact_relevance() scores per call. */
#define MEMORY_RELEVANCE_MAX_IDS 64

/**
 * @brief Corpus-relative relevance of facts to a query
 *
 * embedding_corpus_relevance() of each fact against the query's cosines over all
 * of the user's cached fact embeddings: how far a fact stands out from the user's
 * typical fact, which (unlike raw cosine) is comparable across embedding models.
 *
 * @param user_id   Owner of the facts
 * @param query_emb Query embedding (dims = engine dims)
 * @param ids       Facts to score
 * @param n         Number of ids (<= MEMORY_RELEVANCE_MAX_IDS)
 * @param out_rel   Per-id relevance, or MEMORY_RELEVANCE_NA if not cached
 * @param pool_out  Facts the mean was taken over (may be NULL)
 * @return SUCCESS, or FAILURE if the cache can't be loaded
 */
int memory_embeddings_fact_relevance(int user_id,
                                     const float *query_emb,
                                     const int64_t *ids,
                                     int n,
                                     float *out_rel,
                                     int *pool_out);

/* Compute L2 norm of a float vector */
float memory_embeddings_l2_norm(const float *vec, int dims);

/* Compute cosine similarity using pre-computed norms */
float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b);

/* Compute cosine similarity (computes norms internally) */
float memory_embeddings_cosine(const float *a, const float *b, int dims);

/* =============================================================================
 * Duplicate-fact clustering (backs the 'find_duplicates' memory tool action)
 * ============================================================================= */
#define MEMORY_DUP_MAX_CLUSTERS 20
#define MEMORY_DUP_MAX_PER_CLUSTER 12

typedef struct {
   int64_t ids[MEMORY_DUP_MAX_PER_CLUSTER];
   int count;            /* facts in this cluster (always >= 2) */
   float min_similarity; /* lowest seed-to-member cosine in the cluster */
} memory_dup_cluster_t;

/**
 * @brief Find clusters of near-duplicate facts by embedding cosine.
 *
 * Snapshots the fact embedding cache under its mutex, then clusters on the copy
 * so the O(N^2) scan never holds the cache lock (which gates the paraphrase-dedup
 * gate and the recompute worker).  @p out_clusters is CALLER-ALLOCATED (at least
 * @p max_clusters entries) — no free contract.  @p threshold in (0,1]; values
 * <= 0 or > 1 default to 0.85.  Sets *@p out_count to the number of clusters
 * found (each of size >= 2).  Returns MEMORY_DB_SUCCESS / MEMORY_DB_FAILURE.
 *
 * @param user_id      User whose fact cache to scan.
 * @param threshold    Cosine similarity threshold in (0,1]; <= 0 or > 1 -> 0.85.
 * @param out_clusters Caller-allocated array of at least @p max_clusters entries.
 * @param max_clusters Maximum clusters to emit; the scan early-stops when reached.
 * @param out_count    Set to the number of clusters found (>= 0); zeroed on error.
 * @return MEMORY_DB_SUCCESS (including the 0-cluster case) or MEMORY_DB_FAILURE.
 */
int memory_embeddings_find_duplicate_clusters(int user_id,
                                              float threshold,
                                              memory_dup_cluster_t *out_clusters,
                                              int max_clusters,
                                              int *out_count);

/**
 * @brief Pure greedy clusterer over caller-supplied embeddings.
 *
 * No cache/DB access — exposed for unit testing.  Greedy: each unvisited fact
 * (skip norm < 1e-6) seeds a cluster of all later facts with cosine >= threshold;
 * emits clusters of size >= 2; early-stops at @p max_clusters.
 *
 * @param ids         Fact IDs, parallel to @p embs / @p norms (length @p count).
 * @param embs        Flat row-major embeddings (@p count * @p dims floats).
 * @param norms       Pre-computed L2 norms, parallel to @p ids.
 * @param count       Number of facts.
 * @param dims        Embedding dimensionality.
 * @param threshold   Cosine similarity threshold for cluster membership.
 * @param out         Caller-allocated array of at least @p max_clusters entries.
 * @param max_clusters Maximum clusters to emit (early-stop bound).
 * @param out_count   Set to the number of clusters found; zeroed on error.
 * @return MEMORY_DB_SUCCESS, or MEMORY_DB_FAILURE on bad args / OOM.
 */
int memory_embeddings_cluster_by_cosine(const int64_t *ids,
                                        const float *embs,
                                        const float *norms,
                                        int count,
                                        int dims,
                                        float threshold,
                                        memory_dup_cluster_t *out,
                                        int max_clusters,
                                        int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EMBEDDINGS_H */
