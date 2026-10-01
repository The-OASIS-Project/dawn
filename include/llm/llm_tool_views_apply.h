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
 * The tool loop's view stage, run on a batch (llm_tool_views.h).
 */

#ifndef LLM_TOOL_VIEWS_APPLY_H
#define LLM_TOOL_VIEWS_APPLY_H

#include <stddef.h>

#include "llm/llm_tool_loop.h"
#include "llm/llm_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

struct session;

/** A batch's budget, planned before it runs. */
typedef struct {
   size_t chars;    /**< the batch's budget in characters */
   int window;      /**< the model's window in tokens (0: unknown) */
   int room_tokens; /**< what the hard threshold leaves (when the window is known) */
} llm_tool_views_budget_t;

/**
 * @brief The budget of the batch @p calls (the assistant's message
 *        @p assistant_text with them): from the model's window, the request
 *        as it stands, and a reply reserve (llm_tool_views_budget_chars)
 */
void llm_tool_views_budget_batch(const llm_tool_loop_params_t *params,
                                 const tool_call_list_t *calls,
                                 const char *assistant_text,
                                 llm_tool_views_budget_t *out);

/** What the stage needs to shape a batch (llm_tool_views_finish_batch). */
typedef struct {
   const llm_tool_loop_params_t *params;
   struct session *session; /**< the turn's session (NULL: nothing is stored) */
   const llm_tool_views_budget_t *budget;
} llm_tool_views_batch_t;

/**
 * @brief The batch's finish step (llm_tools_batch_finish_fn; @p batch is an
 *        llm_tool_views_batch_t): each result over its share is kept whole in
 *        the store (when the session's user may store, and the request
 *        offers result_read) and replaced by its view under a header; then
 *        every result is finished (llm_tools_finish_result)
 *
 * Runs on the turn's thread, without history_mutex held.
 */
void llm_tool_views_finish_batch(const tool_call_list_t *calls,
                                 tool_result_list_t *results,
                                 void *batch);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_VIEWS_APPLY_H */
