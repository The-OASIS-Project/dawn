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
   bool diagnostics; /**< cache-diagnosis: diagnostics object + beta */
   bool binding;     /**< thinking-binding-controls: block_binding + beta */
   char model[64];   /**< The request's model: a rejection is recorded against it */
} claude_betas_t;

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
 * or "Extra inputs are not permitted" on the beta's field).  A rejected beta is
 * turned off for that model for the rest of the process (acceptance can differ
 * by model: one old model must not cost every other model its betas), and the
 * calling thread is marked for a resend (claude_betas_take_retry).
 *
 * @return true if a beta was rejected (the caller shouldn't report the error)
 */
bool claude_betas_rejected(long http_code, const char *body, const claude_betas_t *sent);

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
