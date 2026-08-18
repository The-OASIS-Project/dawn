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
 * Central tool iteration loop for LLM streaming with tool calling.
 *
 * This module extracts the tool call → execute → re-call loop from the
 * individual providers (OpenAI, Claude) into a single central loop that:
 * - Runs auto-compaction between iterations (THE KEY FIX)
 * - Provides duplicate tool call detection for all providers
 * - Handles provider switching mid-loop (switch_llm tool)
 * - Enforces uniform iteration limits
 */

#ifndef LLM_TOOL_LOOP_H
#define LLM_TOOL_LOOP_H

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llm/llm_interface.h"
#include "llm/llm_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Provider single-shot function pointer type
 *
 * Called by the iteration loop to make one HTTP call and return structured results.
 * Must NOT execute tools, recurse, or modify conversation history.
 *
 * @param conversation_history JSON array of messages
 * @param input_text User input (empty string for follow-up calls)
 * @param vision_images Vision images array (NULL if none)
 * @param vision_image_sizes Vision image sizes (NULL if none)
 * @param vision_image_count Number of vision images
 * @param base_url Provider endpoint URL
 * @param api_key API key (NULL for local)
 * @param model Model name
 * @param chunk_callback Text chunk callback for streaming
 * @param callback_userdata User context for callback
 * @param iteration Current iteration number
 * @param result Output: structured response
 * @return 0 on success, non-zero on error
 */
typedef int (*llm_single_shot_fn)(struct json_object *conversation_history,
                                  const char *input_text,
                                  const char **vision_images,
                                  const size_t *vision_image_sizes,
                                  int vision_image_count,
                                  const char *base_url,
                                  const char *api_key,
                                  const char *model,
                                  void *chunk_callback,
                                  void *callback_userdata,
                                  int iteration,
                                  llm_tool_response_t *result);

/**
 * @brief Parameters for the tool iteration loop
 *
 * Bundles all parameters needed by llm_tool_iteration_loop() to avoid
 * excessively long function signatures.
 */
typedef struct {
   struct json_object *conversation_history; /**< Conversation history (modified in place) */
   const char *input_text;                   /**< User input text */
   const char **vision_images;               /**< Vision images (NULL if none) */
   const size_t *vision_image_sizes;         /**< Vision image sizes */
   int vision_image_count;                   /**< Number of vision images */
   const char *base_url;                     /**< Provider endpoint URL */
   const char *api_key;                      /**< API key (NULL for local) */
   const char *model;                        /**< Model name (ptr into model_storage or external) */
   char model_storage[LLM_MODEL_NAME_MAX];   /**< Owned storage for model name across iterations */
   void *chunk_callback;                     /**< Text chunk callback (cast to provider type) */
   void *callback_userdata;                  /**< User context for callback */
   llm_single_shot_fn provider_fn;           /**< Current provider single-shot function */
   llm_history_format_t history_format;      /**< Current history format (OpenAI or Claude) */
   uint32_t session_id;                      /**< Session ID for compaction */
   llm_type_t llm_type;                      /**< Current LLM type */
   cloud_provider_t cloud_provider;          /**< Current cloud provider */
   _Atomic bool *cancel_flag;                /**< Per-session cancel_requested (NULL if none);
                                              *   honored at every iteration boundary + wait.
                                              *   Type must match session_t::cancel_requested
                                              *   (atomic_bool) — a change there surfaces here. */
   bool is_background;                       /**< True for job/research turns: honor cancel_flag
                                              *   only, NOT the global wake-word/Ctrl+C interrupt.
                                              *   Zero-value default (foreground) preserves the
                                              *   legacy global-only behavior for every caller. */
   int64_t cumulative_input_token_ceiling;   /**< Cumulative-session input-token ceiling (0 =
                                              *   unlimited): stop the turn once the SESSION's total
                                              *   input tokens (not this turn's) reach this.  Set only
                                              *   by the research controller (per-round overshoot
                                              *   guard); 0 for every other caller. */
} llm_tool_loop_params_t;

/**
 * @brief Central tool iteration loop
 *
 * Replaces the recursive tool execution in individual providers with a central
 * iterative loop that handles:
 * 1. Auto-compaction between iterations (prevents context overflow)
 * 2. Duplicate tool call detection (prevents infinite loops)
 * 3. Tool execution and history updates
 * 4. Provider switching (switch_llm tool)
 * 5. Vision data forwarding from tool results
 * 6. Iteration limit enforcement
 *
 * @param params Loop parameters (provider fn, history, credentials, etc.)
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_tool_iteration_loop(llm_tool_loop_params_t *params);

/**
 * @brief Check if the last tool loop iteration skipped follow-up
 *
 * Thread-local flag set when a tool with skip_followup=true executes.
 * Used by the caller to distinguish "no response because tool handled
 * it silently" from "no response because of an API error."
 *
 * @return true if the last tool loop skipped follow-up
 */
bool llm_tool_loop_did_skip_followup(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_LOOP_H */
