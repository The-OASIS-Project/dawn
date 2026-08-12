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
 * Deep-research worker (DEEP_RESEARCH_DESIGN.md §4/§15 Step 6).
 *
 * The detached driver that makes a research run runnable end to end: it reserves
 * a background-job pool slot for the run's conversation, marks the job running,
 * loads the run header, and hands the prepared bare session to
 * research_run_execute() (the round loop + synthesis).  When that returns it maps
 * the controller's stop_reason to BOTH the research-run terminal state and the
 * job terminal state, then tears the session down.  Sibling of job_worker.c: it
 * reuses job_manager_* for the pool/caps/teardown but NOT the plain job body — a
 * research job (`job_kind='research'`) runs the controller, not the generic tool
 * loop.  Layer 2 (job_manager + session_manager + research_run + conv_db).
 *
 * Layering note: the worker lives in src/core/ (its job is pool-slot + session
 * lifecycle + terminal-state mapping — a core concern) but drives the controller
 * seam research_run_execute() whose implementation sits in src/tools/
 * (research_run_loop.c — research logic is a tool concern).  This is a deliberate
 * split of a tightly-coupled pair, not a layer inversion: research_run.h is a
 * near-leaf (pulls only auth/auth_db.h), everything compiles into one target, and
 * the same core-includes-tools-header precedent exists (session_manager_llm.c →
 * tools/time_utils.h, scheduler.c → tools/tool_registry.h).
 */

#ifndef RESEARCH_WORKER_H
#define RESEARCH_WORKER_H

#include <stdint.h>

#include "dawn_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Spawn a detached worker that runs an already-created research job.
 *
 * The caller (Step 7's `deep_research` tool) must have already created BOTH the
 * job conversation (conv_db_create_job, status 'queued', job_kind='research') and
 * the research run bound to it (research_db_run_create, status 'planning').  This
 * starts a detached thread that reserves a pool slot, marks the job running,
 * drives research_run_execute() on a bare session, records the terminal states,
 * and frees its session.
 *
 * The brief and all budgets live in the research_runs row keyed on @p conv_id, so
 * unlike job_worker_spawn() no goal text is passed here.
 *
 * @param user_id Owner (the spawner's user_id, non-overridable).
 * @param conv_id The research job conversation id (already created, 'queued').
 * @return SUCCESS if the worker thread was spawned, FAILURE otherwise.
 */
int research_worker_spawn(int user_id, int64_t conv_id);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_WORKER_H */
