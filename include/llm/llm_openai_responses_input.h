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
 * Pure request-input shaping for the OpenAI Responses API: converts DAWN's
 * chat-completions-shaped history into a Responses `input` array, and splits the
 * two-segment system prompt into a byte-stable `instructions` string (stable) plus a
 * repositioned volatile block. Dependency-free (json-c + libc only) so the
 * prompt-cache layout can be unit-tested in isolation.
 *
 * See docs/RESPONSES_CACHE_REORDER_PLAN.md for the caching rationale and the
 * cross-module invariant on the leading system run.
 */

#ifndef LLM_OPENAI_RESPONSES_INPUT_H
#define LLM_OPENAI_RESPONSES_INPUT_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append vision images to a Responses content_part array (data:image/jpeg;base64). */
void llm_responses_append_vision_parts(struct json_object *content_array,
                                       const char **vision_images,
                                       const size_t *vision_image_sizes,
                                       int vision_image_count);

/* Count the leading contiguous run of role:"system" messages from index 0 (the
 * stable+volatile pair; anything after the run is a mid-history broadcast). */
int llm_responses_count_leading_system_run(struct json_object *history);

/* STABLE instructions = the FIRST system message (leading run [0]). Heap string
 * (caller frees), or NULL when there is no leading system message. */
char *llm_responses_extract_stable_instructions(struct json_object *history);

/* VOLATILE block = the leading run's system messages after index 0 ("\n\n"-joined).
 * NULL when the run is <= 1. Keyed on the leading run so a mid-history broadcast is
 * never swept in. Heap string (caller frees). */
char *llm_responses_extract_volatile_context(struct json_object *history);

/* Build a Responses-format `input` array from chat-completions history.
 * `volatile_block` (may be NULL) is repositioned as a user item immediately before
 * the current question; `leading_system_run` marks which leading system messages to
 * skip (later system messages are emitted inline). When `enable_cache_breakpoint` is
 * true, an explicit GPT-5.6+ `prompt_cache_breakpoint` is stamped on the last stable
 * input_text block before the volatile (caller must pass false for pre-5.6 models, which
 * reject the field, and must pair a true value with root `prompt_cache_options`). New
 * array (caller json_object_put), or NULL on error. */
struct json_object *llm_responses_build_input(struct json_object *history,
                                              const char *input_text,
                                              const char **vision_images,
                                              const size_t *vision_image_sizes,
                                              int vision_image_count,
                                              const char *volatile_block,
                                              int leading_system_run,
                                              bool enable_cache_breakpoint);

#ifdef __cplusplus
}
#endif

#endif /* LLM_OPENAI_RESPONSES_INPUT_H */
