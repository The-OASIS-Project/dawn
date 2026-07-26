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
 * Background-job worker (background-jobs Phase 1).
 *
 * Runs a job's LLM turn to completion on a job-pool session: reserve a slot,
 * drive the goal through the shared tool loop (no audio), persist the tool
 * iterations + final answer to the job's own conversation, transition the job
 * status queued -> running -> done|failed|cancelled, and tear the session down.
 * Layer 2 (depends on job_manager + session_manager + text_input_dispatch +
 * conv_db + llm_interface).
 */

#ifndef JOB_WORKER_H
#define JOB_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Spawn a detached worker that runs an already-created (queued) job.
 *
 * The caller (the `job` tool) must have already created the job conversation
 * via conv_db_create_job() (status 'queued').  This starts a detached thread
 * that reserves a pool slot, marks the job running, dispatches @p goal through
 * the LLM tool loop, persists the result to conv @p conv_id, sets the terminal
 * status, and frees its session.
 *
 * @param user_id Owner (the spawner's user_id).
 * @param conv_id The job conversation id (already created, status 'queued').
 * @param goal The job goal / instruction text (copied).
 * @return SUCCESS if the worker thread was spawned, FAILURE otherwise.
 */
int job_worker_spawn(int user_id, int64_t conv_id, const char *goal);

/* NOTE: the unguarded resume primitives (raw spawn-with-history-hydration) are
 * deliberately NOT exported.  They perform no ownership check, no capacity
 * check, no atomic claim and no `resumed` broadcast — the four things
 * job_worker_resume() exists to guarantee — so exposing them next to it would
 * make the unsafe call the shorter one to reach for. */

/** Outcome of job_worker_resume(). */
typedef enum {
   JOB_RESUME_STARTED,       /**< claimed and a worker was spawned */
   JOB_RESUME_NOT_RESUMABLE, /**< exists, but is done/cancelled/running/queued */
   JOB_RESUME_CAPACITY,      /**< job caps full; nothing was mutated */
   JOB_RESUME_FORBIDDEN,     /**< owned by a different user */
   JOB_RESUME_NOT_FOUND,     /**< no such job */
   /* Pre-v74 job that never dispatched: no transcript to continue, and no stored
    * goal to restart from (its goal only ever existed as a `messages` row that
    * was never written).  Refused rather than run, because resuming with nothing
    * to do yields an empty `done` — and `done` is not resumable, so the mistake
    * is destructive. */
   JOB_RESUME_NOTHING_TO_RUN,
   JOB_RESUME_FAILED /**< claimed, but the worker thread would not start */
} job_resume_result_t;

/**
 * @brief Resume an interrupted/failed job — ONE mutation path for every surface.
 *
 * Ownership check, capacity check, atomic claim, `resumed` broadcast and worker
 * spawn, in the order that leaves nothing half-done: capacity is tested BEFORE
 * the claim so a full pool refuses without touching the row.
 *
 * Callers (the `job` tool, the WebUI `job_action` handler) supply their own
 * caller's user_id and only word the outcome — the authorization is here, so a
 * new surface cannot forget it.
 */
job_resume_result_t job_worker_resume(int64_t conv_id, int user_id);

#ifdef __cplusplus
}
#endif

#endif /* JOB_WORKER_H */
