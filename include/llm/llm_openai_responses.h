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
 * OpenAI /v1/responses API single-shot streaming entry point. Mirrors the
 * llm_openai_streaming_single_shot() contract so the OpenAI provider can
 * dispatch internally based on model and config. Used for gpt-5.4* (where
 * reasoning_effort + tools is rejected on /v1/chat/completions) and any model
 * the operator has explicitly set to "always" use the Responses API.
 */

#ifndef LLM_OPENAI_RESPONSES_H
#define LLM_OPENAI_RESPONSES_H

#include <json-c/json.h>
#include <stddef.h>

#include "llm/llm_openai.h"

/* Forward declarations */
struct llm_tool_response;

/**
 * @brief Single-shot OpenAI Responses API call (no tool execution or recursion).
 *
 * Same contract as llm_openai_streaming_single_shot() but uses POST /v1/responses.
 * Caller decides routing via llm_openai_model_prefers_responses_api() + config knob.
 *
 * Stateless mode (Mode B) — echoes prior reasoning items from the assistant
 * turns' blocks in conversation_history (llm_turn_blocks.h). Server state is not
 * relied upon.  The result's blocks are the turn's output items, in order.
 *
 * @param conversation_history JSON array of messages (DAWN's OpenAI-shaped internal format).
 * @param input_text User input (empty string for follow-up calls inside a tool loop).
 * @param vision_images Array of base64 images (NULL if not used).
 * @param vision_image_sizes Array of image sizes (NULL if not used).
 * @param vision_image_count Number of images.
 * @param base_url API base URL (e.g. "https://api.openai.com").
 * @param api_key Cloud API key.
 * @param model Model name (must be a Responses-supported model).
 * @param chunk_callback Streaming text callback (TTS sink).
 * @param callback_userdata User context for callback.
 * @param iteration Current iteration (controls whether tools are included).
 * @param result Output: structured response (text, tool_calls, thinking, response_id,
 * blocks).
 * @return 0 on success, non-zero on error.
 */
int llm_openai_responses_streaming_single_shot(struct json_object *conversation_history,
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

#endif /* LLM_OPENAI_RESPONSES_H */
