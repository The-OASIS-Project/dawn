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
 * WebUI image-marker rehydration (see webui_image_rehydrate.h).
 */

#include "webui/webui_image_rehydrate.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ocp_helpers.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "image_store.h"
#include "logging.h"

#define IMAGE_MARKER_PREFIX "[IMAGE:"
#define IMAGE_MARKER_PREFIX_LEN (sizeof(IMAGE_MARKER_PREFIX) - 1)

/* data: URI literal lengths, for exact buffer sizing. */
#define DATA_URI_SCHEME "data:"
#define DATA_URI_SCHEME_LEN (sizeof(DATA_URI_SCHEME) - 1) /* "data:"     */
#define DATA_URI_BASE64_PARAM ";base64,"
#define DATA_URI_BASE64_PARAM_LEN (sizeof(DATA_URI_BASE64_PARAM) - 1) /* ";base64," */

/* Read an entire file into a freshly-allocated buffer.  Bounded by the upload-time
 * size cap (image_store only stores validated, size-limited images), so no extra
 * cap is enforced here.  Returns NULL on any error; *len_out set to byte count. */
static unsigned char *read_file_bytes(const char *path, size_t *len_out) {
   *len_out = 0;
   FILE *file = fopen(path, "rb");
   if (!file) {
      return NULL;
   }
   if (fseek(file, 0, SEEK_END) != 0) {
      fclose(file);
      return NULL;
   }
   long size = ftell(file);
   if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      return NULL;
   }

   unsigned char *buffer = malloc((size_t)size);
   if (!buffer) {
      fclose(file);
      return NULL;
   }
   size_t got = fread(buffer, 1, (size_t)size, file);
   fclose(file);
   if (got != (size_t)size) {
      free(buffer);
      return NULL;
   }
   *len_out = (size_t)size;
   return buffer;
}

/* Append an image_url content part wrapping @p data_uri to @p arr. */
static bool append_image_url_part(struct json_object *arr, const char *data_uri) {
   struct json_object *part = json_object_new_object();
   if (!part) {
      return false;
   }
   json_object_object_add(part, "type", json_object_new_string("image_url"));
   struct json_object *image_url = json_object_new_object();
   json_object_object_add(image_url, "url", json_object_new_string(data_uri));
   json_object_object_add(part, "image_url", image_url);
   json_object_array_add(arr, part);
   return true;
}

char *webui_build_image_marker_content(const char *text,
                                       const char ids[][IMAGE_ID_LEN],
                                       int count) {
   if (!text) {
      return NULL;
   }
   strbuf_t sb;
   strbuf_init(&sb, strlen(text) + 32);
   if (strbuf_append(&sb, text) < 0) {
      strbuf_free(&sb);
      return NULL;
   }
   for (int i = 0; i < count; i++) {
      /* image_store_validate_id is the single authoritative id validator — mirrors
       * the parse side (webui_collect_image_ids), so a bad id can't forge a marker. */
      if (!ids || !image_store_validate_id(ids[i])) {
         OLOG_WARNING("WebUI: skipping invalid image id while building persist markers");
         continue;
      }
      if (strbuf_appendf(&sb, "\n[IMAGE:%s]", ids[i]) < 0) {
         strbuf_free(&sb);
         return NULL;
      }
   }
   char *out = strbuf_steal(&sb); /* text is non-empty, so never NULL */
   strbuf_free(&sb);
   return out;
}

int webui_collect_image_ids(const char *content,
                            char ids_out[][IMAGE_ID_LEN],
                            int max,
                            int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   if (!content || !ids_out || max <= 0) {
      return FAILURE;
   }

   int n = 0;
   const char *p = content;
   while (n < max) {
      const char *marker = strstr(p, IMAGE_MARKER_PREFIX);
      if (!marker) {
         break;
      }
      const char *close = strchr(marker + IMAGE_MARKER_PREFIX_LEN, ']');
      if (!close) {
         break;
      }
      const char *body = marker + IMAGE_MARKER_PREFIX_LEN;
      size_t body_len = (size_t)(close - body);
      p = close + 1;

      if (body_len == IMAGE_ID_LEN - 1) {
         char id[IMAGE_ID_LEN];
         memcpy(id, body, body_len);
         id[body_len] = '\0';
         if (image_store_validate_id(id)) {
            memcpy(ids_out[n], id, IMAGE_ID_LEN);
            n++;
         }
      }
      /* Legacy [IMAGE:data:...] markers carry no stored id — nothing to collect. */
   }

   if (count_out) {
      *count_out = n;
   }
   return SUCCESS;
}

int webui_rehydrate_message_into_session(session_t *session,
                                         int user_id,
                                         const char *role,
                                         const char *content) {
   if (!session || !role || !content) {
      return FAILURE;
   }

   /* Fast path: no markers → plain text message, unchanged. */
   if (!strstr(content, IMAGE_MARKER_PREFIX)) {
      session_add_message(session, role, content);
      return SUCCESS;
   }

   /* Collect image parts into a temporary array while building the prose text
    * (literal text with markers removed; misses become inline placeholders). */
   struct json_object *image_parts = json_object_new_array();
   strbuf_t prose;
   strbuf_init(&prose, strlen(content) + 32);
   if (!image_parts) {
      strbuf_free(&prose);
      session_add_message(session, role, content);
      return SUCCESS;
   }

   size_t total_bytes = 0;
   const char *p = content;
   while (*p) {
      const char *marker = strstr(p, IMAGE_MARKER_PREFIX);
      if (!marker) {
         strbuf_append(&prose, p); /* trailing literal text */
         break;
      }
      strbuf_append_n(&prose, p, (size_t)(marker - p)); /* text before the marker */

      const char *close = strchr(marker + IMAGE_MARKER_PREFIX_LEN, ']');
      if (!close) {
         strbuf_append(&prose, marker); /* malformed — keep the rest verbatim */
         break;
      }
      const char *body = marker + IMAGE_MARKER_PREFIX_LEN;
      size_t body_len = (size_t)(close - body);
      p = close + 1;

      /* Legacy inline data URI → use directly as an image_url. */
      if (body_len > DATA_URI_SCHEME_LEN &&
          strncmp(body, DATA_URI_SCHEME, DATA_URI_SCHEME_LEN) == 0) {
         if (json_object_array_length(image_parts) >= WEBUI_MAX_REHYDRATE_IMAGES ||
             total_bytes + body_len > WEBUI_MAX_REHYDRATE_BYTES) {
            strbuf_append(&prose, " [earlier image omitted]");
            continue;
         }
         char *uri = malloc(body_len + 1);
         if (uri) {
            memcpy(uri, body, body_len);
            uri[body_len] = '\0';
            if (append_image_url_part(image_parts, uri)) {
               total_bytes += body_len; /* raw bytes, consistent with the id-path ceiling */
            }
            free(uri);
            uri = NULL;
         }
         continue;
      }

      /* ID form. */
      if (body_len != IMAGE_ID_LEN - 1) {
         strbuf_append(&prose, " [image no longer available]");
         continue;
      }
      char id[IMAGE_ID_LEN];
      memcpy(id, body, body_len);
      id[body_len] = '\0';
      if (!image_store_validate_id(id)) {
         strbuf_append(&prose, " [image no longer available]");
         continue;
      }

      /* Owner-only: image_store_get_path's owner check is source-conditional
       * (only UPLOAD/MMS); require ownership here for ALL sources so a crafted
       * marker can't pull another user's generated/document image into context. */
      image_metadata_t meta;
      if (image_store_get_metadata(id, &meta) != IMAGE_STORE_SUCCESS || meta.user_id != user_id) {
         strbuf_append(&prose, " [image no longer available]");
         continue;
      }

      /* Defensive ceilings (crash backstop — see header): total bytes and part count. */
      if (json_object_array_length(image_parts) >= WEBUI_MAX_REHYDRATE_IMAGES ||
          total_bytes + meta.size > WEBUI_MAX_REHYDRATE_BYTES) {
         strbuf_append(&prose, " [earlier image omitted]");
         continue;
      }

      char path[IMAGE_PATH_MAX];
      char mime[IMAGE_MIME_MAX];
      if (image_store_get_path(id, user_id, path, mime) != IMAGE_STORE_SUCCESS) {
         strbuf_append(&prose, " [image no longer available]");
         continue;
      }

      size_t raw_len = 0;
      unsigned char *raw = read_file_bytes(path, &raw_len);
      if (!raw) {
         strbuf_append(&prose, " [image no longer available]");
         continue;
      }
      char *b64 = ocp_base64_encode(raw, raw_len);
      free(raw); /* free raw bytes immediately so raw + base64 don't co-reside */
      raw = NULL;
      if (!b64) {
         strbuf_append(&prose, " [image no longer available]");
         continue;
      }

      /* data:<mime>;base64,<b64> */
      size_t uri_len = DATA_URI_SCHEME_LEN + strlen(mime) + DATA_URI_BASE64_PARAM_LEN +
                       strlen(b64) + 1;
      char *uri = malloc(uri_len);
      if (uri) {
         snprintf(uri, uri_len, "%s%s%s%s", DATA_URI_SCHEME, mime, DATA_URI_BASE64_PARAM, b64);
         if (append_image_url_part(image_parts, uri)) {
            total_bytes += raw_len; /* track raw bytes, consistent with the ceiling check */
         }
         free(uri);
         uri = NULL;
      }
      free(b64);
      b64 = NULL;
   }

   int n_images = json_object_array_length(image_parts);
   if (n_images == 0) {
      /* Nothing materialized — emit the prose as a plain text message. */
      const char *text = strbuf_str(&prose);
      session_add_message(session, role, text ? text : content);
      strbuf_free(&prose);
      json_object_put(image_parts);
      return SUCCESS;
   }

   /* Assemble {role, content:[text, image_url...]} and hand ownership to the session. */
   struct json_object *message = json_object_new_object();
   struct json_object *content_arr = json_object_new_array();
   if (!message || !content_arr) {
      if (message) {
         json_object_put(message);
      }
      if (content_arr) {
         json_object_put(content_arr);
      }
      const char *text = strbuf_str(&prose);
      session_add_message(session, role, text ? text : content);
      strbuf_free(&prose);
      json_object_put(image_parts);
      return SUCCESS;
   }

   json_object_object_add(message, "role", json_object_new_string(role));

   struct json_object *text_part = json_object_new_object();
   json_object_object_add(text_part, "type", json_object_new_string("text"));
   json_object_object_add(text_part, "text", json_object_new_string(strbuf_str(&prose)));
   json_object_array_add(content_arr, text_part);
   strbuf_free(&prose);

   for (int i = 0; i < n_images; i++) {
      struct json_object *part = json_object_array_get_idx(image_parts, i);
      json_object_array_add(content_arr, json_object_get(part)); /* +1 ref, moved */
   }
   json_object_put(image_parts);

   json_object_object_add(message, "content", content_arr);
   session_add_message_multipart(session, message); /* takes ownership */

   OLOG_INFO("WebUI: rehydrated %d image(s) into restored message", n_images);
   return SUCCESS;
}
