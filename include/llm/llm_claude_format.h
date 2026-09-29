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
 * - Adds vision content if provided
 * - Handles thinking blocks for extended thinking mode
 *
 * @param openai_conversation OpenAI-format conversation array
 * @param input_text Current user input (may be NULL if already in conversation)
 * @param vision_images Array of base64-encoded image data (may be NULL)
 * @param vision_image_sizes Array of image sizes (may be NULL)
 * @param vision_image_count Number of images (0 if not used)
 * @param model Model name (NULL to use config default)
 * @param carrier The request's carrier (llm_request_carrier): whose stored
 *                reasoning goes back
 * @param iteration Tool iteration count (0 for initial call, >0 for follow-ups).
 *                  Orphaned tool_use filtering only runs on iteration 0.
 * @return json_object containing Claude-format request, or NULL on error
 *         Caller must json_object_put() when done
 */
json_object *convert_to_claude_format(struct json_object *openai_conversation,
                                      const char *input_text,
                                      const char **vision_images,
                                      const size_t *vision_image_sizes,
                                      int vision_image_count,
                                      const char *model,
                                      const char *carrier,
                                      int iteration);

/**
 * @brief Detect an image's MIME type from its base64-encoded magic bytes
 *
 * Checks the leading base64 characters against known PNG/JPEG/GIF/WebP
 * signatures. Falls back to "image/jpeg" when the data is too short or
 * unrecognized.
 *
 * @param base64_data Base64-encoded image data
 * @return MIME type string (static, do not free)
 */
const char *llm_claude_detect_image_mime_type(const char *base64_data);

/**
 * @brief Build a Claude-format image content block
 *
 * Wraps base64 image data as {"type":"image","source":{"type":"base64",
 * "media_type":<detected>,"data":<base64_data>}}. MIME type is
 * auto-detected via llm_claude_detect_image_mime_type().
 *
 * @param vision_image Base64-encoded image data
 * @return json_object owned by the caller (json_object_put() when done)
 */
json_object *llm_claude_create_image_block(const char *vision_image);

#endif  // LLM_CLAUDE_FORMAT_H
