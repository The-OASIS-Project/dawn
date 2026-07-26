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

static int append_message_to_history(const conversation_message_t *msg, void *ctx_ptr) {
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
   json_object_array_add(ctx->array, entry);
   return 0;
}

struct json_object *memory_history_load_from_db(int64_t conv_id,
                                                int user_id,
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

   int rc = (watermark > 0)
                ? conv_db_get_messages_after(conv_id, user_id, watermark, append_message_to_history,
                                             &ctx)
                : conv_db_get_messages(conv_id, user_id, append_message_to_history, &ctx);
   if (rc != AUTH_DB_SUCCESS) {
      json_object_put(ctx.array);
      return NULL;
   }
   if (text_len_out) {
      *text_len_out = ctx.total_text_len;
   }

   return ctx.array;
}
