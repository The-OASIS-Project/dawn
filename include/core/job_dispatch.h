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
 * identically — the provider-counter selector and the tool-turn persist
 * callback.  Kept in their own translation unit (NOT job_manager.c) so the
 * job-pool unit test can keep bare-linking job_manager.c without stubbing the
 * LLM/conv_db surface these helpers pull in.
 */

#ifndef JOB_DISPATCH_H
#define JOB_DISPATCH_H

#include <stdint.h>

#include "core/job_manager.h" /* job_provider_class_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve which provider counter a default-config job runs against.
 * @return JOB_PROVIDER_LOCAL for a local LLM, JOB_PROVIDER_CLOUD otherwise.
 */
job_provider_class_t job_provider_from_default(void);

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
 */
void job_dispatch_tool_persist_cb(void *userdata,
                                  const char *role,
                                  const char *content,
                                  const char *tool_calls_json,
                                  const char *tool_call_id,
                                  const char *reasoning_json);

#ifdef __cplusplus
}
#endif

#endif /* JOB_DISPATCH_H */
