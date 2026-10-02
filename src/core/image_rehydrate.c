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
 * Image-marker rehydration for replay: a stored [IMAGE:<id>] marker becomes
 * the image content a request sends (see image_rehydrate.h).
 */

#include "core/image_rehydrate.h"

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

/* Append an image_url content part wrapping @p data_uri to @p arr, marked with
 * the stored image it came from when @p id is set. */
static bool append_image_url_part(struct json_object *arr, const char *data_uri, const char *id) {
   struct json_object *part = json_object_new_object();
   if (!part) {
      return false;
   }
   json_object_object_add(part, "type", json_object_new_string("image_url"));
   struct json_object *image_url = json_object_new_object();
   struct json_object *url = image_url ? json_object_new_string(data_uri) : NULL;
   if (!url) {
      /* No half-built image part: the caller stands one in. */
      json_object_put(image_url);
      json_object_put(part);
      return false;
   }
   json_object_object_add(image_url, "url", url);
   json_object_object_add(part, "image_url", image_url);
   if (id) {
      json_object_object_add(part, IMAGE_PART_ID_KEY, json_object_new_string(id));
   }
   json_object_array_add(arr, part);
   return true;
}

/* Any source (image_rehydrate_message: markers are owner-checked only). */
#define IMAGE_SOURCE_ANY (-1)

typedef enum {
   LOAD_OK,
   LOAD_MISSING,      /* invalid, not found, not the owner's, wrong source, unreadable */
   LOAD_OVER_CEILING, /* past IMAGE_REHYDRATE_MAX_IMAGES / _MAX_BYTES */
} load_result_t;

/* Stored image @p id as a data: URI (caller frees *@p uri_out), with its raw
 * size.  Owner-only for every source: an id names an image of @p user_id's or
 * nothing.  @p source, when not IMAGE_SOURCE_ANY, must match too.  The
 * ceilings count @p have images and @p have_bytes raw bytes already loaded. */
static load_result_t load_image_uri(int user_id,
                                    const char *id,
                                    int source,
                                    int have,
                                    size_t have_bytes,
                                    char **uri_out,
                                    size_t *raw_len_out) {
   *uri_out = NULL;
   *raw_len_out = 0;
   if (user_id <= 0 || !image_store_validate_id(id)) {
      return LOAD_MISSING;
   }
   /* Owner-only: image_store_get_path's owner check is source-conditional, so
    * ownership is required here for ALL sources (a crafted id can't pull another
    * user's generated or document image into context). */
   image_metadata_t meta;
   if (image_store_get_metadata(id, &meta) != IMAGE_STORE_SUCCESS || meta.user_id != user_id ||
       (source != IMAGE_SOURCE_ANY && (int)meta.source != source)) {
      return LOAD_MISSING;
   }
   /* Defensive ceilings (crash backstop — see header): part count and bytes. */
   if (have >= IMAGE_REHYDRATE_MAX_IMAGES || have_bytes + meta.size > IMAGE_REHYDRATE_MAX_BYTES) {
      return LOAD_OVER_CEILING;
   }

   char path[IMAGE_PATH_MAX];
   char mime[IMAGE_MIME_MAX];
   if (image_store_get_path(id, user_id, path, mime) != IMAGE_STORE_SUCCESS) {
      return LOAD_MISSING;
   }
   size_t raw_len = 0;
   unsigned char *raw = read_file_bytes(path, &raw_len);
   if (!raw) {
      return LOAD_MISSING;
   }
   char *b64 = ocp_base64_encode(raw, raw_len);
   free(raw); /* free raw bytes immediately so raw + base64 don't co-reside */
   raw = NULL;
   if (!b64) {
      return LOAD_MISSING;
   }
   /* data:<mime>;base64,<b64> */
   const size_t uri_len = DATA_URI_SCHEME_LEN + strlen(mime) + DATA_URI_BASE64_PARAM_LEN +
                          strlen(b64) + 1;
   char *uri = malloc(uri_len);
   if (uri) {
      snprintf(uri, uri_len, "%s%s%s%s", DATA_URI_SCHEME, mime, DATA_URI_BASE64_PARAM, b64);
   }
   free(b64);
   b64 = NULL;
   if (!uri) {
      return LOAD_MISSING;
   }
   *uri_out = uri;
   *raw_len_out = raw_len;
   return LOAD_OK;
}

/* A text part standing in for stored image @p id. */
static bool append_stand_in_part(struct json_object *arr, const char *text, const char *id) {
   struct json_object *part = json_object_new_object();
   if (!part) {
      return false;
   }
   json_object_object_add(part, "type", json_object_new_string("text"));
   json_object_object_add(part, "text", json_object_new_string(text));
   json_object_object_add(part, IMAGE_PART_ID_KEY, json_object_new_string(id));
   json_object_array_add(arr, part);
   return true;
}

struct json_object *image_rehydrate_parts(int user_id,
                                          const char ids[][IMAGE_ID_LEN],
                                          int count,
                                          int source) {
   if (!ids || count <= 0) {
      return NULL;
   }
   struct json_object *parts = json_object_new_array();
   if (!parts) {
      return NULL;
   }
   size_t total_bytes = 0;
   int loaded = 0;
   for (int i = 0; i < count; i++) {
      if (!image_store_validate_id(ids[i])) {
         continue; /* nothing to name it by */
      }
      char *uri = NULL;
      size_t raw_len = 0;
      const load_result_t r = load_image_uri(user_id, ids[i], source, loaded, total_bytes, &uri,
                                             &raw_len);
      bool added;
      if (r == LOAD_OK) {
         added = append_image_url_part(parts, uri, ids[i]);
         free(uri);
         uri = NULL;
         if (added) {
            total_bytes += raw_len;
            loaded++;
         } else {
            /* Out of memory building it: the fixed stand-in, not nothing. */
            added = append_stand_in_part(parts, IMAGE_REHYDRATE_MISSING_TEXT, ids[i]);
         }
      } else {
         added = append_stand_in_part(parts,
                                      r == LOAD_OVER_CEILING ? IMAGE_REHYDRATE_OMITTED_TEXT
                                                             : IMAGE_REHYDRATE_MISSING_TEXT,
                                      ids[i]);
      }
      if (!added) {
         json_object_put(parts);
         return NULL;
      }
   }
   return parts;
}

char *image_marker_build_content(const char *text, const char ids[][IMAGE_ID_LEN], int count) {
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
       * the parse side (image_marker_collect_ids), so a bad id can't forge a marker. */
      if (!ids || !image_store_validate_id(ids[i])) {
         OLOG_WARNING("WebUI: skipping invalid image id while building persist markers");
         continue;
      }
      /* An image-only turn has no text: its markers start the content. */
      if (strbuf_appendf(&sb, "%s[IMAGE:%s]", strbuf_len(&sb) ? "\n" : "", ids[i]) < 0) {
         strbuf_free(&sb);
         return NULL;
      }
   }
   if (strbuf_len(&sb) == 0) { /* no text and no valid id: nothing to persist */
      strbuf_free(&sb);
      return NULL;
   }
   char *out = strbuf_steal(&sb);
   strbuf_free(&sb);
   return out;
}

int image_marker_collect_ids(const char *content,
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

/* {role, content} text message; NULL on OOM. */
static struct json_object *text_message(const char *role, const char *content) {
   struct json_object *m = json_object_new_object();
   if (!m) {
      return NULL;
   }
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

/* Index of @p id in @p ids, or -1. */
static int find_id(const char (*ids)[IMAGE_ID_LEN], int count, const char *id) {
   for (int i = 0; ids && i < count; i++) {
      if (strcmp(ids[i], id) == 0) {
         return i;
      }
   }
   return -1;
}

/* The message stored @p content rebuilds into.  Lenient (@p required NULL), a
 * marker that can't be shown becomes a stand-in note and OOM degrades to text:
 * a reload never fails over one image.  Strict (@p required: the ids the
 * question was sent with), one of those that can't be shown, an inline data:
 * image, or OOM fails the build instead (NULL, *@p err_out an
 * IMAGE_REHYDRATE_ERR_* code), so a question being asked now is sent whole or
 * not at all.  Any other marker in its text reads as on reload. */
static struct json_object *rehydrate_build(int user_id,
                                           const char *role,
                                           const char *content,
                                           const char (*required)[IMAGE_ID_LEN],
                                           int n_required,
                                           int *err_out) {
   const bool strict = required != NULL;
   *err_out = SUCCESS;
   if (!role || !content) {
      *err_out = IMAGE_REHYDRATE_ERR_NOMEM;
      return NULL;
   }
   if (strict && n_required > IMAGE_REHYDRATE_MAX_IMAGES) {
      *err_out = IMAGE_REHYDRATE_ERR_LIMIT;
      return NULL;
   }
   bool shown[IMAGE_REHYDRATE_MAX_IMAGES] = { false }; /* strict: each required id loaded */

   /* Fast path: no markers → plain text message, unchanged. */
   if (!strict && !strstr(content, IMAGE_MARKER_PREFIX)) {
      struct json_object *m = text_message(role, content);
      if (!m) {
         *err_out = IMAGE_REHYDRATE_ERR_NOMEM;
      }
      return m;
   }

   /* Collect image parts into a temporary array while building the prose text
    * (literal text with markers removed; misses become inline placeholders). */
   struct json_object *image_parts = json_object_new_array();
   strbuf_t prose;
   strbuf_init(&prose, strlen(content) + 32);
   if (!image_parts) {
      strbuf_free(&prose);
      if (strict) {
         *err_out = IMAGE_REHYDRATE_ERR_NOMEM;
         return NULL;
      }
      return text_message(role, content);
   }

   int fail = SUCCESS; /* strict: the first reason the build fails */
   size_t total_bytes = 0;
   const char *p = content;
   while (*p && fail == SUCCESS) {
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

      /* Legacy inline data URI → use directly as an image_url.  Never for a
       * question being asked now: its images are stored ones, named by id. */
      if (body_len > DATA_URI_SCHEME_LEN &&
          strncmp(body, DATA_URI_SCHEME, DATA_URI_SCHEME_LEN) == 0) {
         if (strict) {
            fail = IMAGE_REHYDRATE_ERR_NOT_FOUND;
            break;
         }
         if (json_object_array_length(image_parts) >= IMAGE_REHYDRATE_MAX_IMAGES ||
             total_bytes + body_len > IMAGE_REHYDRATE_MAX_BYTES) {
            strbuf_append(&prose, " " IMAGE_REHYDRATE_OMITTED_TEXT);
            continue;
         }
         char *uri = malloc(body_len + 1);
         if (uri) {
            memcpy(uri, body, body_len);
            uri[body_len] = '\0';
            if (append_image_url_part(image_parts, uri, NULL)) {
               total_bytes += body_len; /* raw bytes, consistent with the id-path ceiling */
            }
            free(uri);
            uri = NULL;
         }
         continue;
      }

      /* ID form. */
      if (body_len != IMAGE_ID_LEN - 1) {
         strbuf_append(&prose, " " IMAGE_REHYDRATE_MISSING_TEXT);
         continue;
      }
      char id[IMAGE_ID_LEN];
      memcpy(id, body, body_len);
      id[body_len] = '\0';
      const int req = strict ? find_id(required, n_required, id) : -1;
      char *uri = NULL;
      size_t raw_len = 0;
      switch (load_image_uri(user_id, id, IMAGE_SOURCE_ANY,
                             (int)json_object_array_length(image_parts), total_bytes, &uri,
                             &raw_len)) {
         case LOAD_OK:
            if (append_image_url_part(image_parts, uri, NULL)) {
               total_bytes += raw_len; /* raw bytes, consistent with the ceiling check */
               if (req >= 0) {
                  shown[req] = true;
               }
            } else if (strict) {
               fail = IMAGE_REHYDRATE_ERR_NOMEM;
            }
            free(uri);
            uri = NULL;
            break;
         case LOAD_OVER_CEILING:
            if (req >= 0) {
               fail = IMAGE_REHYDRATE_ERR_LIMIT;
               break;
            }
            strbuf_append(&prose, " " IMAGE_REHYDRATE_OMITTED_TEXT);
            break;
         default:
            if (req >= 0) {
               fail = IMAGE_REHYDRATE_ERR_NOT_FOUND;
               break;
            }
            strbuf_append(&prose, " " IMAGE_REHYDRATE_MISSING_TEXT);
            break;
      }
   }
   if (strict && fail == SUCCESS && strbuf_oom(&prose)) {
      fail = IMAGE_REHYDRATE_ERR_NOMEM;
   }
   for (int i = 0; strict && fail == SUCCESS && i < n_required; i++) {
      if (!shown[find_id(required, n_required, required[i])]) { /* a repeat: its first */
         fail = IMAGE_REHYDRATE_ERR_NOT_FOUND; /* named by the question, no marker of it */
      }
   }
   if (fail != SUCCESS) {
      strbuf_free(&prose);
      json_object_put(image_parts);
      *err_out = fail;
      return NULL;
   }

   int n_images = json_object_array_length(image_parts);
   if (n_images == 0) {
      /* Nothing materialized — emit the prose as a plain text message. */
      const char *text = strbuf_str(&prose);
      struct json_object *m = text_message(role, text ? text : content);
      strbuf_free(&prose);
      json_object_put(image_parts);
      if (!m) {
         *err_out = IMAGE_REHYDRATE_ERR_NOMEM;
      }
      return m;
   }

   /* Assemble {role, content:[text, image_url...]}. */
   struct json_object *message = json_object_new_object();
   struct json_object *content_arr = json_object_new_array();
   if (!message || !content_arr) {
      if (message) {
         json_object_put(message);
      }
      if (content_arr) {
         json_object_put(content_arr);
      }
      json_object_put(image_parts);
      if (strict) {
         strbuf_free(&prose);
         *err_out = IMAGE_REHYDRATE_ERR_NOMEM;
         return NULL;
      }
      const char *text = strbuf_str(&prose);
      struct json_object *m = text_message(role, text ? text : content);
      strbuf_free(&prose);
      return m;
   }

   json_object_object_add(message, "role", json_object_new_string(role));

   /* An image-only question has no words: no text part (providers refuse an
    * empty one). */
   const char *prose_text = strbuf_str(&prose);
   if (prose_text && prose_text[strspn(prose_text, " \t\r\n")] != '\0') {
      struct json_object *text_part = json_object_new_object();
      json_object_object_add(text_part, "type", json_object_new_string("text"));
      json_object_object_add(text_part, "text", json_object_new_string(prose_text));
      json_object_array_add(content_arr, text_part);
   }
   strbuf_free(&prose);

   for (int i = 0; i < n_images; i++) {
      struct json_object *part = json_object_array_get_idx(image_parts, i);
      json_object_array_add(content_arr, json_object_get(part)); /* +1 ref, moved */
   }
   json_object_put(image_parts);

   json_object_object_add(message, "content", content_arr);

   OLOG_INFO("Rehydrated %d image(s) into a %s message", n_images,
             strict ? "question's" : "restored");
   return message;
}

struct json_object *image_rehydrate_message(int user_id, const char *role, const char *content) {
   int err = SUCCESS;
   return rehydrate_build(user_id, role, content, NULL, 0, &err);
}

int image_rehydrate_question(int user_id,
                             const char *content,
                             const char ids[][IMAGE_ID_LEN],
                             int count,
                             struct json_object **msg_out) {
   if (!msg_out) {
      return FAILURE;
   }
   *msg_out = NULL;
   if (!content || !ids || count <= 0) {
      return FAILURE;
   }
   for (int i = 0; i < count; i++) {
      if (!image_store_validate_id(ids[i])) {
         return IMAGE_REHYDRATE_ERR_NOT_FOUND;
      }
   }
   int err = SUCCESS;
   *msg_out = rehydrate_build(user_id, "user", content, ids, count, &err);
   return *msg_out ? SUCCESS : err;
}

int image_turn_ids_parse(struct json_object *payload,
                         int max,
                         char ids_out[][IMAGE_ID_LEN],
                         int *count_out) {
   if (!count_out) {
      return FAILURE;
   }
   *count_out = 0;
   struct json_object *arr = NULL;
   if (!payload || !json_object_object_get_ex(payload, "image_ids", &arr) || arr == NULL) {
      return SUCCESS; /* a text turn; images[] is never read */
   }
   if (!json_object_is_type(arr, json_type_array)) {
      return IMAGE_REHYDRATE_ERR_NOT_FOUND;
   }
   const int n = (int)json_object_array_length(arr);
   if (n > max) {
      return IMAGE_REHYDRATE_ERR_LIMIT;
   }
   for (int i = 0; i < n; i++) {
      struct json_object *entry = json_object_array_get_idx(arr, i);
      const char *id = json_object_is_type(entry, json_type_string) ? json_object_get_string(entry)
                                                                    : NULL;
      if (!id || !image_store_validate_id(id)) {
         return IMAGE_REHYDRATE_ERR_NOT_FOUND;
      }
      snprintf(ids_out[i], IMAGE_ID_LEN, "%s", id);
   }
   *count_out = n;
   return SUCCESS;
}
