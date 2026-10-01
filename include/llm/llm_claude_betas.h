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
 * The Anthropic betas DAWN sends on first-party Claude requests: their request
 * fields, the matching anthropic-beta header, and turning a beta off when the
 * API rejects it.
 */

#ifndef LLM_CLAUDE_BETAS_H
#define LLM_CLAUDE_BETAS_H

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sends of one request: the first, plus one per beta that can be rejected. */
#define CLAUDE_BETA_ATTEMPTS 3

/** The betas one request carries (filled by claude_betas_add). */
typedef struct {
   bool diagnostics;  /**< cache-diagnosis: diagnostics object + beta */
   bool binding;      /**< thinking-binding-controls: block_binding + beta */
   bool inline_tools; /**< inline-tools: the body defines a tool in a message */
   /** Set by claude_betas_rejected: the API rejected tools defined in a
    *  message (the turn folds them; claude_betas_take_inline_rejected) */
   bool inline_rejected;
   char model[64]; /**< The request's model: a rejection is recorded against it */
} claude_betas_t;

/**
 * @brief Whether a request to @p base_url for @p model may send a
 *        conversation's tool changes in place (tool_addition blocks carrying a
 *        definition, beta inline-tools-2026-09-15): the Claude API itself, a
 *        models.toml [inline_tools] model, and the API hasn't rejected the beta
 *        for it in this process
 */
bool claude_betas_inline_tools_ok(const char *base_url, const char *model);

/**
 * @brief Whether a request to @p base_url may send a conversation's stored
 *        inline tool changes in place: the Claude API itself, and the calling
 *        thread's turn hasn't had them rejected
 *
 * Never the process's rejection table or models.toml: whether a stored
 * change goes in place is the conversation's (llm_tool_change_renders_inline);
 * a target that stops taking them is marked on it at the next seam, with a
 * declared boundary (prefix_tools_apply).
 */
bool claude_betas_render_inline(const char *base_url);

/**
 * @brief Whether the calling thread's turn had tools defined in a message
 *        rejected (claude_betas_rejected); clears it
 *
 * Governs only the thread's later requests in the turn (they fold the
 * changes); a turn starts clean through llm_turn_result_reset().  The caller
 * learns of a rejection through the provider-neutral turn result
 * (llm_take_inline_tools_rejected), which the Claude provider sets from
 * claude_betas_t.inline_rejected.
 */
bool claude_betas_take_inline_rejected(void);

/**
 * @brief Add the betas' request fields to a finished request body
 *
 * Call once the body is otherwise complete: the thinking config decides the
 * binding field.  Only the Anthropic API itself (api.anthropic.com) gets them.
 *
 * @param request The Claude request body
 * @param base_url The endpoint it goes to
 * @param sent Filled with what was added (the header must match)
 */
void claude_betas_add(struct json_object *request, const char *base_url, claude_betas_t *sent);

/**
 * @brief Append the anthropic-beta header for what the body carries
 *
 * @return The header list (curl_slist_append semantics)
 */
struct curl_slist *claude_betas_header(struct curl_slist *headers, const claude_betas_t *sent);

/**
 * @brief Handle a failed request's error: is it a rejection of a beta it sent?
 *
 * Matches only the API's own rejection shapes (an unknown anthropic-beta value,
 * or "Extra inputs are not permitted" on the beta's field; for inline tools, an
 * error that names the beta).  A rejected beta is turned off for that model for
 * the rest of the process (acceptance can differ by model: one old model must
 * not cost every other model its betas), and the calling thread is marked for
 * a resend (claude_betas_take_retry).  Rejected inline tools also set
 * @p sent->inline_rejected and mark the calling thread's turn: its resend and
 * later requests fold them; the Claude provider reports it in the turn's
 * result (llm_take_inline_tools_rejected) and the session records it on the
 * conversation, after a restart too.
 *
 * @return true if a beta was rejected (the caller shouldn't report the error)
 */
bool claude_betas_rejected(long http_code, const char *body, claude_betas_t *sent);

/**
 * @brief Whether the calling thread's last request should be sent again
 *
 * Clears the mark.  Callers resend at most CLAUDE_BETA_ATTEMPTS times in all
 * and call this once more after giving up, so no mark outlives its request.
 */
bool claude_betas_take_retry(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLAUDE_BETAS_H */
