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
 * A coding job's card in the WebUI: what the job waits on, and the user's
 * click answering it, checked against the live request (coding_agent_requests.c).
 */

#ifndef CODING_AGENT_CARD_H
#define CODING_AGENT_CARD_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A commit's full hash, and a SHA-256 in hex: what the viewer's buttons bind. */
#define CODING_HEAD_LEN 40
#define CODING_SHA256_LEN 64

/** The user's own pull request title and description at publish, in bytes, so
 *  never more than the gateway's character limits: its PR_TITLE_CAP, and its
 *  PR_BODY_CAP less room for its footer (it checks the exact sum itself). */
#define CODING_PR_TITLE_MAX 120
#define CODING_PR_DESC_MAX 5900

/** What the user chose on the WebUI card (coding_requests_card_reply). */
typedef enum {
   CODING_CARD_ALLOW,      /**< allow for this task */
   CODING_CARD_ALLOW_REPO, /**< allow for every task on the job's repo */
   CODING_CARD_DENY,       /**< deny (a question: declined without an answer) */
   CODING_CARD_ANSWER,     /**< the user's answer to the agent's question */
} coding_card_decision_t;

/**
 * @brief Job @p job for the WebUI: {agent ("Claude Code"), model (the model its
 *        agent last ran on, when known), requests: its open requests, each
 *        {request_id, kind ("permission"|"question"), line (DAWN's checked words,
 *        "" when it can't show it), repo (when allowing on the repo can be
 *        offered), question (the agent's question)}}.  Blocks on the gateway:
 *        call off the lws thread.
 * @return The object (caller puts), or NULL with the error result in *err_out.
 */
struct json_object *coding_requests_open(int user_id, int64_t job, char **err_out);

/**
 * @brief The user's click on the WebUI card, checked against the live request:
 *        decisions for a host or tool only, Allow only when DAWN renders the
 *        request and @p shown (the line the card showed) still matches, Allow on
 *        the repo only when it can be named, an answer only for a question.
 *        Blocks on the gateway: call off the lws thread.
 * @return The result (TOOL_RESULT_ERROR_MARK-prefixed on refusal); caller frees.
 */
char *coding_requests_card_reply(int user_id,
                                 int64_t job,
                                 int64_t rid,
                                 coding_card_decision_t decision,
                                 const char *answer,
                                 const char *shown);

/**
 * @brief The user's standing approvals for the WebUI: an array of {id, kind,
 *        value (as stored: control characters dropped, clipped), checked (DAWN
 *        can show it as a host or tool name), repo (the repo it covers; absent:
 *        every repo)}.  Every row is listed, checked or not, so each can be
 *        revoked.  Blocks on the gateway: call off the lws thread.
 * @return The array (caller puts), or NULL with the error result in *err_out.
 */
struct json_object *coding_requests_grant_list(int user_id, char **err_out);

/**
 * @brief Remove standing approval @p grant of @p user_id.  Blocks on the gateway.
 * @return The result (TOOL_RESULT_ERROR_MARK-prefixed on refusal); caller frees.
 */
char *coding_requests_grant_revoke(int user_id, int64_t grant);

/**
 * @brief A coding job's work for the WebUI's viewer, from the gateway's mirror:
 *        task_changes' answer plus job_state {status, closed, published, pr_url
 *        (checked), unpublished_commits (-1: unknown)}, the facts the viewer's
 *        buttons rest on; a closed job's work is gone, so its files and commits
 *        come back empty without asking for them.  Or (@p file) task_file's for
 *        @p path as changes listed it ("a -> b" for a rename) in @p view ("diff"
 *        or "new").  @p scope (NULL or "": all) narrows either to since_push or
 *        one of the job's commits by hash; the caller checks its characters.
 *        The user's own coding job only.  Blocks on the gateway: call
 *        off the lws thread.  None of it reaches the model.
 * @return The gateway's object (caller puts), or NULL with the error result
 *         (TOOL_RESULT_ERROR_MARK-prefixed) in *err_out.
 */
struct json_object *coding_view_fetch(int user_id,
                                      int64_t job,
                                      bool file,
                                      const char *path,
                                      const char *view,
                                      const char *scope,
                                      char **err_out);

/**
 * @brief The WebUI's publish preview of @p job: what a push would do, from the
 *        gateway, with the same checks as the tool's publish.  {head, repo,
 *        branch, base, commits, commits_more, files, files_total, to_check:
 *        [{path, why}] (files to check before publishing; why "" for none),
 *        history_only: [{path, to_check, why?}], workflows, pr_title, pr_body
 *        (exactly what would go public), pr_body_sha256, pr_title_by_agent,
 *        pr_footer (what the gateway appends to the user's own description),
 *        mentions, closes,
 *        pr_url (its open pull request, when it has one)}.  The agent's text is
 *        raw: the client shows it as text.  Blocks on the gateway.
 * @return The object (caller puts), or NULL with the refusal in *err_out.
 */
struct json_object *coding_publish_preview(int user_id, int64_t job, char **err_out);

/**
 * @brief The user's click on Publish: push @p job's branch at @p head and open
 *        (or join) its pull request, refused by the gateway when the branch moved
 *        or the agent's description no longer hashes to @p body_sha.  The title
 *        is the agent's, else the job's name; @p pr_title / @p pr_description
 *        (NULL: the agent's) are the user's own text for a new pull request, and
 *        with @p pr_description, @p body_sha may be NULL.  On success a
 *        chat-staged publish of the job is dropped and DAWN's line is posted to
 *        the job's conversation.  Blocks on the gateway.
 * @param pr_url_out The pull request's address on success (to free), else NULL.
 * @return The result (TOOL_RESULT_ERROR_MARK-prefixed on refusal); caller frees.
 */
char *coding_publish_click(int user_id,
                           int64_t job,
                           const char *head,
                           const char *body_sha,
                           const char *pr_title,
                           const char *pr_description,
                           char **pr_url_out);

/**
 * @brief The user's click on Close: without @p confirm, closes @p job when
 *        nothing would be lost (stopping its task first if it still runs), else
 *        says what would be; with @p confirm, closes it while its branch is at
 *        @p head ("" when it has no commits).  {closed, message, needs_confirm:
 *        {commits, files, branch, head} when work would be lost}.  On a close a
 *        chat-staged close is dropped and DAWN's line posted.  Blocks on the
 *        gateway.
 * @return The object (caller puts), or NULL with the refusal in *err_out.
 */
struct json_object *coding_close_click(int user_id,
                                       int64_t job,
                                       bool confirm,
                                       const char *head,
                                       char **err_out);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_CARD_H */
