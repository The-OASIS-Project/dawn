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
 * A tool exchange as the tool loop appends it to a conversation's history:
 * the assistant's call message in the history's format (OpenAI or Claude),
 * with the turn's blocks, then the tool results.
 */

#include "llm/llm_tool_history.h"

#include <json-c/json.h>

#include "core/session_manager.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"

_Static_assert(LLM_TURN_CALLS_MAX >= LLM_TOOLS_MAX_PARALLEL_CALLS,
               "a turn's blocks must be able to hold every call that runs");

/* The response's blocks with their tool calls replaced by the calls that ran
 * (llm_turn_blocks_with_calls): the calls the stream carried and the calls
 * admitted and answered can differ (a cap, an over-long id, a stream cut
 * short), and a replay must hold each call with its result.  NULL without
 * blocks. */
json_object *llm_tool_history_blocks_as_run(const llm_tool_response_t *response, bool calls_ran) {
   if (!response || !response->blocks) {
      return NULL;
   }
   llm_turn_call_t calls[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int n = 0;
   for (int i = 0; calls_ran && i < response->tool_calls.count && i < LLM_TOOLS_MAX_PARALLEL_CALLS;
        i++) {
      calls[n].id = response->tool_calls.calls[i].id;
      calls[n].name = response->tool_calls.calls[i].name;
      calls[n].arguments = response->tool_calls.calls[i].arguments;
      n++;
   }
   return llm_turn_blocks_with_calls(response->blocks, calls, n);
}

/**
 * @brief Add assistant message with tool calls in OpenAI format
 *
 * Appends the assistant message containing tool_calls array, then adds
 * tool result messages.  The turn's blocks (reasoning, a call's signature)
 * go with it; each request renders what belongs to its endpoint.
 */
void llm_tool_history_append_openai(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));
   /* Preserve any text the LLM streamed before its tool calls, so the follow-up
    * iteration can see what was already said and won't repeat itself.
    * Use empty string as fallback for Gemini API compatibility (rejects NULL). */
   const char *pre_tool_text = (response->text && response->text[0] != '\0') ? response->text : "";
   json_object_object_add(assistant_msg, "content", json_object_new_string(pre_tool_text));

   json_object *tc_array = json_object_new_array();
   for (int i = 0; i < response->tool_calls.count; i++) {
      json_object *tc = json_object_new_object();
      json_object_object_add(tc, "id", json_object_new_string(response->tool_calls.calls[i].id));
      json_object_object_add(tc, "type", json_object_new_string("function"));

      json_object *func = json_object_new_object();
      json_object_object_add(func, "name",
                             json_object_new_string(response->tool_calls.calls[i].name));
      json_object_object_add(func, "arguments",
                             json_object_new_string(response->tool_calls.calls[i].arguments));
      json_object_object_add(tc, "function", func);

      json_object_array_add(tc_array, tc);
   }
   json_object_object_add(assistant_msg, "tool_calls", tc_array);

   /* The turn's blocks, where the path captured them (OpenAI Responses): its
    * reasoning items go back with the next request, its calls as they ran. */
   json_object *blocks = llm_tool_history_blocks_as_run(response, true);
   if (blocks) {
      json_object_object_add(assistant_msg, LLM_TURN_BLOCKS_KEY, blocks);
   }

   session_history_append(history, assistant_msg);

   /* Add tool results */
   llm_tools_add_results_openai(history, results);
}

/**
 * @brief Add assistant message with tool calls in Claude format
 *
 * Appends the assistant message containing thinking blocks (if any) and
 * tool_use content blocks, then adds tool result messages.
 */
void llm_tool_history_append_claude(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results,
                                    const char *carrier) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

   /* The turn exactly as the model produced it: every block in order, each
    * thinking block with its own signature (empty text or not).  The blocks
    * travel with the message; each formatter renders them for its provider. */
   json_object *blocks = llm_tool_history_blocks_as_run(response, true);
   json_object *rendered = llm_turn_blocks_render_claude(blocks, carrier);
   if (rendered) {
      json_object_object_add(assistant_msg, "content", rendered);
      json_object_object_add(assistant_msg, LLM_TURN_BLOCKS_KEY, blocks);
      session_history_append(history, assistant_msg);
      llm_tools_add_results_claude(history, results);
      return;
   }
   json_object_put(blocks);

   /* A path without blocks: the text and tool calls (its reasoning, if any,
    * can't be replayed without the blocks it came in). */
   json_object *content_array = json_object_new_array();

   /* Preserve any text the LLM streamed before its tool_use blocks, so the follow-up
    * iteration can see what was already said and won't repeat itself. */
   if (response->text && response->text[0] != '\0') {
      json_object *text_block = json_object_new_object();
      json_object_object_add(text_block, "type", json_object_new_string("text"));
      json_object_object_add(text_block, "text", json_object_new_string(response->text));
      json_object_array_add(content_array, text_block);
   }

   /* Add tool_use blocks */
   for (int i = 0; i < response->tool_calls.count; i++) {
      json_object *tool_use = json_object_new_object();
      json_object_object_add(tool_use, "type", json_object_new_string("tool_use"));
      json_object_object_add(tool_use, "id",
                             json_object_new_string(response->tool_calls.calls[i].id));
      json_object_object_add(tool_use, "name",
                             json_object_new_string(response->tool_calls.calls[i].name));

      json_object *args = json_tokener_parse(response->tool_calls.calls[i].arguments);
      if (args) {
         json_object_object_add(tool_use, "input", args);
      } else {
         json_object_object_add(tool_use, "input", json_object_new_object());
      }

      json_object_array_add(content_array, tool_use);
   }

   json_object_object_add(assistant_msg, "content", content_array);
   session_history_append(history, assistant_msg);

   /* Add tool results in Claude format */
   llm_tools_add_results_claude(history, results);
}
