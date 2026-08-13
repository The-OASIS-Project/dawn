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
 * built on top in research_run_loop.c (session/dispatch-coupled) and driven by
 * the detached research_worker.
 */

#ifndef RESEARCH_RUN_H
#define RESEARCH_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h" /* research_run_t */

#ifdef __cplusplus
extern "C" {
#endif

struct session; /* core/session_manager.h — full type only needed in the .c */

/* P0 budget/shape defaults — the single source of truth for both the compile-
 * time fallback (research_budgets_defaults) and the [research] config defaults
 * (config_defaults.c, which includes the same leaf header).  See research_defaults.h. */
#include "config/research_defaults.h"

/* Hard bound on how many ledger questions the core loads at once (digest +
 * coverage refresh).  A run with more than this many questions is pathological;
 * the extras are simply not considered this pass. */
#define RESEARCH_MAX_LEDGER_QUESTIONS 128

/* Earliest claims per open question carried into the round digest as the
 * "findings so far" gloss (§4a) — enough to build on, bounded to keep the digest
 * within round_digest_max_chars. */
#define RESEARCH_DIGEST_GLOSS_CLAIMS 2

/** Per-run budgets + digest shape.  Populated by research_budgets_defaults(),
 *  later overridden from [research] config (Step 9). */
typedef struct {
   int max_rounds;             /**< stop after this many rounds (§6.1) */
   int max_tool_calls;         /**< stop past this many tool calls (§6.1) */
   int64_t max_input_tokens;   /**< the real cost ceiling (§6.1, eff M4) */
   int min_sources;            /**< distinct source_urls to call a question answered (§6.2) */
   int round_digest_max_chars; /**< cap on the reconstructed round prompt (§4a.2, eff H1) */
   int top_k_questions;        /**< open questions surfaced per round digest */
   int saturation_rounds; /**< consecutive dry rounds (0 new closures) before stopping; 0 off */
} research_budgets_t;

/** @brief Fill @p out with the compile-time P0 defaults. */
void research_budgets_defaults(research_budgets_t *out);

/**
 * @brief Fill @p out with the RUNTIME budgets: compile-time defaults overlaid
 *        with the [research] config section (config_defaults mirrors the same
 *        defaults, so this equals research_budgets_defaults() until the operator
 *        changes config).  Defined in the ENABLE_WEBUI half (research_run_loop.c)
 *        because it reads g_config; the deterministic core stays config-free.
 */
void research_budgets_load(research_budgets_t *out);

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
 * @brief The continue/stop decision (§6).
 *
 * "Natural end" reasons are checked BEFORE the hard budgets so the stop_reason
 * reflects why the run is actually done, not merely that a ceiling was crossed on
 * the same round: the agent's own completion signal (@p concluded), then all
 * questions closed (@p all_closed), then saturation (@p no_progress_rounds dry
 * rounds).  Hard budgets (rounds / tool_calls / input_tokens) are the backstop,
 * checked last — they still fire every round boundary, so ordering only affects the
 * LABEL when two conditions coincide; nothing here lets a run exceed a budget.
 *
 * @param run              the run's current meters (rounds_run / tool_calls / input_tokens).
 * @param b                the active budgets (incl. saturation_rounds).
 * @param all_closed       true iff every question is answered/unanswerable.
 * @param no_progress_rounds consecutive rounds that closed no new question.
 * @param concluded        the agent called research_conclude (already gated on having
 *                         recorded findings by the caller).
 * @return a stop_reason string ("concluded" | "coverage" | "saturation" | "budget" |
 *         "token_budget") to stop, or NULL to continue.  On a NULL-arg programming
 *         error it returns the defensive sentinel "failed".  Always a static literal.
 */
const char *research_should_stop(const research_run_t *run,
                                 const research_budgets_t *b,
                                 bool all_closed,
                                 int no_progress_rounds,
                                 bool concluded);

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

/**
 * @brief Render the final report as markdown — a view over research_claims
 *        grouped by question (§4/§8).  Reads the persisted claim rows (not any
 *        round digest), so compression cannot lose evidence.  Allocates
 *        *out_markdown (caller frees).
 *
 * @return AUTH_DB_SUCCESS (even with zero claims — emits a "no findings" note),
 *         or a failure code (*out_markdown left NULL).
 */
int research_render_report(int64_t run_id, const char *brief, char **out_markdown);

/**
 * @brief Run the research controller loop on a prepared bare job session (§4).
 *
 * setup (research system prompt) → round loop { reset history to [system] +
 * bounded digest + dispatch with skip_prompt_rebuild + meter tokens + refresh
 * coverage + P0 stop decision } → synthesize (render report → final revision).
 * The session MUST already be a bare SESSION_TYPE_JOB session (job_manager_begin)
 * run native-tools-only with legacy <command> execution disabled — the read-only
 * allowlist gates only the native tool path (§11 HIGH-1).
 *
 * The caller (research_worker) owns the session lifecycle + the terminal job
 * transition; this returns the terminal stop_reason.
 *
 * @return a static stop_reason literal: "concluded" | "coverage" | "saturation" |
 *         "budget" | "token_budget" | "cancelled" | "failed".
 */
const char *research_run_execute(struct session *s,
                                 const research_run_t *run0,
                                 const research_budgets_t *b);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_RUN_H */
