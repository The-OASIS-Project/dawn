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
 * A tool exchange as the tool loop appends it to a conversation's history
 * (llm_tool_loop.c): the assistant's call message in the history's format,
 * with the turn's blocks, then the tool results; and the rows they are saved
 * as.
 */

#ifndef LLM_TOOL_HISTORY_H
#define LLM_TOOL_HISTORY_H

#include <stdbool.h>

#include "llm/llm_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;
struct session;

/**
 * @brief The response's blocks with their tool calls replaced by the calls
 *        that ran (all of them with @p calls_ran, none without).
 *
 * @return New blocks (caller owns), or NULL when the response has none.
 */
struct json_object *llm_tool_history_blocks_as_run(const llm_tool_response_t *response,
                                                   bool calls_ran);

/**
 * @brief Append an assistant message with its tool calls, then the tool
 *        results, in OpenAI format.
 */
void llm_tool_history_append_openai(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results);

/**
 * @brief Append an assistant message with its blocks rendered for Claude (or,
 *        without blocks, its text and tool_use blocks), then the tool results
 *        as one user message.
 *
 * @param carrier The request's carrier (llm_request_carrier), for the render.
 */
void llm_tool_history_append_claude(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results,
                                    const char *carrier);

/**
 * @brief Save the messages a tool iteration appended to @p history, from
 *        index @p before_len to its end, through the session's tool-persist
 *        hook, and tell the turn's observers of each call and result.
 *
 * Each message is saved as the rows llm_history_rows_append makes of it: an
 * assistant row with its tool calls, stored blocks and @p reasoning_json; a
 * loop note with its kind; a role="tool" row with its images and whether its
 * call failed (from @p results, which may be NULL).  Each message the hook
 * saved is stamped with its row's id, as a reload of the conversation holds
 * it (a Claude message of results: each result's part with its own, the
 * message with its last).  Runs with no lock held.
 *
 * @param s The session the turn runs on (the caller holds a reference)
 * @param iteration The tool loop's iteration, for the observers' events
 */
void llm_tool_history_persist(struct session *s,
                              struct json_object *history,
                              int before_len,
                              const char *reasoning_json,
                              int iteration,
                              const tool_result_list_t *results);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_HISTORY_H */
