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
 * Focus-source framework — pluggable adapter contract for per-turn context
 * injection.  Phase 1b of the Dynamic Context Injection workstream
 * (`docs/DYNAMIC_CONTEXT_INJECTION_DESIGN.md`).
 *
 * Architecture:
 *   - Each candidate context source (memory facts, entities, relations,
 *     summaries, document chunks, calendar events, recent emails, future
 *     DAWN background context) registers a `focus_source_adapter_t` at
 *     daemon init.
 *   - `focus_compose()` queries every registered adapter, runs an
 *     unconditional filter-on-retrieval pass over each returned candidate's
 *     text, ranks survivors with a three-factor formula
 *     (semantic × recency × importance × source-weight), trims to top_k +
 *     min_score + token-budget, and returns the result set.
 *   - The framework owns user_id + include_private at the contract level
 *     (positional parameters; type-system enforced) so adapters cannot
 *     forget privacy scoping.
 *
 * Intentional drift from rev-2 design doc — call out for adapter authors:
 *   - `source_type` is a typed enum here, NOT a `const char *`.  WebSocket
 *     payload (Phase 1g) will serialize back to "internal" / "external" /
 *     "user-content" strings so the design doc JSON schema stays accurate.
 *   - Adapter MAY pre-filter candidate text, but the framework ALWAYS
 *     re-runs `memory_filter_check()` on every candidate before including
 *     it in the result set.  Defense in depth — correctness must not
 *     depend on adapter discipline.
 *
 * Layer: 1.  Header pulls only `memory_types.h` + libc.  Implementation
 * depends on `core/memory_filter.h` (Layer 1 leaf — blocklist + Unicode
 * normalize, no upstream deps).  Adapters that link against L2/L3 modules
 * (document_search, calendar, email) register polymorphically via function
 * pointer — the framework has zero link-time dependency on any adapter's
 * source.  Bench / L2-only binaries link the framework and register only
 * their L2 adapters.
 *
 * Thread safety: registration is mutex-protected and expected only at
 * daemon init.  After init completes, the registry is read-only and
 * `focus_compose()` is reentrant + lock-free for iteration — multiple
 * session threads call it concurrently safely.
 */

#ifndef MEMORY_FOCUS_SOURCE_H
#define MEMORY_FOCUS_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of adapters that may register.  Linear-array registry
 * iterated on every `focus_compose()` call; N is small (~8-10 in
 * production).  Fixed cap avoids dynamic alloc on the hot path. */
#define MAX_FOCUS_SOURCES 16

/* Sentinel for `focus_candidate_t.semantic_score` /
 * `focus_candidate_t.recency_score` when the source doesn't compute that
 * dimension.  The ranker treats NA as a 0.0 contribution (not as a
 * negative weighted by w_sem / w_rec, which would penalize undated or
 * non-semantic items).  importance_score has no NA path — every adapter
 * MUST emit a meaningful 0.0-1.0 importance value. */
#define FOCUS_SCORE_NA (-1.0f)

/* =============================================================================
 * Source classification — typed enum drives UI default-expansion (1g) and
 * per-source ranking lookups.
 * ============================================================================= */
typedef enum {
   FOCUS_SOURCE_INTERNAL,    /* memory facts/entities/relations/summaries,
                                DAWN background context */
   FOCUS_SOURCE_EXTERNAL,    /* document chunks, calendar events */
   FOCUS_SOURCE_USER_CONTENT /* email body, future external feeds —
                                potentially-malicious user-controlled text */
} focus_source_type_t;

/* =============================================================================
 * Candidate emitted by an adapter.
 *
 * Memory ownership: adapter mallocs `text` and `item_id` and the
 * containing `focus_candidate_t` array.  Framework takes ownership on
 * adapter SUCCESS — `focus_result_free()` releases everything that
 * survives ranking, and the framework frees rejected/dropped candidates
 * inline as it walks the working pool.
 *
 * `item_id` MUST be opaque / server-generated — never user-controlled
 * content.  Used for dedup keying in 1f; an attacker-controlled item_id
 * would let user-injected text influence dedup behavior.
 * ============================================================================= */
/**
 * @brief Single candidate emitted by an adapter for a focus-injection turn.
 *
 * See the block comment above for full ownership and `item_id` opacity
 * contracts.  Note: the framework overwrites `source_id` with the
 * adapter's registered static string after copy — one adapter ==
 * one source_id.  Adapters that need to emit heterogeneous source-id
 * categories must register multiple adapters.
 */
typedef struct {
   const char *source_id; /* Set by adapter; framework collapses to the
                             registered adapter's static string after copy.
                             NOT heap, NOT freed by framework. */
   focus_source_type_t source_type;
   float semantic_score;           /* Cosine or other 0..1 sim score;
                                     FOCUS_SCORE_NA if not applicable */
   float recency_score;            /* 0..1 from time-decay;
                                     FOCUS_SCORE_NA if not applicable */
   float importance_score;         /* 0..1, source-specific (confidence,
                                     explicit-flag, etc.); always meaningful */
   time_t item_timestamp;          /* Original event/creation time, used as
                                     tie-breaker in the ranker */
   char *text;                     /* HEAP — framework owns / frees */
   char *item_id;                  /* HEAP — framework owns / frees;
                                     opaque, server-generated */
   memory_provenance_t provenance; /* Source back-link (conv_id +
                                     msg_id_start/end) — zeroed if not
                                     applicable */
} focus_candidate_t;

/* =============================================================================
 * Adapter contract
 *
 * `user_id` and `include_private` are MANDATORY positional parameters —
 * type-system enforced.  The framework cannot invoke an adapter without
 * scoping; adapters cannot accidentally drop privacy filtering.
 *
 * `query_text` is the user's raw turn text (or NULL on non-turn-driven
 * triggers like session-start).  Adapters that need keyword fallback when
 * embedding confidence is low (calendar, email subject) consume this;
 * memory adapters typically ignore it.
 *
 * `query_embedding` is NULL when no embedding is available for this turn
 * (e.g., embedding subsystem disabled).  The framework will skip adapters
 * with `requires_embedding=true` when `query_embedding=NULL`; adapters
 * with `requires_embedding=false` may receive NULL and must handle it.
 *
 * `max_candidates` is a hint, not a contract — the framework defensively
 * caps at the same value on the survivor side.  Adapters that return more
 * have the excess silently discarded (with a per-call DEBUG log).
 *
 * Return semantics:
 *   SUCCESS (0): `*out_candidates` points to a heap-allocated array of
 *                `*out_count` entries; framework takes ownership of
 *                array + per-candidate `text`/`item_id` allocations.
 *                Adapter is permitted to return zero candidates with
 *                `*out_candidates = NULL` and `*out_count = 0` (and
 *                still SUCCESS).
 *   FAILURE (1): Adapter MUST free any partial allocation it made,
 *                set `*out_candidates = NULL` and `*out_count = 0`,
 *                then return.  Framework will not attempt cleanup on
 *                FAILURE — leak prevention is the adapter's contract.
 * ============================================================================= */
/**
 * @brief Adapter callback: query a single source for candidates.
 *
 * Full contract documented in the block comment above.  `user_id` /
 * `include_private` are positional and mandatory by type — adapters
 * cannot drop privacy scoping.  See the SUCCESS / FAILURE return
 * semantics for the heap-ownership contract.
 *
 * @return SUCCESS or FAILURE per the contract above.
 */
typedef int (*focus_source_query_fn)(int user_id,
                                     bool include_private,
                                     const char *query_text,
                                     const float *query_embedding,
                                     size_t embed_dim,
                                     time_t now,
                                     int max_candidates,
                                     focus_candidate_t **out_candidates,
                                     int *out_count);

/**
 * @brief Adapter registration record.
 *
 * Caller retains ownership of this struct and the strings it points to —
 * static / global allocation expected.  The registry stores the pointer,
 * never copies, so the struct must outlive the framework.
 *
 * `source_id` MUST be a stable static string (the framework's
 * rejection-counter relies on string-identity-by-content; see
 * `rejection_bump` in `focus_source.c`).
 */
typedef struct {
   const char *source_id; /* Stable static string — registry stores
                             pointer, never copies */
   focus_source_type_t source_type;
   bool requires_embedding; /* If true, adapter is skipped when
                               `query_embedding == NULL` */
   focus_source_query_fn query;
} focus_source_adapter_t;

/* =============================================================================
 * Per-source filter rejection counter — surfaced via the result struct
 * for the UI warning chip (1g) and metrics.
 * ============================================================================= */
typedef struct {
   const char *source_id;
   int count;
} focus_filter_rejection_t;

/* =============================================================================
 * Per-candidate score breakdown — Phase 1g-i.
 *
 * The ranker computes a final score from four contributions; the WebSocket
 * `context_injection` event surfaces the breakdown so the UI can show
 * users WHY each item appeared.  Lives in a parallel array on
 * `focus_compose_result_t` rather than as fields on `focus_candidate_t`
 * — adapters allocate candidates and the framework cannot inject extra
 * fields into adapter-owned structs without breaking the L2 contract.
 *
 * Each `*_contribution` is the ALREADY-WEIGHTED contribution
 * (`w_X * raw_score`), so the four sum to `final_score`.  UI consumers
 * don't need to know the weights.
 *
 * `applied_source_weight` is the raw per-source weight (pre-`w_src`
 * multiply) that the ranker looked up — useful for diagnostics ("why
 * did calendar rank below memory_fact?").
 *
 * ============================================================================= */
typedef struct {
   float semantic_contribution;   /* w_sem * candidate.semantic_score (NA→0) */
   float recency_contribution;    /* w_rec * candidate.recency_score   (NA→0) */
   float importance_contribution; /* w_imp * candidate.importance_score */
   float source_contribution;     /* w_src * applied_source_weight */
   float final_score;             /* sum of the four — what the ranker ordered by */
   float applied_source_weight;   /* raw per-source weight, pre-w_src multiply */
} focus_score_breakdown_t;

/* =============================================================================
 * Result of a `focus_compose()` call.  `focus_result_free()` releases
 * `candidates` (including each candidate's `text` + `item_id`) AND the
 * parallel `score_breakdowns` array.  `rejections[]` holds counts only
 * — no heap state.
 *
 * INVARIANT (1g-i): `score_breakdowns[i]` describes `candidates[i]` for
 * every i in [0, candidate_count).  The arrays MUST be compacted
 * in lockstep by any consumer that drops candidates (e.g.,
 * apply_dedup_locked).
 * ============================================================================= */
/* Tagged so a forward declaration in non-memory headers (e.g.,
 * webui/webui_server.h's broadcast prototype) can refer to the type
 * without dragging this file into them. */
typedef struct focus_compose_result_s {
   focus_candidate_t *candidates; /* HEAP — sorted by final_score desc */
   int candidate_count;
   focus_score_breakdown_t *score_breakdowns; /* HEAP — parallel to candidates[] */
   focus_filter_rejection_t rejections[MAX_FOCUS_SOURCES];
   int rejection_count;
} focus_compose_result_t;

/* =============================================================================
 * Per-call trim-limit overrides (focus_compose_ex)
 *
 * The per-turn focus path (build_focus_block) uses the config-driven trim limits
 * in `g_config.memory.focus_injection`.  The unified `recall` tool needs a
 * larger, separate budget for a deep cross-source gather.  `focus_limits_t`
 * overrides ONLY the three trim values; a NULL pointer or a zero/negative field
 * falls back to the config value for that field (item_bytes has no config
 * counterpart: unset, a candidate costs its full text).  Ranking WEIGHTS are NOT
 * overridable here — they stay config-sourced and shared, so recall changes how
 * MUCH is kept, not HOW it is ranked.
 * ============================================================================= */
typedef struct {
   int top_k;        /* > 0 overrides focus_injection.top_k              */
   float min_score;  /* >= 0 overrides focus_injection.min_score         */
   int budget_bytes; /* > 0 overrides focus_injection.focus_budget_bytes */
   /* Non-NULL: the bytes the caller will show for a candidate, charged
    * against the budget instead of its full text (recall shows each item
    * as a capped line with a pointer to the rest). */
   int (*item_bytes)(const focus_candidate_t *c);
} focus_limits_t;

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Register a source adapter with the framework.
 *
 * Expected once per source at daemon init.  After all expected registers
 * complete, the registry is treated as read-only and `focus_compose()` is
 * lock-free for iteration.
 *
 * Returns FAILURE if `adapter`/`adapter->source_id`/`adapter->query` is
 * NULL, if the registry is full (cap = `MAX_FOCUS_SOURCES`), or if
 * another adapter with the same `source_id` is already registered (no
 * silent overwrite).
 *
 * @param adapter Adapter pointer — caller retains ownership of the
 *                struct and the strings it points to (must outlive the
 *                framework).  Static / global allocation expected.
 * @return SUCCESS on registration, FAILURE on duplicate / full / NULL.
 */
int focus_register_source(const focus_source_adapter_t *adapter);

/**
 * @brief Query every registered adapter and return a ranked + filtered
 *        result set.
 *
 * Pipeline:
 *   1. For each adapter:
 *        - Skip if `requires_embedding && query_embedding == NULL`.
 *        - Call adapter with all positional parameters.
 *        - Reject candidates whose text matches the memory injection
 *          filter — gated by source trust tier.  INTERNAL items are
 *          filtered at extraction-time ingestion (see memory_filter.h
 *          call sites in memory_extraction.c / memory_callback.c /
 *          llm_silent_observe.c / webui_memory.c).  EXTERNAL items
 *          are user-trusted (uploaded docs / authenticated calendar
 *          accounts) — filtering at retrieval would false-positive on
 *          the user's own content.  Only USER_CONTENT (potentially-
 *          attacker-controlled inbound feeds like email body) passes
 *          through the retrieval-time filter check.
 *        - Defensively cap survivors per adapter at `per_source_max_candidates`.
 *   2. Rank survivors by `final_score = w_sem*sem + w_rec*rec +
 *      w_imp*imp + w_src*source_weight(source_id)`.  Sort descending;
 *      ties broken by `item_timestamp` descending.
 *   3. Drop everything below `g_config.memory.focus_injection.min_score`.
 *   4. Trim to `g_config.memory.focus_injection.top_k`.
 *   5. Truncate to fit `g_config.memory.focus_injection.focus_budget_bytes`
 *      (exact byte cost = `strlen(text)` per candidate).
 *
 * Empty registry returns SUCCESS with `candidate_count=0`,
 * `rejection_count=0` — production state in 1b before adapters land in
 * 1c/1d.
 *
 * @param user_id Authenticated user — passed to every adapter
 * @param include_private Per-session "include items from private
 *                        conversations" flag
 * @param query_text User's raw turn text (NULL for session-start path)
 * @param query_embedding Embedding of the query, or NULL if not available
 * @param embed_dim Embedding dimensions; ignored if `query_embedding == NULL`
 * @param now Current time (passed in so tests can pin it)
 * @param per_source_max_candidates Hint for adapters; framework caps too
 * @param out_result Caller-allocated struct; framework populates
 * @return SUCCESS on a fully-completed compose pass, FAILURE on
 *         allocation error.  Per-adapter FAILUREs are logged but do not
 *         fail the compose pass — the rest of the registry is still
 *         consulted.
 */
int focus_compose(int user_id,
                  bool include_private,
                  const char *query_text,
                  const float *query_embedding,
                  size_t embed_dim,
                  time_t now,
                  int per_source_max_candidates,
                  focus_compose_result_t *out_result);

/**
 * @brief Like focus_compose(), but with per-call trim-limit overrides.
 *
 * Identical pipeline to focus_compose(); the only difference is steps 3-5 (the
 * min_score / top_k / byte-budget trim) consult `limits` first, falling back to
 * `g_config.memory.focus_injection` per field when `limits` is NULL or a field
 * is zero/negative.  Ranking (step 2) is unchanged — weights are always config.
 *
 * The unified `recall` tool uses this for a deep cross-source gather at a budget
 * larger than the per-turn injection.  focus_compose() is a thin wrapper passing
 * `limits = NULL`.
 *
 * @param limits Per-call trim overrides, or NULL to use config for all three.
 * @see focus_compose for all other parameter semantics.
 * @return SUCCESS on success, FAILURE on error (same contract as focus_compose).
 */
int focus_compose_ex(int user_id,
                     bool include_private,
                     const char *query_text,
                     const float *query_embedding,
                     size_t embed_dim,
                     time_t now,
                     int per_source_max_candidates,
                     const focus_limits_t *limits,
                     focus_compose_result_t *out_result);

/**
 * @brief Release everything `focus_compose()` allocated into `result`.
 *
 * Idempotent: calling on a zeroed/already-freed result is a no-op.
 * Resets `result->candidates = NULL`, `candidate_count = 0`, and
 * `rejection_count = 0` so the struct is safe to reuse.
 *
 * @param result Result struct previously populated by `focus_compose()`
 */
void focus_result_free(focus_compose_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FOCUS_SOURCE_H */
