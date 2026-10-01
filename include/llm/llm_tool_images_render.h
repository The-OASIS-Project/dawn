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
 * A tool's images in a request, per provider.  Pure JSON work, no state.
 *
 * In a history, a tool result that carried images holds them in its content:
 * [text part, image parts], an image part an OpenAI "image_url" part (marked
 * with its stored id, image_rehydrate.h IMAGE_PART_ID_KEY), on every history
 * format: a role "tool" message's content, or a Claude tool_result block's.
 * Each provider shows them its own way, rendered from that one shape, so a
 * live request and a reloaded one read the same:
 *   Claude          image blocks inside the tool_result (llm_claude_format.c)
 *   Responses       function_call_output whose output is input_text and
 *                   input_image items (llm_tool_images_responses_output)
 *   chat completions (OpenAI, Gemini, OpenRouter, local): the tool messages
 *                   carry text, and one user message after all of a turn's
 *                   tool messages carries their images
 *                   (llm_tool_images_render_chat)
 * A model that takes no images gets LLM_TOOL_IMAGES_NO_VISION_TEXT in place
 * of each one (llm_tool_images_without), the same every time.
 */

#ifndef LLM_TOOL_IMAGES_RENDER_H
#define LLM_TOOL_IMAGES_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llm/llm_capabilities.h" /* llm_image_limit_t */

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** What a model that takes no images reads in place of a tool's image. */
#define LLM_TOOL_IMAGES_NO_VISION_TEXT "[image not shown: this model doesn't take images]"

/** What labels a tool call's images in a chat-completions image message. */
#define LLM_TOOL_IMAGES_CHAT_LABEL_FMT "Images returned by tool call %s:"

/** @brief Whether @p part is an image ("image_url" or a Claude "image"). */
bool llm_part_is_image(struct json_object *part);

/** @brief Whether content parts @p parts hold an image, a tool_result's
 *         nested ones included. */
bool llm_parts_have_image(struct json_object *parts);

/**
 * @brief The images in messages [@p start, @p end) of @p history and their
 *        bytes as sent (a data URI's or a base64 source's length), counting
 *        those inside tool results (a Claude tool_result's content) too
 *
 * @p end past the history's length means its end.  Either output may be NULL.
 */
void llm_history_image_totals(struct json_object *history,
                              int start,
                              int end,
                              int *count_out,
                              int64_t *bytes_out);

/**
 * @brief @p history with every image in a tool result replaced by a text part
 *        LLM_TOOL_IMAGES_NO_VISION_TEXT (for a model that takes no images)
 *
 * Messages it doesn't change are shared (a reference); the history is never
 * changed.
 *
 * @return New reference (caller owns), or NULL on out of memory
 */
struct json_object *llm_tool_images_without(struct json_object *history);

/**
 * @brief A chat-completions history (OpenAI shape, no DAWN keys) with each
 *        tool message's content as its text, and after each run of tool
 *        messages one user message carrying the images they held, in order,
 *        each call's labelled (LLM_TOOL_IMAGES_CHAT_LABEL_FMT)
 *
 * A run with no images adds nothing.  Messages it doesn't change are shared;
 * the history is never changed.
 *
 * @return New reference (caller owns), or NULL on out of memory
 */
struct json_object *llm_tool_images_render_chat(struct json_object *history);

/**
 * @brief A tool result's content as a Responses function_call_output's
 *        "output": a string (its text) when it holds no image, else an array
 *        of input_text and input_image items in its order
 *
 * @return New object (caller owns), or NULL on out of memory
 */
struct json_object *llm_tool_images_responses_output(struct json_object *content);

/**
 * @brief Whether @p history's images reach @p fraction of @p limit (with room
 *        for one more): the seam compacts it then
 */
bool llm_tool_images_history_over(struct json_object *history,
                                  const llm_image_limit_t *limit,
                                  float fraction);

/**
 * @brief Whether the images in messages [@p start, @p end) of @p history (end
 *        past its length: its end) reach @p fraction of @p limit, with room for
 *        one more (llm_tool_images_history_over over a range)
 */
bool llm_tool_images_range_over(struct json_object *history,
                                int start,
                                int end,
                                const llm_image_limit_t *limit,
                                float fraction);

/**
 * @brief Strip image content blocks from conversation history
 *
 * Replaces `image_url` (OpenAI shape) / `image` (Claude shape) content parts
 * with a short text placeholder, preserving any sibling text in the same
 * message. Two callers: llm_openai_prepare_chat_history() strips vision
 * content when the active model doesn't support it; llm_context.c's
 * LLM-summarization compaction path strips it so a persisted tool-captured
 * image (see llm_tools_add_results_openai/claude) doesn't get JSON-serialized
 * whole into the summarizer prompt as literal base64 text.
 *
 * If history has no vision content, returns a new reference to the same
 * array (json_object_get) rather than copying — cheap no-op path.
 *
 * @param history JSON array of messages.
 * @return New array (caller json_object_put), or NULL on error.
 */
struct json_object *llm_history_strip_vision_content(struct json_object *history);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_IMAGES_RENDER_H */
