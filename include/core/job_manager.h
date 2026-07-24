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
 * @brief Completion monitor — call once per second from the main-loop heartbeat.
 *
 * When dirty, drains up to `[jobs] monitor_followups_per_tick` terminal jobs
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
 * @brief Push a parent conversation's active background-job count to its owner's
 *        browser sessions (drives the per-conversation + global "jobs running"
 *        pills).  Weak symbol: a no-op unless WebUI links its strong override.
 */
void webui_broadcast_job_activity(int user_id, int64_t parent_id, int active_count);

/**
 * @brief Query @p parent_id's current active (queued/running) child-job count and
 *        broadcast it.  Call on each spawn / terminal / cancel transition.  No-op
 *        for parent_id <= 0 (a rootless job has no conversation pill to update).
 */
void job_activity_emit(int user_id, int64_t parent_id);

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
