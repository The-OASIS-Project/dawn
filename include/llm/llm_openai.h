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

#ifndef LLM_OPENAI_H
#define LLM_OPENAI_H

#include <json-c/json.h>
#include <stddef.h>

/* Forward declaration for single-shot result type (defined in llm_tools.h) */
struct llm_tool_response;

/*
 * OpenAI Configuration
 *
 * Model and max_tokens are now configured via g_config.llm.cloud.model
 * and g_config.llm.max_tokens (see config/dawn_config.h).
 *
 * Legacy OPENAI_MODEL and OPENAI_MAX_TOKENS defines have been removed.
 */

/* API endpoint path */
#define OPENAI_CHAT_ENDPOINT "/v1/chat/completions"

/**
 * @brief Callback function type for streaming text chunks
 */
typedef void (*llm_openai_text_chunk_callback)(const char *chunk, void *userdata);

/**
 * @brief OpenAI chat completion (non-streaming)
 *
 * Handles OpenAI-compatible API calls (works for both cloud OpenAI and local LLMs).
 * Supports vision API when vision_images is provided.
 * Conversation history is always in OpenAI format (role/content pairs).
 *
 * @param conversation_history JSON array of messages (OpenAI format)
 * @param input_text User input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes in bytes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param base_url Base URL (cloud: https://api.openai.com, local: http://127.0.0.1:8080)
 * @param api_key API key (NULL for local LLM, required for cloud)
 * @param model Model name (NULL = use config default)
 * @return Response text (caller must free), or NULL on error
 */
char *llm_openai_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 const char **vision_images,
                                 const size_t *vision_image_sizes,
                                 int vision_image_count,
                                 const char *base_url,
                                 const char *api_key,
                                 const char *model);

/**
 * @brief Single-shot OpenAI streaming call (no tool execution or recursion)
 *
 * Makes exactly one HTTP call and returns structured results. Does NOT execute
 * tools, append to history, or recurse. Used by the central tool iteration loop.
 *
 * @param conversation_history JSON array of messages (OpenAI format)
 * @param input_text User input text (empty string for follow-up calls)
 * @param vision_images Array of base64 images (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param base_url API base URL
 * @param api_key API key (NULL for local LLM)
 * @param model Model name (NULL = use config default)
 * @param chunk_callback Streaming text callback
 * @param callback_userdata User context for callback
 * @param iteration Current iteration (controls whether tools are included)
 * @param result Output: structured response (see llm_tools.h llm_tool_response_t)
 * @return 0 on success, non-zero on error (result not populated)
 */
int llm_openai_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char **vision_images,
                                     const size_t *vision_image_sizes,
                                     int vision_image_count,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_openai_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     struct llm_tool_response *result);

#endif  // LLM_OPENAI_H
