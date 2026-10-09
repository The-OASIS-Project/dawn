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
 * Request context on a history, in memory (llm_history_kind.h).
 */

#include "llm/llm_history_kind.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_context_text.h"
#include "prompts.h"

static const char *str_field(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   if (!json_object_is_type(obj, json_type_object) || !json_object_object_get_ex(obj, key, &v) ||
       !json_object_is_type(v, json_type_string)) {
      return NULL;
   }
   return json_object_get_string(v);
}

static bool is_role(struct json_object *msg, const char *role) {
   const char *r = str_field(msg, "role");
   return r && strcmp(r, role) == 0;
}

struct json_object *llm_history_prefix(struct json_object *history) {
   if (!json_object_is_type(history, json_type_array) || json_object_array_length(history) == 0) {
      return NULL;
   }
   struct json_object *first = json_object_array_get_idx(history, 0);
   return llm_history_kind_of(first) == MESSAGE_KIND_PREFIX ? first : NULL;
}

struct json_object *llm_history_in_force(struct json_object *prefix, bool create) {
   struct json_object *rec = NULL;
   if (json_object_object_get_ex(prefix, LLM_HISTORY_IN_FORCE_KEY, &rec) &&
       json_object_is_type(rec, json_type_object)) {
      return rec;
   }
   if (!create || !json_object_is_type(prefix, json_type_object)) {
      return NULL;
   }
   rec = json_object_new_object();
   if (rec) {
      json_object_object_add(prefix, LLM_HISTORY_IN_FORCE_KEY, rec);
   }
   return rec;
}

struct json_object *llm_history_frozen_tools(struct json_object *history) {
   struct json_object *tools = NULL;
   if (!json_object_object_get_ex(llm_history_prefix(history), LLM_HISTORY_TOOLS_KEY, &tools) ||
       !json_object_is_type(tools, json_type_array)) {
      return NULL;
   }
   return tools;
}

const char *llm_history_tag(struct json_object *history) {
   const char *tag = str_field(llm_history_in_force(llm_history_prefix(history), false), "tag");
   return (tag && tag[0]) ? tag : NULL;
}

message_kind_t llm_history_kind_of(struct json_object *obj) {
   return message_kind_parse(str_field(obj, MESSAGE_KIND_KEY));
}

void llm_history_set_kind(struct json_object *obj, message_kind_t kind) {
   if (!json_object_is_type(obj, json_type_object)) {
      return;
   }
   const char *name = message_kind_name(kind);
   if (name) {
      json_object_object_add(obj, MESSAGE_KIND_KEY, json_object_new_string(name));
   } else {
      json_object_object_del(obj, MESSAGE_KIND_KEY);
   }
}

static struct json_object *content_array(struct json_object *msg) {
   struct json_object *content = NULL;
   if (!json_object_is_type(msg, json_type_object) ||
       !json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return NULL;
   }
   return content;
}

bool llm_history_has_context_parts(struct json_object *msg) {
   struct json_object *parts = content_array(msg);
   const size_t n = parts ? json_object_array_length(parts) : 0;
   for (size_t i = 0; i < n; i++) {
      if (llm_history_kind_of(json_object_array_get_idx(parts, i)) != MESSAGE_KIND_NONE) {
         return true;
      }
   }
   return false;
}

bool llm_history_is_context(struct json_object *msg) {
   if (llm_history_kind_of(msg) != MESSAGE_KIND_NONE) {
      return true;
   }
   struct json_object *parts = content_array(msg);
   const size_t n = parts ? json_object_array_length(parts) : 0;
   if (n == 0) {
      return false;
   }
   for (size_t i = 0; i < n; i++) {
      if (llm_history_kind_of(json_object_array_get_idx(parts, i)) == MESSAGE_KIND_NONE) {
         return false;
      }
   }
   return true;
}

struct json_object *llm_history_context_part(const char *text, message_kind_t kind) {
   struct json_object *part = json_object_new_object();
   if (!part) {
      return NULL;
   }
   json_object_object_add(part, "type", json_object_new_string("text"));
   json_object_object_add(part, "text", json_object_new_string(text ? text : ""));
   llm_history_set_kind(part, kind);
   return part;
}

static bool is_note(struct json_object *msg) {
   const message_kind_t k = llm_history_kind_of(msg);
   return (k == MESSAGE_KIND_DIRECTIVE || k == MESSAGE_KIND_INSTRUCTION) && is_role(msg, "system");
}

/* A copy of @p msg (its keys shared) whose content is @p parts (taken). */
static struct json_object *with_parts(struct json_object *msg, struct json_object *parts) {
   struct json_object *copy = json_object_new_object();
   if (!copy) {
      json_object_put(parts);
      return NULL;
   }
   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") != 0) {
         json_object_object_add(copy, key, json_object_get(val));
      }
   }
   json_object_object_add(copy, "content", parts);
   return copy;
}

/* @p msg's content as parts with the note parts of @p notes (messages
 * [first, end) of @p history) after its leading context parts. */
static struct json_object *parts_with_notes(struct json_object *msg,
                                            struct json_object *history,
                                            size_t first,
                                            size_t end,
                                            const char *label) {
   struct json_object *parts = json_object_new_array();
   if (!parts) {
      return NULL;
   }
   struct json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   struct json_object *own = json_object_is_type(content, json_type_array) ? content : NULL;
   const size_t n = own ? json_object_array_length(own) : 0;
   size_t lead = 0;
   while (lead < n &&
          llm_history_kind_of(json_object_array_get_idx(own, lead)) != MESSAGE_KIND_NONE) {
      json_object_array_add(parts, json_object_get(json_object_array_get_idx(own, lead)));
      lead++;
   }
   for (size_t j = first; j < end; j++) {
      struct json_object *note = json_object_array_get_idx(history, j);
      const char *text = str_field(note, "content");
      const size_t len = strlen(label) + strlen(text ? text : "") + 1;
      char *headed = malloc(len);
      struct json_object *part = NULL;
      if (headed) {
         snprintf(headed, len, "%s%s", label, text ? text : "");
         part = llm_history_context_part(headed, llm_history_kind_of(note));
         free(headed);
      }
      if (!part) {
         json_object_put(parts);
         return NULL;
      }
      json_object_array_add(parts, part);
   }
   for (size_t i = lead; i < n; i++) {
      json_object_array_add(parts, json_object_get(json_object_array_get_idx(own, i)));
   }
   const char *text = own ? NULL : str_field(msg, "content");
   if (text && *text) {
      struct json_object *words = json_object_new_object();
      if (!words) {
         json_object_put(parts);
         return NULL;
      }
      json_object_object_add(words, "type", json_object_new_string("text"));
      json_object_object_add(words, "text", json_object_new_string(text));
      json_object_array_add(parts, words);
   }
   return parts;
}

struct json_object *llm_history_notes_before_words(struct json_object *history) {
   char label[LLM_CONTEXT_TAG_MAX + 24];
   llm_operator_note_label(llm_history_tag(history), label, sizeof(label));
   const size_t n = json_object_is_type(history, json_type_array)
                        ? json_object_array_length(history)
                        : 0;
   struct json_object *out = json_object_new_array();
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      size_t end = i + 1;
      while (end < n && is_note(json_object_array_get_idx(history, end))) {
         end++;
      }
      if (end == i + 1 || !llm_history_is_question(msg)) {
         json_object_array_add(out, json_object_get(msg));
         continue;
      }
      /* An envelope's notes go in the context message in front of it, or in
       * one of their own there: DAWN's text never shares a message with
       * untrusted text (an earlier envelope is not a context message). */
      const bool envelope = llm_history_kind_of(msg) == MESSAGE_KIND_ENVELOPE;
      const size_t have = json_object_array_length(out);
      struct json_object *prev = have ? json_object_array_get_idx(out, have - 1) : NULL;
      const bool into_prev = envelope && prev && is_role(prev, "user") &&
                             llm_history_kind_of(prev) == MESSAGE_KIND_NONE &&
                             llm_history_is_context(prev);
      struct json_object *holder = !envelope ? msg : into_prev ? prev : NULL;
      struct json_object *empty = holder ? NULL : json_object_new_object();
      struct json_object *parts = (holder || empty) ? parts_with_notes(holder ? holder : empty,
                                                                       history, i + 1, end, label)
                                                    : NULL;
      json_object_put(empty);
      struct json_object *noted = !parts   ? NULL
                                  : holder ? with_parts(holder, parts)
                                           : llm_history_context_message(parts);
      if (!noted) {
         json_object_put(out);
         return NULL;
      }
      if (into_prev) {
         json_object_array_put_idx(out, have - 1, noted);
      } else {
         json_object_array_add(out, noted);
      }
      if (envelope) {
         json_object_array_add(out, json_object_get(msg));
      }
      i = end - 1;
   }
   return out;
}

/* A loaded row that becomes a part of its turn's question. */
static bool folds_into_question(struct json_object *msg) {
   const message_kind_t k = llm_history_kind_of(msg);
   return (k == MESSAGE_KIND_TURN_CONTEXT || k == MESSAGE_KIND_MEMORY) && is_role(msg, "user");
}

bool llm_history_role_is(struct json_object *msg, const char *role) {
   return is_role(msg, role);
}

const char *llm_history_text(struct json_object *msg) {
   return str_field(msg, "content");
}

bool llm_history_is_question(struct json_object *msg) {
   const message_kind_t k = llm_history_kind_of(msg);
   if (!is_role(msg, "user") || (k != MESSAGE_KIND_NONE && k != MESSAGE_KIND_ENVELOPE) ||
       json_object_object_get_ex(msg, "tool_call_id", NULL)) {
      return false;
   }
   struct json_object *parts = content_array(msg);
   const size_t n = parts ? json_object_array_length(parts) : 0;
   for (size_t i = 0; i < n; i++) {
      const char *type = str_field(json_object_array_get_idx(parts, i), "type");
      if (type && strcmp(type, "tool_result") == 0) {
         return false;
      }
   }
   return true;
}

const char *llm_history_question_text(struct json_object *msg) {
   const char *text = str_field(msg, "content");
   if (text) {
      return text;
   }
   struct json_object *parts = content_array(msg);
   const size_t n = parts ? json_object_array_length(parts) : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(parts, i);
      const char *type = str_field(part, "type");
      if (llm_history_kind_of(part) == MESSAGE_KIND_NONE && type && strcmp(type, "text") == 0) {
         return str_field(part, "text");
      }
   }
   return NULL;
}

void llm_history_for_each_context_part(struct json_object *history,
                                       llm_history_part_fn fn,
                                       void *ctx) {
   const size_t n = (fn && json_object_is_type(history, json_type_array))
                        ? json_object_array_length(history)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *parts = content_array(msg);
      const size_t np = parts ? json_object_array_length(parts) : 0;
      for (size_t k = 0; k < np; k++) {
         struct json_object *part = json_object_array_get_idx(parts, k);
         const message_kind_t kind = llm_history_kind_of(part);
         if (kind == MESSAGE_KIND_NONE) {
            continue;
         }
         const char *text = str_field(part, "text");
         if (text) {
            fn(msg, part, kind, text, ctx);
         }
      }
   }
}

const char *llm_history_memory_in_force(struct json_object *history) {
   const int len = json_object_is_type(history, json_type_array)
                       ? (int)json_object_array_length(history)
                       : 0;
   for (int i = len - 1; i >= 0; i--) {
      struct json_object *parts = content_array(json_object_array_get_idx(history, i));
      for (int j = parts ? (int)json_object_array_length(parts) - 1 : -1; j >= 0; j--) {
         struct json_object *part = json_object_array_get_idx(parts, j);
         if (llm_history_kind_of(part) == MESSAGE_KIND_MEMORY) {
            return str_field(part, "text");
         }
      }
   }
   return NULL;
}

/* A copy of @p msg (its values shared) whose content has @p pending's parts in
 * front of it; the history's own message is left as it is. NULL on failure. */
static struct json_object *with_parts_in_front(struct json_object *msg,
                                               struct json_object *pending) {
   struct json_object *merged = json_object_new_array();
   struct json_object *copy = json_object_new_object();
   if (!merged || !copy) {
      json_object_put(merged);
      json_object_put(copy);
      return NULL;
   }
   const size_t np = json_object_array_length(pending);
   for (size_t i = 0; i < np; i++) {
      json_object_array_add(merged, json_object_get(json_object_array_get_idx(pending, i)));
   }
   struct json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   if (json_object_is_type(content, json_type_array)) {
      const size_t n = json_object_array_length(content);
      for (size_t i = 0; i < n; i++) {
         json_object_array_add(merged, json_object_get(json_object_array_get_idx(content, i)));
      }
   } else {
      const char *text = json_object_is_type(content, json_type_string)
                             ? json_object_get_string(content)
                             : "";
      /* An empty question adds no part (an empty text part is refused). */
      struct json_object *part = *text ? llm_history_context_part(text, MESSAGE_KIND_NONE) : NULL;
      if (*text && !part) {
         json_object_put(merged);
         json_object_put(copy);
         return NULL;
      }
      if (part) {
         json_object_array_add(merged, part);
      }
   }
   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") != 0) {
         json_object_object_add(copy, key, json_object_get(val));
      }
   }
   json_object_object_add(copy, "content", merged);
   return copy;
}

/* A message of @p pending's parts alone (a turn's context with no question),
 * carrying the id of its last row. */
static struct json_object *context_only_message(struct json_object *pending, int64_t last_id) {
   struct json_object *msg = json_object_new_object();
   if (!msg) {
      return NULL;
   }
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", json_object_get(pending));
   if (last_id > 0) {
      json_object_object_add(msg, "id", json_object_new_int64(last_id));
   }
   return msg;
}

/* A row's id, or 0. */
static int64_t id_of(struct json_object *msg) {
   struct json_object *id = NULL;
   return json_object_object_get_ex(msg, "id", &id) ? json_object_get_int64(id) : 0;
}

/* Put @p parts in front of the question at @p index of @p out (a copy: the
 * history's own message is left as it is).  False on allocation failure. */
static bool attach_parts(struct json_object *out, int index, struct json_object *parts) {
   if (index < 0 || !parts || json_object_array_length(parts) == 0) {
      return true;
   }
   struct json_object *merged = with_parts_in_front(json_object_array_get_idx(out, index), parts);
   if (!merged) {
      return false;
   }
   json_object_array_put_idx(out, (size_t)index, merged);
   return true;
}

bool llm_history_insert(struct json_object *history, size_t index, struct json_object *msg) {
   const size_t n = json_object_is_type(history, json_type_array)
                        ? json_object_array_length(history)
                        : 0;
   if (!msg || !history || index > n) {
      json_object_put(msg);
      return false;
   }
   struct json_object *tail = json_object_new_array();
   if (!tail) {
      json_object_put(msg);
      return false;
   }
   for (size_t i = index; i < n; i++) {
      json_object_array_add(tail, json_object_get(json_object_array_get_idx(history, i)));
   }
   if (n > index) {
      json_object_array_del_idx(history, index, n - index);
   }
   json_object_array_add(history, msg);
   for (size_t i = 0; i < n - index; i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(tail, i)));
   }
   json_object_put(tail);
   return true;
}

bool llm_history_is_unsaved_context(struct json_object *msg) {
   return msg && is_role(msg, "user") && id_of(msg) == 0 &&
          llm_history_kind_of(msg) == MESSAGE_KIND_NONE && llm_history_is_context(msg);
}

struct json_object *llm_history_context_message(struct json_object *parts) {
   struct json_object *msg = json_object_new_object();
   if (!msg) {
      json_object_put(parts);
      return NULL;
   }
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", parts);
   return msg;
}

/* The question @p msg names (a kinded row saved with its turn), or 0. */
static int64_t anchor_of(struct json_object *msg) {
   struct json_object *v = NULL;
   if (llm_history_kind_of(msg) == MESSAGE_KIND_NONE ||
       !json_object_object_get_ex(msg, LLM_HISTORY_CONTEXT_OF_KEY, &v)) {
      return 0;
   }
   return json_object_get_int64(v);
}

/* A context row's text as a kind part. */
static struct json_object *row_part(struct json_object *msg) {
   struct json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   return llm_history_context_part(json_object_is_type(content, json_type_string)
                                       ? json_object_get_string(content)
                                       : "",
                                   llm_history_kind_of(msg));
}

/* Append @p item to the array at @p *arr, making it first.  False on failure. */
static bool push(struct json_object **arr, struct json_object *item) {
   if (!item || (!*arr && !(*arr = json_object_new_array()))) {
      json_object_put(item);
      return false;
   }
   return json_object_array_add(*arr, item) == 0;
}

/* The rows saved naming their question, gathered per question: its parts
 * (turn context, memory) and the messages that follow it (a directive or an
 * instruction change).  A row whose question isn't loaded (left out of this
 * load) goes with it.  Every anchored row is marked in @p taken. */
static bool gather_anchored(struct json_object *history,
                            int from,
                            int len,
                            bool *taken,
                            struct json_object **parts,
                            struct json_object **after) {
   for (int i = from; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      const int64_t anchor = anchor_of(msg);
      if (anchor <= 0) {
         continue;
      }
      taken[i] = true;
      int q = -1;
      /* Rows load in id order: past a smaller id, the question isn't here. */
      for (int j = i - 1; j >= from && q < 0; j--) {
         struct json_object *cand = json_object_array_get_idx(history, j);
         const int64_t cid = id_of(cand);
         if (cid > 0 && cid < anchor) {
            break;
         }
         if (cid == anchor && llm_history_is_question(cand)) {
            q = j;
         }
      }
      if (q < 0) {
         continue;
      }
      if (folds_into_question(msg) ? !push(&parts[q], row_part(msg))
                                   : !push(&after[q], json_object_get(msg))) {
         return false;
      }
   }
   return true;
}

int llm_history_fold_context(struct json_object *history, int from) {
   if (!json_object_is_type(history, json_type_array) || from < 0) {
      return 0;
   }
   const int len = (int)json_object_array_length(history);
   bool any = false;
   for (int i = from; i < len && !any; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      any = folds_into_question(msg) || anchor_of(msg) > 0;
   }
   if (!any) {
      return 0;
   }

   /* Built beside the history and swapped in whole, so a failure leaves the
    * history as loaded.  A row saved naming its question goes with it,
    * whatever landed between them; one without (saved before rows named
    * theirs) follows its question, and a run with no question since the last
    * reply is a message of its own. */
   struct json_object *out = json_object_new_array();
   bool *taken = calloc((size_t)len, sizeof(*taken));
   struct json_object **anchored_parts = calloc((size_t)len, sizeof(*anchored_parts));
   struct json_object **anchored_after = calloc((size_t)len, sizeof(*anchored_after));
   int failed = !out || !taken || !anchored_parts || !anchored_after ||
                !gather_anchored(history, from, len, taken, anchored_parts, anchored_after);
   int question = -1;                /* index in out of the turn's question, or -1 */
   struct json_object *parts = NULL; /* its context, in row order */
   struct json_object *orphan = NULL;
   int64_t orphan_id = 0;
   for (int i = from; i < len && !failed; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (taken[i]) {
         continue;
      }
      if (folds_into_question(msg)) {
         if (!push(question >= 0 ? &parts : &orphan, row_part(msg))) {
            failed = 1;
            break;
         }
         if (question < 0 && id_of(msg) > 0) {
            orphan_id = id_of(msg);
         }
         continue;
      }
      if (llm_history_kind_of(msg) != MESSAGE_KIND_NONE && !llm_history_is_question(msg)) {
         /* Other request context (a directive) doesn't end the turn's. */
         json_object_array_add(out, json_object_get(msg));
         continue;
      }
      /* Anything else ends the turn's context: attach it, then start over. */
      if (!attach_parts(out, question, parts)) {
         failed = 1;
         break;
      }
      json_object_put(parts);
      parts = NULL;
      if (orphan) {
         struct json_object *alone = context_only_message(orphan, orphan_id);
         json_object_put(orphan);
         orphan = NULL;
         orphan_id = 0;
         if (!alone || json_object_array_add(out, alone) != 0) {
            json_object_put(alone);
            failed = 1;
            break;
         }
      }
      if (llm_history_is_question(msg) && anchored_parts[i] &&
          llm_history_kind_of(msg) == MESSAGE_KIND_ENVELOPE) {
         /* An envelope's context: a message of its own just before it. */
         struct json_object *own = llm_history_context_message(anchored_parts[i]);
         anchored_parts[i] = NULL;
         if (!own || json_object_array_add(out, own) != 0) {
            json_object_put(own);
            failed = 1;
            break;
         }
      }
      json_object_array_add(out, json_object_get(msg));
      question = llm_history_is_question(msg) ? (int)json_object_array_length(out) - 1 : -1;
      if (question >= 0) {
         /* Its own rows: the parts in front, what it appended right after. */
         parts = anchored_parts[i];
         anchored_parts[i] = NULL;
         const size_t n = anchored_after[i] ? json_object_array_length(anchored_after[i]) : 0;
         for (size_t k = 0; k < n; k++) {
            json_object_array_add(out,
                                  json_object_get(json_object_array_get_idx(anchored_after[i], k)));
         }
      }
   }
   if (!failed && !attach_parts(out, question, parts)) {
      failed = 1;
   }
   if (!failed && orphan) {
      struct json_object *alone = context_only_message(orphan, orphan_id);
      if (!alone || json_object_array_add(out, alone) != 0) {
         json_object_put(alone);
         failed = 1;
      }
   }
   json_object_put(parts);
   json_object_put(orphan);
   for (int i = 0; i < len && anchored_parts && anchored_after; i++) {
      json_object_put(anchored_parts[i]);
      json_object_put(anchored_after[i]);
   }
   free(anchored_parts);
   free(anchored_after);
   free(taken);
   if (failed) {
      json_object_put(out);
      return 1;
   }

   json_object_array_del_idx(history, (size_t)from, (size_t)(len - from));
   const size_t n = json_object_array_length(out);
   for (size_t i = 0; i < n; i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(out, i)));
   }
   json_object_put(out);
   return 0;
}

void llm_history_drop_context(struct json_object *history) {
   if (!json_object_is_type(history, json_type_array)) {
      return;
   }
   for (size_t i = json_object_array_length(history); i-- > 0;) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (llm_history_is_context(msg)) {
         json_object_array_del_idx(history, i, 1);
         continue;
      }
      struct json_object *parts = content_array(msg);
      bool dropped = false;
      for (size_t j = parts ? json_object_array_length(parts) : 0; j-- > 0;) {
         if (llm_history_kind_of(json_object_array_get_idx(parts, j)) != MESSAGE_KIND_NONE) {
            json_object_array_del_idx(parts, j, 1);
            dropped = true;
         }
      }
      /* A question that was text is text again. */
      struct json_object *only = (dropped && json_object_array_length(parts) == 1)
                                     ? json_object_array_get_idx(parts, 0)
                                     : NULL;
      const char *type = str_field(only, "type");
      const char *text = str_field(only, "text");
      if (type && strcmp(type, "text") == 0 && text) {
         json_object_object_add(msg, "content", json_object_new_string(text));
      }
   }
}

char *llm_history_summary_text(const char *summary, const char *tag) {
   const char *name = "CONVERSATION SUMMARY";
   const char *lead = CONVERSATION_SUMMARY_LEAD;
   /* The summary as stored, verbatim: it was neutralized once, when it was
    * made (llm_compaction), so each render (live, and every reload after) is
    * the same bytes whatever the neutralizer's rules become.  Masking the
    * conversation's tag finds nothing in a summary made safe that way; it is
    * kept for one that names the tag in any other form. */
   char *copy = strdup(summary ? summary : "");
   char *safe = copy ? llm_context_mask_tag(copy, tag) : NULL;
   if (!safe) {
      return NULL;
   }
   const char *body = safe;
   const bool newline = *body && body[strlen(body) - 1] == '\n';
   char head[96];
   char tail[96];
   if (tag && *tag) {
      snprintf(head, sizeof(head), "--- %s (%s) ---\n", name, tag);
      snprintf(tail, sizeof(tail), "--- END %s (%s) ---\n", name, tag);
   } else {
      snprintf(head, sizeof(head), "--- %s ---\n", name);
      snprintf(tail, sizeof(tail), "--- END %s ---\n", name);
   }
   const size_t len = strlen(head) + strlen(lead) + strlen(body) + 1 + strlen(tail) + 1;
   char *out = malloc(len);
   if (out) {
      snprintf(out, len, "%s%s%s%s%s", head, lead, body, newline ? "" : "\n", tail);
   }
   free(safe);
   return out;
}

int llm_history_attach_summary(struct json_object *history,
                               int from,
                               const char *summary,
                               const char *tag) {
   if (!json_object_is_type(history, json_type_array) || from < 0 || !summary) {
      return -1;
   }
   const int len = (int)json_object_array_length(history);
   int at = -1;
   for (int i = from; i < len && at < 0; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (llm_history_is_question(msg) && !llm_history_is_context(msg)) {
         at = i;
      }
   }
   char *text = llm_history_summary_text(summary, tag);
   struct json_object *part = text ? llm_history_context_part(text, MESSAGE_KIND_SUMMARY) : NULL;
   free(text);
   struct json_object *parts = part ? json_object_new_array() : NULL;
   if (!parts) {
      json_object_put(part);
      return -1;
   }
   json_object_array_add(parts, part);
   if (at < 0) {
      /* No question follows (the kept part is only replies): a message of its
       * own where the summarized part was, after the leading system messages. */
      int where = from;
      while (where < len && is_role(json_object_array_get_idx(history, where), "system")) {
         where++;
      }
      struct json_object *own = llm_history_context_message(parts);
      return llm_history_insert(history, (size_t)where, own) ? where : -1;
   }

   /* The summary first, then what the question had (its context, its words). */
   struct json_object *msg = json_object_array_get_idx(history, at);
   struct json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   if (json_object_is_type(content, json_type_array)) {
      const size_t n = json_object_array_length(content);
      for (size_t i = 0; i < n; i++) {
         json_object_array_add(parts, json_object_get(json_object_array_get_idx(content, i)));
      }
   } else {
      struct json_object *own = llm_history_context_part(json_object_get_string(content),
                                                         MESSAGE_KIND_NONE);
      if (!own) {
         json_object_put(parts);
         return -1;
      }
      json_object_array_add(parts, own);
   }
   json_object_object_add(msg, "content", parts);
   return at;
}
