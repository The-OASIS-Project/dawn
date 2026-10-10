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
 * Internal interface for memory_callback.c.  Exposes the per-action search
 * and recent helpers so the bench harness can drive the production memory
 * retrieval path with explicit user_id and source_budget overrides — without
 * routing through memoryCallback's value-string parsing.  Not part of the
 * public memory API; do not include outside memory_callback.c and the bench.
 */

#ifndef MEMORY_CALLBACK_INTERNAL_H
#define MEMORY_CALLBACK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* 'list_merged': the facts merged into (or replaced by) a fact, by its ID, or
 * those whose text contains @p query, or the newest (memory_callback_restore.c). */
char *memory_action_list_merged(int user_id, const char *query);

/* 'restore': bring a merged or replaced fact back by ID (memory_db_fact_restore). */
char *memory_action_restore(int user_id, const char *query);

/**
 * @brief Search memory facts/preferences/summaries/relations.
 *
 * @param user_id Owner of the memory store
 * @param keywords Search query (NULL or empty returns an error string)
 * @param since_ts If > 0, restricts to items created on/after this timestamp
 * @param category Optional fact category filter (NULL = all)
 * @param as_of_ts For relation validity (0 = now)
 * @param include_historical Bypass relation validity filter
 * @param with_source Append verbatim source excerpts to each fact
 * @param source_budget Char budget for verbatim excerpts; 0 = use default
 *                      (MEMORY_SOURCE_BUDGET_CHARS = 3072).  Bench harness
 *                      passes a non-zero override to sweep budget values
 *                      without mutating shared global state.
 * @return Heap-allocated formatted string (caller frees), or NULL on alloc fail
 */
char *memory_action_search(int user_id,
                           const char *keywords,
                           time_t since_ts,
                           const char *category,
                           int64_t as_of_ts,
                           bool include_historical,
                           bool with_source,
                           int source_budget);

/**
 * @brief List facts/summaries within a time window with optional sort order.
 *
 * Bundle 3 (2026-05-13) extension of the original "recent" surface:
 *
 *   - `period`   lower-bound time period ("24h", "7d") relative to now (NULL
 *                or empty → default '7d').
 *   - `before`   optional upper-bound time period (NULL or empty → "until
 *                now").  Use with `period` to query a window in the past
 *                — e.g., period='30d', before='7d' returns memories from
 *                30 days ago up to 7 days ago.
 *   - `sort_asc` true = oldest first; false = newest first (historical
 *                default).  The "find your earliest stored memory" use
 *                case is `period='10y', sort_asc=true, limit=1`.
 *   - `limit`    max records per category (facts + summaries).  Clamped
 *                to [1, 50] internally; 0 = use defaults (20 facts /
 *                10 summaries).
 *
 * @param user_id Owner of the memory store
 * @param period Lower-bound time period string (NULL/empty → '7d')
 * @param before Upper-bound time period string (NULL/empty → "until now")
 * @param sort_asc True = oldest first; false = newest first
 * @param limit Max records per category (0 = defaults)
 * @param with_source Append verbatim source excerpts to each fact
 * @param source_budget Char budget for verbatim excerpts; 0 = use default
 * @return Heap-allocated formatted string (caller frees), or NULL on alloc fail
 */
/* Per-record-category caps for memory_action_recent's output arrays.  The
 * tool layer's 'limit' param clamps to MEMORY_RECENT_LIMIT_MAX as
 * defense-in-depth (the tool layer's enum range is the first gate; this is
 * the second).  Exposed here so the tool schema description and the action
 * layer share one source of truth — bumping the cap requires updating both. */
#define MEMORY_RECENT_DEFAULT_FACT_LIMIT 20
#define MEMORY_RECENT_DEFAULT_SUMMARY_LIMIT 10
#define MEMORY_RECENT_LIMIT_MAX 50

char *memory_action_recent(int user_id,
                           const char *period,
                           const char *before,
                           bool sort_asc,
                           int limit,
                           bool with_source,
                           int source_budget);

/* =============================================================================
 * Source-fetch dedup set (Phase B).
 *
 * Spans a single memory_action_search / memory_action_recent call so that the
 * same `(conversation_id, msg_start, msg_end)` cited by multiple records
 * (fact + summary + relation) emits a verbatim excerpt only on first mention
 * and a one-line back-ref on subsequent mentions.  Saves both LLM context and
 * SQLite round-trips.
 *
 * INVARIANT (Sec M1 in PROVENANCE.md): callers must add ONLY post-batch-filter
 * triples — the `_get_sources` batch APIs return `out_conv_ids[i] = 0` for
 * private-conversation rows, so a triple where conv_id > 0 is provably public.
 * Adding a private triple here would cause a later public record sharing the
 * same range to emit "[source: same as above]" which betrays existence of the
 * upstream private record.  Helper functions enforce nothing on their own —
 * the discipline lives at the call sites in memory_callback.c.
 *
 * Exposed in this internal header so unit tests can exercise the helpers
 * directly without compiling the whole callback module.
 * ============================================================================= */

#define SOURCE_DEDUP_CAP 24

typedef struct {
   int64_t conv_ids[SOURCE_DEDUP_CAP];
   int64_t starts[SOURCE_DEDUP_CAP];
   int64_t ends[SOURCE_DEDUP_CAP];
   int count;
} source_dedup_set_t;

bool source_dedup_seen(const source_dedup_set_t *set, int64_t conv, int64_t s, int64_t e);
void source_dedup_add(source_dedup_set_t *set, int64_t conv, int64_t s, int64_t e);

/* `memory_search_apply_score_floor` lives in include/memory/memory_fact_search.h
 * — promoted from the internal header so the per-turn focus-injection fact
 * adapter (memory_focus_adapters.c) can apply the same "I don't remember"
 * gate without breaching the internal-only contract of this header. */

#endif /* MEMORY_CALLBACK_INTERNAL_H */
