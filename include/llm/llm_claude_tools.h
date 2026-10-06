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
 * A Claude request's tools: the `tools` array (the conversation's frozen
 * definitions and the changes folded into it), and a tool change sent in
 * place as tool_addition blocks carrying the full definition (beta
 * inline-tools-2026-09-15; llm_tool_defs.h decides which changes go where).
 */

#ifndef LLM_CLAUDE_TOOLS_H
#define LLM_CLAUDE_TOOLS_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add the request's `tools` (a cache breakpoint on the last) and, on
 *        the tool loop's last call, tool_choice none
 *
 * @param inline_ok Whether the request may send tool changes in place
 * @return true when the tools are the conversation's own set (its changes
 *         that go in place may then be sent: llm_claude_tool_change_blocks)
 */
bool llm_claude_tools_add(json_object *request,
                          json_object *history,
                          bool inline_ok,
                          bool is_remote,
                          int iteration);

/**
 * @brief The content of the system message that sends @p history's tool
 *        change at @p idx in place: one tool_addition block per definition
 * @return A new array (caller puts), or NULL when the change folds into
 *         `tools` instead (llm_tool_change_renders_inline)
 */
json_object *llm_claude_tool_change_blocks(json_object *history, size_t idx, bool inline_ok);

/**
 * @brief Add @p history's tool change at @p idx to @p messages where it goes:
 *        in place (llm_claude_tool_change_blocks) as a system message after
 *        the user turn before it, or joined to the system message already
 *        there (a run of them is one message); nothing when it folds into
 *        `tools`.  @p last_message / @p last_role track the request's newest
 *        message, as the formatter keeps them.
 */
void llm_claude_tool_change_add(json_object *messages,
                                json_object *history,
                                size_t idx,
                                bool inline_ok,
                                json_object **last_message,
                                const char **last_role);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLAUDE_TOOLS_H */
