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
#include "llm/llm_history_kind.h"
#include "llm/llm_third_party.h"
#include "logging.h"
#include "memory/memory_note_guard.h"
#include "prompts.h"
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


/* Whether a tool result's content (a string, or text parts) was framed as
 * someone else's (llm_third_party_frame). */
static bool content_is_third_party(struct json_object *content) {
   if (json_object_is_type(content, json_type_string)) {
      return llm_third_party_present(json_object_get_string(content)) != NULL;
   }
   if (!json_object_is_type(content, json_type_array)) {
      return false;
   }
   for (size_t i = 0; i < json_object_array_length(content); i++) {
      struct json_object *text = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(content, i), "text", &text) &&
          llm_third_party_present(json_object_get_string(text))) {
         return true;
      }
   }
   return false;
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

struct json_object *memory_extraction_build_input(int user_id,
                                                  int64_t conversation_id,
                                                  struct json_object *conversation_history,
                                                  int64_t last_msg_id) {
   /* Note-extraction guard: collect note-filed bodies + document_read echoes
    * from the FULL history so they can be redacted out of the extraction input
    * (keeps reference text the user filed as a note out of semantic memory).
    * NULL when disabled or nothing was filed → redact is a pass-through. */
   memory_note_guard_t *note_guard = memory_note_guard_create(conversation_history);

   /* ID-based filter: include messages where id > last_msg_id.
    * Messages with missing or zero id (compaction markers, a turn's intermediate
    * tool-call entries, satellite sessions that persist nothing) are included
    * unconditionally.  System messages are always skipped by the role check. */
   size_t arr_len = json_object_array_length(conversation_history);
   struct json_object *filtered = json_object_new_array();
   struct json_object *row_ids = json_object_new_array(); /* DB ids of the rows sent */
   int unverified_user = 0;                               /* user messages with no row id */

   for (size_t i = 0; i < arr_len; i++) {
      struct json_object *msg = json_object_array_get_idx(conversation_history, i);
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
   json_object_put(row_ids);
   if (!rows_ok) {
      json_object_put(filtered);
      return NULL;
   }
   return filtered;
}
