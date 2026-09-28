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
 * Internal helpers shared between the OpenAI provider files (llm_openai.c,
 * llm_openai_chat_completions.c, llm_openai_history.c, llm_openai_responses.c).
 * Not part of the public LLM API; do not include outside the OpenAI provider
 * module.
 */

#ifndef LLM_OPENAI_INTERNAL_H
#define LLM_OPENAI_INTERNAL_H

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "llm/llm_openai.h"

/* Forward declaration for single-shot result type (defined in llm_tools.h) */
typedef struct llm_tool_response llm_tool_response_t;

/**
 * @brief Build standard OpenAI HTTP headers (Content-Type + optional Bearer auth).
 *
 * @param api_key API key for cloud (NULL for local LLM — auth header omitted).
 * @param base_url Request base URL; when it points at OpenRouter, the optional
 *                 HTTP-Referer + X-Title attribution headers are added. May be NULL.
 * @return CURL header list (caller must free with curl_slist_free_all).
 */
struct curl_slist *llm_openai_build_headers(const char *api_key, const char *base_url);

/**
 * @brief Extract the human-readable error message from an OpenAI API error body.
 *
 * Parses the standard `{"error": {"message": "...", ...}}` envelope shared by
 * both /v1/chat/completions and /v1/responses. Returns a thread-local buffer;
 * do not free, do not retain across calls.
 *
 * @param response_body Raw HTTP response body (may be empty/NULL).
 * @param http_code HTTP status code (used in fallback message).
 * @return Pointer to thread-local error string.
 */
const char *llm_openai_parse_error_message(const char *response_body, long http_code);

/**
 * @brief Whether the model should be routed to /v1/responses instead of /v1/chat/completions.
 *
 * Returns true for gpt-5.4* (and any future model OpenAI gates the same way), where
 * combining reasoning_effort with function tools is rejected on chat completions.
 * The caller still chooses based on the configured mode (auto/always/never).
 */
bool llm_openai_model_prefers_responses_api(const char *model_name);

/* ── History conversion (llm_openai_history.c) ──────────────────────────── */

/**
 * @brief Prepare conversation history for a /v1/chat/completions request.
 *
 * Filters orphaned tool messages (from restored conversations), converts
 * Claude-format tool/image blocks to OpenAI format, and strips vision content
 * when the target LLM lacks vision support.
 *
 * An assistant turn with blocks (llm_turn_blocks.h) is rendered from them:
 * its text and tool calls, and the reasoning @p carrier and @p model issued
 * (OpenRouter reasoning_details, a Gemini call's thought signature).
 *
 * @param conversation_history Original conversation history (not modified).
 * @param carrier The request's carrier (llm_turn_blocks_carrier).
 * @param model   The request's model.
 * @return Converted history (new object, caller frees with json_object_put).
 */
json_object *llm_openai_prepare_chat_history(struct json_object *conversation_history,
                                             const char *carrier,
                                             const char *model);

/**
 * @brief Return a request-private messages array with vision images applied,
 *        WITHOUT mutating @p history.
 *
 * llm_openai_prepare_chat_history may return the caller's conversation history
 * by shared reference, so the in-place "add content to the last message" pattern
 * would corrupt shared session state.  This copies-on-write: the changed message
 * is private, every other message is shared read-only.  If the last message is a
 * user turn its content becomes [text(@p input_text), image_url...]; otherwise a
 * new user message is appended.
 *
 * @return New array (caller owns — json_object_put or hand to root "messages"),
 *         or NULL on bad input / OOM (caller keeps @p history; vision dropped,
 *         never corrupted).
 */
json_object *llm_openai_apply_vision_images(json_object *history,
                                            const char *input_text,
                                            const char **vision_images,
                                            const size_t *vision_image_sizes,
                                            int vision_image_count);

/**
 * @brief Reconstruct a Claude-shaped tool message into OpenAI-canonical entries.
 *
 * Appends to @p out_array: a Claude assistant {content:[text, tool_use...]} →
 * one OpenAI assistant with a tool_calls array; a Claude user {content:[tool_result...]}
 * → one OpenAI {role:"tool", tool_call_id, content} per result (fan-out). Returns
 * the number of messages appended, or 0 if @p msg has no tool blocks. Shared by the
 * request-build path and the tool-turn persist path (normalizing Claude history to
 * the OpenAI-canonical storage shape).
 */
int convert_claude_tool_to_openai(struct json_object *msg, struct json_object *out_array);

/* ── Chat-completions implementation (llm_openai_chat_completions.c) ────── */

/**
 * @brief Non-streaming /v1/chat/completions request.
 *
 * Pure implementation — the public llm_openai_chat_completion() entry point
 * in llm_openai.c guards against Responses-only models before calling this.
 */
char *llm_openai_cc_chat_completion(struct json_object *conversation_history,
                                    const char *input_text,
                                    const char **vision_images,
                                    const size_t *vision_image_sizes,
                                    int vision_image_count,
                                    const char *base_url,
                                    const char *api_key,
                                    const char *model);

/**
 * @brief Single-shot /v1/chat/completions streaming call (no tool loop).
 *
 * Makes exactly one HTTP call and returns structured results. Does NOT
 * execute tools, append to history, or recurse. The public
 * llm_openai_streaming_single_shot() dispatches here after deciding not
 * to route to the /v1/responses implementation.
 */
int llm_openai_cc_streaming_single_shot(struct json_object *conversation_history,
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
                                        llm_tool_response_t *result);

#endif /* LLM_OPENAI_INTERNAL_H */
