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
 * Background-job session pool (background-jobs Phase 1).
 *
 * A background job IS a parented conversation run in its OWN text-only session
 * pool — separate from the interactive sessions[] array so a job can never
 * starve an audio-bound WebUI/satellite client (different array) and is never
 * counted against MAX_SESSIONS.  This module owns that pool: reservation under
 * the running caps, the ownership-checked cancel path, cancel-then-wait
 * teardown, and the resolver hook that lets a job's shared LLM tool loop
 * re-acquire its own session by id (session_get()/_for_reconnect()).
 *
 * See docs/BACKGROUND_JOBS_DESIGN.md §4 (hazards) / §7 (caps).  Layer 2.
 */

#ifndef JOB_MANAGER_H
#define JOB_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reinvoke processor: consumes the monitor's drained reinvoke_parent rows.
 *
 * Registered by job_reinvoke (Layer 2) so the completion monitor can hand off
 * reinvoke rows WITHOUT job_manager depending on job_reinvoke (one-way edge; the
 * webui_broadcast_job_notification weak-symbol idiom).
 */
typedef void (*job_reinvoke_processor_fn)(const job_record_t *rows, int n);

/**
 * @brief Register (or clear, with NULL) the reinvoke processor.  Idempotent.
 * @param fn Processor to invoke with drained reinvoke rows, or NULL to clear.
 */
void job_manager_register_reinvoke_processor(job_reinvoke_processor_fn fn);

/* Return codes (positive-only, per project convention). */
#define JOB_MGR_OK 0
#define JOB_MGR_FAIL 1
#define JOB_MGR_CAP_GLOBAL 2   /**< global max_active_jobs reached */
#define JOB_MGR_CAP_PROVIDER 3 /**< per-provider concurrency reached */
#define JOB_MGR_CAP_USER 4     /**< per-user concurrent running reached */
#define JOB_MGR_NOT_FOUND 5    /**< no running job for that conversation */
#define JOB_MGR_FORBIDDEN 6    /**< running job owned by a different user */

/** Max job rows the completion monitor drains (and hands a processor) per tick. */
#define JOB_MONITOR_MAX_PER_TICK 16

/**
 * @brief Job-session pool slot count.
 *
 * The pool is allocated ONCE at job_manager_init() to this fixed size, NOT to
 * `[jobs] max_active_jobs` — that setting is runtime-mutable via the WebUI
 * settings panel, and sizing the array from it would let a raised cap pass
 * job_manager_capacity() (a counter comparison) while job_manager_begin_ex()
 * found no free slot in a smaller boot-time array, producing phantom
 * "job capacity reached" failures.  `max_active_jobs` is therefore a pure policy
 * counter with nothing allocated from it.
 *
 * @note MUST stay >= the `max_active_jobs` ceiling in config_clamp_jobs()
 *       (config_parser.c).  256 slots x sizeof(job_slot_t) is ~10 KB — cheap
 *       enough to pre-allocate outright.  Keep the two values in sync.
 */
#define JOB_POOL_MAX_SLOTS 256

/** Max reap notices logged per tick (bounds log volume, not reap work). */
#define JOB_REAP_LOG_MAX_PER_TICK 16

/** Re-issue the cancel + re-log this often while a reaped job refuses to die. */
#define JOB_REAP_NAG_INTERVAL_SEC 60

/**
 * @brief Grace period after a reap before the job's provider counter is
 *        force-released.
 *
 * A reap is a cancel *request*: it is only observed where the session cancel
 * flag is checked (the LLM/CURL transfer).  A job wedged somewhere that does not
 * poll it — notably inside a tool call, since the tool loop's per-iteration gate
 * checks the GLOBAL wake-word interrupt rather than the session flag — never
 * returns, so its worker never calls job_manager_end() and its provider counter
 * is held forever.  With the default max_concurrent_local = 1 that is a
 * permanent block on every later local job.  After this grace period the
 * provider counter is reclaimed so the pool keeps working; the slot itself stays
 * owned by the zombie worker so its session pointer remains valid.
 */
#define JOB_REAP_FORCE_RELEASE_SEC 300

/** Provider resource class a running job is accounted against. */
typedef enum {
   JOB_PROVIDER_LOCAL = 0, /**< local (GPU) LLM — scarce, keep concurrency low */
   JOB_PROVIDER_CLOUD = 1  /**< cloud LLM — parallelism is cheap */
} job_provider_class_t;

/**
 * @brief Initialize the job-session pool and register the resolver hook.
 *
 * Sizes the pool to `[jobs] max_active_jobs`.  Idempotent.
 * @return SUCCESS or FAILURE.
 */
int job_manager_init(void);

/**
 * @brief Cancel all running jobs and tear down the pool.
 *
 * Unregisters the resolver first (so no new session_get() resolves a job),
 * then requests cancellation of every running job.  Call after the worker
 * threads have been asked to stop.
 */
void job_manager_shutdown(void);

/**
 * @brief Reserve a running slot and create the job session (enforces the
 *        running caps atomically under the pool lock).
 *
 * On success the returned session has type SESSION_TYPE_JOB, user_id bound, and
 * stream_conversation_id set to @p conv_id — ready for the worker to dispatch a
 * turn on.  The caller (worker) MUST call job_manager_end() when the turn
 * finishes to release the slot + provider counter and free the session.
 *
 * @param user_id Owner (the spawner's user_id, non-overridable).
 * @param conv_id The job's conversation id.
 * @param provider Resource class to account against.
 * @param out Receives the new job session (unmodified on failure).
 * @return JOB_MGR_OK, or JOB_MGR_CAP_{GLOBAL,PROVIDER,USER}, or JOB_MGR_FAIL.
 */
int job_manager_begin(int user_id, int64_t conv_id, job_provider_class_t provider, session_t **out);

/**
 * @brief Like job_manager_begin, but optionally exempt from the per-user cap.
 *
 * A reinvoke_parent re-engagement passes count_against_user_cap=false so it is
 * never refused by max_jobs_per_user at the moment a user's jobs are completing
 * (the completed job has already freed its slot; global + provider caps still
 * apply and still leave the reinvoke to retry on a lost race).
 *
 * @param user_id Owner (the spawner's user_id, non-overridable).
 * @param conv_id The conversation the session is bound to.
 * @param provider Resource class to account against.
 * @param count_against_user_cap false exempts this begin from max_jobs_per_user.
 * @param out Receives the new session (unmodified on failure).
 * @return JOB_MGR_OK, or JOB_MGR_CAP_{GLOBAL,PROVIDER,USER}, or JOB_MGR_FAIL.
 */
int job_manager_begin_ex(int user_id,
                         int64_t conv_id,
                         job_provider_class_t provider,
                         bool count_against_user_cap,
                         session_t **out);

/**
 * @brief Worker end-of-turn teardown: remove from pool + release the provider
 *        counter, then cancel-then-wait drain any transient retains and free.
 *
 * Called by the owning worker after it has persisted the job's terminal state.
 * Removes the session from the pool under the pool lock (so no new resolver /
 * cancel can reach it), releases the base ref, waits (bounded) for outstanding
 * transient retains to drain, then frees.  On wait timeout the session is
 * leaked rather than freed (never a use-after-free).
 */
void job_manager_end(session_t *session);

/**
 * @brief Ownership-checked cancel of a running job by conversation id.
 * @return JOB_MGR_OK (cancel requested), JOB_MGR_NOT_FOUND, or JOB_MGR_FORBIDDEN.
 */
int job_manager_cancel(int64_t conv_id, int user_id);

/** @return current number of running jobs across all users/providers. */
int job_manager_running_count(void);

/**
 * @brief Flag that a job reached a terminal state, so the next monitor tick
 *        scans for pending completion follow-ups (dirty-gate: an idle system
 *        does zero per-tick DB work).  Called by the worker + boot scan.
 */
void job_manager_mark_dirty(void);

/**
 * @brief `job_error` text stamped on a job reaped for exceeding max_runtime_sec.
 *
 * Shared by the producer (job_worker's terminal write) and the consumer (the
 * monitor's notify wording), so the "did this fail or time out?" test is a
 * single named constant rather than a string duplicated at both ends.
 */
#define JOB_ERR_TIMED_OUT "timed out (exceeded [jobs] max_runtime_sec)"

/**
 * @brief Request cancellation of every job that has outrun `[jobs] max_runtime_sec`.
 *
 * Without this a wedged worker never reaches a terminal state, so it holds its
 * pool slot AND its provider counter forever — with the default
 * max_concurrent_local=1 a single hung local job permanently blocks every
 * subsequent local spawn until the daemon restarts.
 *
 * Scans the in-memory pool only (no DB work) and so is deliberately NOT behind
 * the completion monitor's dirty-gate: a job that hangs never marks anything
 * dirty, which is exactly the case this exists to catch.  Overdue slots are
 * flagged reaped and cancel-requested; the worker observes the cancel, unwinds,
 * and (seeing job_manager_was_reaped) records `failed`/JOB_ERR_TIMED_OUT while
 * still letting its completion follow-up fire — per design §5 a timeout must
 * never silently no-op.  `max_runtime_sec <= 0` disables reaping.
 *
 * @param now Current time (the heartbeat's, so it matches jobs_monitor_tick).
 * @return number of jobs newly reaped on this call (0 when nothing is overdue).
 */
int job_manager_reap_overdue(time_t now);

/**
 * @brief Claim this job session's reap verdict, making the slot non-reapable.
 *
 * Lets the worker distinguish a timeout reap from a user cancel: both arrive as
 * `cancel_requested`, but a user cancel suppresses the completion follow-up
 * while a reap must still deliver one.
 *
 * This is a CLAIM, not a plain query — it stops the reap clock for this slot as
 * a side effect.  Without that, the slot stays reapable across the worker's
 * whole terminal-write window (several DB round-trips), so a job that finished
 * microseconds before its deadline could be flagged *after* it had already
 * decided it was `done`, and a heartbeat could log "reaping…" for a job the DB
 * already recorded as complete.  Calling this closes both windows.
 *
 * Call it exactly once, before job_manager_end().  Idempotent, but a second call
 * returns false because the first already consumed the verdict.
 *
 * @param session The job session (NULL-safe).
 * @return true if this session's slot had been flagged by job_manager_reap_overdue().
 */
bool job_manager_claim_reaped(const session_t *session);

/**
 * @brief Did this job record end as a runtime-reap timeout?
 *
 * Single definition of the "failed + timed out" test, so consumers (completion
 * notice wording, reinvoke envelope, future jobs panel) cannot drift from each
 * other — and so JOB_ERR_TIMED_OUT stays free to change without silently
 * breaking a hand-written strcmp somewhere.
 *
 * @param rec Job record (NULL-safe).
 * @return true if the job terminated via the max_runtime_sec reap.
 */
bool job_record_timed_out(const job_record_t *rec);

/**
 * @brief Completion monitor — call once per second from the main-loop heartbeat.
 *
 * Reaps overdue jobs (unconditionally — see job_manager_reap_overdue), then,
 * when dirty, drains up to `[jobs] monitor_followups_per_tick` terminal jobs
 * awaiting their follow-up, marks them fired, and dispatches the "notify"
 * completions on a detached delivery thread (scheduler_emit_alert does blocking
 * TTS/messaging, which must not run on the voice loop).
 */
void jobs_monitor_tick(time_t now);

/**
 * @brief Push a silent job-completion toast to the owner's browser sessions.
 *
 * Weak symbol: a no-op unless the WebUI module links its strong override
 * (webui_broadcasts.c).  Called by the completion monitor for jobs with no
 * messaging `deliver_to` target — a quiet in-browser banner, NO voice.
 *
 * @param user_id Owner.
 * @param text Completion message.
 * @param conv_id The job conversation id (so the client can deep-link later).
 * @param running_count Jobs still running (for a "N running" indicator).
 */
void webui_broadcast_job_notification(int user_id,
                                      const char *text,
                                      int64_t conv_id,
                                      int running_count);

/**
 * @brief Push ONE job's current row to its owner's browser sessions.
 *
 * The lifecycle delta of the job frames (§6.4): clients keep an active-job set
 * keyed by conversation id and upsert this row into it, dropping it when the row
 * arrives terminal.  Set membership — never +/-1 arithmetic — so reconnect,
 * multi-tab and out-of-order delivery all converge.
 *
 * Weak symbol: a no-op unless WebUI links its strong override.
 */
void webui_broadcast_job_update(int user_id, const job_record_t *rec);

/**
 * @brief Read @p conv_id's job row and broadcast it (see webui_broadcast_job_update).
 *
 * Three call sites, one per lifecycle transition — enter (`job_tool.c`, BEFORE
 * the worker is spawned so nothing can race the read), queued->running
 * (`job_worker.c`), and leave (`job_manager_set_terminal()`, which is the choke
 * point for all eight terminal dispositions).
 *
 * ORDERING: after the row is created, the owning worker is the only emitter, so
 * frames for a given job are enqueued in DB order.  A new transition MUST emit
 * from whichever thread owns the row at that moment — emitting from a second
 * thread reintroduces the stale-read race documented at the spawn site.
 */
void job_update_emit(int64_t conv_id, int user_id);

/**
 * @brief Record a job's terminal state AND emit its `complete` event (§6.2).
 *
 * Use this instead of calling conv_db_job_set_terminal() directly. A job reaches
 * a terminal state from EIGHT places — the worker's four dispositions, the
 * tool's spawn-failure and queued-cancel, and the boot interrupted-scan — and a
 * tailer that misses any one of them never sees that job end. Pairing the two
 * writes here means adding a ninth disposition can't silently skip the event.
 *
 * Deliberately NOT folded into conv_db_job_set_terminal(): that is the DB layer,
 * and emitting from inside it would have the DB calling a WebUI weak symbol.
 *
 * @param conv_id          The job conversation.
 * @param user_id          Owner, for broadcast targeting (<= 0 skips fan-out).
 * @param status           done|failed|interrupted|cancelled.
 * @param error_or_null    Failure reason; redacted before persisting (§8.6).
 * @param finished_at      Terminal timestamp.
 * @param final_message_id `messages.id` of the final answer, or 0 if none — lets
 *                         a tailer correlate the disposition with the body.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (from the DB write; the event is
 *         best-effort and never fails the call).
 */
int job_manager_set_terminal(int64_t conv_id,
                             int user_id,
                             const char *status,
                             const char *error_or_null,
                             time_t finished_at,
                             int64_t final_message_id);

/**
 * @brief Peek whether a new job could start now (no reservation).
 *
 * Lets the `job` tool refuse cleanly past a cap before creating the row.
 * job_manager_begin() re-checks authoritatively, so a lost race just fails the
 * job at start rather than corrupting accounting.
 * @return JOB_MGR_OK, or JOB_MGR_CAP_{GLOBAL,PROVIDER,USER}, or JOB_MGR_FAIL.
 */
int job_manager_capacity(int user_id, job_provider_class_t provider);

#ifdef __cplusplus
}
#endif

#endif /* JOB_MANAGER_H */
