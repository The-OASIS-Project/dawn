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
 * Claude message parts read for other providers and for a reply's text.  See llm_claude_parts.h.
 */

#include "llm/llm_claude_parts.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return (obj && json_object_object_get_ex(obj, key, &v)) ? json_object_get_string(v) : NULL;
}

char *llm_claude_content_text(struct json_object *part) {
   struct json_object *content = NULL;
   json_object_object_get_ex(part, "content", &content);
   if (!content || !json_object_is_type(content, json_type_array)) {
      const char *text = content ? json_object_get_string(content) : NULL;
      return strdup(text ? text : "");
   }
   /* Size it first: a tool's output can be large, and must not be cut. */
   const size_t n = json_object_array_length(content);
   size_t total = 1;
   for (size_t k = 0; k < n; k++) {
      struct json_object *p = json_object_array_get_idx(content, k);
      const char *t = str_of(p, "type");
      const char *text = str_of(p, "text");
      if (t && strcmp(t, "text") == 0 && text) {
         total += strlen(text) + 1;
      }
   }
   char *out = malloc(total);
   if (!out) {
      return NULL;
   }
   size_t off = 0;
   for (size_t k = 0; k < n; k++) {
      struct json_object *p = json_object_array_get_idx(content, k);
      const char *t = str_of(p, "type");
      const char *text = str_of(p, "text");
      if (!t || strcmp(t, "text") != 0 || !text) {
         continue;
      }
      if (off > 0) {
         out[off++] = '\n';
      }
      const size_t len = strlen(text);
      memcpy(out + off, text, len);
      off += len;
   }
   out[off] = '\0';
   return out;
}

char *llm_claude_image_data_url(struct json_object *part) {
   static const char *const TYPES[] = { "image/jpeg", "image/png", "image/gif", "image/webp" };
   struct json_object *source = NULL;
   if (!json_object_object_get_ex(part, "source", &source)) {
      return NULL;
   }
   const char *kind = str_of(source, "type");
   const char *media = str_of(source, "media_type");
   const char *data = str_of(source, "data");
   if (!kind || strcmp(kind, "base64") != 0 || !data || !*data) {
      return NULL;
   }
   if (!media) {
      media = "image/jpeg";
   }
   bool known = false;
   for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]) && !known; i++) {
      known = strcmp(media, TYPES[i]) == 0;
   }
   if (!known) {
      return NULL;
   }
   static const char PREFIX[] = "data:";
   static const char MIDDLE[] = ";base64,";
   const size_t len = strlen(PREFIX) + strlen(media) + strlen(MIDDLE) + strlen(data) + 1;
   char *out = malloc(len);
   if (!out) {
      return NULL;
   }
   snprintf(out, len, "%s%s%s%s", PREFIX, media, MIDDLE, data);
   return out;
}
