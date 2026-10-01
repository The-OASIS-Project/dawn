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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Claude API format conversion utilities.
 * Converts OpenAI-format conversation history to Claude's native format.
 */

#ifndef LLM_CLAUDE_FORMAT_H
#define LLM_CLAUDE_FORMAT_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "llm/llm_tools.h"

/**
 * @brief Convert OpenAI-format conversation to Claude's native format
 *
 * Transforms conversation history from OpenAI's message format to Claude's format:
 * - Extracts system messages for Claude's system parameter
 * - Converts role names and content structure
 * - Handles tool calls and results
 * - Handles thinking blocks for extended thinking mode
 *
 * @param openai_conversation OpenAI-format conversation array
 * @param input_text Current user input (may be NULL if already in conversation)
 * @param model Model name (NULL to use config default)
 * @param carrier The request's carrier (llm_request_carrier): whose stored
 *                reasoning goes back
 * @param iteration Tool iteration count (0 for initial call, >0 for follow-ups).
 *                  Orphaned tool_use filtering only runs on iteration 0.
 * @param inline_tools Whether the request may send the conversation's stored
 *                     inline tool changes in place (claude_betas_render_inline):
 *                     the rows and the conversation's record decide the rest
 * @return json_object containing Claude-format request, or NULL on error
 *         Caller must json_object_put() when done
 */
json_object *convert_to_claude_format(struct json_object *openai_conversation,
                                      const char *input_text,
                                      const char *model,
                                      const char *carrier,
                                      int iteration,
                                      bool inline_tools);

#endif  // LLM_CLAUDE_FORMAT_H
