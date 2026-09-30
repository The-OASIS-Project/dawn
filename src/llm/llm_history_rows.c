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
 * The rows a history message is saved as.  See llm_history_rows.h.
 */

#include "llm/llm_history_rows.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_messages.h"
#include "llm/llm_claude_parts.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

_Static_assert(LLM_TURN_BLOCKS_STORED_MAX == CONV_LLM_BLOCKS_MAX,
               "what the turn blocks store is what the database holds");

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return (obj && json_object_object_get_ex(obj, key, &v)) ? json_object_get_string(v) : NULL;
}

static bool is(const char *a, const char *b) {
   return a && b && strcmp(a, b) == 0;
}

static struct json_object *new_row(const char *role, const char *content) {
   struct json_object *row = json_object_new_object();
   if (row) {
      json_object_object_add(row, "role", json_object_new_string(role));
      json_object_object_add(row, "content", json_object_new_string(content ? content : ""));
   }
   return row;
}

/* Whether the message holds a turn in its own blocks (recorded blocks, or a
 * Claude content array), as opposed to plain text and tool_calls. */
static bool has_own_blocks(struct json_object *msg) {
   struct json_object *v = NULL;
   if (json_object_object_get_ex(msg, LLM_TURN_BLOCKS_KEY, &v)) {
      return true;
   }
   return json_object_object_get_ex(msg, "content", &v) && json_object_is_type(v, json_type_array);
}

/* An assistant turn: its display row, and its blocks when it has its own. */
static int append_assistant(struct json_object *msg, struct json_object *out) {
   struct json_object *blocks = llm_turn_message_blocks(msg);
   struct json_object *shown = blocks ? llm_turn_blocks_render_chat(blocks, NULL, NULL) : NULL;
   if (!shown) {
      json_object_put(blocks);
      struct json_object *row = new_row("assistant", str_of(msg, "content"));
      if (!row) {
         return 0;
      }
      json_object_array_add(out, row);
      return 1;
   }

   struct json_object *row = new_row("assistant", str_of(shown, "content"));
   struct json_object *calls = NULL;
   if (row && json_object_object_get_ex(shown, "tool_calls", &calls)) {
      json_object_object_add(row, "tool_calls", json_object_get(calls));
   }
   if (row && has_own_blocks(msg) && llm_turn_blocks_calls_match(blocks, calls)) {
      char *stored = llm_turn_blocks_to_stored(blocks);
      if (stored) {
         json_object_object_add(row, LLM_HISTORY_ROW_STORED_KEY, json_object_new_string(stored));
         free(stored);
      }
   }
   json_object_put(shown);
   json_object_put(blocks);
   if (!row) {
      return 0;
   }
   json_object_array_add(out, row);
   return 1;
}

/* A Claude user message's tool results, one tool row each.  -1 when the
 * message isn't one. */
static int append_claude_results(struct json_object *msg, struct json_object *out) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return -1;
   }
   const size_t n = json_object_array_length(content);
   bool any = false;
   for (size_t i = 0; i < n && !any; i++) {
      any = is(str_of(json_object_array_get_idx(content, i), "type"), "tool_result");
   }
   if (!any) {
      return -1;
   }
   int added = 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      const char *id = str_of(part, "tool_use_id");
      if (!is(str_of(part, "type"), "tool_result")) {
         continue;
      }
      if (!id || !*id) {
         OLOG_WARNING("history rows: a tool result with no call id not saved");
         continue;
      }
      char *text = llm_claude_tool_result_text(part);
      struct json_object *row = new_row("tool", text ? text : "");
      free(text);
      if (row) {
         json_object_object_add(row, "tool_call_id", json_object_new_string(id));
         json_object_array_add(out, row);
         added++;
      }
   }
   return added;
}

/* What a content part reads as: its text, "[image]" for an image, else NULL. */
static const char *part_text(struct json_object *part) {
   const char *type = str_of(part, "type");
   if (is(type, "text")) {
      return str_of(part, "text");
   }
   if (is(type, "image") || is(type, "image_url")) {
      return "[image]";
   }
   return NULL;
}

/* A content array's text: its text parts joined, an image as "[image]". */
static char *parts_as_text(struct json_object *parts) {
   const size_t n = json_object_array_length(parts);
   size_t len = 0;
   for (size_t i = 0; i < n; i++) {
      const char *t = part_text(json_object_array_get_idx(parts, i));
      len += (t ? strlen(t) : 0) + 2; /* and a "\n\n" before it */
   }
   char *out = malloc(len + 1);
   if (!out) {
      return NULL;
   }
   size_t off = 0;
   for (size_t i = 0; i < n; i++) {
      const char *t = part_text(json_object_array_get_idx(parts, i));
      if (!t || !*t) {
         continue;
      }
      if (off > 0) {
         memcpy(out + off, "\n\n", 2);
         off += 2;
      }
      memcpy(out + off, t, strlen(t));
      off += strlen(t);
   }
   out[off] = '\0';
   return out;
}

/* The rows of a message without request context of its own. */
static int append_message(struct json_object *msg, const char *role, struct json_object *out) {
   if (is(role, "assistant")) {
      return append_assistant(msg, out);
   }
   if (is(role, "user")) {
      const int results = append_claude_results(msg, out);
      if (results >= 0) {
         return results;
      }
   }
   struct json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   char *parts_text = json_object_is_type(content, json_type_array) ? parts_as_text(content) : NULL;
   const char *text = "";
   if (parts_text) {
      text = parts_text;
   } else if (json_object_is_type(content, json_type_string)) {
      text = json_object_get_string(content);
   }
   struct json_object *row = new_row(role, text);
   free(parts_text);
   if (!row) {
      return 0;
   }
   const char *call_id = str_of(msg, "tool_call_id");
   if (is(role, "tool") && call_id) {
      json_object_object_add(row, "tool_call_id", json_object_new_string(call_id));
   }
   json_object_array_add(out, row);
   return 1;
}

/* Mark the rows from @p from on with @p kind. */
static void mark_rows(struct json_object *out, size_t from, message_kind_t kind) {
   const size_t n = json_object_array_length(out);
   for (size_t i = from; i < n; i++) {
      llm_history_set_kind(json_object_array_get_idx(out, i), kind);
   }
}

/* A question with its turn's context in front: the question's own rows from
 * its other parts, then a row per context part (the order a live turn saves
 * them in: the question before the context composed for it). */
static int append_with_context(struct json_object *msg,
                               const char *role,
                               message_kind_t kind,
                               struct json_object *out) {
   struct json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   struct json_object *own = json_object_new_array();
   if (!own) {
      return 0;
   }
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      if (llm_history_kind_of(part) == MESSAGE_KIND_NONE) {
         json_object_array_add(own, json_object_get(part));
      }
   }
   int added = 0;
   if (json_object_array_length(own) > 0) {
      struct json_object *question = json_object_new_object();
      if (question) {
         json_object_object_foreach(msg, key, val) {
            if (strcmp(key, "content") != 0 && strcmp(key, MESSAGE_KIND_KEY) != 0) {
               json_object_object_add(question, key, json_object_get(val));
            }
         }
         json_object_object_add(question, "content", json_object_get(own));
         const size_t from = json_object_array_length(out);
         added += append_message(question, role, out);
         mark_rows(out, from, kind);
         json_object_put(question);
      }
   }
   json_object_put(own);
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      const message_kind_t part_kind = llm_history_kind_of(part);
      if (part_kind == MESSAGE_KIND_NONE || message_kind_in_memory_only(part_kind)) {
         continue; /* its own words, above; or the conversation's (a summary) */
      }
      struct json_object *row = new_row(role, str_of(part, "text"));
      if (row) {
         llm_history_set_kind(row, part_kind);
         json_object_array_add(out, row);
         added++;
      }
   }
   return added;
}

int llm_history_rows_append(struct json_object *msg, struct json_object *out) {
   const char *role = str_of(msg, "role");
   if (!role || !out) {
      return 0;
   }
   const message_kind_t kind = llm_history_kind_of(msg);
   if (message_kind_in_memory_only(kind)) {
      return 0; /* the conversation's (the frozen prefix), never a row */
   }
   if (llm_history_has_context_parts(msg)) {
      return append_with_context(msg, role, kind, out);
   }
   const size_t from = json_object_array_length(out);
   const int added = append_message(msg, role, out);
   mark_rows(out, from, kind);
   return added;
}

int llm_history_rows_append_text(struct json_object *msg, struct json_object *out) {
   const size_t before = out ? json_object_array_length(out) : 0;
   const int added = llm_history_rows_append(msg, out);
   for (int r = 0; r < added; r++) {
      json_object_object_del(json_object_array_get_idx(out, before + (size_t)r),
                             LLM_HISTORY_ROW_STORED_KEY);
   }
   return added;
}
