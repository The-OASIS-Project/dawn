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
 * The coding_agent tool's parts (coding_agent_tool.c: the callback, the
 * reads and these helpers; coding_agent_delegate.c: starting a task) and
 * what they share.  Not for use outside the tool.
 */

#ifndef CODING_AGENT_INTERNAL_H
#define CODING_AGENT_INTERNAL_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "core/turn_origin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- coding_agent_tool.c ---- */

/** @p text as an error result (TOOL_RESULT_ERROR_MARK in front); NULL on OOM. */
char *coding_agent_error(const char *text);

/** coding_agent_error() of @p text, which it frees ("Failed." when NULL). */
char *coding_agent_error_free(char *text);

/** Note that the current turn returned the agent's words (a delegate or a
 *  confirm in it would rest on them). */
void coding_agent_mark_read(void);

/** Whether the turn @p o returned the agent's words (coding_agent_mark_read). */
bool coding_agent_turn_read(const turn_origin_t *o);

/** The job a call names ({"job": N}); 0 when none. */
int64_t coding_agent_details_job(struct json_object *details);

/** NULL when @p job is a coding job owned by @p user_id (@p rec filled); else
 *  the error result to return. */
char *coding_agent_check_job(int64_t job, int user_id, job_record_t *rec);

/** New arguments naming coding job @p job's task at the gateway (the
 *  caller's; coding_gateway_call takes it). */
struct json_object *coding_agent_job_args(int64_t job);

/** The pending id a confirm names ({"id": N}); 0 when none or out of range. */
uint32_t coding_agent_details_id(struct json_object *details);

/** End a job the user hears about in this reply (no completion notice too). */
void coding_agent_end_quietly(int64_t conv_id, int user_id, const char *status, const char *why);

/* ---- coding_agent_delegate.c ---- */

/** delegate: stage a task and return the preview the user confirms. */
char *coding_delegate_propose(struct json_object *details,
                              int user_id,
                              int64_t parent_conv,
                              const turn_origin_t *origin);

/** confirm_delegate: create the coding job and start its task. */
char *coding_delegate_confirm(struct json_object *details,
                              int user_id,
                              const turn_origin_t *origin);

/** describe_call for confirm_delegate (tool_metadata_t.describe_call). */
int coding_delegate_describe(const char *value, char *out, size_t out_len, int *valid_for_sec);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_INTERNAL_H */
