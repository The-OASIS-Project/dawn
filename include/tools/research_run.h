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

/* Controller stop-reason vocabulary — the single source of truth for the strings
 * research_should_stop() RETURNS and research_worker.c re-classifies to map a stop
 * to a job/research disposition.  Shared as named constants because the same set is
 * produced in research_run.c and consumed by strcmp in research_run_loop.c and
 * research_worker.c: a bare-literal typo (e.g. "token_budget" vs "tokens_budget")
 * compiles clean and silently misclassifies a run's outcome.  These are the
 * CONTROLLER's stop reasons, distinct from the JOB-lifecycle status strings
 * ("done"/"interrupted"/"timeout") the worker also emits.
 *   Natural ends:  CONCLUDED, COVERAGE, SATURATION   (research_is_natural_end)
 *   Fuse stops:    BUDGET (max_rounds), TOKEN_BUDGET (max_input_tokens)
 *   Non-success:   CANCELLED, FAILED */
#define RESEARCH_STOP_CONCLUDED "concluded"
#define RESEARCH_STOP_COVERAGE "coverage"
#define RESEARCH_STOP_SATURATION "saturation"
#define RESEARCH_STOP_BUDGET "budget"
#define RESEARCH_STOP_TOKEN_BUDGET "token_budget"
#define RESEARCH_STOP_CANCELLED "cancelled"
#define RESEARCH_STOP_FAILED "failed"

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
   int64_t max_input_tokens;   /**< the real cost ceiling (§6.1, eff M4) */
   int min_sources;            /**< distinct source_urls to call a question answered (§6.2) */
   int round_digest_max_chars; /**< cap on the reconstructed round prompt (§4a.2, eff H1) */
   int top_k_questions;        /**< open questions surfaced per round digest */
   int saturation_rounds; /**< consecutive dry rounds (0 new closures) before stopping; 0 off */
   int stale_rounds;      /**< consecutive rounds a question gains NO new source before it is
                               auto-retired as unanswerable; 0 off (§6.3, P1 Phase 2) */
   int critic_max_rearm;  /**< times the completeness critic may re-arm at stop-eligibility;
                               0 disables the critic (§6 item 4) */
} research_budgets_t;

/* Cap on gap sub-questions the critic may add per re-arm — bounds the denominator
 * growth from a single critic pass (the whole run is still fuse-bounded). */
#define RESEARCH_CRITIC_MAX_GAPS 8

/**
 * @brief A parsed completeness-critic verdict (§6 item 4).
 *
 * @c re_arm true  → continue: research the @c gaps (each a NEW targeted sub-question
 *                   capturing an untried angle) for the remaining rounds.
 * @c re_arm false → confirm the pending stop.  The parser FAILS SAFE to this: a
 *                   malformed/absent verdict, a "stop" decision, or zero gaps all
 *                   yield re_arm=false, so a parse miss can only end a run, never
 *                   run it away.
 */
typedef struct {
   bool re_arm;
   int n_gaps;
   char gaps[RESEARCH_CRITIC_MAX_GAPS][RESEARCH_QUESTION_MAX];
} research_critic_verdict_t;

/**
 * @brief One question's in-memory staleness state (P1 Phase 2).
 *
 * The per-question source-count baseline + dry-round streak the caller carries
 * across rounds so research_retire_stale_questions() can detect "no new source for
 * N rounds".  DELIBERATELY in-memory, not a ledger column: a hard-killed run is
 * reconciled to 'interrupted' and never resumes (research_db_reconcile_orphaned —
 * "P0 has no research resume"), so this state's useful life is exactly ONE
 * research_run_execute() invocation.  If research runs ever become resumable, this
 * must move to a persisted research_questions column so a resumed run doesn't reset
 * every streak (see DEEP_RESEARCH_DESIGN.md §6.3).
 */
typedef struct {
   int64_t qid;      /**< the question being tracked */
   int last_sources; /**< distinct-source count at its last observation */
   int stale_rounds; /**< consecutive rounds since last_sources grew */
} research_stale_entry_t;

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
 * @brief Auto-retire open questions that have gone STALE — no new distinct source
 *        for @p stale_threshold consecutive rounds — by marking them 'unanswerable'
 *        (§6.3, P1 Phase 2).
 *
 * Run at each round boundary AFTER research_refresh_coverage(), so only truly-open,
 * under-sourced questions are candidates (a question that reached min_sources this
 * round is already 'answered' and skipped).  Retiring drops it from the open set so
 * a run stuck on unclosable questions converges (coverage / saturation) instead of
 * grinding to the token fuse.  Not data loss: the question's recorded claims stay in
 * research_claims and still render in the report.
 *
 * The staleness state is IN-MEMORY (@p tracker), owned by the caller for the run's
 * lifetime — see research_stale_entry_t on why it is not persisted.  Only the first
 * observation of a question seeds a baseline (never counts as a dry round).
 *
 * @param run_id
 * @param min_sources     coverage threshold (only informational here — refresh_coverage
 *                        already promoted anything at/above it).
 * @param stale_threshold consecutive dry rounds before retiring; <= 0 disables (no-op).
 * @param tracker         caller-owned per-question state array.
 * @param tracker_n       in/out: live entry count in @p tracker.
 * @param tracker_max     capacity of @p tracker (RESEARCH_MAX_LEDGER_QUESTIONS).
 * @param retired_out     caller-owned; receives the qids retired THIS call (may be NULL).
 * @param retired_max     capacity of @p retired_out.
 * @param retired_n_out   number of qids written to @p retired_out (may be NULL).
 * @return AUTH_DB_SUCCESS or a failure code (tracker left usable).
 */
int research_retire_stale_questions(int64_t run_id,
                                    int min_sources,
                                    int stale_threshold,
                                    research_stale_entry_t *tracker,
                                    int *tracker_n,
                                    int tracker_max,
                                    int64_t *retired_out,
                                    int retired_max,
                                    int *retired_n_out);

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
 * @brief True iff @p stop_reason is a NATURAL END (the run judged itself done) rather
 *        than a hard fuse (ran out of budget).  Natural ends — "concluded" /
 *        "coverage" / "saturation" — are the only stops the completeness critic may
 *        re-arm; re-arming a "budget"/"token_budget" fuse is pointless (no budget
 *        left, the same fuse fires again).  NULL/unknown → false (fail closed).
 */
bool research_is_natural_end(const char *stop_reason);

/**
 * @brief Parse a completeness-critic verdict from the judge turn's response text
 *        (§6 item 4).  Tolerant: finds the first JSON object in @p response and reads
 *        {"decision":"stop"|"continue","gaps":[{"question":"…"},…]}.
 *
 * FAILS SAFE to @c re_arm=false — a NULL/empty/malformed response, a non-"continue"
 * decision, or an empty/absent gaps array all yield a "confirm stop" verdict, so a
 * parse miss can only end a run, never re-arm it.  On a "continue" with gaps, copies
 * up to RESEARCH_CRITIC_MAX_GAPS non-empty question strings into @p out->gaps.
 *
 * @return AUTH_DB_SUCCESS on any well-formed parse (including a fail-safe stop), or
 *         AUTH_DB_INVALID on a NULL @p out.  (The verdict itself is in *@p out.)
 */
int research_critic_parse_verdict(const char *response, research_critic_verdict_t *out);

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
 * @brief Render the completeness critic's input digest into @p out (§6 item 4).
 *
 * The brief + the pending-stop context (@p stop_reason, rounds/tokens used vs. their
 * budgets, which re-arm this is) + EVERY question with its status AND resolution
 * reason ("stale" = exhausted, "agent" = judged dead-end) + distinct-source coverage
 * + a short claim gloss.  The reason is what lets the critic tell an exhausted gap
 * (don't re-arm) from an un-attempted one (a legitimate untried-angle target).
 * Bounded by @p b->round_digest_max_chars; @p out is always NUL-terminated.
 *
 * @return AUTH_DB_SUCCESS, or a failure code (out gets a minimal brief-only digest).
 */
int research_render_critic_digest(int64_t run_id,
                                  const char *brief,
                                  const research_budgets_t *b,
                                  const research_run_t *cur,
                                  const char *stop_reason,
                                  int rearm_num,
                                  char *out,
                                  size_t out_size);

/**
 * @brief Render the EVIDENCE section as markdown — a view over research_claims
 *        grouped by question (§4/§8).  Reads the persisted claim rows (not any
 *        round digest), so compression cannot lose evidence.  The controller layers
 *        a written prose answer on top of this (research_run_loop.c synthesis) to
 *        form the final report.  Allocates *out_markdown (caller frees).
 *
 * @return AUTH_DB_SUCCESS (even with zero claims — emits a "no findings" note),
 *         or a failure code (*out_markdown left NULL).
 */
int research_render_report(int64_t run_id, const char *brief, char **out_markdown);

/**
 * @brief Canonicalize a source URL for honest DISTINCT-source counting (B1).
 *
 * The coverage stop-controller decides `coverage` from COUNT(DISTINCT source_url)
 * (§6.2).  Taken verbatim, that count is gameable: two links to the SAME page that
 * differ only cosmetically — scheme/host case, a leading "www.", an explicit
 * default port, a trailing slash, a "#fragment", or tracking query params
 * (utm_*, fbclid, gclid, …) — would each count as an independent source and let a
 * run declare coverage without genuinely distinct evidence.  Canonicalizing at the
 * record path collapses those variants to one string so the DISTINCT count is honest
 * (and the stored citation renders clean).
 *
 * Transforms (host/scheme only — the PATH is left byte-exact because paths are
 * case-sensitive on many servers):
 *   - lowercase the scheme and host; drop a leading "www."; drop an explicit
 *     default port (:80 http / :443 https)
 *   - drop the "#fragment"
 *   - drop known tracking query params, preserving every other param in order
 *     (an emptied query drops the "?" entirely)
 *   - drop a single trailing "/"
 *
 * Conservative: a non-HTTP(S) input (or anything it cannot confidently parse) is
 * copied through verbatim — the caller drops non-web URLs separately.  Pure and
 * deterministic (no ledger/LLM), so it is unit-tested directly.
 *
 * @param in        the raw source_url (may be NULL → empty out)
 * @param out       destination buffer (always NUL-terminated on return)
 * @param out_size  capacity of @p out
 * @return @p out.
 */
char *research_canonicalize_url(const char *in, char *out, size_t out_size);

/**
 * @brief Run the research controller loop on a prepared bare job session (§4).
 *
 * setup (research system prompt) → round loop { reset history to [system] +
 * bounded digest + dispatch with skip_prompt_rebuild + meter tokens + refresh
 * coverage + stop decision } → synthesize (no-tools LLM turn writes the answer from
 * the evidence → final revision + notes + job-conversation copy).
 * The session MUST already be a bare SESSION_TYPE_JOB session (job_manager_begin)
 * run native-tools-only with legacy <command> execution disabled — the read-only
 * allowlist gates only the native tool path (§11 HIGH-1).
 *
 * The caller (research_worker) owns the session lifecycle + the terminal job
 * transition; this returns the terminal stop_reason.
 *
 * @param out_summary  optional (may be NULL): receives a heap-allocated short lead
 *                     from the synthesized report (caller frees), for the chat
 *                     completion message.  Set to NULL when there is no synthesis.
 * @return a static stop_reason literal: "concluded" | "coverage" | "saturation" |
 *         "budget" | "token_budget" | "cancelled" | "failed".
 */
const char *research_run_execute(struct session *s,
                                 const research_run_t *run0,
                                 const research_budgets_t *b,
                                 char **out_summary);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_RUN_H */
