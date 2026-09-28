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
 */

#ifndef LLM_CLAUDE_H
#define LLM_CLAUDE_H

#include <json-c/json.h>
#include <stddef.h>

/* Forward declaration for single-shot result type (defined in llm_tools.h) */
struct llm_tool_response;

/*
 * Anthropic Claude Configuration
 *
 * Model, API version, and behavior settings for Claude provider.
 * These are compile-time defaults; runtime config in dawn.toml takes precedence.
 */

/* Default model for Claude */
#define CLAUDE_MODEL "claude-sonnet-4-5-20250929"
/* Alternative models:
 * #define CLAUDE_MODEL "claude-haiku-4-5-20241022"    // Faster, cheaper
 * #define CLAUDE_MODEL "claude-opus-4-20250514"       // Most capable
 */

/* Claude API version header value */
#define CLAUDE_API_VERSION "2023-06-01"

/* Max tokens for completion */
#define CLAUDE_MAX_TOKENS 4096

/* API endpoint path */
#define CLAUDE_MESSAGES_ENDPOINT "/v1/messages"

/**
 * @brief Callback function type for streaming text chunks
 */
typedef void (*llm_claude_text_chunk_callback)(const char *chunk, void *userdata);

/**
 * @brief Claude chat completion (non-streaming)
 *
 * Handles Anthropic Claude API calls with automatic format conversion.
 * Conversation history is provided in OpenAI format and converted internally
 * to Claude's format. Supports vision API and prompt caching.
 *
 * @param conversation_history JSON array of messages (OpenAI format - will be converted)
 * @param input_text User input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes in bytes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param base_url Base URL (should be https://api.anthropic.com)
 * @param api_key Anthropic API key (required)
 * @param model Model name (NULL to use config default)
 * @return Response text (caller must free), or NULL on error
 */
char *llm_claude_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 const char **vision_images,
                                 const size_t *vision_image_sizes,
                                 int vision_image_count,
                                 const char *base_url,
                                 const char *api_key,
                                 const char *model);

/**
 * @brief Single-shot Claude streaming call (no tool execution or recursion)
 *
 * Makes exactly one HTTP call and returns structured results. Does NOT execute
 * tools, append to history, or recurse. Used by the central tool iteration loop.
 *
 * @param conversation_history JSON array of messages (OpenAI format - converted internally)
 * @param input_text User input text (empty string for follow-up calls)
 * @param vision_images Array of base64 images (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param base_url API base URL
 * @param api_key Anthropic API key (required)
 * @param model Model name (NULL = use config default)
 * @param chunk_callback Streaming text callback
 * @param callback_userdata User context for callback
 * @param iteration Current iteration (controls tool inclusion and format conversion)
 * @param result Output: structured response (see llm_tools.h llm_tool_response_t)
 * @return 0 on success, non-zero on error (result not populated)
 */
int llm_claude_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char **vision_images,
                                     const size_t *vision_image_sizes,
                                     int vision_image_count,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_claude_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     struct llm_tool_response *result);

#endif  // LLM_CLAUDE_H
