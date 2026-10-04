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
 * State and helpers llm_tools.c shares with llm_tools_filter.c.  Not for
 * anything else: the public API is llm_tools.h.
 */

#ifndef LLM_TOOLS_INTERNAL_H
#define LLM_TOOLS_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/tool_call_policy.h"
#include "llm/llm_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** The registered tools (llm_tools_count of them).  Read under
 *  llm_tools_mutex; its entries' flags are written under it.  Which tools it
 *  holds is written only by llm_tools_init / llm_tools_cleanup, at startup
 *  and shutdown, with no other reader running.  Lock order: llm_tools_mutex,
 *  then the tool registry's own (schemas are built from registry lookups). */
extern tool_definition_t llm_tools_table[LLM_TOOLS_MAX_TOOLS];
extern int llm_tools_count;
/** Set once the table is built (llm_tools_init). */
extern bool llm_tools_ready;
extern pthread_mutex_t llm_tools_mutex;
/** Rises when the table's tools change (llm_tools_init / cleanup): with
 *  tool_registry_generation(), what cached schema work is keyed on. */
extern _Atomic uint64_t llm_tools_generation;

/** Tell the registered observer (the WebUI) a tool call started or ended. */
void llm_tools_notify_execution(const char *tool_name,
                                const char *tool_args,
                                const char *result,
                                bool success);

/* --- Reply codes (llm_tools_reply_code.c): a call from a text that waits for
 *     the user's code, and running it once approved --- */

/** A call as resolved, for checking an approved call is the one held. */
#define LLM_TOOLS_BINDING_HEX 65

/**
 * @brief The fingerprint of a resolved call as the user is shown it: tool,
 *        device, action, kind, value and DAWN's description of it
 *
 * Made when the call is held and again when the approved call runs; they
 * must match.
 * @param why Receives the tool's reason when it can't be described (may be
 *            NULL; "" when it gave none)
 * @return SUCCESS, or FAILURE when it can't be described now (out is "")
 */
/** A new request by text prepares something: an action an earlier message
 *  held for its code is dropped.  @return whether one was. */
bool llm_tools_drop_earlier_code(void);

/**
 * @brief After a request by text prepared something: its confirm is asked for
 *        now, not after a "yes" (it waits for the user's reply code, and the
 *        code text is the confirmation); and the model is told when an
 *        earlier waiting request was dropped for it.  Appended to @p result.
 */
void llm_tools_note_text_preview(const tool_metadata_t *meta,
                                 const char *action,
                                 bool dropped_earlier,
                                 tool_result_t *result);

int llm_tools_approved_binding(const tool_call_t *call,
                               const tool_metadata_t *meta,
                               const tool_call_verdict_t *verdict,
                               const char *device,
                               const char *value_buf,
                               char out[LLM_TOOLS_BINDING_HEX],
                               char *why,
                               size_t why_len);

/** Hold a call the gate decided waits for the user's reply code; fills
 *  @p result with what the model is told.  @return 1 (it didn't run). */
int llm_tools_hold_for_reply_code(const tool_call_t *call,
                                  const tool_metadata_t *meta,
                                  const tool_call_verdict_t *verdict,
                                  const char *device,
                                  const char *value_buf,
                                  tool_result_t *result);

/** Run @p call approved by code: allowed only if it resolves to @p binding. */
int llm_tools_execute_approved(const tool_call_t *call, tool_result_t *result, const char *binding);

/** Free what llm_tools_filter.c caches (llm_tools_cleanup). */
void llm_tools_filter_release(void);

/** @p tool's parameters as a JSON schema (caller puts). */
struct json_object *llm_tools_parameters_schema(const tool_definition_t *tool);

/** @p t's description as the model is sent it (the registry's, whole). */
const char *llm_tools_effective_description(const tool_definition_t *t);

/**
 * @brief Whether @p t may be used (and is advertised) on the current command
 *        context's surface now: enabled, allowed for a local or remote
 *        session, and under the research allowlist and a turn's tool masks
 */
bool llm_tools_enabled_for_session(const tool_definition_t *t, bool is_remote);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOLS_INTERNAL_H */
