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
 * The documents a text turn attaches, built into its text (webui_attachments.h).
 */

#include "webui/webui_attachments.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "blob_store.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"

/* The lines that open and close a document in a turn's text, and what a
 * document's own copy of one becomes (a body can't end itself early). */
#define DOC_OPEN LLM_CONTEXT_DOC_OPEN
#define DOC_CLOSE LLM_CONTEXT_DOC_CLOSE
/* What's quoted inside a filename or body: the open marker with or without
 * its space, so no reader's variant of it can start a block. */
#define DOC_OPEN_STEM "[ATTACHED DOCUMENT:"

/* A block's framing past its filename and body: the markers, the size and
 * blob suffix, line breaks and the blank line between blocks (generous). */
#define DOC_FRAME_ROOM 128

/* Stored originals' blob ids ("blb_" + 12). */
#define DOC_BLOB_PREFIX "blb_"

/* @p text with every copy of a document marker quoted, appended to @p sb.
 * One pass over the text (a body can be MBs of markers). */
static int append_quoting_markers(strbuf_t *sb, const char *text) {
   const size_t open_len = sizeof(DOC_OPEN_STEM) - 1;
   const size_t close_len = sizeof(DOC_CLOSE) - 1;
   const char *seg = text; /* first byte not yet appended */
   const char *end = text + strlen(text);
   const char *p = text;
   while ((p = memchr(p, '[', (size_t)(end - p))) != NULL) {
      const size_t left = (size_t)(end - p);
      const bool marker = (left >= open_len && memcmp(p, DOC_OPEN_STEM, open_len) == 0) ||
                          (left >= close_len && memcmp(p, DOC_CLOSE, close_len) == 0);
      if (marker) {
         /* "[" becomes "(": the parsers and the turn's own span finder read
          * neither as a marker; the rest of the line is kept. */
         if (strbuf_append_n(sb, seg, (size_t)(p - seg)) < 0 || strbuf_append(sb, "(") < 0) {
            return FAILURE;
         }
         seg = p + 1;
      }
      p++;
   }
   return strbuf_append_n(sb, seg, (size_t)(end - seg)) < 0 ? FAILURE : SUCCESS;
}

/* A filename the header can carry: 1 to WEBUI_ATTACHMENT_FILENAME_MAX bytes,
 * no line break or NUL inside. */
static bool filename_ok(struct json_object *v) {
   if (!v || !json_object_is_type(v, json_type_string)) {
      return false;
   }
   const char *s = json_object_get_string(v);
   const size_t n = (size_t)json_object_get_string_len(v);
   return n > 0 && n <= WEBUI_ATTACHMENT_FILENAME_MAX && strlen(s) == n &&
          strpbrk(s, "\r\n") == NULL;
}

/* One document's block appended to @p sb. */
static int append_document(strbuf_t *sb,
                           struct json_object *entry,
                           int user_id,
                           size_t max_content,
                           webui_attachment_owner_fn owner) {
   struct json_object *filename = NULL;
   struct json_object *size = NULL;
   struct json_object *content = NULL;
   struct json_object *blob = NULL;
   if (!json_object_is_type(entry, json_type_object) ||
       !json_object_object_get_ex(entry, "filename", &filename) || !filename_ok(filename) ||
       !json_object_object_get_ex(entry, "size", &size) ||
       !json_object_is_type(size, json_type_int) || json_object_get_int64(size) < 0 ||
       !json_object_object_get_ex(entry, "content", &content) ||
       !json_object_is_type(content, json_type_string) ||
       (size_t)json_object_get_string_len(content) > max_content ||
       strlen(json_object_get_string(content)) != (size_t)json_object_get_string_len(content)) {
      return WEBUI_ATTACHMENTS_INVALID;
   }
   const char *blob_id = NULL;
   /* Present means a string id the user can read; absent is omitted, never null. */
   if (json_object_object_get_ex(entry, "blob_id", &blob)) {
      blob_id = json_object_is_type(blob, json_type_string) ? json_object_get_string(blob) : NULL;
      if (!blob_id || !blob_validate_id(blob_id, DOC_BLOB_PREFIX) || !owner) {
         return WEBUI_ATTACHMENTS_INVALID;
      }
      const int own = owner(blob_id, user_id);
      if (own == WEBUI_ATTACHMENT_BLOB_GONE) {
         /* Reclaimed before the turn was sent (an attachment left in the
          * composer past the originals' grace window): the text still goes,
          * without a link to a file that is gone. */
         blob_id = NULL;
      } else if (own != SUCCESS) {
         return WEBUI_ATTACHMENTS_INVALID;
      }
   }

   /* The block's boundary is the daemon's: a marker inside the filename or
    * body is quoted so the body can't end itself.  Their other contents are
    * defused when the turn runs (llm_context_neutralize_attachments, on the
    * turn's own thread: a turn's documents can be MBs). */
   int rc = (strbuf_append(sb, DOC_OPEN) < 0) ? FAILURE : SUCCESS;
   if (rc == SUCCESS) {
      rc = append_quoting_markers(sb, json_object_get_string(filename));
   }
   /* The stored original's marker ("blob:<id>]") is what the reload parser and
    * the originals sweep look for: scripts/check_blob_marker_sync.sh. */
   if (rc == SUCCESS &&
       strbuf_appendf(sb, " (%lld bytes)", (long long)json_object_get_int64(size)) < 0) {
      rc = FAILURE;
   }
   if (rc == SUCCESS &&
       (blob_id ? strbuf_appendf(sb, " blob:%s]\n", blob_id) : strbuf_append(sb, "]\n")) < 0) {
      rc = FAILURE;
   }
   if (rc == SUCCESS) {
      rc = append_quoting_markers(sb, json_object_get_string(content));
   }
   if (rc == SUCCESS && strbuf_append(sb, "\n" DOC_CLOSE) < 0) {
      rc = FAILURE;
   }
   return rc;
}

int webui_attachments_build(struct json_object *attachments,
                            int user_id,
                            int max_docs,
                            size_t max_content,
                            webui_attachment_owner_fn owner,
                            char **out) {
   if (!out) {
      return FAILURE;
   }
   *out = NULL;
   if (!attachments || !json_object_is_type(attachments, json_type_array)) {
      return WEBUI_ATTACHMENTS_INVALID;
   }
   const size_t n = json_object_array_length(attachments);
   if (n == 0 || max_docs <= 0 || n > (size_t)max_docs) {
      return WEBUI_ATTACHMENTS_INVALID;
   }
   /* Room for every document at its largest (quoting never grows the text):
    * the default cap would refuse documents the configured limits allow. */
   const size_t per_doc = max_content + WEBUI_ATTACHMENT_FILENAME_MAX + DOC_FRAME_ROOM;
   strbuf_t sb;
   strbuf_init_with_max(&sb, 1024, n * per_doc + 1);
   for (size_t i = 0; i < n; i++) {
      if (i > 0 && strbuf_append(&sb, "\n\n") < 0) {
         strbuf_free(&sb);
         return FAILURE;
      }
      const int rc = append_document(&sb, json_object_array_get_idx(attachments, i), user_id,
                                     max_content, owner);
      if (rc != SUCCESS) {
         strbuf_free(&sb);
         return rc;
      }
   }
   *out = strbuf_steal(&sb);
   strbuf_free(&sb);
   return *out ? SUCCESS : FAILURE;
}
