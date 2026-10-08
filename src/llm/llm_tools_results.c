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
 * Tool results added to a conversation's history (each provider's shape,
 * an image a tool returned inside its result: llm_tool_images.h) and tool
 * calls parsed from a provider's response.  Split from llm_tools.c.
 */

#include <json-c/json.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_tools.h"
#include "llm/llm_tools_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Tool Result Formatting for Conversation History
 * ============================================================================= */

/* A result's content: its text, and the image it holds (llm_tool_images.h).
 * The text alone when building the content fails. */
static struct json_object *result_content(const tool_result_t *r) {
   struct json_object *content = llm_tool_images_result_content(r);
   if (!content) {
      OLOG_ERROR("Tool result %s: its image left out (out of memory)", r->tool_call_id);
      content = json_object_new_string(tool_result_content(r));
   }
   return content;
}

int llm_tools_add_results_openai(struct json_object *history, const tool_result_list_t *results) {
   if (!history || !results) {
      return 1;
   }

   /*
    * OpenAI format: Add a "tool" role message for each result
    * {
    *   "role": "tool",
    *   "tool_call_id": "call_xxx",
    *   "content": "result text"
    * }
    */
   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];

      struct json_object *msg = json_object_new_object();
      json_object_object_add(msg, "role", json_object_new_string("tool"));
      json_object_object_add(msg, "tool_call_id", json_object_new_string(r->tool_call_id));
      json_object_object_add(msg, "content", result_content(r));

      session_history_append(history, msg);
   }

   return 0;
}

int llm_tools_add_results_claude(struct json_object *history, const tool_result_list_t *results) {
   if (!history || !results) {
      return 1;
   }

   /*
    * Claude format: Add a single "user" message with tool_result content blocks
    * {
    *   "role": "user",
    *   "content": [
    *     {
    *       "type": "tool_result",
    *       "tool_use_id": "toolu_xxx",
    *       "content": "result text"
    *     }
    *   ]
    * }
    */
   struct json_object *content_array = json_object_new_array();

   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];

      struct json_object *block = json_object_new_object();
      json_object_object_add(block, "type", json_object_new_string("tool_result"));
      json_object_object_add(block, "tool_use_id", json_object_new_string(r->tool_call_id));
      json_object_object_add(block, "content", result_content(r));

      json_object_array_add(content_array, block);
   }

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);

   session_history_append(history, msg);

   return 0;
}

/* =============================================================================
 * Response Parsing
 * ============================================================================= */

int llm_tools_parse_openai_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return FAILURE;
   }

   out->count = 0;

   /*
    * OpenAI response structure:
    * {
    *   "choices": [{
    *     "message": {
    *       "tool_calls": [{
    *         "id": "call_xxx",
    *         "function": {
    *           "name": "weather",
    *           "arguments": "{...}"
    *         }
    *       }]
    *     },
    *     "finish_reason": "tool_calls"
    *   }]
    * }
    */
   struct json_object *choices;
   if (!json_object_object_get_ex(response, "choices", &choices)) {
      return 1; /* No tool calls */
   }

   if (json_object_array_length(choices) == 0) {
      return 1;
   }

   struct json_object *first_choice = json_object_array_get_idx(choices, 0);
   struct json_object *message;
   if (!json_object_object_get_ex(first_choice, "message", &message)) {
      return 1;
   }

   struct json_object *tool_calls;
   if (!json_object_object_get_ex(message, "tool_calls", &tool_calls)) {
      return 1; /* No tool calls */
   }

   int len = json_object_array_length(tool_calls);
   for (int i = 0; i < len && out->count < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      struct json_object *tc = json_object_array_get_idx(tool_calls, i);
      struct json_object *id_obj, *function_obj;

      if (!json_object_object_get_ex(tc, "id", &id_obj) ||
          !json_object_object_get_ex(tc, "function", &function_obj)) {
         continue;
      }

      struct json_object *name_obj, *args_obj;
      if (!json_object_object_get_ex(function_obj, "name", &name_obj) ||
          !json_object_object_get_ex(function_obj, "arguments", &args_obj)) {
         continue;
      }

      tool_call_t *call = &out->calls[out->count++];
      safe_strncpy(call->id, json_object_get_string(id_obj), LLM_TOOLS_ID_LEN);
      safe_strncpy(call->name, json_object_get_string(name_obj), LLM_TOOLS_NAME_LEN);

      const char *args_str = json_object_get_string(args_obj);
      size_t args_len = 0;
      llm_tools_args_append(call->arguments, &args_len, &call->args_truncated,
                            args_str ? args_str : "", args_str ? strlen(args_str) : 0, true);
      if (call->args_truncated) {
         OLOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                      /* a NULL string is passed with n = 0 and never read */
                      // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
                      strlen(args_str), LLM_TOOLS_ARGS_LEN - 1);
      }
   }

   return out->count > 0 ? 0 : 1;
}

void llm_tools_args_append(char *buf,
                           size_t *len,
                           bool *overflow,
                           const char *text,
                           size_t n,
                           bool replace) {
   if (!buf || !len || !overflow || !text) {
      return;
   }
   if (replace) {
      *len = 0;
      *overflow = false;
   }
   if (*len >= LLM_TOOLS_ARGS_LEN) {
      *overflow = true;
      return;
   }
   size_t room = LLM_TOOLS_ARGS_LEN - 1 - *len;
   if (n > room) {
      *overflow = true;
      n = room;
      /* Never end inside a UTF-8 character: a refused call is still replayed in
       * history, and a strict provider rejects invalid UTF-8. */
      while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80)
         n--;
   }
   memcpy(buf + *len, text, n);
   *len += n;
   buf[*len] = '\0';
}

int llm_tools_parse_claude_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return FAILURE;
   }

   out->count = 0;

   /*
    * Claude response structure:
    * {
    *   "content": [
    *     {
    *       "type": "tool_use",
    *       "id": "toolu_xxx",
    *       "name": "weather",
    *       "input": { ... }
    *     }
    *   ],
    *   "stop_reason": "tool_use"
    * }
    */
   struct json_object *content;
   if (!json_object_object_get_ex(response, "content", &content)) {
      return 1;
   }

   int len = json_object_array_length(content);
   for (int i = 0; i < len && out->count < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      struct json_object *block = json_object_array_get_idx(content, i);
      struct json_object *type_obj;

      if (!json_object_object_get_ex(block, "type", &type_obj)) {
         continue;
      }

      if (strcmp(json_object_get_string(type_obj), "tool_use") != 0) {
         continue;
      }

      struct json_object *id_obj, *name_obj, *input_obj;
      if (!json_object_object_get_ex(block, "id", &id_obj) ||
          !json_object_object_get_ex(block, "name", &name_obj) ||
          !json_object_object_get_ex(block, "input", &input_obj)) {
         continue;
      }

      tool_call_t *call = &out->calls[out->count++];
      safe_strncpy(call->id, json_object_get_string(id_obj), LLM_TOOLS_ID_LEN);
      safe_strncpy(call->name, json_object_get_string(name_obj), LLM_TOOLS_NAME_LEN);

      /* Claude sends input as object, we need it as string */
      const char *input_str = json_object_to_json_string(input_obj);
      size_t input_len = 0;
      llm_tools_args_append(call->arguments, &input_len, &call->args_truncated,
                            input_str ? input_str : "", input_str ? strlen(input_str) : 0, true);
      if (call->args_truncated) {
         OLOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                      /* a NULL string is passed with n = 0 and never read */
                      // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
                      strlen(input_str), LLM_TOOLS_ARGS_LEN - 1);
      }
   }

   return out->count > 0 ? 0 : 1;
}
