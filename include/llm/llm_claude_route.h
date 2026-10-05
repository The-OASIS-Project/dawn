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
 * Which calls go to an Anthropic Messages endpoint, and what each one gets:
 * the Anthropic API itself, or OpenRouter's Messages endpoint for its
 * anthropic/ models.  The route (where a call goes, in which wire format) is
 * separate from the provider (whose key, prices and metrics the call books).
 */

#ifndef LLM_CLAUDE_ROUTE_H
#define LLM_CLAUDE_ROUTE_H

#include <stdbool.h>

#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a Messages endpoint takes, by its host. */
typedef struct {
   /** Whose call it is: usage, prices and metrics book here.  OPENROUTER for
    *  openrouter.ai: Bearer key, binding controls, pinned to Anthropic */
   cloud_provider_t provider;
   bool first_party; /**< api.anthropic.com: every beta, x-api-key */
} llm_claude_route_t;

/**
 * @brief The route a Messages request to @p base_url takes
 * Any other host (a proxy) is treated as Claude with no betas, x-api-key.
 */
llm_claude_route_t llm_claude_route(const char *base_url);

/**
 * @brief Whether a call goes out in the Anthropic Messages format
 * Claude always; OpenRouter for an anthropic/ model (a :variant too) on
 * openrouter.ai itself (a custom OpenRouter endpoint stays on Chat Completions).
 * @param model The call's model (an OpenRouter call needs one: pass the default)
 */
bool llm_uses_anthropic_messages(llm_type_t type,
                                 cloud_provider_t provider,
                                 const char *model,
                                 const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLAUDE_ROUTE_H */
