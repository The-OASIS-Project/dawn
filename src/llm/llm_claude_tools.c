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
 * A Claude request's tools (llm_claude_tools.h).  Split from
 * llm_claude_format.c.
 */

#include "llm/llm_claude_tools.h"

#include <stdatomic.h>
#include <string.h>

#include "llm/llm_history_kind.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_tools.h"
#include "logging.h"

/* A text block holding @p text. */
static json_object *text_of(const char *text) {
   json_object *block = json_object_new_object();
   if (block) {
      json_object_object_add(block, "type", json_object_new_string("text"));
      json_object_object_add(block, "text", json_object_new_string(text ? text : ""));
   }
   return block;
}

/**
 * A tool change sent in place (@p blocks, taken): a system message after the
 * user message before it, or added to the system message already there (a
 * run of them is one message).  llm_tool_change_renders_inline placed it
 * after a user turn; anywhere else it can't go, and is logged.
 */
static void add_system_blocks(json_object *messages,
                              json_object *blocks,
                              json_object **last_message,
                              const char **last_role) {
   if (*last_role && strcmp(*last_role, "system") == 0 && *last_message) {
      json_object *content = NULL;
      json_object_object_get_ex(*last_message, "content", &content);
      json_object *merged = json_object_new_array();
      if (!merged) {
         json_object_put(blocks);
         return;
      }
      if (json_object_is_type(content, json_type_array)) {
         const size_t n = json_object_array_length(content);
         for (size_t i = 0; i < n; i++) {
            json_object_array_add(merged, json_object_get(json_object_array_get_idx(content, i)));
         }
      } else {
         json_object *block = text_of(json_object_get_string(content));
         if (block) {
            json_object_array_add(merged, block);
         }
      }
      const size_t nb = json_object_array_length(blocks);
      for (size_t i = 0; i < nb; i++) {
         json_object_array_add(merged, json_object_get(json_object_array_get_idx(blocks, i)));
      }
      json_object_put(blocks);
      json_object_object_add(*last_message, "content", merged);
      return;
   }
   if (!(*last_role && strcmp(*last_role, "user") == 0 && *last_message)) {
      OLOG_WARNING("Claude: a tool change with no user turn before it was left out");
      json_object_put(blocks);
      return;
   }
   json_object *sys = json_object_new_object();
   if (!sys) {
      json_object_put(blocks);
      return;
   }
   json_object_object_add(sys, "role", json_object_new_string("system"));
   json_object_object_add(sys, "content", blocks);
   json_object_array_add(messages, sys);
   *last_message = sys;
   *last_role = "system";
}

/* A stored inline change this request folds though its conversation would
 * send it in place (another endpoint, or rejected on this turn: the seam marks
 * the conversation, and declares the boundary, at its next turn).  Logged
 * once. */
static void note_request_fold(json_object *history, bool inline_ok) {
   static atomic_bool logged;
   if (inline_ok || atomic_load(&logged) || llm_tool_defs_inline_rejected(history)) {
      return;
   }
   const size_t n = json_object_is_type(history, json_type_array)
                        ? json_object_array_length(history)
                        : 0;
   for (size_t i = 1; i < n; i++) {
      if (llm_tool_change_stored_inline(json_object_array_get_idx(history, i)) &&
          !atomic_exchange(&logged, true)) {
         OLOG_INFO("Claude: a conversation's in-place tool change folds into this request's "
                   "tools (its endpoint, or a rejection on this turn); its next turn records "
                   "it");
         return;
      }
   }
}

bool llm_claude_tools_add(json_object *request,
                          json_object *history,
                          bool inline_ok,
                          bool is_remote,
                          int iteration) {
   /* The conversation's own tool set when it has one: the same on every
    * request (a tool this surface may not use is refused when called). */
   const char *source = NULL;
   json_object *tools = llm_tools_request_tools(history, is_remote, true, inline_ok, &source);
   if (!tools) {
      return false;
   }
   note_request_fold(history, inline_ok);
   json_object_object_add(request, "tools", tools);
   OLOG_INFO("Claude: Added %zu tools to request (%s)", json_object_array_length(tools), source);
   /* The loop's last call, for a text answer: the tools stay (the request
    * reads as every other did), none may be called. */
   if (iteration >= LLM_TOOLS_MAX_ITERATIONS) {
      json_object *choice = json_object_new_object();
      json_object_object_add(choice, "type", json_object_new_string("none"));
      json_object_object_add(request, "tool_choice", choice);
   }

   /* A cache breakpoint on the last tool: the tools array is a cached prefix
    * of its own, apart from the system prompt's. */
   const size_t tool_count = json_object_array_length(tools);
   json_object *last_tool = tool_count > 0 ? json_object_array_get_idx(tools, tool_count - 1)
                                           : NULL;
   if (json_object_is_type(last_tool, json_type_object)) {
      json_object *cache_control = json_object_new_object();
      json_object_object_add(cache_control, "type", json_object_new_string("ephemeral"));
      json_object_object_add(last_tool, "cache_control", cache_control);
   }
   return source && strcmp(source, "the conversation's set") == 0;
}

json_object *llm_claude_tool_change_blocks(json_object *history, size_t idx, bool inline_ok) {
   if (!llm_tool_change_renders_inline(history, idx, inline_ok)) {
      return NULL;
   }
   json_object *defs = llm_tool_change_defs(json_object_array_get_idx(history, idx));
   const size_t n = defs ? json_object_array_length(defs) : 0;
   json_object *blocks = n ? json_object_new_array() : NULL;
   for (size_t i = 0; blocks && i < n; i++) {
      json_object *definition = llm_tool_def_render(json_object_array_get_idx(defs, i), true);
      json_object *tool = definition ? json_object_new_object() : NULL;
      json_object *block = tool ? json_object_new_object() : NULL;
      if (!block) {
         json_object_put(definition);
         json_object_put(tool);
         continue;
      }
      json_object_object_add(tool, "type", json_object_new_string("tool_definition"));
      json_object_object_add(tool, "definition", definition);
      json_object_object_add(block, "type", json_object_new_string("tool_addition"));
      json_object_object_add(block, "tool", tool);
      json_object_array_add(blocks, block);
   }
   json_object_put(defs);
   if (blocks && json_object_array_length(blocks) == 0) {
      json_object_put(blocks);
      return NULL;
   }
   return blocks;
}

void llm_claude_tool_change_add(json_object *messages,
                                json_object *history,
                                size_t idx,
                                bool inline_ok,
                                json_object **last_message,
                                const char **last_role) {
   json_object *blocks = llm_claude_tool_change_blocks(history, idx, inline_ok);
   if (blocks) {
      add_system_blocks(messages, blocks, last_message, last_role);
   }
}
