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
 * coding_agent_watch.c: the gateway's events; coding_agent_github.c: reading
 * GitHub pull requests; coding_agent_github_post.c: replying on them and asking
 * Copilot to review) and what they share.  Not for
 * use outside the tool.
 */

#ifndef CODING_AGENT_INTERNAL_H
#define CODING_AGENT_INTERNAL_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/** Whether @p s has anything but whitespace. */
static inline bool coding_agent_has_text(const char *s) {
   return s[strspn(s, " \t\r\n")] != '\0';
}

/** The longest line a confirm's describe_call gives a reply code: under
 *  TOOL_CALL_CHALLENGE_DESC_MAX (core/tool_call_challenge.h) with room to spare;
 *  a longer one is refused, never cut. */
#define CA_DESCRIBE_ROOM (TOOL_CALL_CHALLENGE_DESC_MAX - 48)

/** A macro's value as a string literal. */
#define CA_STR_(x) #x
#define CA_STR(x) CA_STR_(x)

/** Room for a repo's name at the gateway. */
#define CODING_AGENT_REPO_MAX 64

/** Whether a gateway task with @p status is still open: waiting its turn,
 *  working, or waiting on the user (a close stops it first). */
bool coding_task_status_open(const char *status);

/* Files to check before publishing: changed files matching the repo's `check`
 * patterns at the gateway, each pattern with an optional reason.  Flagged to the
 * user before publishing, never blocked. */

/** Files to check named in DAWN's own words; the rest are counted. */
#define CODING_CHECK_SHOWN 5
/** The longest path and reason named there. */
#define CODING_CHECK_PATH_MAX 120
#define CODING_CHECK_WHY_MAX (80 * 4) /* the gateway's 80 characters, in UTF-8 bytes */

/** Whether @p path (named by the agent) is plainly a path DAWN can name in its
 *  own words: short, and only letters, digits, '.', '_', '/', '-'. */
static inline bool coding_check_path_shown(const char *path) {
   const size_t n = path ? strlen(path) : 0;
   return n > 0 && n <= CODING_CHECK_PATH_MAX &&
          strspn(path, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._/-") == n;
}

/** Whether @p why (a reason from the gateway's settings) is shown: short and
 *  printable. */
static inline bool coding_check_why_shown(const char *why) {
   const size_t n = why ? strlen(why) : 0;
   for (size_t i = 0; i < n; i++) {
      if ((unsigned char)why[i] < 0x20 || why[i] == 0x7f) {
         return false;
      }
   }
   return n > 0 && n <= CODING_CHECK_WHY_MAX;
}

/** Whether changed file @p f of a gateway changes listing is one to check (an
 *  older gateway called it `protected`). */
static inline bool coding_file_check(struct json_object *f) {
   return json_object_get_boolean(json_object_object_get(f, "to_check")) ||
          json_object_get_boolean(json_object_object_get(f, "protected"));
}

/** The gateway lists at most this many of a task's commits (its COMMITS_CAP). */
#define CODING_COMMITS_LISTED 100

/** Whether @p repo is a gateway repo name DAWN shows and grants for (letters,
 *  digits, '.', '_', '-'; not "." or ".."). */
bool coding_agent_repo_name_ok(const char *repo);

/** coding_agent_error() of @p text, which it frees ("Failed." when NULL). */
char *coding_agent_error_free(char *text);

/** Note that the current turn returned the agent's words: a preview the user
 *  has seen is never replaced in it. */
void coding_agent_mark_read(void);

/** A report posted as message @p msg_id of @p conv_id carries a take on the
 *  agent's words: the turn it is told to reads them (coding_agent_turn_read). */
void coding_agent_note_take(int64_t conv_id, int64_t msg_id);

/** A preview staged over this session's earlier one of the same kind, made in
 *  turn @p replaced_turn (the slot's replaced_turn; 0: none): that one won't be
 *  answered now, so it no longer holds DAWN's own questions back.  Call with no
 *  tool lock held. */
void coding_agent_preview_replaced(uint32_t replaced_turn);

/** Whether the turn @p o returned the agent's words (coding_agent_mark_read), or
 *  was told a report with a take on them. */
bool coding_agent_turn_read(const turn_origin_t *o);

/** @p obj's @p key as a yes/no in *@p out (@p dflt when it has none): a boolean,
 *  or "true"/"false" as a model may write it.  False for anything else. */
bool coding_agent_yes_no(struct json_object *obj, const char *key, bool dflt, bool *out);

/** The job a call names ({"job": N}); 0 when none. */
int64_t coding_agent_details_job(struct json_object *details);

/** NULL when @p job is a coding job owned by @p user_id (@p rec filled); else
 *  the error result to return. */
char *coding_agent_check_job(int64_t job, int user_id, job_record_t *rec);

/** New arguments naming coding job @p job's task at the gateway (the
 *  caller's; coding_gateway_call takes it). */
struct json_object *coding_agent_job_args(int64_t job);

/** What a staged item is, for confirm: its kind, in the order a confirm of
 *  several carries them out (the work first, close last). */
typedef enum {
   CA_CONFIRM_NONE = 0,
   CA_CONFIRM_DELEGATE,
   CA_CONFIRM_REVISE,
   CA_CONFIRM_APPROVE,
   CA_CONFIRM_PUBLISH,
   CA_CONFIRM_REPLY,
   CA_CONFIRM_REVIEW,
   CA_CONFIRM_CLOSE,
} ca_confirm_kind_t;

/** End a job the user hears about in this reply (no completion notice too). */
void coding_agent_end_quietly(int64_t conv_id, int user_id, const char *status, const char *why);

/** Whether replies on GitHub are signed with the assistant's name
 *  ([coding_agent] sign_replies). */
bool coding_agent_sign_replies(void);

/* ---- coding_agent_delegate.c ---- */

/** delegate: stage a task and return the preview the user confirms. */
char *coding_delegate_propose(struct json_object *details,
                              int user_id,
                              int64_t parent_conv,
                              const turn_origin_t *origin);

/** confirm of staged task @p id: create the coding job and start its task. */
char *coding_delegate_confirm(uint32_t id, int user_id, const turn_origin_t *origin);

/** Whether session @p session_id has a delegate or revise preview waiting for its yes. */
bool coding_delegate_live(uint32_t session_id);

/** What a confirm of staged task @p id starts, for a reply code. */
int coding_delegate_describe(uint32_t id, char *out, size_t out_len, int *valid_for_sec);

/** The kind of item @p id when it is a delegate or revise this session's
 *  @p user_id staged (CA_CONFIRM_NONE otherwise). */
ca_confirm_kind_t coding_delegate_staged(uint32_t id, const turn_origin_t *origin, int user_id);

/** revise: stage more work for a finished job and return the preview the user confirms. */
char *coding_revise_propose(struct json_object *details,
                            int user_id,
                            int64_t parent_conv,
                            const turn_origin_t *origin);

/** confirm of staged revise @p id: reopen the job and send its task the follow-up. */
char *coding_revise_confirm(uint32_t id, int user_id, const turn_origin_t *origin);

/** What a confirm of staged revise @p id sends, for a reply code. */
int coding_revise_describe(uint32_t id, char *out, size_t out_len, int *valid_for_sec);

/* ========== publish and close (coding_agent_publish.c) ========== */

/** publish: a preview of pushing a finished job's branch (and its pull request). */
char *coding_publish_propose(struct json_object *details, int user_id, const turn_origin_t *origin);

/** confirm of staged publish @p id: pushes the commit the preview showed. */
char *coding_publish_confirm(uint32_t id, int user_id, const turn_origin_t *origin);

/** close: drops a finished job's work at once when nothing would be lost, else
 *  stages a preview of what would be. */
char *coding_close(struct json_object *details, int user_id, const turn_origin_t *origin);

/** confirm of staged close @p id: drops it, only while the branch is at the commit shown. */
char *coding_close_confirm(uint32_t id, int user_id, const turn_origin_t *origin);

/** What a confirm of staged publish (or close, @p close) @p id does, for a reply code. */
int coding_publish_describe(uint32_t id, bool close, char *out, size_t out_len, int *valid_for_sec);

/** The kind of item @p id when it is a publish or close this session's
 *  @p user_id staged (CA_CONFIRM_NONE otherwise). */
ca_confirm_kind_t coding_publish_staged(uint32_t id, const turn_origin_t *origin, int user_id);

/** Whether @p url is a pull request's address as GitHub gives it. */
bool coding_publish_pr_url_ok(const char *url);

/* ---- coding_agent_requests.c ---- */

/** approve: stage allowing the host or tool a job waits on; return the preview. */
char *coding_requests_approve(struct json_object *details,
                              int user_id,
                              const turn_origin_t *origin);

/** confirm of staged approval @p id: allow it, in the scope shown (or, for DAWN's
 *  own question, the one {scope} in @p details names), if it is still open. */
char *coding_requests_confirm_approve(uint32_t id,
                                      struct json_object *details,
                                      int user_id,
                                      const turn_origin_t *origin);

/** What a confirm of staged approval @p id allows ({scope} in @p details), for a reply code. */
int coding_requests_describe(struct json_object *details,
                             uint32_t id,
                             char *out,
                             size_t out_len,
                             int *valid_for_sec);

/** CA_CONFIRM_APPROVE when item @p id is an approval staged in this session
 *  for @p user_id (CA_CONFIRM_NONE otherwise). */
ca_confirm_kind_t coding_requests_staged(uint32_t id, const turn_origin_t *origin, int user_id);

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

/* ---- coding_agent_github.c ---- */

/** pr_status: a pull request's state, its blockers in plain words first.
 *  {ref, repo?} or {job}.  Result: caller frees. */
char *coding_github_pr_status(struct json_object *details, int user_id);

/** pr_threads: its unresolved review threads (with all: every one), by
 *  comment_id.  {ref, repo?, all?} or {job}.  Result: caller frees. */
char *coding_github_pr_threads(struct json_object *details, int user_id);

/** pr_log: an excerpt of one check's log.  {ref, repo?, check} or {job, check}.
 *  Result: caller frees. */
char *coding_github_pr_log(struct json_object *details, int user_id);

/** The gateway's arguments naming the pull request a call names: the one job
 *  {job} published, or {ref, repo}.  NULL with the error result in *@p refusal. */
struct json_object *coding_github_pr_args(struct json_object *details, int user_id, char **refusal);

/** Call @p tool on the gateway with @p args (taken); its answer, or NULL with
 *  the error result in *@p refusal. */
struct json_object *coding_github_call(int user_id,
                                       const char *tool,
                                       struct json_object *args,
                                       char **refusal);

/* ---- coding_agent_github_post.c ---- */

/** Replies one confirm of a reply batch posts (the gateway's REPLIES_CAP), and a reply's
 *  text before its signature, in bytes. */
#define CODING_GITHUB_REPLIES_MAX 20
#define CODING_GITHUB_REPLY_BODY_MAX 1000

/** reply: a preview of replies on a pull request's review threads (each
 *  {comment_id, body}), resolving them unless resolve is false. */
char *coding_github_reply(struct json_object *details, int user_id, const turn_origin_t *origin);

/** confirm of staged reply batch @p id: posts the replies the preview showed and
 *  resolves their threads.  An error result only when none was posted;
 *  *@p complete (may be NULL) is false when any reply wasn't. */
char *coding_github_confirm_reply(uint32_t id,
                                  int user_id,
                                  const turn_origin_t *origin,
                                  bool *complete);

/** request_review: a preview of asking Copilot to review a pull request again,
 *  refused while its review is pending or when nothing came after its last one. */
char *coding_github_request_review(struct json_object *details,
                                   int user_id,
                                   const turn_origin_t *origin);

/** confirm of staged Copilot request @p id: asks Copilot. */
char *coding_github_confirm_request_review(uint32_t id, int user_id, const turn_origin_t *origin);

/** What a confirm of staged replies (or Copilot request, @p review) @p id carries
 *  out, for a reply code. */
int coding_github_describe(uint32_t id, bool review, char *out, size_t out_len, int *valid_for_sec);

/** The kind of item @p id when it is a reply batch or Copilot request this
 *  session's @p user_id staged (CA_CONFIRM_NONE otherwise). */
ca_confirm_kind_t coding_github_staged(uint32_t id, const turn_origin_t *origin, int user_id);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_INTERNAL_H */
