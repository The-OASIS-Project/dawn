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
 * Shared background-job dispatch helpers (Layer 2).
 *
 * Small pieces the job worker, the reinvoke worker, and the job tool all need
 * identically — the job's LLM selection, its provider counter, and the tool-turn persist
 * callback.  Kept in their own translation unit (NOT job_manager.c) so the
 * job-pool unit test can keep bare-linking job_manager.c without stubbing the
 * LLM/conv_db surface these helpers pull in.
 */

#ifndef JOB_DISPATCH_H
#define JOB_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db.h"      /* conv_llm_columns_t */
#include "core/job_manager.h"  /* job_provider_class_t */
#include "llm/llm_interface.h" /* session_llm_config_t */

struct session;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The LLM a job spawned now runs on: the spawning turn's (its session's
 *        resolved selection), or dawn.toml's default when no session spawned it
 *        (the admin verb).
 */
void job_selection_of_spawner(session_llm_config_t *out);

/**
 * @brief A job's LLM as the columns conv_db_create_job_ex writes, so its runs and
 *        Resume use the selection it was spawned on (type, provider, model and
 *        reasoning; the endpoint comes from dawn.toml)
 *
 * @p out points into @p cfg, which must outlive it.
 */
void job_selection_columns(const session_llm_config_t *cfg, conv_llm_columns_t *out);

/**
 * @brief A conversation's stored LLM as stored: none of its own is LLM_UNDEFINED
 *        (follows the default), as a parent conversation's row may be
 * @return AUTH_DB_SUCCESS or an AUTH_DB_* error
 */
int job_selection_load(int64_t conv_id, int user_id, session_llm_config_t *out);

/**
 * @brief Which provider counter a job on @p cfg runs against (an unset selection
 *        counts as the default's)
 * @return JOB_PROVIDER_LOCAL for a local LLM, JOB_PROVIDER_CLOUD otherwise.
 */
job_provider_class_t job_provider_for(const session_llm_config_t *cfg);

/**
 * @brief Resolve the three AUTHORITATIVE terminal-disposition signals for a
 *        finished worker turn, race-free and in one place.
 *
 * The tool loop's own "cancelled"/empty result is ambiguous: a human Cancel, the
 * runtime reap, and daemon shutdown all arrive as the session cancel flag.  This
 * disambiguates them the way both workers must:
 *   - @p user_cancelled — a human asked to stop.  job_manager_cancel() sets this
 *     under the pool lock BEFORE raising cancel_requested, and claim_reaped() reads
 *     it under the same lock, so keying on it (never the bare cancel_requested
 *     snapshot) is race-free.  Reading cancel_requested instead once filed real
 *     Cancels/Ctrl+C as "failed: no response" (live-verified, conv 1038).
 *   - @p reaped — the runtime deadline fired.  claim_reaped() also STOPS the reap
 *     clock, so nothing can re-flag the job while the terminal writes are in flight.
 *   - @p shutdown_stop — the daemon pulled the rug (!reaped && !user_cancelled &&
 *     shutting down), isolated so it neither suppresses a completion notice as a
 *     user-cancel would nor lands in a human-only-resumable state.
 *
 * Call ONCE per finished turn (it claims the reap under the lock).  Each worker
 * then maps these to its own status strings — job_worker keys 'done' on a produced
 * answer, research_worker on the controller's stop reason — so the string mapping
 * stays per-worker; only this shared signal resolution is extracted.  Any out-ptr
 * may be NULL.
 */
void job_disposition_signals(struct session *s,
                             bool *user_cancelled,
                             bool *reaped,
                             bool *shutdown_stop);

/**
 * @brief Resolve the spawn surface for the current command context, for
 *        conversations.origin (gates completion delivery).
 *
 * Maps the thread's command-context session type to "voice"/"webui"/"messaging"/
 * "satellite", or "job" when there is no session (or a job spawning a job).  Only
 * "voice" (the local mic) speaks on the Jetson speaker; a satellite has its own.
 * Shared by every job-spawning surface (the `job` and `deep_research` tools) so
 * the mapping cannot drift between them.
 *
 * @return a static string (do not free).
 */
const char *job_spawn_origin_string(void);

/** Context for the tool-turn persist callback: the job's own conversation. */
typedef struct {
   int64_t conv_id;
   int user_id;
} job_persist_ctx_t;

/**
 * @brief Tool-loop persist hook: write a structured tool-turn row (assistant
 *        tool_calls / role:tool result) to the conversation named by
 *        @p userdata (a job_persist_ctx_t*).  Matches session_tool_persist_fn.
 * @return The row's id, or 0 when it wasn't saved
 */
int64_t job_dispatch_tool_persist_cb(void *userdata, const session_tool_row_t *row);

/**
 * @brief The WebUI's tool-iteration hook (seals a viewer's streaming bubble at
 *        each tool boundary), defined in webui_text_processing.c, or as a no-op
 *        in webui_absent.c.  Declared weak so a job runner reaches it without
 *        including the WebUI; every daemon build links one of the two, so it can
 *        be absent only in a test binary.  A file that DEFINES it must not include
 *        this header, or its definition becomes weak.
 */
__attribute__((weak)) void webui_tool_iteration_cb(struct session *session, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* JOB_DISPATCH_H */
