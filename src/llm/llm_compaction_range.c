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
 * The compaction boundary: which part of a history a compaction summarizes,
 * which database rows that is, and how a reload past it starts cleanly.  See
 * llm_compaction_range.h.
 */

#include "llm/llm_compaction_range.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>

#include "llm/llm_history_kind.h"
#include "logging.h"

static const char *role_of(struct json_object *msg) {
   struct json_object *role = NULL;
   return (msg && json_object_object_get_ex(msg, "role", &role)) ? json_object_get_string(role)
                                                                 : NULL;
}

static bool is(const char *a, const char *b) {
   return a && b && strcmp(a, b) == 0;
}

/* How many of @p msg's content parts have type @p type; *@p total_out gets
 * the part count (0 when content isn't an array). */
static int parts_of_type(struct json_object *msg, const char *type, int *total_out) {
   struct json_object *content = NULL;
   *total_out = 0;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return 0;
   }
   const int n = json_object_array_length(content);
   int count = 0;
   for (int i = 0; i < n; i++) {
      struct json_object *t = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(content, i), "type", &t) &&
          is(json_object_get_string(t), type)) {
         count++;
      }
   }
   *total_out = n;
   return count;
}

/* Part of a tool exchange, in the OpenAI or Claude shape: a role "tool"
 * result, an assistant with tool_calls or tool_use parts, or a user message
 * with tool_result parts. */
bool llm_compaction_is_tool_exchange(struct json_object *msg) {
   const char *role = role_of(msg);
   int total = 0;
   if (is(role, "tool")) {
      return true;
   }
   if (is(role, "assistant")) {
      return json_object_object_get_ex(msg, "tool_calls", NULL) ||
             parts_of_type(msg, "tool_use", &total) > 0;
   }
   return is(role, "user") && parts_of_type(msg, "tool_result", &total) > 0;
}

/* Only tool results: a role "tool" message, or a user message whose parts are
 * all tool_result. */
static bool is_tool_result_only(struct json_object *msg) {
   const char *role = role_of(msg);
   int total = 0;
   if (is(role, "tool")) {
      return true;
   }
   return is(role, "user") && parts_of_type(msg, "tool_result", &total) == total && total > 0;
}

int llm_compaction_keep_start(struct json_object *history, int start_idx, int keep_messages) {
   const int history_len = history ? (int)json_object_array_length(history) : 0;
   int end_idx = history_len - keep_messages;

   /* Move back past the tool calls and results the cut would split, and the
    * request context between them (a directive, a loop note: it belongs to the
    * turn it sits in), then onto the user message that started them.  A
    * question DAWN asked (a continuation's) starts a turn too. */
   while (end_idx > start_idx) {
      struct json_object *msg = json_object_array_get_idx(history, end_idx - 1);
      if (!llm_compaction_is_tool_exchange(msg) &&
          !(llm_history_is_context(msg) && !llm_history_is_question(msg))) {
         break;
      }
      end_idx--;
   }
   if (end_idx > start_idx &&
       is(role_of(json_object_array_get_idx(history, end_idx - 1)), "user")) {
      end_idx--;
   }
   return end_idx;
}

int64_t llm_compaction_row_id(struct json_object *msg) {
   struct json_object *id_obj = NULL;
   if (!msg || !json_object_object_get_ex(msg, "id", &id_obj)) {
      return 0;
   }
   const int64_t id = json_object_get_int64(id_obj);
   return id > 0 ? id : 0;
}

void llm_compaction_summary_ids(struct json_object *history,
                                int start_idx,
                                int end_idx,
                                int64_t *first_out,
                                int64_t *last_out) {
   int64_t first = 0;
   int64_t last = 0;
   const int history_len = history ? (int)json_object_array_length(history) : 0;
   for (int i = start_idx; i < end_idx && i < history_len; i++) {
      const int64_t id = llm_compaction_row_id(json_object_array_get_idx(history, i));
      if (id > 0) {
         if (first == 0) {
            first = id;
         }
         last = id;
      }
   }
   *first_out = first;
   *last_out = last;
}

/* Add string @p id to @p ids (skipping NULL and empty). */
static void add_id(struct json_object *ids, const char *id) {
   if (id && *id) {
      json_object_array_add(ids, json_object_new_string(id));
   }
}

/* Add the call ids in @p msg: its tool_calls, its tool_call_id, its tool_use
 * and tool_result parts. */
static void add_call_ids(struct json_object *ids, struct json_object *msg) {
   struct json_object *v = NULL;
   if (json_object_object_get_ex(msg, "tool_call_id", &v)) {
      add_id(ids, json_object_get_string(v));
   }
   if (json_object_object_get_ex(msg, "tool_calls", &v) &&
       json_object_is_type(v, json_type_array)) {
      const int n = json_object_array_length(v);
      for (int i = 0; i < n; i++) {
         struct json_object *id = NULL;
         if (json_object_object_get_ex(json_object_array_get_idx(v, i), "id", &id)) {
            add_id(ids, json_object_get_string(id));
         }
      }
   }
   if (json_object_object_get_ex(msg, "content", &v) && json_object_is_type(v, json_type_array)) {
      const int n = json_object_array_length(v);
      for (int i = 0; i < n; i++) {
         struct json_object *part = json_object_array_get_idx(v, i), *t = NULL, *id = NULL;
         if (!json_object_object_get_ex(part, "type", &t)) {
            continue;
         }
         const char *type = json_object_get_string(t);
         if ((is(type, "tool_use") && json_object_object_get_ex(part, "id", &id)) ||
             (is(type, "tool_result") && json_object_object_get_ex(part, "tool_use_id", &id))) {
            add_id(ids, json_object_get_string(id));
         }
      }
   }
}

struct json_object *llm_compaction_tail_call_ids(struct json_object *history,
                                                 int start_idx,
                                                 int end_idx) {
   struct json_object *ids = json_object_new_array();
   const int history_len = history ? (int)json_object_array_length(history) : 0;
   if (!ids || end_idx > history_len) {
      end_idx = history_len;
   }
   int from = start_idx;
   for (int i = start_idx; i < end_idx; i++) {
      if (llm_compaction_row_id(json_object_array_get_idx(history, i)) > 0) {
         from = i + 1; /* after the last message that has its row id */
      }
   }
   for (int i = from; ids && i < end_idx; i++) {
      add_call_ids(ids, json_object_array_get_idx(history, i));
   }
   /* An id the kept part uses too (a provider that numbers calls per
    * response reuses them) can't tell the rows apart: leave it out. */
   struct json_object *kept = ids ? json_object_new_array() : NULL;
   for (int i = end_idx; kept && i < history_len; i++) {
      add_call_ids(kept, json_object_array_get_idx(history, i));
   }
   struct json_object *out = kept ? json_object_new_array() : NULL;
   const int n = out ? (int)json_object_array_length(ids) : 0;
   const int k = kept ? (int)json_object_array_length(kept) : 0;
   for (int i = 0; i < n; i++) {
      struct json_object *id = json_object_array_get_idx(ids, i);
      bool shared = false;
      for (int j = 0; j < k && !shared; j++) {
         shared = is(json_object_get_string(id),
                     json_object_get_string(json_object_array_get_idx(kept, j)));
      }
      if (!shared) {
         json_object_array_add(out, json_object_get(id));
      }
   }
   json_object_put(kept);
   json_object_put(ids);
   return out;
}

bool llm_compaction_row_in_calls(const char *role,
                                 const char *tool_calls,
                                 const char *tool_call_id,
                                 struct json_object *ids) {
   const int n = ids ? (int)json_object_array_length(ids) : 0;
   for (int i = 0; i < n; i++) {
      const char *id = json_object_get_string(json_object_array_get_idx(ids, i));
      if (!id || !*id) {
         continue;
      }
      if (is(role, "tool") && is(tool_call_id, id)) {
         return true;
      }
      if (is(role, "assistant") && tool_calls) {
         /* The id as a whole JSON string in the row's tool_calls. */
         const char *at = tool_calls;
         const size_t len = strlen(id);
         while ((at = strstr(at, id)) != NULL) {
            if (at > tool_calls && at[-1] == '"' && at[len] == '"') {
               return true;
            }
            at += len;
         }
      }
   }
   return false;
}

int llm_history_drop_leading_results(struct json_object *history, int from, size_t *chars_out) {
   if (chars_out) {
      *chars_out = 0;
   }
   if (!history || !json_object_is_type(history, json_type_array) || from < 0) {
      return 0;
   }
   const int len = json_object_array_length(history);
   int end = from;
   while (end < len && is_tool_result_only(json_object_array_get_idx(history, end))) {
      struct json_object *content = NULL;
      if (chars_out &&
          json_object_object_get_ex(json_object_array_get_idx(history, end), "content", &content) &&
          json_object_is_type(content, json_type_string)) {
         *chars_out += (size_t)json_object_get_string_len(content);
      }
      end++;
   }
   const int dropped = end - from;
   if (dropped > 0) {
      json_object_array_del_idx(history, from, dropped);
      OLOG_WARNING("history: dropped %d tool result(s) whose call is before the compaction point",
                   dropped);
   }
   return dropped;
}

int64_t llm_compaction_kept_first_id(struct json_object *history, int end_idx) {
   const int history_len = history ? (int)json_object_array_length(history) : 0;
   for (int i = end_idx < 0 ? 0 : end_idx; i < history_len; i++) {
      const int64_t id = llm_compaction_row_id(json_object_array_get_idx(history, i));
      if (id > 0) {
         return id;
      }
   }
   return 0;
}
