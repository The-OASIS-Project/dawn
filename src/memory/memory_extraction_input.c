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
 * Memory extraction input: which of a session's messages are sent to the
 * extraction model, and the check that they really belong to the conversation
 * they are being extracted for.
 */

#include "memory/memory_extraction_input.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/automated_event.h"
#include "core/outside_kind.h"
#include "core/outside_markers.h"
#include "llm/llm_context_text.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_third_party.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_history_loader.h"
#include "memory/memory_note_guard.h"
#include "memory/memory_types.h"
#include "prompts.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* Return a message for the extraction transcript with inline base64 image data
 * replaced by a "[image]" text placeholder.  Vision turns store content as an OpenAI
 * multi-part array ({type:"image_url",image_url:{url:"data:image/...;base64,..."}});
 * serializing that verbatim into the extraction prompt injects ~250 KB of base64 per
 * image, overflowing the model context (observed: ~333K-token payload → HTTP 400) for
 * zero extraction value — extraction mines text, not pixels.
 *
 * Never mutates @p msg (it is a live reference into the session's conversation history):
 * builds a NEW object only when stripping is needed, copying top-level keys and sharing
 * immutable child refs.  Returns an owned (+1) reference in every case, so the caller
 * adds it to the array unconditionally. */
static struct json_object *extraction_message_strip_images(struct json_object *msg) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return json_object_get(msg); /* plain-string content — no images to strip */
   }

   int part_count = json_object_array_length(content);
   bool has_image = false;
   for (int i = 0; i < part_count; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      struct json_object *type_obj = NULL;
      if (part != NULL && json_object_object_get_ex(part, "type", &type_obj) &&
          strcmp(json_object_get_string(type_obj), "image_url") == 0) {
         has_image = true;
         break;
      }
   }
   if (!has_image) {
      return json_object_get(msg); /* multi-part but text-only — share untouched */
   }

   struct json_object *clean = json_object_new_object();
   if (clean == NULL) {
      return json_object_get(msg); /* OOM — fall back to original (oversized but valid) */
   }

   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") == 0) {
         struct json_object *clean_content = json_object_new_array();
         for (int i = 0; i < part_count; i++) {
            struct json_object *part = json_object_array_get_idx(content, i);
            struct json_object *type_obj = NULL;
            bool is_image = part != NULL && json_object_object_get_ex(part, "type", &type_obj) &&
                            strcmp(json_object_get_string(type_obj), "image_url") == 0;
            if (is_image) {
               struct json_object *placeholder = json_object_new_object();
               json_object_object_add(placeholder, "type", json_object_new_string("text"));
               json_object_object_add(placeholder, "text", json_object_new_string("[image]"));
               json_object_array_add(clean_content, placeholder);
            } else {
               json_object_array_add(clean_content, json_object_get(part));
            }
         }
         json_object_object_add(clean, "content", clean_content);
      } else {
         json_object_object_add(clean, key, json_object_get(val));
      }
   }
   return clean;
}

/* Per-message content budget for extraction input. A single oversized tool
 * result (observed: a runaway 705 KB graph-query dump) can push the whole
 * extraction payload past the model's context window and fail the request
 * (613K-token payload -> HTTP 400). 16 KB is generous for real conversational
 * content; tool results carry context, not user facts, so truncating them for
 * extraction is safe. */
#define MEMORY_EXTRACTION_MAX_MSG_BYTES 16384


/* The frame a tool result's content (a string, or text parts) was put in as
 * someone else's text (llm_third_party_frame), or NULL. */
static const char *content_frame(struct json_object *content) {
   if (json_object_is_type(content, json_type_string)) {
      return llm_third_party_present(json_object_get_string(content));
   }
   if (!json_object_is_type(content, json_type_array)) {
      return NULL;
   }
   for (size_t i = 0; i < json_object_array_length(content); i++) {
      struct json_object *text = NULL;
      const char *frame = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(content, i), "text", &text) &&
          (frame = llm_third_party_present(json_object_get_string(text))) != NULL) {
         return frame;
      }
   }
   return NULL;
}

static bool content_is_third_party(struct json_object *content) {
   return content_frame(content) != NULL;
}

/* The call @p id named, from the latest message before @p before that made
 * it (an OpenAI tool_calls entry or a Claude tool_use part): its tool's name
 * and its arguments (an object; OpenAI's are a JSON string, parsed into
 * *@p parsed, which the caller frees).  False when no message made it. */
static bool tool_call_find(struct json_object *history,
                           size_t before,
                           const char *id,
                           const char **name,
                           struct json_object **args,
                           struct json_object **parsed) {
   *name = NULL;
   *args = *parsed = NULL;
   for (size_t i = before; id && i-- > 0;) {
      struct json_object *msg = json_object_array_get_idx(history, i), *calls = NULL;
      if (!json_object_object_get_ex(msg, "tool_calls", &calls) &&
          !json_object_object_get_ex(msg, "content", &calls)) {
         continue;
      }
      for (size_t k = 0;
           json_object_is_type(calls, json_type_array) && k < json_object_array_length(calls);
           k++) {
         struct json_object *call = json_object_array_get_idx(calls, k), *call_id = NULL;
         struct json_object *v = NULL, *fn = NULL;
         if (!json_object_object_get_ex(call, "id", &call_id) ||
             strcmp(json_object_get_string(call_id), id) != 0) {
            continue;
         }
         if (json_object_object_get_ex(call, "function", &fn)) {
            if (json_object_object_get_ex(fn, "arguments", &v)) {
               *parsed = json_tokener_parse(json_object_get_string(v));
               *args = *parsed;
            }
            call = fn;
         } else if (json_object_object_get_ex(call, "input", &v)) {
            *args = v;
         }
         *name = json_object_object_get_ex(call, "name", &v) ? json_object_get_string(v) : NULL;
         return true;
      }
   }
   return false;
}

/* Whether call @p id's result is DAWN's own text: its tool (and action) says
 * so in the registry (tool_registry.h, results).  A call not found, or a tool
 * not registered, isn't. */
static bool result_is_own(struct json_object *history, size_t at, const char *id) {
   const char *name = NULL;
   struct json_object *args = NULL, *parsed = NULL;
   if (!tool_call_find(history, at, id, &name, &args, &parsed) || !name) {
      json_object_put(parsed);
      return false;
   }
   const tool_metadata_t *meta = tool_registry_find(name);
   const char *param = meta ? tool_registry_get_action_param_name(meta->name) : NULL;
   struct json_object *action = NULL;
   if (param && json_object_is_type(args, json_type_object)) {
      json_object_object_get_ex(args, param, &action);
   }
   const bool own = tool_results_own(meta, action ? json_object_get_string(action) : NULL);
   json_object_put(parsed);
   return own;
}

/* Whether @p content (a string or text parts) holds @p needle. */
static bool content_has(struct json_object *content, const char *needle) {
   if (json_object_is_type(content, json_type_string)) {
      return strstr(json_object_get_string(content), needle) != NULL;
   }
   for (size_t i = 0;
        json_object_is_type(content, json_type_array) && i < json_object_array_length(content);
        i++) {
      struct json_object *text = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(content, i), "text", &text) &&
          strstr(json_object_get_string(text), needle)) {
         return true;
      }
   }
   return false;
}

/* A tool result's outside kind: its frame's, when DAWN framed it; none for one
 * of DAWN's own tools, unless it is the memory store returning rows learned
 * from outside text (each listed with its kind word); any other tool's (or
 * one whose call isn't found) counts as outside text. */
static int result_outside(struct json_object *history,
                          size_t at,
                          const char *call_id,
                          struct json_object *content) {
   const char *frame = content_frame(content);
   if (frame) {
      return llm_third_party_outside_kind(frame);
   }
   if (!result_is_own(history, at, call_id) ||
       content_has(content, OUTSIDE_MARKER_KIND_SUFFIX ")") ||
       content_has(content, MEMORY_OUTSIDE_MARK)) {
      return OUTSIDE_OTHER;
   }
   return OUTSIDE_NONE;
}

/* The outside kind of the turn's items DAWN put in a turn context
 * (focus_incremental.c renders "[M3 source kind date] text", or without the
 * handle when an item has none): an item from an outside source, or a memory
 * learned from outside text, carries "outside" in its kind word.  A document's
 * chunk is a document; anything else OTHER. */
static int focus_items_outside(const char *text) {
   for (const char *line = text; line && *line;) {
      const char *end = strchr(line, '\n');
      const char *close = memchr(line, ']', end ? (size_t)(end - line) : strlen(line));
      if (*line == '[' && close) {
         const size_t n = (size_t)(close - line);
         /* The header after its '[', led by a space so its first word (the
          * source, when the line has no handle) is matched like the rest. */
         char head[160];
         const size_t len = n - 1 < sizeof(head) - 2 ? n - 1 : sizeof(head) - 2;
         snprintf(head, sizeof(head), " %.*s", (int)len, line + 1);
         if (strstr(head, " " OUTSIDE_MARKER_ITEM_KIND)) {
            return strstr(head, " " OUTSIDE_MARKER_DOC_SOURCE " ") ? OUTSIDE_DOCUMENT
                                                                   : OUTSIDE_OTHER;
         }
      }
      line = end ? end + 1 : NULL;
   }
   return OUTSIDE_NONE;
}

static bool text_has_document(struct json_object *text) {
   return json_object_is_type(text, json_type_string) &&
          strstr(json_object_get_string(text), LLM_CONTEXT_DOC_OPEN) != NULL;
}

/* A user message's text that DAWN wrote for the turn and that holds outside
 * text: a background job's result, a device's relayed data. */
static int dawn_input_outside(const char *text) {
   if (strncmp(text, AUTOMATED_EVENT_JOB_UPDATE, sizeof(AUTOMATED_EVENT_JOB_UPDATE) - 1) == 0) {
      return OUTSIDE_RESEARCH;
   }
   if (strncmp(text, MQTT_DEVICE_DATA_PREFIX, sizeof(MQTT_DEVICE_DATA_PREFIX) - 1) == 0) {
      return OUTSIDE_OTHER;
   }
   return OUTSIDE_NONE;
}

/* One part of a user message: a tool's result, a turn context's items, a
 * document attached to the text. */
static int part_outside(struct json_object *history, size_t at, struct json_object *part) {
   struct json_object *type = NULL, *inner = NULL, *text = NULL, *id = NULL;
   if (json_object_object_get_ex(part, "type", &type) &&
       strcmp(json_object_get_string(type), "tool_result") == 0 &&
       json_object_object_get_ex(part, "content", &inner)) {
      json_object_object_get_ex(part, "tool_use_id", &id);
      return result_outside(history, at, json_object_get_string(id), inner);
   }
   if (!json_object_object_get_ex(part, "text", &text)) {
      return OUTSIDE_NONE;
   }
   if (llm_history_kind_of(part) == MESSAGE_KIND_TURN_CONTEXT) {
      return focus_items_outside(json_object_get_string(text));
   }
   if (llm_history_is_context(part)) {
      return dawn_input_outside(json_object_get_string(text));
   }
   return text_has_document(text) ? OUTSIDE_DOCUMENT : OUTSIDE_NONE;
}

/* What outside text message @p at of @p history carries.  DAWN's own input
 * (a job's result, a turn context's items) is read too: it is where outside
 * text reaches a turn without a tool. */
static int message_outside(struct json_object *history, size_t at) {
   struct json_object *msg = json_object_array_get_idx(history, at);
   struct json_object *role = NULL, *content = NULL, *id = NULL;
   if (!json_object_object_get_ex(msg, "role", &role) ||
       !json_object_object_get_ex(msg, "content", &content)) {
      return OUTSIDE_NONE;
   }
   if (strcmp(json_object_get_string(role), "tool") == 0) {
      json_object_object_get_ex(msg, "tool_call_id", &id);
      return result_outside(history, at, json_object_get_string(id), content);
   }
   if (strcmp(json_object_get_string(role), "user") != 0) {
      return OUTSIDE_NONE;
   }
   if (json_object_is_type(content, json_type_string)) {
      const char *text = json_object_get_string(content);
      if (llm_history_kind_of(msg) == MESSAGE_KIND_TURN_CONTEXT) {
         return focus_items_outside(text);
      }
      const int dawn = dawn_input_outside(text);
      if (outside_is_tainted(dawn) || llm_history_is_context(msg)) {
         return dawn;
      }
      return text_has_document(content) ? OUTSIDE_DOCUMENT : OUTSIDE_NONE;
   }
   for (size_t i = 0;
        json_object_is_type(content, json_type_array) && i < json_object_array_length(content);
        i++) {
      const int kind = part_outside(history, at, json_object_array_get_idx(content, i));
      if (outside_is_tainted(kind)) {
         return kind;
      }
   }
   return OUTSIDE_NONE;
}

/* Whether message @p i of @p history is one a batch from @p last_msg_id reads
 * (id-less, or past the cursor). */
static bool in_batch(struct json_object *history, size_t i, int64_t last_msg_id) {
   struct json_object *id = NULL;
   return !json_object_object_get_ex(json_object_array_get_idx(history, i), "id", &id) ||
          json_object_get_int64(id) <= 0 || json_object_get_int64(id) > last_msg_id;
}

/* The first outside text in the rows of @p history a batch from
 * @p last_msg_id reads; what came before is the conversation's floor
 * (conversations.outside). */
static int batch_outside(struct json_object *history, int64_t last_msg_id) {
   for (size_t i = 0; i < json_object_array_length(history); i++) {
      const int kind = in_batch(history, i, last_msg_id) ? message_outside(history, i)
                                                         : OUTSIDE_NONE;
      if (outside_is_tainted(kind)) {
         return kind;
      }
   }
   return OUTSIDE_NONE;
}

static bool has_summary(struct json_object *obj) {
   return llm_history_kind_of(obj) == MESSAGE_KIND_SUMMARY;
}

/* Whether the batch's rows hold a compaction's summary: the rows it replaced
 * are gone from the history, unread. */
static bool batch_summarized(struct json_object *history, int64_t last_msg_id) {
   for (size_t i = 0; i < json_object_array_length(history); i++) {
      struct json_object *msg = json_object_array_get_idx(history, i), *content = NULL;
      if (!in_batch(history, i, last_msg_id)) {
         continue;
      }
      if (has_summary(msg)) {
         return true;
      }
      for (size_t k = 0;
           json_object_object_get_ex(msg, "content", &content) &&
           json_object_is_type(content, json_type_array) && k < json_object_array_length(content);
           k++) {
         if (has_summary(json_object_array_get_idx(content, k))) {
            return true;
         }
      }
   }
   return false;
}

/* What the batch's rows read, those a compaction removed included: read from
 * the conversation's stored rows, or, with none (a history never saved),
 * counted as outside text. */
static int history_outside(int user_id,
                           int64_t conversation_id,
                           struct json_object *history,
                           int64_t last_msg_id) {
   const int kind = batch_outside(history, last_msg_id);
   if (outside_is_tainted(kind) || !batch_summarized(history, last_msg_id)) {
      return kind;
   }
   struct json_object *stored = conversation_id > 0
                                    ? memory_history_load_from_db(conversation_id, user_id, NULL)
                                    : NULL;
   const int removed = stored ? batch_outside(stored, last_msg_id) : OUTSIDE_OTHER;
   json_object_put(stored);
   return outside_combine(kind, removed);
}

/* A copy of @p obj with its "content" replaced by @p content (taken). */
static struct json_object *with_content(struct json_object *obj, struct json_object *content) {
   struct json_object *copy = json_object_new_object();
   if (!copy) {
      json_object_put(content);
      return NULL;
   }
   json_object_object_foreach(obj, key, val) {
      if (strcmp(key, "content") != 0) {
         json_object_object_add(copy, key, json_object_get(val));
      }
   }
   json_object_object_add(copy, "content", content);
   return copy;
}

/* @p msg with every tool result DAWN framed as someone else's text (an email,
 * a page) reduced to a stub: an email the model read can't plant a "fact"
 * ("my accountant is now x@y") that a later request then acts on.  The model's
 * own words about it are kept.  Never mutates @p msg; returns an owned ref, or
 * NULL on allocation failure (the message is then left out, never the email
 * kept). */
static struct json_object *extraction_message_stub_third_party(struct json_object *msg) {
   struct json_object *role = NULL, *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_object_get_ex(msg, "role", &role)) {
      return json_object_get(msg);
   }
   /* OpenAI: a role:tool message. */
   if (strcmp(json_object_get_string(role), "tool") == 0) {
      if (!content_is_third_party(content)) {
         return json_object_get(msg);
      }
      return with_content(msg, json_object_new_string(MEMORY_EXTRACTION_THIRD_PARTY_STUB));
   }
   /* Claude: tool_result parts in a user message. */
   if (!json_object_is_type(content, json_type_array)) {
      return json_object_get(msg);
   }
   bool any = false;
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n && !any; i++) {
      struct json_object *part = json_object_array_get_idx(content, i), *type = NULL, *inner = NULL;
      any = json_object_object_get_ex(part, "type", &type) &&
            strcmp(json_object_get_string(type), "tool_result") == 0 &&
            json_object_object_get_ex(part, "content", &inner) && content_is_third_party(inner);
   }
   if (!any) {
      return json_object_get(msg);
   }
   struct json_object *parts = json_object_new_array();
   for (size_t i = 0; parts && i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i), *type = NULL, *inner = NULL;
      struct json_object *keep = json_object_get(part);
      if (json_object_object_get_ex(part, "type", &type) &&
          strcmp(json_object_get_string(type), "tool_result") == 0 &&
          json_object_object_get_ex(part, "content", &inner) && content_is_third_party(inner)) {
         json_object_put(keep);
         keep = with_content(part, json_object_new_string(MEMORY_EXTRACTION_THIRD_PARTY_STUB));
      }
      if (!keep || json_object_array_add(parts, keep) != 0) {
         json_object_put(keep);
         json_object_put(parts);
         parts = NULL;
      }
   }
   return parts ? with_content(msg, parts) : NULL;
}

/* Cap a single message's string content to MEMORY_EXTRACTION_MAX_MSG_BYTES.
 * Returns an owned ref: the original when within budget, else a copy with
 * UTF-8-safe-truncated content plus a "[... N bytes truncated]" marker. Array
 * (multimodal) content is not capped here — strip_images already replaces image
 * parts, and an oversized multipart *text* part is user-typed, not the runaway
 * tool-result case this guards (the observed failure class is plain strings). */
static struct json_object *extraction_message_cap_content(struct json_object *msg,
                                                          size_t max_bytes) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_string)) {
      return json_object_get(msg);
   }
   const char *text = json_object_get_string(content);
   size_t len = text != NULL ? strlen(text) : 0;
   if (len <= max_bytes) {
      return json_object_get(msg);
   }

   /* Copy a little past max_bytes so utf8_truncate can trim a split codepoint. */
   size_t copy_len = len < max_bytes + 4 ? len : max_bytes + 4;
   char *buf = malloc(copy_len + 1);
   if (buf == NULL) {
      return json_object_get(msg); /* OOM — keep original (oversized but valid) */
   }
   memcpy(buf, text, copy_len);
   buf[copy_len] = '\0';
   utf8_truncate(buf, max_bytes);

   char marker[96];
   snprintf(marker, sizeof(marker), "\n[... %zu bytes truncated for memory extraction]",
            len - strlen(buf));

   struct json_object *clean = json_object_new_object();
   if (clean == NULL) {
      free(buf);
      return json_object_get(msg);
   }
   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") == 0) {
         size_t need = strlen(buf) + strlen(marker) + 1;
         char *joined = malloc(need);
         if (joined != NULL) {
            snprintf(joined, need, "%s%s", buf, marker);
            json_object_object_add(clean, "content", json_object_new_string(joined));
            free(joined);
         } else {
            json_object_object_add(clean, "content", json_object_new_string(buf));
         }
      } else {
         json_object_object_add(clean, key, json_object_get(val));
      }
   }
   free(buf);
   return clean;
}

#ifdef ENABLE_AUTH /* the row-ownership check below */
static int cmp_int64(const void *a, const void *b) {
   int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
   return (x > y) - (x < y);
}

/* Number of distinct ids in a JSON array of int64 ids. */
static int distinct_row_id_count(struct json_object *ids) {
   size_t n = json_object_array_length(ids);
   if (n == 0) {
      return 0;
   }
   int64_t *v = malloc(n * sizeof(*v));
   if (!v) {
      return (int)n; /* can't dedupe: assume distinct (a shortfall then fails closed) */
   }
   for (size_t i = 0; i < n; i++) {
      v[i] = json_object_get_int64(json_object_array_get_idx(ids, i));
   }
   qsort(v, n, sizeof(*v), cmp_int64);
   int distinct = 1;
   for (size_t i = 1; i < n; i++) {
      if (v[i] != v[i - 1]) {
         distinct++;
      }
   }
   free(v);
   return distinct;
}
#endif /* ENABLE_AUTH */

int64_t memory_extraction_history_max_id(struct json_object *history) {
   int64_t max_id = 0;
   for (size_t i = 0; history && i < json_object_array_length(history); i++) {
      struct json_object *id_obj = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(history, i), "id", &id_obj)) {
         const int64_t id = json_object_get_int64(id_obj);
         if (id > max_id)
            max_id = id;
      }
   }
   return max_id;
}

int64_t memory_extraction_history_first_id(struct json_object *history) {
   int64_t first = 0;
   for (size_t i = 0; history && i < json_object_array_length(history); i++) {
      struct json_object *id_obj = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(history, i), "id", &id_obj)) {
         const int64_t id = json_object_get_int64(id_obj);
         if (id > 0 && (first == 0 || id < first))
            first = id;
      }
   }
   return first;
}

bool memory_extraction_input_has_question(struct json_object *input) {
   for (size_t i = 0; input && i < json_object_array_length(input); i++) {
      if (llm_history_is_question(json_object_array_get_idx(input, i))) {
         return true;
      }
   }
   return false;
}

/* @p filtered without the rows DAWN wrote on its own (message_unprompted: a
 * briefing or message delivered to a channel, a completion note): DAWN's
 * words, never the user's.  Posts reach a history only from the database, so
 * they always carry their row id.  Takes @p filtered; NULL when the lookup
 * fails (the caller then extracts nothing, and recovery retries). */
static struct json_object *drop_unprompted(int user_id,
                                           struct json_object *row_ids,
                                           struct json_object *filtered,
                                           bool *dropped) {
   const int n = (int)json_object_array_length(row_ids);
   int64_t *unprompted = n > 0 ? malloc((size_t)n * sizeof(*unprompted)) : NULL;
   int count = 0;
   if (n > 0 &&
       (!unprompted || conv_db_messages_unprompted(
                           user_id, json_object_to_json_string_ext(row_ids, JSON_C_TO_STRING_PLAIN),
                           unprompted, n, &count) != AUTH_DB_SUCCESS)) {
      free(unprompted);
      json_object_put(filtered);
      return NULL;
   }
   *dropped = count > 0;
   if (count == 0) {
      free(unprompted);
      return filtered;
   }
   struct json_object *kept = json_object_new_array();
   for (size_t i = 0; kept && i < json_object_array_length(filtered); i++) {
      struct json_object *msg = json_object_array_get_idx(filtered, i);
      struct json_object *id_obj = NULL;
      const int64_t id = json_object_object_get_ex(msg, "id", &id_obj)
                             ? json_object_get_int64(id_obj)
                             : 0;
      bool is_post = false;
      for (int k = 0; id > 0 && k < count && !is_post; k++) {
         is_post = unprompted[k] == id;
      }
      if (!is_post) {
         json_object_array_add(kept, json_object_get(msg));
      }
   }
   free(unprompted);
   json_object_put(filtered);
   return kept;
}

int memory_extraction_history_outside(int user_id,
                                      int64_t conversation_id,
                                      struct json_object *history,
                                      int64_t last_msg_id) {
   const int live = history_outside(user_id, conversation_id, history, last_msg_id);
   /* What DAWN wrote into the stored rows past the cursor, as far as this
    * history reaches: a history loaded from the database has no request
    * context, and a compaction's rows aren't in the live one.  Unread (a
    * failed query): outside text. */
   int stored = OUTSIDE_NONE;
   if (conversation_id > 0 && conv_db_rows_outside(conversation_id, user_id, last_msg_id,
                                                   memory_extraction_history_max_id(history),
                                                   &stored) != AUTH_DB_SUCCESS) {
      stored = OUTSIDE_OTHER;
   }
   return outside_combine(live, stored);
}

struct json_object *memory_extraction_build_input(int user_id,
                                                  int64_t conversation_id,
                                                  struct json_object *conversation_history,
                                                  int64_t last_msg_id,
                                                  int *outside_out) {
   /* Read with DAWN's request context (what it put in front of the model),
    * then extracted without it. */
   *outside_out = memory_extraction_history_outside(user_id, conversation_id, conversation_history,
                                                    last_msg_id);
   struct json_object *history = llm_history_strip_internal(conversation_history);
   if (!history) {
      return NULL;
   }

   /* Note-extraction guard: collect note-filed bodies + document_read echoes
    * from the FULL history so they can be redacted out of the extraction input
    * (keeps reference text the user filed as a note out of semantic memory).
    * NULL when disabled or nothing was filed → redact is a pass-through. */
   memory_note_guard_t *note_guard = memory_note_guard_create(conversation_history);

   /* ID-based filter: include messages where id > last_msg_id.
    * Messages with missing or zero id (compaction markers, a turn's intermediate
    * tool-call entries, satellite sessions that persist nothing) are included
    * unconditionally.  System messages are always skipped by the role check. */
   size_t arr_len = json_object_array_length(history);
   struct json_object *filtered = json_object_new_array();
   struct json_object *row_ids = json_object_new_array(); /* DB ids of the rows sent */
   int unverified_user = 0;                               /* user messages with no row id */

   for (size_t i = 0; i < arr_len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj, *id_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), "system") == 0)
         continue;
      /* Request context (a directive, an envelope, a loop note) is DAWN's own,
       * not something the conversation said. */
      if (llm_history_is_context(msg))
         continue;
      /* A job-result envelope is DAWN's own injected event, not the user speaking. */
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_is_type(content_obj, json_type_string) &&
          strncmp(json_object_get_string(content_obj), AUTOMATED_EVENT_JOB_UPDATE,
                  sizeof(AUTOMATED_EVENT_JOB_UPDATE) - 1) == 0) {
         continue;
      }
      int64_t msg_id = 0;
      if (json_object_object_get_ex(msg, "id", &id_obj))
         msg_id = json_object_get_int64(id_obj);
      if (msg_id == 0 || msg_id > last_msg_id) {
         if (msg_id > 0 && row_ids) {
            json_object_array_add(row_ids, json_object_new_int64(msg_id));
         } else if (msg_id <= 0 && llm_history_is_question(msg)) {
            /* A question the user asked; a tool batch's results (a Claude
             * user message of tool_result parts) are the tool's, like an
             * OpenAI role:tool message, and carry no id of their own. */
            unverified_user++;
         }
         /* third-party stubs, strip_images, then guard-redact: each returns an
          * owned ref and never mutates the live history; chaining yields one
          * redacted copy (or a shared ref when no stage changes anything). */
         struct json_object *unframed = extraction_message_stub_third_party(msg);
         if (!unframed) {
            continue;
         }
         struct json_object *stripped = extraction_message_strip_images(unframed);
         json_object_put(unframed);
         struct json_object *guarded = memory_note_guard_redact(note_guard, stripped);
         json_object_put(stripped);
         struct json_object *capped = extraction_message_cap_content(
             guarded, MEMORY_EXTRACTION_MAX_MSG_BYTES);
         json_object_put(guarded);
         json_object_array_add(filtered, capped);
      }
   }

   memory_note_guard_free(note_guard);
   json_object_put(history);

   /* Verify from the rows themselves that what is about to be extracted belongs
    * to conversation_id and to no private or background-job conversation.  The
    * caller's conversation_id comes from session bookkeeping; this catches any
    * way that bookkeeping and the history's contents disagree.  Fail closed:
    * memory_recovery re-extracts eligible conversations from the DB. */
   bool rows_ok = (row_ids != NULL); /* can't verify without the id list: fail closed */
   if (!rows_ok) {
      OLOG_ERROR("memory_extraction: refusing conv %lld for user %d: out of memory building "
                 "row list",
                 (long long)conversation_id, user_id);
   } else if (conversation_id > 0 && unverified_user > 0) {
      /* Every question of a DB conversation is persisted and stamped before the
       * model sees it, so one without a row id can't be attributed. */
      OLOG_WARNING("memory_extraction: refusing conv %lld for user %d: %d user message(s) "
                   "carry no row id",
                   (long long)conversation_id, user_id, unverified_user);
      rows_ok = false;
   }
#ifdef ENABLE_AUTH
   if (row_ids && json_object_array_length(row_ids) > 0) {
      /* The lookup matches each distinct id once, so compare against the
       * distinct count (a repeated id must not read as a missing row). */
      int n_ids = distinct_row_id_count(row_ids);
      conv_msg_ownership_t own;
      int own_rc = conv_db_messages_ownership(
          user_id, json_object_to_json_string_ext(row_ids, JSON_C_TO_STRING_PLAIN), &own);
      const char *why = NULL;
      if (own_rc != AUTH_DB_SUCCESS) {
         why = "row lookup failed";
      } else if (own.matched < n_ids) {
         why = "some rows are missing or not this user's";
      } else if (own.any_private) {
         why = "rows from a private conversation";
      } else if (own.any_job) {
         why = "rows from a background-job conversation";
      } else if (own.distinct_convs > 1) {
         why = "rows span several conversations";
      } else if (own.conv_id != conversation_id) {
         why = "rows belong to a different conversation";
      }
      if (why) {
         OLOG_WARNING("memory_extraction: refusing conv %lld for user %d: %s (%d rows, "
                      "resolved conv %lld)",
                      (long long)conversation_id, user_id, why, n_ids, (long long)own.conv_id);
         rows_ok = false;
      }
   }
#endif
   /* In every build: DAWN's posts are in the database whether or not this
    * build checks row ownership (the bench re-extracts without the WebUI). */
   if (rows_ok && row_ids) {
      bool posts = false;
      filtered = drop_unprompted(user_id, row_ids, filtered, &posts);
      /* DAWN's posts (a briefing, a delivered digest) retell outside content
       * the user may answer. */
      if (posts && !outside_is_tainted(*outside_out)) {
         *outside_out = OUTSIDE_OTHER;
      }
      if (!filtered) {
         OLOG_WARNING("memory_extraction: refusing conv %lld for user %d: unprompted-row lookup "
                      "failed",
                      (long long)conversation_id, user_id);
         json_object_put(row_ids);
         return NULL;
      }
   }
   json_object_put(row_ids);
   if (!rows_ok) {
      json_object_put(filtered);
      return NULL;
   }
   return filtered;
}
