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
 * Deep-research controller (orchestrator) — DEEP_RESEARCH_DESIGN.md §4/§6.
 *
 * This header exposes the controller's DETERMINISTIC CORE: the per-round bounded
 * digest renderer, coverage-driven question promotion, and the P0 continue/stop
 * decision.  These are pure functions over the SQLite ledger (research_db_*) with
 * no LLM involvement — the "continue signal is the delta between the ledger and
 * the evidence, computed in C, not LLM self-assessment" (§6).  The live
 * orchestration (bare session + research prompt + round loop + synthesis) is
 * built on top in research_run.c and driven by the detached research_worker.
 */

#ifndef RESEARCH_RUN_H
#define RESEARCH_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h" /* research_run_t */

/* P0 budget/shape defaults.  Step 9 wires the [research] config section to
 * override these; until then research_budgets_defaults() is the single source. */
#define RESEARCH_DEFAULT_MAX_ROUNDS 6
#define RESEARCH_DEFAULT_MAX_TOOL_CALLS 40
#define RESEARCH_DEFAULT_MAX_INPUT_TOKENS 200000
#define RESEARCH_DEFAULT_MIN_SOURCES 2
#define RESEARCH_DEFAULT_ROUND_DIGEST_MAX_CHARS 6000
#define RESEARCH_DEFAULT_TOP_K_QUESTIONS 8

/* Hard bound on how many ledger questions the core loads at once (digest +
 * coverage refresh).  A run with more than this many questions is pathological;
 * the extras are simply not considered this pass. */
#define RESEARCH_MAX_LEDGER_QUESTIONS 128

/** Per-run budgets + digest shape.  Populated by research_budgets_defaults(),
 *  later overridden from [research] config (Step 9). */
typedef struct {
   int max_rounds;             /**< stop after this many rounds (§6.1) */
   int max_tool_calls;         /**< stop past this many tool calls (§6.1) */
   int64_t max_input_tokens;   /**< the real cost ceiling (§6.1, eff M4) */
   int min_sources;            /**< distinct source_urls to call a question answered (§6.2) */
   int round_digest_max_chars; /**< cap on the reconstructed round prompt (§4a.2, eff H1) */
   int top_k_questions;        /**< open questions surfaced per round digest */
} research_budgets_t;

/** @brief Fill @p out with the compile-time P0 defaults. */
void research_budgets_defaults(research_budgets_t *out);

/**
 * @brief Promote every open question whose distinct-source coverage has reached
 *        @p min_sources to 'answered' (confidence = min(1.0, sources/min_sources)).
 *
 * Run at each round boundary — this is the controller computing coverage in C
 * (§3/§6.2), not the LLM asserting completion.  Questions already answered or
 * unanswerable are left as-is.
 *
 * @param closed_out  receives the count of non-open questions after promotion.
 * @param total_out   receives the total question count.
 * @return AUTH_DB_SUCCESS or a failure code.
 */
int research_refresh_coverage(int64_t run_id, int min_sources, int *closed_out, int *total_out);

/**
 * @brief The P0 continue/stop decision (§6, layers 1-2 only).
 *
 * Layered outermost-first: hard budgets (rounds / tool_calls / input_tokens),
 * then the all-questions-closed early exit.  Saturation, the completeness critic,
 * and UNANSWERABLE verdicts are P1.
 *
 * @param run        the run's current meters (rounds_run / tool_calls / input_tokens).
 * @param b          the active budgets.
 * @param all_closed true iff every question is answered/unanswerable (from
 *                   research_refresh_coverage's closed==total).
 * @return a stop_reason string ("budget" | "token_budget" | "coverage") to stop,
 *         or NULL to continue.  On a NULL-arg programming error it returns the
 *         defensive sentinel "failed" (a run status, not a normal stop_reason).
 *         The returned pointer is always a static literal.
 */
const char *research_should_stop(const research_run_t *run,
                                 const research_budgets_t *b,
                                 bool all_closed);

/**
 * @brief Render the BOUNDED per-round digest into @p out (§4a.2).
 *
 * Emits, capped at @p b->round_digest_max_chars: the brief, the top-K open
 * questions with a one-line coverage roll-up each, and the last revision's prose
 * (omitted if none).  NEVER the full claim set — that is materialized only at
 * synthesis, straight from research_claims, so compression here cannot lose
 * evidence.  @p out is always NUL-terminated.
 *
 * @return AUTH_DB_SUCCESS, or a failure code (out gets a minimal brief-only digest).
 */
int research_render_round_digest(int64_t run_id,
                                 const char *brief,
                                 const research_budgets_t *b,
                                 char *out,
                                 size_t out_size);

#endif /* RESEARCH_RUN_H */
