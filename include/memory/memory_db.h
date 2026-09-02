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
 * Memory Database API
 *
 * Provides CRUD operations for memory facts, preferences, and summaries.
 * Uses the auth_db module's SQLite database and prepared statements.
 * All functions are thread-safe via auth_db's mutex protection.
 *
 * Phase 1a (May 2026) split this header for cohesion: the entity graph
 * surface lives in memory_db_entities.h and fact-embedding storage lives
 * in memory_db_embeddings.h.  Both are included transitively below so
 * existing callers that `#include "memory/memory_db.h"` continue to
 * compile unchanged.
 */

#ifndef MEMORY_DB_H
#define MEMORY_DB_H

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes — defined in memory_types.h so the split sub-headers can
 * reference them without pulling in the memory_db.h umbrella.  Names
 * preserved exactly:
 *   MEMORY_DB_SUCCESS
 *   MEMORY_DB_FAILURE
 *   MEMORY_DB_NOT_FOUND
 *   MEMORY_DB_DUPLICATE
 *
 * Provenance typedef (memory_provenance_t) likewise lives in memory_types.h.
 */

/* =============================================================================
 * Fact Operations
 * ============================================================================= */

/**
 * @brief Create a new memory fact
 *
 * @param user_id User who owns this fact
 * @param fact_text The fact content
 * @param confidence Confidence level (0.0-1.0)
 * @param source Source of fact ("explicit" or "inferred")
 * @param category One of MEMORY_FACT_CATEGORIES; NULL or empty defaults to "general" (v34)
 * @param prov Provenance (source range in conversation); NULL or conv_id==0 = no provenance
 * @param id_out Output: fact ID on success (may be NULL if caller doesn't need it)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_create(int user_id,
                          const char *fact_text,
                          float confidence,
                          const char *source,
                          const char *category,
                          const memory_provenance_t *prov,
                          int64_t *id_out);

/**
 * @brief Create a fact with an explicit created_at timestamp.
 *
 * Same semantics as memory_db_fact_create, except the caller controls the
 * created_at value.  Used by extraction paths that want the new row to
 * inherit the source conversation's creation time so a full reextract
 * preserves the natural temporal spread that recency-based retrieval
 * relies on (LIMIT-by-recency, weight_recency tiebreaks).
 *
 * @param created_at_override Unix epoch seconds. 0 = use time(NULL).
 *
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE.
 */
int memory_db_fact_create_at(int user_id,
                             const char *fact_text,
                             float confidence,
                             const char *source,
                             const char *category,
                             const memory_provenance_t *prov,
                             int64_t created_at_override,
                             int64_t *id_out);

/**
 * @brief Update a fact's category in place (v34).  Used by the embedding-centroid
 * backfill pass and the future LLM recategorize-all admin command.
 *
 * @param fact_id Fact ID
 * @param user_id User ID (ownership check — SQL filter, defense-in-depth)
 * @param category New category (must be one of MEMORY_FACT_CATEGORIES)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_category(int64_t fact_id, int user_id, const char *category);

/**
 * @brief List facts with category='general' for a user, paginated by id.
 *
 * Used by LLM recategorization to fetch batches of uncategorized facts.
 *
 * @param user_id     User ID
 * @param cursor_id   Return facts with id > cursor_id (0 for first batch)
 * @param out_facts   Output array (caller allocates)
 * @param max_facts   Size of out_facts array
 * @param count_out   Output: number of facts fetched
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list_general(int user_id,
                                int64_t cursor_id,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Count non-superseded facts with category='general' for a user.
 *
 * @param user_id User ID
 * @param count_out Output: count of general facts
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_count_general(int user_id, int *count_out);

/**
 * @brief Get a fact by ID, scoped to a user (CWE-639 defense-in-depth).
 *
 * SQL filters on `(id = ? AND user_id = ?)`, so a fact owned by a different
 * user returns MEMORY_DB_NOT_FOUND — same response a legitimately-missing
 * fact would get.  Don't use the response to distinguish the two cases.
 *
 * @param fact_id Fact ID to retrieve
 * @param user_id User ID (ownership check)
 * @param out_fact Output: populated fact structure
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_get(int64_t fact_id, int user_id, memory_fact_t *out_fact);

/**
 * @brief Whether a fact's expires_at currently hides it from retrieval (v58).
 *
 * Mirrors the SQL expiry retrieval guard `(expires_at IS NULL OR expires_at >=
 * now)` for the one retrieval path that cannot use it: the by-id semantic fetch
 * (`memory_db_fact_get` in the vector-only branch of hybrid search), which must
 * return the row so the caller can decide.  Returns true only when
 * `[memory] expire_enabled` is on, @p expires_at is set (non-zero), and it is in
 * the past — so a disabled config or a durable (0/NULL) fact is never hidden.
 *
 * @param expires_at The fact's expires_at (unix seconds; 0 = durable)
 * @return true if the fact should be hidden from retrieval right now
 */
bool memory_db_fact_expiry_hidden(int64_t expires_at);

/**
 * @brief List facts for a user (non-superseded only)
 *
 * @param user_id User ID
 * @param out_facts Output: array of facts (caller allocates)
 * @param max_facts Maximum facts to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of facts returned
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list(int user_id,
                        memory_fact_t *out_facts,
                        int max_facts,
                        int offset,
                        int *count_out);

/**
 * @brief Search facts by keyword
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param out_facts Output: array of matching facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts,
                          int *count_out);

/**
 * @brief BM25-ranked keyword search via FTS5 (v48).
 *
 * Stems @p query with Porter2, builds an FTS5 MATCH expression
 * (`"stem1" OR "stem2" ...`), runs the search, and sigmoid-normalizes
 * each row's raw BM25 score into [0, 1] using a query-length-adaptive
 * curve.  See `memory_bm25_get_params` for the curve.
 *
 * Behavior:
 *   - Empty / NULL query → returns SUCCESS with *count_out=0.
 *   - DB not yet migrated to v48 (statement NULL) → returns SUCCESS
 *     with *count_out=0 (caller should fall back to the LIKE path).
 *   - FTS5 not populated for some rows → those rows are absent from
 *     the result set; the caller's hybrid pipeline still surfaces them
 *     via the vector channel if their embeddings match.
 *
 * Scoring + normalization are adapted from Mem0 (Apache-2.0).  See
 * NOTICE.
 *
 * @param user_id    User to scope results to.
 * @param query      Free-form natural language query.
 * @param out_facts  Output: facts ordered by BM25 score (best first).
 * @param out_scores Output: sigmoid-normalized BM25 scores in [0, 1]
 *                   parallel to @p out_facts.
 * @param max_facts  Capacity of @p out_facts / @p out_scores.
 * @param count_out  Output: number of facts written.
 * @return MEMORY_DB_SUCCESS on success, MEMORY_DB_FAILURE on bad args.
 */
int memory_db_fact_search_bm25(int user_id,
                               const char *query,
                               memory_fact_t *out_facts,
                               float *out_scores,
                               int max_facts,
                               int *count_out);

/**
 * @brief BM25 search restricted to facts created on/after @p since_ts.
 *
 * Same behavior as memory_db_fact_search_bm25 but adds
 * `AND mf.created_at >= since_ts` to the SQL.  Used by focus-injection
 * adapters and time-windowed memory tool actions so windowed queries
 * use BM25 too rather than falling back to the legacy LIKE path.
 *
 * @param since_ts Unix epoch seconds.  0 → no time filter (equivalent
 *                 to calling memory_db_fact_search_bm25).
 * @return MEMORY_DB_SUCCESS on success, MEMORY_DB_FAILURE on bad args.
 */
int memory_db_fact_search_bm25_since(int user_id,
                                     const char *query,
                                     time_t since_ts,
                                     memory_fact_t *out_facts,
                                     float *out_scores,
                                     int max_facts,
                                     int *count_out);

/**
 * @brief Update fact access time, count, and reinforcement boost
 *
 * Called when a fact is retrieved for context injection.
 * Includes time-gated confidence reinforcement (only boosts if
 * last_accessed > 1 hour ago to prevent confidence pinning).
 *
 * @param fact_id Fact ID to update
 * @param user_id User ID (for ownership isolation)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_access(int64_t fact_id, int user_id);

/**
 * @brief Reinforce a fact's confidence because the model CITED it (Phase 2)
 *
 * Bumps confidence by `citation_reinforcement_boost` (ceilinged at 1.0), gated
 * by a 1 h cooldown on the dedicated `last_cited` column (distinct from
 * last_accessed, which the recall path stamps at injection time — see
 * auth_db_statements.c).  A cooldown-blocked or foreign-user call is a legitimate
 * no-op (0 rows changed) and still returns MEMORY_DB_SUCCESS.  Facts only (v1).
 *
 * @param fact_id Fact ID that was cited
 * @param user_id User ID (ownership check — SQL filter, defense-in-depth)
 * @return MEMORY_DB_SUCCESS (incl. cooldown no-op) or MEMORY_DB_FAILURE
 */
int memory_db_fact_reinforce_citation(int64_t fact_id, int user_id);

/**
 * @brief Update fact confidence
 *
 * @param fact_id Fact ID
 * @param user_id User ID (ownership check — SQL filter, defense-in-depth)
 * @param confidence New confidence value
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_confidence(int64_t fact_id, int user_id, float confidence);

/**
 * @brief Set the subject_entity_id FK on an existing fact (v47).
 *
 * Phase 0 — the extraction prompt now requires a `subject` field on every
 * fact, and the extractor resolves that text to an entity_id (creating
 * the entity if needed).  This helper writes the resulting FK back onto
 * the fact row so graph traversal can go fact → entity directly.
 *
 * @param fact_id Fact ID
 * @param user_id User ID (ownership check)
 * @param entity_id Subject entity ID (must already exist in memory_entities)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND (no such fact for user),
 *         or MEMORY_DB_FAILURE
 */
int memory_db_fact_set_subject_entity(int64_t fact_id, int user_id, int64_t entity_id);

/**
 * @brief Set (or clear) a fact's expiry timestamp (v58, C3)
 *
 * Writes memory_facts.expires_at for the given fact.  @p expires_at <= 0 stores
 * NULL (clears expiry → durable).  Scoped to (id, user_id) — CWE-639.  Used by
 * extraction when a transient dated fact is detected; not on the hot path.
 *
 * @param fact_id Fact ID
 * @param user_id Owner user ID
 * @param expires_at Unix seconds after which the fact is hidden/pruned; <=0 clears
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_set_expires_at(int64_t fact_id, int user_id, int64_t expires_at);

/**
 * @brief Link a fact to a saved note (memory→note bridge, v61).
 *
 * Sets memory_facts.note_doc_id so the fact becomes a thin "gloss" pointer to a
 * reference note in the document store.  Glosses stay semantically retrievable
 * but are exempt from paraphrase-dedup, prune, decay, and pattern-delete.  Set
 * once at note-save time; not on the hot path.  CWE-639: (id, user_id) scope.
 *
 * @param fact_id Fact ID
 * @param user_id Owner user ID
 * @param note_doc_id documents.id to point at; <=0 clears the link (NULL)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_set_note_doc_id(int64_t fact_id, int user_id, int64_t note_doc_id);

/**
 * @brief Find the bridge gloss fact for a note (memory→note bridge, v61).
 *
 * @param user_id Owner user ID
 * @param note_doc_id documents.id the gloss points at
 * @param fact_id_out Output: gloss fact id, or 0 if none exists
 * @return MEMORY_DB_SUCCESS (even when none found) or MEMORY_DB_FAILURE
 */
int memory_db_fact_find_by_note_doc_id(int user_id, int64_t note_doc_id, int64_t *fact_id_out);

/**
 * @brief Mark a fact as superseded by another
 *
 * Used when a fact is corrected or updated.  Both fact IDs must belong to
 * the same user (defense-in-depth: foreign-rowid CWE-639 + cross-user
 * pointer prevention — a foreign new_fact_id could otherwise "hide" another
 * user's fact from their own retrieval).  SQL enforces both owners.
 *
 * @param old_fact_id Fact being superseded
 * @param new_fact_id Fact that supersedes it
 * @param user_id     User ID (ownership check for BOTH fact IDs)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_supersede(int64_t old_fact_id, int64_t new_fact_id, int user_id);

/**
 * @brief Delete a fact
 *
 * @param fact_id Fact ID
 * @param user_id User ID (for ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_delete(int64_t fact_id, int user_id);

/**
 * @brief Find similar facts (for duplicate detection)
 *
 * Uses LIKE pattern matching on fact text.
 *
 * @param user_id User ID
 * @param fact_text Text to search for
 * @param out_facts Output: array of similar facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of similar facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_find_similar(int user_id,
                                const char *fact_text,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Find facts by normalized hash (fast duplicate detection)
 *
 * Looks up facts by their normalized text hash for O(1) exact duplicate
 * detection. Hash collisions are expected; callers should verify with
 * Jaccard similarity.
 *
 * @param user_id User ID
 * @param hash Normalized text hash from memory_normalize_and_hash()
 * @param out_facts Output: array of facts with matching hash
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_find_by_hash(int user_id,
                                uint32_t hash,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Prune old superseded facts
 *
 * Deletes facts that have been superseded by newer facts and are older
 * than the retention period.
 *
 * @param user_id User ID
 * @param retention_days Keep superseded facts for this many days
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_prune_superseded(int user_id, int retention_days, int *count_out);

/**
 * @brief Prune expired facts past the recoverable window (v58, C3)
 *
 * Hard-deletes facts whose expires_at passed more than @p retention_days ago —
 * the "hard phase" of fact ephemerality.  The retrieval guard already hides
 * expired facts (soft phase); this reclaims the row after the buffer.
 * @p retention_days = 0 hard-expires on the reference date (no buffer).
 *
 * @param user_id User ID
 * @param retention_days Keep expired facts recoverable for this many days
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_prune_expired(int user_id, int retention_days, int *count_out);

/**
 * @brief Prune stale low-confidence facts
 *
 * Deletes facts that haven't been accessed in a long time and have
 * low confidence scores.
 *
 * @param user_id User ID
 * @param stale_days Prune facts not accessed in this many days
 * @param min_confidence Only prune facts with confidence below this
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_prune_stale(int user_id, int stale_days, float min_confidence, int *count_out);

/**
 * @brief Bulk-delete facts whose `fact_text` matches any of the given LIKE
 * patterns.  Used by the `dawn-admin memory cleanup-meta-facts` admin
 * command to drop interaction-event "meta-facts" (e.g. `User asked%`,
 * `User inquired%`, `User requested%`) that the May 2026 prompt fix now
 * blocks at extraction time but that pre-existing rows still carry.
 *
 * @param user_id User ID (rows scoped to this user only)
 * @param patterns SQL LIKE patterns, ESCAPE '\\' (e.g. "User asked%")
 * @param n_patterns Length of patterns array (1..64)
 * @param dry_run When true, count matches but do not delete
 * @param count_out Output: matched (dry-run) or deleted (commit) count
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_facts_delete_by_patterns(int user_id,
                                       const char *const *patterns,
                                       int n_patterns,
                                       bool dry_run,
                                       int *count_out);

/* =============================================================================
 * Decay and Maintenance Operations (Phase 5)
 * ============================================================================= */

/**
 * @brief Apply confidence decay to all active facts for a user
 *
 * Uses atomic SQL UPDATE with powf() — no C-side row iteration needed.
 * Decay is proportional to time since last_accessed.
 *
 * @param user_id User ID
 * @param inferred_rate Weekly decay multiplier for inferred facts
 * @param explicit_rate Weekly decay multiplier for explicit facts
 * @param inferred_floor Minimum confidence for inferred facts
 * @param explicit_floor Minimum confidence for explicit facts
 * @param count_out Output: number of rows affected
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_apply_fact_decay(int user_id,
                               float inferred_rate,
                               float explicit_rate,
                               float inferred_floor,
                               float explicit_floor,
                               int *count_out);

/**
 * @brief Apply confidence decay to all preferences for a user
 *
 * @param user_id User ID
 * @param pref_rate Weekly decay multiplier for preferences
 * @param pref_floor Minimum confidence for preferences
 * @param count_out Output: number of rows affected
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_apply_pref_decay(int user_id, float pref_rate, float pref_floor, int *count_out);

/**
 * @brief Delete facts with confidence below threshold
 *
 * Logs pruned facts before deletion for audit trail.
 *
 * @param user_id User ID
 * @param threshold Delete facts below this confidence
 * @param count_out Output: number of facts deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_prune_low_confidence(int user_id, float threshold, int *count_out);

/**
 * @brief Delete summaries older than retention period
 *
 * @param user_id User ID
 * @param retention_days Delete summaries older than this
 * @param count_out Output: number of summaries deleted
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_prune_old_summaries(int user_id, int retention_days, int *count_out);

/**
 * @brief Get all user IDs that have memory data
 *
 * @param out_ids Output: array of user IDs (caller allocates)
 * @param max_ids Maximum IDs to return
 * @param count_out Output: number of user IDs found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_all_user_ids(int *out_ids, int max_ids, int *count_out);

/* =============================================================================
 * Date-Filtered Queries
 *
 * Variants of search/list that only return results created at or after
 * a given timestamp. Used for time_range search and fixed recent action.
 * ============================================================================= */

/**
 * @brief Search facts by keyword with time filter
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param since_ts Only return facts created at or after this timestamp
 * @param out_facts Output: array of matching facts
 * @param max_facts Maximum facts to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_search_since(int user_id,
                                const char *keywords,
                                time_t since_ts,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out);

/**
 * @brief Search summaries by keyword with time filter
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param since_ts Only return summaries created at or after this timestamp
 * @param out_summaries Output: array of matching summaries
 * @param max_summaries Maximum to return
 * @param count_out Output: number of matches
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_search_since(int user_id,
                                   const char *keywords,
                                   time_t since_ts,
                                   memory_summary_t *out_summaries,
                                   int max_summaries,
                                   int *count_out);

/**
 * @brief List facts within a time window with explicit sort order
 *
 * Lists facts created within [since_ts, until_ts] with an explicit sort
 * direction.  Used by the LLM 'recent' tool action
 * to support queries like "find my oldest stored memory" (sort_asc=true)
 * or "show me memories from a slice in the past" (until_ts < now).
 *
 * @param user_id   User ID
 * @param since_ts  Lower bound on created_at (inclusive).  Pass 0 for "from
 *                  the beginning of time" (no lower bound).
 * @param until_ts  Upper bound on created_at (inclusive).  Pass 0 for "until
 *                  now" — caller-supplied sentinel resolved internally to
 *                  INT64_MAX so the SAME prepared statement serves both
 *                  windowed and open-ended queries.
 * @param sort_asc  True = ORDER BY created_at ASC (oldest first); false =
 *                  DESC (newest first, the historical default).
 * @param out_facts Output array
 * @param max_facts Max rows to return (caller-supplied; tool layer clamps
 *                  to a defense-in-depth max via the 'limit' param).
 * @param count_out Output: rows returned
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list_window(int user_id,
                               time_t since_ts,
                               time_t until_ts,
                               bool sort_asc,
                               memory_fact_t *out_facts,
                               int max_facts,
                               int *count_out);

/**
 * @brief List summaries within a time window with explicit sort order
 *
 * Mirrors memory_db_fact_list_window — see that function for the rationale.
 */
int memory_db_summary_list_window(int user_id,
                                  time_t since_ts,
                                  time_t until_ts,
                                  bool sort_asc,
                                  memory_summary_t *out_summaries,
                                  int max_summaries,
                                  int *count_out);

/* =============================================================================
 * Preference Operations
 * ============================================================================= */

/**
 * @brief Upsert a preference (insert or update if exists)
 *
 * If a preference with the same category exists for this user,
 * it will be updated with the new value and its reinforcement_count
 * will be incremented.  Latest-source-wins on upsert.
 *
 * @param user_id User ID
 * @param category Preference category
 * @param value Preference value
 * @param confidence Confidence level
 * @param source Source ("explicit" or "inferred")
 * @param prov Provenance; NULL or conv_id==0 = no provenance
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source,
                          const memory_provenance_t *prov);

/**
 * @brief Get a preference by category
 *
 * @param user_id User ID
 * @param category Category to look up
 * @param out_pref Output: populated preference
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_pref_get(int user_id, const char *category, memory_preference_t *out_pref);

/**
 * @brief List all preferences for a user
 *
 * @param user_id User ID
 * @param out_prefs Output: array of preferences
 * @param max_prefs Maximum to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of preferences
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_list(int user_id,
                        memory_preference_t *out_prefs,
                        int max_prefs,
                        int offset,
                        int *count_out);

/**
 * @brief Search preferences by keyword (LIKE on category and value)
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param out_prefs Output: array of matching preferences
 * @param max_prefs Maximum to return
 * @param count_out Output: number of matches
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_search(int user_id,
                          const char *keywords,
                          memory_preference_t *out_prefs,
                          int max_prefs,
                          int *count_out);

/**
 * @brief Delete a preference
 *
 * @param user_id User ID
 * @param category Category to delete
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_pref_delete(int user_id, const char *category);

/* =============================================================================
 * Summary Operations
 * ============================================================================= */

/**
 * @brief Create a conversation summary
 *
 * @param user_id User ID
 * @param session_id Session identifier
 * @param summary Summary text
 * @param topics Comma-separated topics
 * @param sentiment Overall sentiment
 * @param message_count Number of messages in conversation
 * @param duration_seconds Session duration
 * @param prov Provenance; NULL or conv_id==0 = no provenance
 * @param id_out Output: summary ID on success (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_create(int user_id,
                             const char *session_id,
                             const char *summary,
                             const char *topics,
                             const char *sentiment,
                             int message_count,
                             int duration_seconds,
                             const memory_provenance_t *prov,
                             int64_t *id_out);

/**
 * @brief Create a summary with an explicit created_at timestamp.
 *
 * Same semantics as memory_db_summary_create except the caller controls the
 * created_at value.  Used by extraction paths so a full reextract preserves
 * the natural temporal spread that recency-based retrieval relies on
 * (LIMIT-by-recency in semantic + keyword scans, weight_recency tiebreaks).
 *
 * @param created_at_override Unix epoch seconds. 0 = use time(NULL).
 *
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE.
 */
int memory_db_summary_create_at(int user_id,
                                const char *session_id,
                                const char *summary,
                                const char *topics,
                                const char *sentiment,
                                int message_count,
                                int duration_seconds,
                                const memory_provenance_t *prov,
                                int64_t created_at_override,
                                int64_t *id_out);

/**
 * @brief List recent summaries for a user
 *
 * Only returns non-consolidated summaries.
 *
 * @param user_id User ID
 * @param out_summaries Output: array of summaries
 * @param max_summaries Maximum to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of summaries
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_list(int user_id,
                           memory_summary_t *out_summaries,
                           int max_summaries,
                           int offset,
                           int *count_out);

/**
 * @brief Mark a summary as consolidated
 *
 * @param summary_id Summary ID
 * @param user_id    User ID (ownership check — SQL filter, defense-in-depth)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_mark_consolidated(int64_t summary_id, int user_id);

/**
 * @brief Search summaries by keyword
 *
 * Searches both summary text and topics.
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param out_summaries Output: array of matching summaries
 * @param max_summaries Maximum to return
 * @param count_out Output: number of matches
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_search(int user_id,
                             const char *keywords,
                             memory_summary_t *out_summaries,
                             int max_summaries,
                             int *count_out);

/**
 * @brief Delete a summary
 *
 * @param summary_id Summary ID
 * @param user_id User ID (for ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_summary_delete(int64_t summary_id, int user_id);

/* =============================================================================
 * Summary Embedding Operations (v45 — semantic summary adapter)
 * ============================================================================= */

/**
 * @brief Store an embedding vector for a summary row.
 *
 * Mirrors memory_db_fact_update_embedding.  The norm is not stored on
 * summaries — it's recomputed inside the semantic scan because the
 * per-user summary count is small enough that the saving isn't worth
 * an extra column.
 *
 * @param user_id     owning user ID (ownership filter)
 * @param summary_id  summary row ID
 * @param embedding   float vector to store
 * @param dims        vector dimensions (must be > 0)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_FAILURE
 */
int memory_db_summary_update_embedding(int user_id,
                                       int64_t summary_id,
                                       const float *embedding,
                                       int dims);

/* Default scan cap for memory_db_summary_search_semantic.  Sized so a
 * single full reextract event (which clusters every summary's created_at
 * into a narrow window) cannot drop oldest-extracted summaries outside
 * the cosine ranking pool — the original 256 cap broke specifically
 * because of this.  At 4096 × 384-float vectors per row the scan stays
 * sub-millisecond on Jetson; corpora past this size should bump
 * memory.focus_injection.summary_max_scan in config rather than the
 * compile-time default. */
#define MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT 4096

/**
 * @brief Semantic top-N search over user's embedded summaries.
 *
 * Locked single-pass scan: loads each row with an embedding of matching
 * dimensions, scores it against @p query_vec by cosine similarity, and
 * keeps the top-N by score.  Caller passes the embedded query vector;
 * dimension mismatches are silently skipped (the recompute worker will
 * eventually rewrite them on a model swap).
 *
 * Designed for hybrid + keyword merging — the adapter combines this
 * output with the keyword search and re-ranks.
 *
 * @param user_id        owning user ID
 * @param query_vec      pre-computed query embedding (caller owns)
 * @param query_dims     query embedding dimensions
 * @param since_ts       lower bound on created_at (0 = no bound)
 * @param max_summaries  cap on returned summaries.  Internally clamped to
 *                       MEMORY_SUMMARY_SEMANTIC_TOPK_CAP (16) to keep the
 *                       stack-resident ranking buffer small enough for
 *                       the per-turn focus-injection path.
 * @param max_scan       cap on rows actually inspected by the scan
 *                       (defense against pathological per-user counts).
 *                       Pass 0 to use MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT.
 * @param out_summaries  output array of memory_summary_t (caller allocates)
 * @param out_scores     output array of cosine scores aligned with out_summaries
 * @param count_out      number of returned matches
 * @return MEMORY_DB_SUCCESS / MEMORY_DB_FAILURE
 */
int memory_db_summary_search_semantic(int user_id,
                                      const float *query_vec,
                                      int query_dims,
                                      time_t since_ts,
                                      int max_summaries,
                                      int max_scan,
                                      memory_summary_t *out_summaries,
                                      float *out_scores,
                                      int *count_out);

/**
 * @brief List summaries missing an embedding (or with stale dimensions).
 *
 * Used by the recompute worker on model swap.  Returns the row id and
 * summary text — caller embeds the text and stores via
 * memory_db_summary_update_embedding().
 *
 * @param user_id        owning user ID
 * @param expected_dims  the dimensions the active engine produces
 * @param out_ids        output array of summary IDs (caller allocates)
 * @param out_texts      output array of summary text strings;
 *                       caller-allocated `char[max_count][MEMORY_SUMMARY_MAX]`
 * @param max_count      cap on returned rows
 * @param count_out      number of returned rows
 * @return MEMORY_DB_SUCCESS / MEMORY_DB_FAILURE
 */
int memory_db_summary_list_without_embedding(int user_id,
                                             int expected_dims,
                                             int64_t *out_ids,
                                             char (*out_texts)[MEMORY_SUMMARY_MAX],
                                             int max_count,
                                             int *count_out);

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

/**
 * @brief Delete all memories for a user
 *
 * Used when a user requests to be forgotten.
 *
 * @param user_id User ID
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_delete_user_memories(int user_id);

/**
 * @brief Get memory statistics for a user
 *
 * @param user_id User ID
 * @param out_stats Output: statistics
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_stats(int user_id, memory_stats_t *out_stats);

/* =============================================================================
 * Extraction Tracking
 * ============================================================================= */

/**
 * @brief Get last extracted message count for a conversation
 *
 * Used to track which messages have already been processed for
 * memory extraction, enabling incremental extraction.
 *
 * @param conversation_id Conversation ID
 * @param count_out Output: last extracted message count (0 if never extracted)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_last_extracted(int64_t conversation_id, int *count_out);

/**
 * @brief Set last extracted message count and message-ID cursor for a conversation
 *
 * Atomically clears the recovery counters (`extraction_attempts = 0`,
 * `extraction_last_attempt_at = 0`) and advances `last_extracted_msg_id`
 * to the caller-supplied value (0 = leave unchanged, used on early-skip paths).
 * Passing the cursor value captured *before* LLM inference avoids the TOCTOU
 * race of re-querying MAX(id) at commit time.
 *
 * @param conversation_id Conversation ID
 * @param message_count Message count to record (legacy count-based cursor)
 * @param last_msg_id Highest message ID processed; 0 = do not advance cursor
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_set_last_extracted(int64_t conversation_id, int message_count, int64_t last_msg_id);

/**
 * @brief Roll back an extraction_attempts stamp for a conversation.
 *
 * Decrements `conversations.extraction_attempts` to `MAX(0, ... - 1)` and
 * resets `extraction_last_attempt_at` to 0.  Called by the recovery /
 * reextract orchestrator when an extraction failed for a transient reason
 * (cloud unreachable, etc.) — without rollback, the attempt counter would
 * keep climbing on transient errors and shelve the conversation after
 * `max_attempts` hits even though the LLM never actually rejected the
 * request.
 *
 * @param conversation_id Conversation ID to roll back
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_undo_extraction_attempt(int64_t conversation_id);

/**
 * @brief Get the last-extracted message ID cursor for a conversation (v40).
 *
 * Returns the highest message.id that was included in the most recent successful
 * extraction.  0 = never extracted.  Role-agnostic (counts all message types).
 *
 * @param conversation_id Conversation ID
 * @param msg_id_out Output: last extracted message ID (0 if never extracted)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_last_extracted_msg_id(int64_t conversation_id, int64_t *msg_id_out);

/* memory_db_facts_get_sources / memory_db_fact_get_source — see
 * memory/memory_db_provenance.h.  Phase B (May 2026) split provenance readers
 * into their own module so future relation/summary/preference variants live
 * alongside the fact variants. */

#ifdef __cplusplus
}
#endif

/* Phase 1a (May 2026) sub-headers — pulled in transitively so existing
 * callers that `#include "memory/memory_db.h"` see the full surface
 * unchanged.  Sub-headers may also be included directly when a file
 * uses only one slice (entity-graph or fact-embedding storage).
 *
 * Note: the includes deliberately sit OUTSIDE the `extern "C" { ... }` block
 * above — each sub-header carries its own `extern "C"` linkage guard, so
 * nesting them here would produce redundant (legal but noisy) extern blocks.
 *
 * `memory_db_provenance.h` is intentionally NOT in this umbrella — provenance
 * readers are an opt-in module, not part of the core memory_db surface. */
#include "memory/memory_db_embeddings.h"
#include "memory/memory_db_entities.h"

#endif /* MEMORY_DB_H */
