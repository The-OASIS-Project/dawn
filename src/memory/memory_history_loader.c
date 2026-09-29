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
#include "dawn_error.h"
#include "llm/llm_compaction_range.h"
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

static struct json_object *load_history(int64_t conv_id,
                                        int user_id,
                                        bool with_blocks,
                                        size_t *text_len_out) {
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
   conversation_t conv = { 0 };
   if (conv_db_get(conv_id, user_id, &conv) == AUTH_DB_SUCCESS) {
      watermark = conv.context_watermark_msg_id;
      if (watermark > 0 && conv.compaction_summary && conv.compaction_summary[0]) {
         struct json_object *summary_msg = json_object_new_object();
         if (summary_msg) {
            char note[CONV_SUMMARY_MAX];
            /* Same reconstructed [COMPACTED ...] marker as the WebUI restore path, so a
             * reloaded messaging session also keeps a context_expand handle.  ASSISTANT
             * role (not system): the per-turn two-system-message rebuild drops extra
             * system messages — matches the live compaction marker so it survives. */
            conv_db_format_compaction_context(conv_id, conv.compaction_summary, note, sizeof(note));
            json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));
            json_object_object_add(summary_msg, "content", json_object_new_string(note));
            json_object_array_add(ctx.array, summary_msg);
            ctx.total_text_len += strlen(note);
         }
      }
   }
   conv_free(&conv);

   size_t dropped_chars = 0;
   const int rc = memory_history_load_rows(conv_id, user_id, watermark, with_blocks,
                                           append_message_to_history, &ctx, ctx.array,
                                           &dropped_chars);
   if (rc != AUTH_DB_SUCCESS) {
      json_object_put(ctx.array);
      return NULL;
   }
   ctx.total_text_len -= dropped_chars <= ctx.total_text_len ? dropped_chars : ctx.total_text_len;
   if (text_len_out) {
      *text_len_out = ctx.total_text_len;
   }

   return ctx.array;
}

struct json_object *memory_history_load_from_db(int64_t conv_id,
                                                int user_id,
                                                size_t *text_len_out) {
   return load_history(conv_id, user_id, false, text_len_out);
}

struct json_object *memory_history_load_for_llm(int64_t conv_id,
                                                int user_id,
                                                size_t *text_len_out) {
   return load_history(conv_id, user_id, true, text_len_out);
}
