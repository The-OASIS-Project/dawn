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
 *        task_changes' answer, or (@p file) task_file's for @p path as changes
 *        listed it ("a -> b" for a rename) in @p view ("diff" or "new").  The
 *        user's own coding job only.  Blocks on the gateway: call off the lws
 *        thread.  None of it reaches the model.
 * @return The gateway's object (caller puts), or NULL with the error result
 *         (TOOL_RESULT_ERROR_MARK-prefixed) in *err_out.
 */
struct json_object *coding_view_fetch(int user_id,
                                      int64_t job,
                                      bool file,
                                      const char *path,
                                      const char *view,
                                      char **err_out);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_CARD_H */
