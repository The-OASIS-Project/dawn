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
 * Conversation history loader implementation.
 */

#include "memory/memory_history_loader.h"

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "core/image_rehydrate.h"
#include "core/prefix_message.h"
#include "dawn_error.h"
#include "llm/llm_compaction_range.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

typedef struct {
   struct json_object *array;
   size_t total_text_len;
} history_build_ctx_t;

char *memory_history_strip_image_markers(const char *src) {
   if (!src) {
      return strdup("");
   }
   size_t in_len = strlen(src);
   /* Worst case (no markers) keeps the source byte-for-byte. */
   char *dst = malloc(in_len + 1);
   if (!dst) {
      return NULL;
   }
   const char *p = src;
   const char *end = src + in_len;
   char *q = dst;
   /* Skip to the next '[' with memchr rather than testing strncmp at every byte.
    * A marker can only start at a '[', so the 7-byte compare runs at candidates
    * instead of ~once per character — and memchr is vectorized in ARM64 glibc
    * while the byte loop is not.  Measured 7x on a real job transcript (239 KB:
    * 1070us -> 154us), which matters because this runs INSIDE the callback that
    * conv_db_get_messages holds the global auth_db mutex across.  Six callers
    * share it, two of them hotter than the resume path this was found on. */
   for (;;) {
      const char *br = memchr(p, '[', (size_t)(end - p));
      if (!br) {
         memcpy(q, p, (size_t)(end - p));
         q += (end - p);
         break;
      }
      memcpy(q, p, (size_t)(br - p));
      q += (br - p);
      p = br;
      if (strncmp(p, "[IMAGE:", 7) == 0) {
         const char *close = strchr(p + 7, ']');
         if (close) {
            memcpy(q, "[image]", 7);
            q += 7;
            p = close + 1;
            continue;
         }
         /* Unterminated marker — copy the rest verbatim and stop. */
      }
      *q++ = *p++;
   }
   *q = '\0';
   return dst;
}

static int append_message_to_history(const conversation_llm_row_t *msg, void *ctx_ptr) {
   history_build_ctx_t *ctx = (history_build_ctx_t *)ctx_ptr;
   if (!ctx || !ctx->array || !msg) {
      return 0;
   }

   char *stripped = memory_history_strip_image_markers(msg->content);
   if (!stripped) {
      OLOG_WARNING("memory_history_loader: OOM stripping message content; aborting iteration");
      return 1;
   }

   struct json_object *entry = json_object_new_object();
   struct json_object *role = json_object_new_string(msg->role);
   struct json_object *content = json_object_new_string(stripped);
   if (!entry || !role || !content) {
      OLOG_WARNING("memory_history_loader: OOM building history; aborting iteration");
      if (entry)
         json_object_put(entry);
      if (role)
         json_object_put(role);
      if (content)
         json_object_put(content);
      free(stripped);
      return 1;
   }
   ctx->total_text_len += strlen(stripped);
   free(stripped);

   json_object_object_add(entry, "role", role);
   json_object_object_add(entry, "content", content);
   json_object_object_add(entry, "id", json_object_new_int64(msg->id));
   /* Carry the structured tool fields (mirrors webui_session_restore_msg_cb) so a
    * reloaded assistant tool-call turn and its role:tool results rebuild as
    * OpenAI-canonical tool messages for the LLM, instead of being orphaned and
    * degraded to a "[Previous tool result: …]" summary.  That summary path is
    * both lossy (a resumed job could no longer tell which tool calls already ran)
    * and a crash vector — its byte-truncation split multi-byte characters until
    * sanitized (3e473be).  Fixing it at the source keeps the transcript intact. */
   if (msg->tool_calls && msg->tool_calls[0]) {
      struct json_object *tc = json_tokener_parse(msg->tool_calls);
      if (tc) {
         json_object_object_add(entry, "tool_calls", tc);
      }
   }
   if (msg->tool_call_id && msg->tool_call_id[0]) {
      json_object_object_add(entry, "tool_call_id", json_object_new_string(msg->tool_call_id));
   }
   json_object_array_add(ctx->array, entry);
   return 0;
}

/* The loader's wrap of a caller's row callback: the stored blocks stay here. */
typedef struct {
   memory_history_row_cb cb;
   void *ctx;
   struct json_object *rows;
   bool with_blocks;
} load_ctx_t;

/* Key a message carries its row's stored text under between the read and the
 * parse; never seen outside this file. */
#define BLOCKS_RAW_KEY "_blocks_raw"

static int on_llm_row(const conversation_llm_row_t *row, void *p) {
   load_ctx_t *lc = p;
   conversation_llm_row_t shown = *row;
   shown.llm_blocks = NULL;
   shown.llm_blocks_len = 0;
   const size_t before = json_object_array_length(lc->rows);
   const int stop = lc->cb(&shown, lc->ctx);
   /* A request-context row keeps its kind (llm_history_kind.h), and the
    * question it was saved with. */
   if (row->kind && json_object_array_length(lc->rows) == before + 1) {
      struct json_object *msg = json_object_array_get_idx(lc->rows, before);
      llm_history_set_kind(msg, message_kind_parse(row->kind));
      if (row->context_of > 0) {
         json_object_object_add(msg, LLM_HISTORY_CONTEXT_OF_KEY,
                                json_object_new_int64(row->context_of));
      }
   }
   /* Copied now (the row's text is gone after the callback) and parsed after
    * the read, outside the database lock. */
   if (lc->with_blocks && row->llm_blocks && json_object_array_length(lc->rows) == before + 1) {
      struct json_object *raw = json_object_new_string_len(row->llm_blocks,
                                                           (int)row->llm_blocks_len);
      if (raw) {
         json_object_object_add(json_object_array_get_idx(lc->rows, before), BLOCKS_RAW_KEY, raw);
      }
   }
   return stop;
}

/* A display row as the loader's row type (no blocks). */
static int on_message(const conversation_message_t *msg, void *p) {
   conversation_llm_row_t row = { .id = msg->id,
                                  .content = msg->content,
                                  .tool_calls = msg->tool_calls,
                                  .tool_call_id = msg->tool_call_id,
                                  .created_at = msg->created_at,
                                  .is_error = msg->is_error };
   memcpy(row.role, msg->role, sizeof(row.role));
   return on_llm_row(&row, p);
}

static int64_t message_id(struct json_object *msg) {
   struct json_object *id = NULL;
   return json_object_object_get_ex(msg, "id", &id) ? json_object_get_int64(id) : 0;
}

/* Replace each message's stored text with its blocks, when they parse and
 * record the message's tool calls. */
static void attach_blocks(struct json_object *rows, int from) {
   const int n = (int)json_object_array_length(rows);
   for (int i = from; i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(rows, i);
      struct json_object *raw = NULL;
      if (!json_object_object_get_ex(msg, BLOCKS_RAW_KEY, &raw)) {
         continue;
      }
      struct json_object *blocks = llm_turn_blocks_from_stored(
          json_object_get_string(raw), (size_t)json_object_get_string_len(raw),
          (long long)message_id(msg));
      struct json_object *calls = NULL;
      json_object_object_get_ex(msg, "tool_calls", &calls);
      if (blocks && llm_turn_blocks_calls_match(blocks, calls)) {
         json_object_object_add(msg, LLM_TURN_BLOCKS_KEY, blocks);
      } else {
         if (blocks) {
            OLOG_WARNING("memory_history_loader: row %lld's blocks don't match its tool calls; "
                         "loaded without them",
                         (long long)message_id(msg));
         }
         json_object_put(blocks);
      }
      json_object_object_del(msg, BLOCKS_RAW_KEY);
   }
}

int memory_history_load_rows(int64_t conv_id,
                             int user_id,
                             int64_t watermark,
                             bool with_blocks,
                             memory_history_row_cb cb,
                             void *ctx,
                             struct json_object *rows,
                             size_t *chars_out) {
   if (chars_out) {
      *chars_out = 0;
   }
   if (!cb || !rows) {
      return AUTH_DB_INVALID;
   }
   const int from = (int)json_object_array_length(rows);
   load_ctx_t lc = { .cb = cb, .ctx = ctx, .rows = rows, .with_blocks = with_blocks };
   int rc;
   if (with_blocks) {
      rc = conv_db_get_messages_for_llm(conv_id, user_id, watermark > 0 ? watermark : 0, on_llm_row,
                                        &lc);
   } else {
      rc = (watermark > 0)
               ? conv_db_get_messages_after(conv_id, user_id, watermark, on_message, &lc)
               : conv_db_get_messages(conv_id, user_id, on_message, &lc);
   }
   if (with_blocks) {
      attach_blocks(rows, from);
   }
   if (rc == AUTH_DB_SUCCESS && watermark > 0) {
      /* A point recorded inside a tool exchange leaves results whose call is
       * in the summary: drop them, on every load. */
      (void)llm_history_drop_leading_results(rows, from, chars_out);
   }
   return rc;
}

/* ------------------------------------------------------------------------
 * A model's request, rebuilt: the one builder every load that continues a
 * conversation goes through, so each rebuilds the bytes its turns were sent.
 * ------------------------------------------------------------------------ */

/* Skip leading ASCII whitespace; returns the first non-whitespace char. */
static const char *skip_ws(const char *s) {
   while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
      s++;
   }
   return s;
}

/* If @p s begins with "<dawn:" then (optional whitespace) @p kw, return a
 * pointer just past @p kw; else NULL.  Whitespace-tolerant so a malformed
 * imitated marker ("<dawn: reasoning") still matches. */
static const char *match_dawn_open(const char *s, const char *kw) {
   static const char prefix[] = "<dawn:";
   if (strncmp(s, prefix, sizeof(prefix) - 1) != 0) {
      return NULL;
   }
   const char *p = skip_ws(s + sizeof(prefix) - 1);
   size_t klen = strlen(kw);
   return (strncmp(p, kw, klen) == 0) ? p + klen : NULL;
}

/* Just past the next "</dawn:" (ws?) "thinking" (ws?) ">", or NULL. */
static const char *find_dawn_close_thinking(const char *s) {
   for (const char *c = strstr(s, "</dawn:"); c; c = strstr(c + 1, "</dawn:")) {
      const char *p = skip_ws(c + 7); /* strlen("</dawn:") */
      if (strncmp(p, "thinking", 8) != 0) {
         continue;
      }
      p = skip_ws(p + 8);
      if (*p == '>') {
         return p + 1;
      }
   }
   return NULL;
}

/*
 * Strip ONLY leading legacy display markers from assistant content before it
 * enters a model's request.  An older client prepended "<dawn:reasoning .../>"
 * and "<dawn:thinking ...>...</dawn:thinking>" blocks to saved assistant
 * content; they are a DISPLAY artifact and must never reach a model — one
 * restored onto such a conversation imitates the marker format in its own
 * output.  Only the leading block(s), assistant messages only: a mid-message
 * mention of these tags (discussing DAWN's code) is left alone.  A new copy, or
 * NULL when nothing was stripped.
 */
static char *strip_leading_dawn_markers(const char *content) {
   if (!content) {
      return NULL;
   }
   const char *p = content;
   for (;;) {
      const char *q = skip_ws(p);
      if (match_dawn_open(q, "reasoning")) {
         const char *gt = strchr(q, '>');
         if (!gt) {
            break; /* malformed/unterminated: keep the remainder intact */
         }
         p = gt + 1;
         continue;
      }
      if (match_dawn_open(q, "thinking")) {
         const char *close = find_dawn_close_thinking(q);
         if (!close) {
            break; /* unterminated block: don't eat the real answer */
         }
         p = close;
         continue;
      }
      break;
   }
   if (p == content) {
      return NULL;
   }
   return strdup(skip_ws(p)); /* trim the blank line before the real answer */
}

/* Key a staged tool row holds its images under until its message is built;
 * never seen outside this file. */
#define STAGED_IMAGES_KEY "_images_staged"

/* A row as read, under the database lock: copied only (no file reads). */
static int stage_row(const conversation_llm_row_t *row, void *ctx) {
   struct json_object *arr = ctx;
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return 1;
   }
   json_object_object_add(obj, "id", json_object_new_int64(row->id));
   json_object_object_add(obj, "role", json_object_new_string(row->role));
   json_object_object_add(obj, "content", json_object_new_string(row->content ? row->content : ""));
   if (row->tool_calls && row->tool_calls[0]) {
      struct json_object *tc = json_tokener_parse(row->tool_calls);
      if (tc) {
         json_object_object_add(obj, "tool_calls", tc);
      }
   }
   if (row->tool_call_id && row->tool_call_id[0]) {
      json_object_object_add(obj, "tool_call_id", json_object_new_string(row->tool_call_id));
   }
   if (row->images && strcmp(row->role, "tool") == 0) {
      struct json_object *images = json_tokener_parse(row->images);
      if (json_object_is_type(images, json_type_array)) {
         json_object_object_add(obj, STAGED_IMAGES_KEY, images);
      } else {
         json_object_put(images);
      }
   }
   json_object_array_add(arr, obj);
   return 0;
}

/* A tool row's content: its text, then the images its result carried, from
 * the store as the live turn built them (image_rehydrate_parts: the user's
 * captures only, under the rehydrate ceilings).  Its text alone when it has
 * none, or on out of memory. */
static struct json_object *tool_content(int user_id, const char *text, struct json_object *images) {
   char ids[IMAGE_REHYDRATE_MAX_IMAGES][IMAGE_ID_LEN];
   int n = 0;
   const size_t len = images ? json_object_array_length(images) : 0;
   for (size_t i = 0; i < len && n < IMAGE_REHYDRATE_MAX_IMAGES; i++) {
      const char *id = json_object_get_string(json_object_array_get_idx(images, i));
      if (id && strlen(id) == IMAGE_ID_LEN - 1) {
         memcpy(ids[n++], id, IMAGE_ID_LEN);
      }
   }
   struct json_object *parts = n > 0 ? image_rehydrate_parts(user_id,
                                                             (const char(*)[IMAGE_ID_LEN])ids, n,
                                                             IMAGE_SOURCE_CAPTURE)
                                     : NULL;
   struct json_object *content = parts ? json_object_new_array() : NULL;
   struct json_object *text_part = content ? json_object_new_object() : NULL;
   if (!text_part) {
      json_object_put(content);
      json_object_put(parts);
      return json_object_new_string(text);
   }
   json_object_object_add(text_part, "type", json_object_new_string("text"));
   json_object_object_add(text_part, "text", json_object_new_string(text));
   json_object_array_add(content, text_part);
   const size_t np = json_object_array_length(parts);
   for (size_t i = 0; i < np; i++) {
      json_object_array_add(content, json_object_get(json_object_array_get_idx(parts, i)));
   }
   json_object_put(parts);
   return content;
}

/* A staged row as the message a model's request carries: a turn's images
 * back in its question (owner-checked), a tool result's images back in its
 * result (owner-checked captures, named by the row's images only: a marker
 * in a result or a reply is text), request context as the text it was sent,
 * tool fields and the turn's blocks as they were. */
static struct json_object *request_message(int user_id, struct json_object *row) {
   const char *role = json_object_get_string(json_object_object_get(row, "role"));
   const char *content = json_object_get_string(json_object_object_get(row, "content"));
   char *stripped = (role && strcmp(role, "assistant") == 0) ? strip_leading_dawn_markers(content)
                                                             : NULL;
   if (stripped) {
      content = stripped;
   }
   struct json_object *tc = NULL;
   struct json_object *tcid = NULL;
   const bool has_tc = json_object_object_get_ex(row, "tool_calls", &tc);
   const bool has_tcid = json_object_object_get_ex(row, "tool_call_id", &tcid);
   const message_kind_t kind = llm_history_kind_of(row);
   struct json_object *m = NULL;
   if (has_tc || has_tcid || kind != MESSAGE_KIND_NONE) {
      /* Tool fields as stored; request context as text (a marker quoted in it
       * is no image of this turn's). */
      m = json_object_new_object();
      if (m) {
         struct json_object *images = NULL;
         json_object_object_get_ex(row, STAGED_IMAGES_KEY, &images);
         json_object_object_add(m, "role", json_object_new_string(role ? role : "user"));
         json_object_object_add(m, "content",
                                has_tcid && images
                                    ? tool_content(user_id, content ? content : "", images)
                                    : json_object_new_string(content ? content : ""));
         if (has_tc) {
            json_object_object_add(m, "tool_calls", json_object_get(tc));
         }
         if (has_tcid) {
            json_object_object_add(m, "tool_call_id", json_object_get(tcid));
         }
      }
   } else if (role && strcmp(role, "user") == 0) {
      m = image_rehydrate_message(user_id, role, content ? content : "");
   } else {
      /* A reply's markers stay text: a model's reply can quote any id. */
      m = json_object_new_object();
      if (m) {
         json_object_object_add(m, "role", json_object_new_string(role ? role : "user"));
         json_object_object_add(m, "content", json_object_new_string(content ? content : ""));
      }
   }
   free(stripped);
   if (!m) {
      return NULL;
   }
   llm_history_set_kind(m, kind);
   /* A tool change's definitions, validated once, here (every request reads
    * them as loaded). */
   if (kind == MESSAGE_KIND_TOOL_CHANGE && !llm_tool_change_normalize(m)) {
      OLOG_WARNING("memory_history_loader: a tool change row holds no usable definition");
   }
   static const char *const carried[] = { "id", LLM_HISTORY_CONTEXT_OF_KEY, LLM_TURN_BLOCKS_KEY };
   for (size_t i = 0; i < sizeof(carried) / sizeof(carried[0]); i++) {
      struct json_object *v = NULL;
      if (json_object_object_get_ex(row, carried[i], &v)) {
         json_object_object_add(m, carried[i], json_object_get(v));
      }
   }
   return m;
}

/* Whether @p msg is a system message of no kind (one saved before prompts were
 * frozen, or a placeholder). */
static bool plain_system(struct json_object *msg) {
   struct json_object *role = NULL;
   return json_object_object_get_ex(msg, "role", &role) &&
          strcmp(json_object_get_string(role), "system") == 0 &&
          llm_history_kind_of(msg) == MESSAGE_KIND_NONE;
}

struct json_object *memory_history_request_context(int64_t conv_id,
                                                   int user_id,
                                                   int64_t watermark,
                                                   const char *compaction_summary,
                                                   size_t *text_len_out,
                                                   int *rows_out) {
   if (text_len_out) {
      *text_len_out = 0;
   }
   if (rows_out) {
      *rows_out = 0;
   }
   struct json_object *staged = json_object_new_array();
   if (!staged) {
      return NULL;
   }
   if (memory_history_load_rows(conv_id, user_id, watermark > 0 ? watermark : 0, true, stage_row,
                                staged, staged, NULL) != AUTH_DB_SUCCESS) {
      json_object_put(staged);
      return NULL;
   }
   struct json_object *hist = json_object_new_array();
   if (!hist) {
      json_object_put(staged);
      return NULL;
   }
   const int n = (int)json_object_array_length(staged);
   size_t text_len = 0;
   int i = 0;
   /* A system message the conversation was saved with leads. */
   if (n > 0 && plain_system(json_object_array_get_idx(staged, 0))) {
      struct json_object *m = request_message(user_id, json_object_array_get_idx(staged, 0));
      if (m) {
         json_object_array_add(hist, m);
      }
      i = 1;
   }
   for (; i < n; i++) {
      struct json_object *row = json_object_array_get_idx(staged, i);
      const char *content = json_object_get_string(json_object_object_get(row, "content"));
      text_len += content ? strlen(content) : 0;
      struct json_object *m = request_message(user_id, row);
      if (m) {
         json_object_array_add(hist, m);
      }
   }
   json_object_put(staged);

   /* Each turn's context goes back into its question, as it was sent, and the
    * conversation's frozen prompt leads it. */
   if (llm_history_fold_context(hist, 0) != 0) {
      OLOG_WARNING("memory_history_loader: conv %lld: turn context left unfolded (out of memory)",
                   (long long)conv_id);
   }
   struct json_object *prefix = prefix_message_stored(conv_id, user_id);
   if (prefix) {
      (void)prefix_message_install(hist, prefix);
   }
   /* The compaction's summary, in front of the first kept question, rendered
    * as the live compaction sent it (llm_history_attach_summary). */
   if (watermark > 0 && compaction_summary && compaction_summary[0]) {
      if (llm_history_attach_summary(hist, 0, compaction_summary, llm_history_tag(hist)) >= 0) {
         text_len += strlen(compaction_summary);
      } else {
         OLOG_WARNING("memory_history_loader: conv %lld: compaction summary left out (out of "
                      "memory)",
                      (long long)conv_id);
      }
   }
   if (text_len_out) {
      *text_len_out = text_len;
   }
   if (rows_out) {
      *rows_out = n;
   }
   return hist;
}

static struct json_object *load_history(int64_t conv_id, int user_id, size_t *text_len_out) {
   history_build_ctx_t ctx = { .array = json_object_new_array(), .total_text_len = 0 };
   if (!ctx.array) {
      return NULL;
   }

   /* v67: if the conversation carries a compaction watermark, bound the reload to
    * post-watermark messages and prepend the summary — mirrors the WebUI restore
    * funnel (webui_restore_conversation_context) so any loader, including the
    * messaging forever-conversation path, stays context-bounded.  watermark == 0
    * (never compacted) keeps the original full-history behavior. */
   int64_t watermark = 0;
   char *summary = NULL;
   conversation_t conv = { 0 };
   if (conv_db_get(conv_id, user_id, &conv) == AUTH_DB_SUCCESS) {
      watermark = conv.context_watermark_msg_id;
      if (watermark > 0 && conv.compaction_summary && conv.compaction_summary[0]) {
         summary = strdup(conv.compaction_summary);
      }
   }
   conv_free(&conv);

   size_t dropped_chars = 0;
   const int rc = memory_history_load_rows(conv_id, user_id, watermark, false,
                                           append_message_to_history, &ctx, ctx.array,
                                           &dropped_chars);
   if (rc != AUTH_DB_SUCCESS) {
      free(summary);
      json_object_put(ctx.array);
      return NULL;
   }
   ctx.total_text_len -= dropped_chars <= ctx.total_text_len ? dropped_chars : ctx.total_text_len;
   /* The compaction's summary, as a request rebuild renders it: framed with
    * the conversation's tag (its frozen prompt holds it; this history is
    * given that prompt later, when it runs). */
   if (summary) {
      char tag[32] = "";
      struct json_object *prefix_only = json_object_new_array();
      struct json_object *prefix = prefix_only ? prefix_message_stored(conv_id, user_id) : NULL;
      if (prefix) {
         json_object_array_add(prefix_only, prefix);
         const char *t = llm_history_tag(prefix_only);
         snprintf(tag, sizeof(tag), "%s", t ? t : "");
      }
      json_object_put(prefix_only);
      if (llm_history_attach_summary(ctx.array, 0, summary, tag[0] ? tag : NULL) >= 0) {
         ctx.total_text_len += strlen(summary);
      }
   }
   free(summary);
   if (text_len_out) {
      *text_len_out = ctx.total_text_len;
   }

   return ctx.array;
}

struct json_object *memory_history_load_from_db(int64_t conv_id,
                                                int user_id,
                                                size_t *text_len_out) {
   return load_history(conv_id, user_id, text_len_out);
}

struct json_object *memory_history_load_for_llm(int64_t conv_id,
                                                int user_id,
                                                size_t *text_len_out) {
   if (text_len_out) {
      *text_len_out = 0;
   }
   conversation_t conv = { 0 };
   if (conv_db_get(conv_id, user_id, &conv) != AUTH_DB_SUCCESS) {
      conv_free(&conv);
      return NULL;
   }
   struct json_object *hist = memory_history_request_context(conv_id, user_id,
                                                             conv.context_watermark_msg_id,
                                                             conv.compaction_summary, text_len_out,
                                                             NULL);
   conv_free(&conv);
   return hist;
}
