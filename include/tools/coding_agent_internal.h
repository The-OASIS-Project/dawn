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
 * reads and these helpers; coding_agent_delegate.c: starting a task;
 * coding_agent_requests.c: answering what a task waits on;
 * coding_agent_watch.c: the gateway's events) and what they share.  Not for
 * use outside the tool.
 */

#ifndef CODING_AGENT_INTERNAL_H
#define CODING_AGENT_INTERNAL_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "core/turn_origin.h"
#include "tools/coding_agent_card.h"

struct session;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- coding_agent_tool.c ---- */

/** @p text as an error result (TOOL_RESULT_ERROR_MARK in front); NULL on OOM. */
char *coding_agent_error(const char *text);

/** Room for a repo's name at the gateway. */
#define CODING_AGENT_REPO_MAX 64

/** Whether @p repo is a gateway repo name DAWN shows and grants for (letters,
 *  digits, '.', '_', '-'; not "." or ".."). */
bool coding_agent_repo_name_ok(const char *repo);

/** coding_agent_error() of @p text, which it frees ("Failed." when NULL). */
char *coding_agent_error_free(char *text);

/** Note that the current turn returned the agent's words (a delegate or a
 *  confirm in it would rest on them). */
void coding_agent_mark_read(void);

/** A report posted as message @p msg_id of @p conv_id carries a take on the
 *  agent's words: the turn it is told to reads them (coding_agent_turn_read). */
void coding_agent_note_take(int64_t conv_id, int64_t msg_id);

/** Whether the turn @p o returned the agent's words (coding_agent_mark_read), or
 *  was told a report with a take on them. */
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

/** Whether session @p session_id has a delegate or revise preview waiting for its yes. */
bool coding_delegate_live(uint32_t session_id);

/** describe_call for confirm_delegate (tool_metadata_t.describe_call). */
int coding_delegate_describe(const char *value, char *out, size_t out_len, int *valid_for_sec);

/** revise: stage more work for a finished job and return the preview the user confirms. */
char *coding_revise_propose(struct json_object *details, int user_id, const turn_origin_t *origin);

/** confirm_revise: reopen the job and send its task the follow-up. */
char *coding_revise_confirm(struct json_object *details, int user_id, const turn_origin_t *origin);

/** describe_call for confirm_revise. */
int coding_revise_describe(const char *value, char *out, size_t out_len, int *valid_for_sec);

/* ========== publish and close (coding_agent_publish.c) ========== */

/** publish: a preview of pushing a finished job's branch (and its pull request). */
char *coding_publish_propose(struct json_object *details, int user_id, const turn_origin_t *origin);

/** confirm_publish: pushes the commit the preview showed. */
char *coding_publish_confirm(struct json_object *details, int user_id, const turn_origin_t *origin);

/** close: drops a finished job's work at once when nothing would be lost, else
 *  stages a preview of what would be. */
char *coding_close(struct json_object *details, int user_id, const turn_origin_t *origin);

/** confirm_close: drops it, only while the branch is at the commit shown. */
char *coding_close_confirm(struct json_object *details, int user_id, const turn_origin_t *origin);

/** What confirm_publish (or confirm_close, @p close) sends, for a reply code. */
int coding_publish_describe(const char *value,
                            bool close,
                            char *out,
                            size_t out_len,
                            int *valid_for_sec);

/** Whether @p url is a pull request's address as GitHub gives it. */
bool coding_publish_pr_url_ok(const char *url);

/* ---- coding_agent_requests.c ---- */

/** approve: stage allowing the host or tool a job waits on; return the preview. */
char *coding_requests_approve(struct json_object *details,
                              int user_id,
                              const turn_origin_t *origin);

/** confirm_approve: allow it, for that task only, if it is still open. */
char *coding_requests_confirm_approve(struct json_object *details,
                                      int user_id,
                                      const turn_origin_t *origin);

/** describe_call for confirm_approve (tool_metadata_t.describe_call). */
int coding_requests_describe(const char *value, char *out, size_t out_len, int *valid_for_sec);

/** deny: decline the host or tool a job waits on.  One step, no confirm and
 *  no turn check, on purpose: declining reaches no further. */
char *coding_requests_deny(struct json_object *details, int user_id);

/**
 * @brief Stage, on DAWN's own initiative, the approval of the host or tool
 *        request @p req of coding job @p job, for the user to answer in
 *        @p session's next message in conversation @p conv.
 *
 * Only a host or tool DAWN's check passes; not while the session runs a turn,
 * holds an approval or delegate preview, or was shown another preview lately
 * (turn_origin_unasked()).  Its confirm takes only that next message.
 *
 * @param repo        The job's repo at the gateway: DAWN's question offers allowing
 *                    it for every task there too (only this task when it can't be
 *                    shown).
 * @param line        Receives what it asks, in DAWN's words (on success).
 * @param already_out Whether this request already waits for the user's
 *                    answer in that session (nothing new to say).
 * @param turn_out    The session's turn count it was staged at (its answer
 *                    is the next turn).
 * @return The approval's id, or 0 when nothing was staged.
 */
uint32_t coding_requests_stage_unasked(struct session *session,
                                       int user_id,
                                       int64_t job,
                                       int64_t conv,
                                       const char *title,
                                       const char *repo,
                                       struct json_object *req,
                                       char *line,
                                       size_t line_len,
                                       bool *already_out,
                                       uint32_t *turn_out);

/** The user's standing approvals (hosts and tools allowed for every task on a
 *  repo, or on every repo), each with its grant number.  Result: caller frees. */
char *coding_requests_grants(int user_id);

/** Remove standing approval {grant: N}.  Result: caller frees. */
char *coding_requests_revoke(struct json_object *details, int user_id);

/** Drop the approval DAWN staged as @p id (its ask couldn't be posted, or the
 *  user's message was on its way when it was). */
void coding_requests_drop_unasked(uint32_t id);

/** answer: send the user's answer to the agent's question.  One step, no
 *  confirm and no turn check, on purpose: an answer only goes back to the
 *  sandboxed agent and allows it nothing. */
char *coding_requests_answer(struct json_object *details, int user_id);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_INTERNAL_H */
