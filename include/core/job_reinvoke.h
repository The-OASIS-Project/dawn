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
 * Background-job result reinjection (reinvoke_parent, background-jobs Phase 3).
 *
 * When a job whose on_complete='reinvoke_parent' finishes, its full result is
 * injected back into the job's PARENT conversation and the LLM is re-engaged so
 * the assistant reacts and tells the user — instead of a passive toast.
 *
 * This module owns ALL reinvoke policy (Layer 2): it filters the monitor's
 * drained rows to the reinvoke ones, groups them by parent conversation
 * (coalescing several completions that share a parent into ONE re-engagement),
 * serializes per parent (single-writer), caps global concurrency, and runs the
 * re-engagement on a detached, hydrated job-pool session.  The completion
 * monitor (job_manager.c) hands rows in via a registered processor fn-pointer,
 * so job_manager has NO compile-time dependency on this module (one-way edge).
 *
 * See docs/BACKGROUND_JOBS_DESIGN.md §Phase 3 and the plan
 * ~/.claude/plans/reinvoke-parent-v1.md.
 */

#ifndef JOB_REINVOKE_H
#define JOB_REINVOKE_H

#include "auth/auth_db.h"
#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Weak Layer-4 seam: find + RETAIN a live, connected WebUI session that is
 *        currently viewing @p conv_id (owned by @p user_id), so a reinvoke turn
 *        can be enqueued onto its turn queue and STREAMED into it (warm cache,
 *        native token stream) instead of run on a detached session.
 *
 * Retain-only: the turn claim (begin_turn_flags + request_generation +
 * turn_in_flight + stream_conversation_id binding) is deliberately NOT done here
 * — it happens at DEQUEUE inside the reinvoke turn closure, so the turn queue's
 * in-flight gate (not a second ad-hoc gate) is the sole serialization point.  A
 * mid-turn viewer is returned (the reinvoke queues behind the active turn).  The
 * caller MUST session_release() the result on every path.
 *
 * Returns NULL when no connected viewer exists (→ detached fallback).  No-op NULL
 * without WebUI (weak default in job_reinvoke.c).
 */
session_t *webui_find_reinvoke_viewer(int64_t conv_id, int user_id);

/**
 * @brief Weak Layer-4 seam: the conversation id the WebUI client on @p s is
 *        currently viewing (0 without WebUI / no attached client).
 *
 * The reinvoke closure uses this at dequeue to decide whether the streamed reply
 * will be client-saved (viewer still on the parent conv → skip server persist to
 * avoid a duplicate row) or must be persisted server-side (viewer switched
 * conversations → the client saver only writes the on-screen conv, so the reply
 * would otherwise be lost).  Weak default returns 0.
 */
int64_t webui_session_active_conversation(session_t *s);

/**
 * @brief Register the reinvoke processor with the completion monitor.
 *
 * Idempotent.  Call once after job_manager_init().  Wires
 * job_reinvoke_process_pending() into jobs_monitor_tick() via job_manager's
 * fn-pointer seam.
 */
void job_reinvoke_init(void);

/**
 * @brief Processor invoked by the completion monitor with a tick's drained rows.
 *
 * Runs ON the main-loop heartbeat thread — does only bounded, non-blocking work
 * (grouping + the in-flight/concurrency checks + spawning detached workers).
 * Rows it takes responsibility for are NOT marked fired by the monitor; the
 * detached worker marks them fired only after the re-engagement persists (or
 * force-fires a runaway).  Rows it defers (parent busy / at the concurrency cap)
 * are left unfired and the dirty gate is re-armed so they retry on a later tick.
 *
 * @param rows The monitor's drained terminal+unfired rows (borrowed; copied out).
 * @param n Row count.
 */
void job_reinvoke_process_pending(const job_record_t *rows, int n);

/**
 * @brief Nudge a user's open tabs to refresh a conversation that just grew.
 *
 * Weak Layer-2 seam (a no-op unless WebUI links its strong override), so a user
 * viewing the parent conversation sees the reinvoked reply appear.  The reply is
 * already persisted, so a not-viewing/absent client simply sees it on next load.
 * The strong override delegates to the existing conversation_messages_appended
 * broadcast.  (v1.1 will additionally reuse a live session's warm cache to stream
 * token-by-token; v1 surfaces the completed message.)
 */
void job_reinvoke_notify_conv_appended(int user_id, int64_t conversation_id);

#ifdef __cplusplus
}
#endif

#endif /* JOB_REINVOKE_H */
