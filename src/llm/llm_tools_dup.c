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
 * Duplicate tool-call detection: whether the model is repeating a call it
 * already made in the current user turn.  Split from llm_tools.c.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>

#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Duplicate Tool Call Detection
 * ============================================================================= */

/* Maximum messages to check for duplicate tool calls (performance optimization) */
#define DUPLICATE_CHECK_LOOKBACK 10

/**
 * @brief Check OpenAI-format history for duplicate tool call
 */
static bool is_duplicate_in_openai_history(struct json_object *history,
                                           const char *tool_name,
                                           const char *tool_args,
                                           int min_idx) {
   int len = json_object_array_length(history);

   for (int i = len - 1; i >= min_idx; i--) {
      json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (!role || strcmp(role, "assistant") != 0)
         continue;

      json_object *tool_calls;
      if (!json_object_object_get_ex(msg, "tool_calls", &tool_calls))
         continue;
      if (!json_object_is_type(tool_calls, json_type_array))
         continue;

      int tc_len = json_object_array_length(tool_calls);
      for (int j = 0; j < tc_len; j++) {
         json_object *tc = json_object_array_get_idx(tool_calls, j);
         if (!tc)
            continue;

         json_object *func;
         if (!json_object_object_get_ex(tc, "function", &func))
            continue;

         json_object *name_obj;
         if (!json_object_object_get_ex(func, "name", &name_obj))
            continue;

         const char *prev_name = json_object_get_string(name_obj);
         if (!prev_name || strcmp(prev_name, tool_name) != 0)
            continue;

         json_object *args_obj;
         if (json_object_object_get_ex(func, "arguments", &args_obj)) {
            const char *prev_args = json_object_get_string(args_obj);
            bool args_match = false;
            if ((!prev_args || prev_args[0] == '\0') && (!tool_args || tool_args[0] == '\0')) {
               args_match = true;
            } else if (prev_args && tool_args && strcmp(prev_args, tool_args) == 0) {
               args_match = true;
            }

            if (args_match) {
               return true;
            }
         }
      }
   }
   return false;
}

/**
 * @brief Check Claude-format history for duplicate tool call
 */
static bool is_duplicate_in_claude_history(struct json_object *history,
                                           const char *tool_name,
                                           const char *tool_args,
                                           int min_idx) {
   /* Claude stores the input as an object, and json-c prints it in its own spacing, so
    * compare parsed JSON: no args is an empty object; args that aren't JSON match nothing */
   json_object *want = (tool_args && tool_args[0] != '\0') ? json_tokener_parse(tool_args)
                                                           : json_object_new_object();
   if (!want)
      return false;

   bool found = false;
   int len = json_object_array_length(history);

   for (int i = len - 1; i >= min_idx && !found; i--) {
      json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (!role || strcmp(role, "assistant") != 0)
         continue;

      json_object *content_obj;
      if (!json_object_object_get_ex(msg, "content", &content_obj))
         continue;
      if (!json_object_is_type(content_obj, json_type_array))
         continue;

      int arr_len = json_object_array_length(content_obj);
      for (int j = 0; j < arr_len; j++) {
         json_object *block = json_object_array_get_idx(content_obj, j);
         if (!block)
            continue;

         json_object *type_obj;
         if (!json_object_object_get_ex(block, "type", &type_obj))
            continue;

         const char *type_str = json_object_get_string(type_obj);
         if (!type_str || strcmp(type_str, "tool_use") != 0)
            continue;

         json_object *name_obj;
         if (!json_object_object_get_ex(block, "name", &name_obj))
            continue;

         const char *prev_name = json_object_get_string(name_obj);
         if (!prev_name || strcmp(prev_name, tool_name) != 0)
            continue;

         json_object *input_obj;
         if (json_object_object_get_ex(block, "input", &input_obj) &&
             json_object_equal(input_obj, want)) {
            found = true;
            break;
         }
      }
   }
   json_object_put(want);
   return found;
}

/* Index of the last real user message — the start of the current turn.  A repeat
 * of a tool call from an EARLIER turn is legitimate (the user asked again, or the
 * underlying data changed between turns); only a repeat within THIS turn is the
 * runaway loop the duplicate check guards against.  Claude tool-result messages
 * are role "user" too (content array with a tool_result block) — they are not a
 * turn boundary, so they're skipped.  Returns 0 when no real user message found. */
static int last_real_user_msg_index(struct json_object *history, llm_history_format_t format) {
   int len = json_object_array_length(history);
   for (int i = len - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!msg || !json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), "user") != 0)
         continue;
      if (format == LLM_HISTORY_CLAUDE) {
         struct json_object *content;
         if (json_object_object_get_ex(msg, "content", &content) &&
             json_object_is_type(content, json_type_array)) {
            bool is_tool_result = false;
            int n = json_object_array_length(content);
            for (int j = 0; j < n; j++) {
               struct json_object *blk = json_object_array_get_idx(content, j);
               struct json_object *type_obj;
               if (blk && json_object_object_get_ex(blk, "type", &type_obj) &&
                   strcmp(json_object_get_string(type_obj), "tool_result") == 0) {
                  is_tool_result = true;
                  break;
               }
            }
            if (is_tool_result)
               continue; /* Claude tool result, not a turn boundary */
         }
      }
      return i;
   }
   return 0;
}

bool llm_tools_is_duplicate_call(struct json_object *history,
                                 const char *tool_name,
                                 const char *tool_args,
                                 llm_history_format_t format) {
   if (!history || !tool_name)
      return false;

   /* Non-deterministic actions are exempt: an identical-args repeat is a feature
    * (e.g. calculator "random" — "pick another number"), not an infinite loop.
    * The registry owns both the args key that carries the action (usually
    * "action", but e.g. switch_llm uses "target") and which action values a tool
    * declares repeatable. */
   if (tool_args && tool_args[0] != '\0') {
      const char *action_key = tool_registry_get_action_param_name(tool_name);
      if (action_key) {
         struct json_object *parsed = json_tokener_parse(tool_args);
         if (parsed) {
            struct json_object *action_obj;
            if (json_object_object_get_ex(parsed, action_key, &action_obj)) {
               const char *action = json_object_get_string(action_obj);
               if (action && tool_registry_action_is_repeatable(tool_name, action)) {
                  json_object_put(parsed);
                  return false;
               }
            }
            json_object_put(parsed);
         }
      }
   }

   int len = json_object_array_length(history);
   int min_idx = len - DUPLICATE_CHECK_LOOKBACK;
   if (min_idx < 0) {
      min_idx = 0;
   }
   /* Confine the scan to the current turn so a user-requested repeat (or a
    * re-read of data that changed since the last turn) isn't blocked as a dup —
    * only same-turn loops are caught. */
   int turn_start = last_real_user_msg_index(history, format);
   if (turn_start > min_idx) {
      min_idx = turn_start;
   }

   bool is_dup;
   if (format == LLM_HISTORY_CLAUDE) {
      is_dup = is_duplicate_in_claude_history(history, tool_name, tool_args, min_idx);
   } else {
      is_dup = is_duplicate_in_openai_history(history, tool_name, tool_args, min_idx);
   }

   if (is_dup) {
      OLOG_INFO("Duplicate tool call detected: %s with args %s", tool_name,
                tool_args ? tool_args : "(none)");
   }
   return is_dup;
}
