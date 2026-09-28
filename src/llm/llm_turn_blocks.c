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

#include <ctype.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

void llm_turn_blocks_add_signed_tool_call(struct json_object *blocks,
                                          const char *id,
                                          const char *name,
                                          const char *arguments,
                                          const char *carrier,
                                          const char *model,
                                          const char *signature) {
   const size_t before = blocks ? json_object_array_length(blocks) : 0;
   llm_turn_blocks_add_tool_call(blocks, id, name, arguments);
   const size_t n = blocks ? json_object_array_length(blocks) : 0;
   if (n != before + 1 || !signature || !*signature) {
      return; /* never on another call's block */
   }
   struct json_object *sig = json_object_new_object();
   if (!sig) {
      return;
   }
   json_object_object_add(sig, "carrier", json_object_new_string(or_empty(carrier)));
   json_object_object_add(sig, "format", json_object_new_string(LLM_FORMAT_GEMINI));
   if (model) {
      json_object_object_add(sig, "model", json_object_new_string(model));
   }
   json_object_object_add(sig, "value", json_object_new_string(signature));
   json_object_object_add(json_object_array_get_idx(blocks, n - 1), "sig", sig);
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

/* Where vendor-issued data may go back: the one rule every renderer uses.
 * Vendor data is (a) a reasoning or opaque block and (b) a tool call's "sig";
 * each carries the carrier, format and model that issued it.  It goes back
 * only to that carrier in that format, and (when both say) the same model:
 * reasoning is encrypted or signed for the model and organization that made
 * it.  Claude is the exception by design: its blocks go back to any Claude
 * model (NULL model), and the API drops what the current model can't read. */
static bool replays_to(struct json_object *v,
                       const char *format,
                       const char *carrier,
                       const char *model) {
   if (!is(str_of(v, "carrier"), carrier) || !is(str_of(v, "format"), format)) {
      return false;
   }
   const char *from = str_of(v, "model");
   return !model || !from || strcmp(from, model) == 0;
}

/* Whether block @p b is reasoning or opaque vendor content. */
static bool is_vendor_block(struct json_object *b) {
   const char *type = str_of(b, "type");
   return is(type, "reasoning") || is(type, "opaque");
}

bool llm_turn_blocks_is_own(struct json_object *block, const char *carrier, const char *format) {
   return is_vendor_block(block) && replays_to(block, format, carrier, NULL);
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


struct json_object *llm_turn_blocks_render_chat(struct json_object *blocks,
                                                const char *carrier,
                                                const char *model) {
   struct json_object *msg = json_object_new_object();
   if (!msg) {
      return NULL;
   }
   json_object_object_add(msg, "role", json_object_new_string("assistant"));
   size_t text_len = 0;
   const size_t n = blocks && json_object_is_type(blocks, json_type_array)
                        ? json_object_array_length(blocks)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      if (is(str_of(b, "type"), "text")) {
         text_len += strlen(or_empty(str_of(b, "text"))) + 2; /* and a "\n\n" between */
      }
   }
   char *text = malloc(text_len + 1);
   if (!text) {
      json_object_put(msg);
      return NULL;
   }
   size_t off = 0;
   struct json_object *calls = NULL;
   struct json_object *details = NULL;
   for (size_t i = 0; i < n; i++) {
      struct json_object *b = json_object_array_get_idx(blocks, i);
      const char *type = str_of(b, "type");
      if (is(type, "text")) {
         const char *t = or_empty(str_of(b, "text"));
         if (off > 0 && *t) {
            memcpy(text + off, "\n\n", 2); /* separate blocks, as a Claude turn reads */
            off += 2;
         }
         memcpy(text + off, t, strlen(t));
         off += strlen(t);
      } else if (is(type, "tool_call")) {
         if (!calls && !(calls = json_object_new_array())) {
            continue;
         }
         const char *args = str_of(b, "arguments");
         struct json_object *call = json_object_new_object();
         struct json_object *fn = json_object_new_object();
         json_object_object_add(call, "id", json_object_new_string(or_empty(str_of(b, "id"))));
         json_object_object_add(call, "type", json_object_new_string("function"));
         json_object_object_add(fn, "name", json_object_new_string(or_empty(str_of(b, "name"))));
         json_object_object_add(fn, "arguments",
                                json_object_new_string(args && *args ? args : "{}"));
         json_object_object_add(call, "function", fn);
         struct json_object *sig = NULL;
         if (json_object_object_get_ex(b, "sig", &sig) &&
             replays_to(sig, LLM_FORMAT_GEMINI, carrier, model)) {
            struct json_object *extra = json_object_new_object();
            struct json_object *google = json_object_new_object();
            json_object_object_add(google, "thought_signature",
                                   json_object_new_string(or_empty(str_of(sig, "value"))));
            json_object_object_add(extra, "google", google);
            json_object_object_add(call, "extra_content", extra);
         }
         json_object_array_add(calls, call);
      } else if (is_vendor_block(b) && replays_to(b, LLM_FORMAT_OPENROUTER, carrier, model)) {
         struct json_object *native = NULL;
         if (json_object_object_get_ex(b, "native", &native) &&
             (details || (details = json_object_new_array()))) {
            struct json_object *copy = copy_of(native);
            if (copy) {
               json_object_array_add(details, copy);
            }
         }
      }
   }
   text[off] = '\0';
   json_object_object_add(msg, "content", json_object_new_string(text));
   free(text);
   if (calls) {
      json_object_object_add(msg, "tool_calls", calls);
   }
   if (details) {
      json_object_object_add(msg, "reasoning_details", details);
   }
   return msg;
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
      } else if (is_vendor_block(b) && replays_to(b, LLM_FORMAT_OPENAI, carrier, model)) {
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
      struct json_object *sig = NULL;
      if (is(str_of(b, "type"), "tool_call") && json_object_object_get_ex(b, "sig", &sig)) {
         const char *v = str_of(sig, "value"); /* a Gemini signature goes back with its call */
         chars += v ? strlen(v) : 0;
         continue;
      }
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
      if (is(str_of(b, "format"), LLM_FORMAT_OPENROUTER)) {
         /* An entry goes back whole: its text, signature, data and summary. */
         static const char *const FIELDS[] = { "text", "signature", "data", "summary" };
         for (size_t k = 0; k < sizeof(FIELDS) / sizeof(FIELDS[0]); k++) {
            const char *v = str_of(native, FIELDS[k]);
            chars += v ? strlen(v) : 0;
         }
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

/* The host of @p url ("https://api.openai.com/v1" → "api.openai.com"). */
static void url_host(const char *url, char *out, size_t out_len) {
   if (out_len == 0) {
      return;
   }
   const char *start = url ? strstr(url, "://") : NULL;
   start = start ? start + 3 : (url ? url : "");
   size_t n = strcspn(start, "/?#");
   const char *at = memchr(start, '@', n); /* never carry credentials */
   if (at) {
      n -= (size_t)(at + 1 - start);
      start = at + 1;
   }
   if (n >= out_len) {
      n = out_len - 1;
   }
   for (size_t i = 0; i < n; i++) {
      out[i] = (char)tolower((unsigned char)start[i]);
   }
   out[n] = '\0';
}

/* The carrier of this request's reasoning: the endpoint's host and a short
 * fingerprint of the API key (an item is encrypted for the organization that
 * produced it; another key may belong to another one).  FNV-1a, 32 bits: it
 * tells keys apart, and says nothing useful about one. */
void llm_turn_blocks_carrier(const char *base_url, const char *api_key, char *out, size_t out_len) {
   char host[96];
   url_host(base_url, host, sizeof(host));
   uint32_t h = 2166136261u;
   for (const char *k = api_key ? api_key : ""; *k; k++) {
      h = (h ^ (uint8_t)*k) * 16777619u;
   }
   snprintf(out, out_len, "%s#%08x", host, h);
}
