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
 * Provider-neutral assistant turn blocks.  See llm_turn_blocks.h.
 */

#include "llm/llm_turn_blocks.h"

#include <json-c/json.h>
#include <string.h>

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return (obj && json_object_object_get_ex(obj, key, &v)) ? json_object_get_string(v) : NULL;
}

/* @p v, or "" for NULL (json_object_new_string must never see NULL). */
static const char *or_empty(const char *v) {
   return v ? v : "";
}

static bool is(const char *a, const char *b) {
   return a && b && strcmp(a, b) == 0;
}

struct json_object *llm_turn_blocks_new(void) {
   return json_object_new_array();
}

void llm_turn_blocks_add_text(struct json_object *blocks, const char *text) {
   if (!blocks || !text || !*text) {
      return;
   }
   struct json_object *b = json_object_new_object();
   json_object_object_add(b, "type", json_object_new_string("text"));
   json_object_object_add(b, "text", json_object_new_string(text));
   json_object_array_add(blocks, b);
}

void llm_turn_blocks_add_tool_call(struct json_object *blocks,
                                   const char *id,
                                   const char *name,
                                   const char *arguments) {
   if (!blocks) {
      return;
   }
   struct json_object *b = json_object_new_object();
   json_object_object_add(b, "type", json_object_new_string("tool_call"));
   json_object_object_add(b, "id", json_object_new_string(id ? id : ""));
   json_object_object_add(b, "name", json_object_new_string(name ? name : ""));
   json_object_object_add(b, "arguments",
                          json_object_new_string(arguments && *arguments ? arguments : "{}"));
   json_object_array_add(blocks, b);
}

static void add_vendor_block(struct json_object *blocks,
                             const char *type,
                             const char *carrier,
                             const char *format,
                             const char *model,
                             struct json_object *native) {
   if (!blocks || !native) {
      json_object_put(native);
      return;
   }
   struct json_object *b = json_object_new_object();
   json_object_object_add(b, "type", json_object_new_string(type));
   json_object_object_add(b, "carrier", json_object_new_string(carrier ? carrier : ""));
   json_object_object_add(b, "format", json_object_new_string(format ? format : ""));
   if (model) {
      json_object_object_add(b, "model", json_object_new_string(model));
   }
   json_object_object_add(b, "native", native);
   json_object_array_add(blocks, b);
}

void llm_turn_blocks_add_reasoning(struct json_object *blocks,
                                   const char *carrier,
                                   const char *format,
                                   const char *model,
                                   struct json_object *native) {
   add_vendor_block(blocks, "reasoning", carrier, format, model, native);
}

bool llm_turn_blocks_is_own(struct json_object *block, const char *carrier, const char *format) {
   const char *type = str_of(block, "type");
   return (is(type, "reasoning") || is(type, "opaque")) && is(str_of(block, "carrier"), carrier) &&
          is(str_of(block, "format"), format);
}

/* A deep copy of @p obj (NULL on failure). */
static struct json_object *copy_of(struct json_object *obj) {
   struct json_object *out = NULL;
   if (obj && json_object_deep_copy(obj, &out, NULL) != 0) {
      return NULL;
   }
   return out;
}

struct json_object *llm_turn_blocks_from_claude(struct json_object *content, const char *model) {
   if (!content || !json_object_is_type(content, json_type_array)) {
      return NULL;
   }
   struct json_object *blocks = llm_turn_blocks_new();
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; blocks && i < n; i++) {
      struct json_object *block = json_object_array_get_idx(content, i);
      const char *type = str_of(block, "type");
      if (is(type, "text")) {
         llm_turn_blocks_add_text(blocks, str_of(block, "text"));
      } else if (is(type, "tool_use")) {
         struct json_object *input = NULL;
         json_object_object_get_ex(block, "input", &input);
         llm_turn_blocks_add_tool_call(
             blocks, str_of(block, "id"), str_of(block, "name"),
             input ? json_object_to_json_string_ext(input, JSON_C_TO_STRING_PLAIN) : "{}");
      } else if (is(type, "thinking") || is(type, "redacted_thinking")) {
         llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, model,
                                       copy_of(block));
      } else if (type) {
         add_vendor_block(blocks, "opaque", LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, model,
                          copy_of(block));
      }
   }
   return blocks;
}

struct json_object *llm_turn_blocks_render_claude(struct json_object *blocks) {
   if (!blocks || !json_object_is_type(blocks, json_type_array)) {
      return NULL;
   }
   struct json_object *content = json_object_new_array();
   const size_t n = json_object_array_length(blocks);
   for (size_t i = 0; content && i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      const char *type = str_of(b, "type");
      if (is(type, "text")) {
         struct json_object *t = json_object_new_object();
         json_object_object_add(t, "type", json_object_new_string("text"));
         json_object_object_add(t, "text", json_object_new_string(or_empty(str_of(b, "text"))));
         json_object_array_add(content, t);
      } else if (is(type, "tool_call")) {
         struct json_object *use = json_object_new_object();
         json_object_object_add(use, "type", json_object_new_string("tool_use"));
         json_object_object_add(use, "id", json_object_new_string(or_empty(str_of(b, "id"))));
         json_object_object_add(use, "name", json_object_new_string(or_empty(str_of(b, "name"))));
         const char *args = str_of(b, "arguments");
         struct json_object *input = args ? json_tokener_parse(args) : NULL;
         if (!input || !json_object_is_type(input, json_type_object)) {
            json_object_put(input);
            input = json_object_new_object();
         }
         json_object_object_add(use, "input", input);
         json_object_array_add(content, use);
      } else if (llm_turn_blocks_is_own(b, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC)) {
         struct json_object *native = NULL;
         if (json_object_object_get_ex(b, "native", &native)) {
            struct json_object *copy = copy_of(native);
            if (copy) {
               json_object_array_add(content, copy);
            }
         }
      }
   }
   return content;
}

/* Whether OpenAI reasoning block @p b may go back to @p carrier's @p model:
 * the same endpoint, and the same model (a reasoning item is encrypted for
 * the model and organization that produced it). */
static bool openai_reasoning_replays(struct json_object *b,
                                     const char *carrier,
                                     const char *model) {
   if (!llm_turn_blocks_is_own(b, carrier, LLM_FORMAT_OPENAI)) {
      return false;
   }
   const char *from = str_of(b, "model");
   return !model || !from || strcmp(from, model) == 0;
}

void llm_turn_blocks_render_responses(struct json_object *blocks,
                                      struct json_object *input,
                                      const char *carrier,
                                      const char *model) {
   if (!blocks || !input || !json_object_is_type(blocks, json_type_array)) {
      return;
   }
   struct json_object *parts = NULL; /* the open assistant message's output_text parts */
   const size_t n = json_object_array_length(blocks);
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      const char *type = str_of(b, "type");
      if (is(type, "text")) {
         if (!parts) {
            struct json_object *msg = json_object_new_object();
            parts = json_object_new_array();
            if (!msg || !parts) {
               json_object_put(msg);
               json_object_put(parts);
               parts = NULL;
               continue;
            }
            json_object_object_add(msg, "type", json_object_new_string("message"));
            json_object_object_add(msg, "role", json_object_new_string("assistant"));
            json_object_object_add(msg, "content", parts);
            json_object_array_add(input, msg);
         }
         struct json_object *t = json_object_new_object();
         json_object_object_add(t, "type", json_object_new_string("output_text"));
         json_object_object_add(t, "text", json_object_new_string(or_empty(str_of(b, "text"))));
         json_object_array_add(parts, t);
         continue;
      }
      parts = NULL; /* anything else ends the message */
      if (is(type, "tool_call")) {
         const char *args = str_of(b, "arguments");
         struct json_object *call = json_object_new_object();
         json_object_object_add(call, "type", json_object_new_string("function_call"));
         json_object_object_add(call, "call_id", json_object_new_string(or_empty(str_of(b, "id"))));
         json_object_object_add(call, "name", json_object_new_string(or_empty(str_of(b, "name"))));
         json_object_object_add(call, "arguments",
                                json_object_new_string(args && *args ? args : "{}"));
         json_object_array_add(input, call);
      } else if (openai_reasoning_replays(b, carrier, model)) {
         struct json_object *native = NULL;
         if (json_object_object_get_ex(b, "native", &native)) {
            struct json_object *copy = copy_of(native);
            if (copy) {
               json_object_array_add(input, copy);
            }
         }
      }
   }
}

struct json_object *llm_turn_blocks_with_calls(struct json_object *blocks,
                                               const llm_turn_call_t *calls,
                                               int count) {
   struct json_object *out = llm_turn_blocks_new();
   if (!out) {
      return NULL;
   }
   const int n_calls = (calls && count > 0) ? count : 0;
   bool placed[LLM_TURN_CALLS_MAX] = { false };
   const size_t n = blocks && json_object_is_type(blocks, json_type_array)
                        ? json_object_array_length(blocks)
                        : 0;
   size_t after_last_call = 0; /* where unplaced calls go: after the last call kept */
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      if (!is(str_of(b, "type"), "tool_call")) {
         json_object_array_add(out, json_object_get(b));
         continue;
      }
      const char *id = str_of(b, "id");
      for (int c = 0; c < n_calls && c < LLM_TURN_CALLS_MAX; c++) {
         if (!placed[c] && is(id, calls[c].id)) {
            /* The call as the model sent it: the run copy may be cut to fit. */
            json_object_array_add(out, json_object_get(b));
            placed[c] = true;
            after_last_call = json_object_array_length(out);
            break;
         }
      }
   }
   /* Calls that ran without a block of their own (the stream ended before it
    * arrived): they go where calls go, after the last one, or at the end. */
   bool any_unplaced = false;
   for (int c = 0; c < n_calls && c < LLM_TURN_CALLS_MAX; c++) {
      any_unplaced = any_unplaced || !placed[c];
   }
   if (any_unplaced) {
      /* json-c has no array insert: rebuild with the calls spliced in. */
      const size_t at = after_last_call ? after_last_call : json_object_array_length(out);
      const size_t len = json_object_array_length(out);
      struct json_object *merged = llm_turn_blocks_new();
      if (!merged) {
         json_object_put(out);
         return NULL;
      }
      for (size_t i = 0; i < at; i++) {
         json_object_array_add(merged, json_object_get(json_object_array_get_idx(out, i)));
      }
      for (int c = 0; c < n_calls && c < LLM_TURN_CALLS_MAX; c++) {
         if (!placed[c]) {
            llm_turn_blocks_add_tool_call(merged, calls[c].id, calls[c].name, calls[c].arguments);
         }
      }
      for (size_t i = at; i < len; i++) {
         json_object_array_add(merged, json_object_get(json_object_array_get_idx(out, i)));
      }
      json_object_put(out);
      out = merged;
   }
   return out;
}

struct json_object *llm_turn_message_blocks(struct json_object *message) {
   struct json_object *blocks = NULL;
   if (json_object_object_get_ex(message, LLM_TURN_BLOCKS_KEY, &blocks) &&
       json_object_is_type(blocks, json_type_array)) {
      return json_object_get(blocks);
   }
   struct json_object *content = NULL;
   json_object_object_get_ex(message, "content", &content);
   if (content && json_object_is_type(content, json_type_array)) {
      return llm_turn_blocks_from_claude(content, NULL);
   }
   struct json_object *out = llm_turn_blocks_new();
   if (!out) {
      return NULL;
   }
   if (content) {
      llm_turn_blocks_add_text(out, json_object_get_string(content));
   }
   struct json_object *tool_calls = NULL;
   if (json_object_object_get_ex(message, "tool_calls", &tool_calls) &&
       json_object_is_type(tool_calls, json_type_array)) {
      const size_t n = json_object_array_length(tool_calls);
      for (size_t k = 0; k < n; k++) {
         struct json_object *tc = json_object_array_get_idx(tool_calls, k);
         struct json_object *fn = NULL;
         const char *id = str_of(tc, "id");
         if (id && json_object_object_get_ex(tc, "function", &fn) && str_of(fn, "name")) {
            llm_turn_blocks_add_tool_call(out, id, str_of(fn, "name"), str_of(fn, "arguments"));
         }
      }
   }
   return out;
}

struct json_object *llm_turn_blocks_with_final_text(struct json_object *blocks,
                                                    const char *final_text) {
   struct json_object *out = llm_turn_blocks_new();
   if (!out) {
      return NULL;
   }
   bool placed = false;
   const size_t n = blocks && json_object_is_type(blocks, json_type_array)
                        ? json_object_array_length(blocks)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      if (is(str_of(b, "type"), "text")) {
         if (!placed) {
            llm_turn_blocks_add_text(out, final_text);
            placed = true;
         }
         continue;
      }
      struct json_object *copy = copy_of(b);
      if (copy) {
         json_object_array_add(out, copy);
      }
   }
   if (!placed) {
      llm_turn_blocks_add_text(out, final_text);
   }
   return out;
}

bool llm_turn_blocks_has_reasoning(struct json_object *blocks, const char *carrier) {
   const size_t n = blocks && json_object_is_type(blocks, json_type_array)
                        ? json_object_array_length(blocks)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      if (is(str_of(b, "type"), "reasoning") && is(str_of(b, "carrier"), carrier)) {
         return true;
      }
   }
   return false;
}

void llm_turn_message_set_text(struct json_object *message, const char *text) {
   if (!message || !text) {
      return;
   }
   json_object_object_add(message, "content", json_object_new_string(text));
   struct json_object *blocks = NULL;
   if (json_object_object_get_ex(message, LLM_TURN_BLOCKS_KEY, &blocks)) {
      struct json_object *updated = llm_turn_blocks_with_final_text(blocks, text);
      if (updated) {
         json_object_object_add(message, LLM_TURN_BLOCKS_KEY, updated);
      } else {
         json_object_object_del(message, LLM_TURN_BLOCKS_KEY); /* never stale text */
      }
   }
}

size_t llm_turn_message_reasoning_chars(struct json_object *message) {
   struct json_object *blocks = NULL;
   if (!message || !json_object_object_get_ex(message, LLM_TURN_BLOCKS_KEY, &blocks) ||
       !json_object_is_type(blocks, json_type_array)) {
      return 0;
   }
   size_t chars = 0;
   const size_t n = json_object_array_length(blocks);
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      struct json_object *native = NULL;
      if (!json_object_object_get_ex(b, "native", &native)) {
         continue;
      }
      if (is(str_of(b, "type"), "opaque")) {
         /* Replayed as sent (a server tool's result, say): count all of it. */
         chars += strlen(json_object_to_json_string_ext(native, JSON_C_TO_STRING_PLAIN));
         continue;
      }
      if (!is(str_of(b, "type"), "reasoning")) {
         continue;
      }
      const char *text = str_of(native, "thinking");
      const char *stand_in = (text && *text) ? text : str_of(native, "signature");
      if (!stand_in) {
         stand_in = str_of(native, "data"); /* redacted_thinking */
      }
      if (!stand_in) {
         stand_in = str_of(native, "encrypted_content"); /* an OpenAI reasoning item */
      }
      chars += stand_in ? strlen(stand_in) : 0;
      struct json_object *summary = NULL; /* an OpenAI item's summary goes back too */
      if (json_object_object_get_ex(native, "summary", &summary) &&
          json_object_is_type(summary, json_type_array)) {
         const size_t k = json_object_array_length(summary);
         for (size_t j = 0; j < k; j++) {
            const char *t = str_of(json_object_array_get_idx(summary, j), "text");
            chars += t ? strlen(t) : 0;
         }
      }
   }
   return chars;
}

static bool has_internal_key(struct json_object *msg) {
   json_object_object_foreach(msg, key, val) {
      (void)val;
      if (key[0] == '_') {
         return true;
      }
   }
   return false;
}

/* A copy of @p msg without DAWN's own keys.  @p deep copies the values too, so
 * the result shares nothing with the history (and the internal values are
 * never copied at all).  NULL on allocation failure. */
static struct json_object *copy_message(struct json_object *msg, bool deep) {
   if (!json_object_is_type(msg, json_type_object) || (!deep && !has_internal_key(msg))) {
      if (!deep) {
         return json_object_get(msg);
      }
      struct json_object *copy = NULL;
      if (json_object_deep_copy(msg, &copy, NULL) != 0) {
         json_object_put(copy);
         return NULL;
      }
      return copy;
   }
   struct json_object *copy = json_object_new_object();
   if (!copy) {
      return NULL;
   }
   json_object_object_foreach(msg, key, val) {
      if (key[0] == '_') {
         continue;
      }
      struct json_object *value = json_object_get(val);
      if (deep && val) {
         json_object_put(value);
         value = NULL;
         if (json_object_deep_copy(val, &value, NULL) != 0) {
            json_object_put(value);
            json_object_put(copy);
            return NULL;
         }
      }
      json_object_object_add(copy, key, value);
   }
   return copy;
}

static struct json_object *copy_history(struct json_object *history, bool deep) {
   if (!history || !json_object_is_type(history, json_type_array)) {
      return NULL;
   }
   const size_t len = json_object_array_length(history);
   struct json_object *out = json_object_new_array_ext((int)len);
   if (!out) {
      return NULL;
   }
   for (size_t i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *copy = msg ? copy_message(msg, deep) : NULL;
      if ((msg && !copy) || json_object_array_add(out, copy) != 0) {
         json_object_put(copy);
         json_object_put(out);
         return NULL; /* whole or nothing: never a hole or an unstripped message */
      }
   }
   return out;
}

struct json_object *llm_history_wire_copy(struct json_object *history) {
   return copy_history(history, false);
}

struct json_object *llm_history_strip_internal(struct json_object *history) {
   return copy_history(history, true);
}
