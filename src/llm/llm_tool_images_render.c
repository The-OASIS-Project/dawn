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
 * A tool's images in a request, per provider.  See llm_tool_images_render.h.
 */

#include "llm/llm_tool_images_render.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_claude_parts.h"
#include "logging.h"
#include "utils/string_utils.h"

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return (obj && json_object_object_get_ex(obj, key, &v)) ? json_object_get_string(v) : NULL;
}

static bool is(const char *a, const char *b) {
   return a && strcmp(a, b) == 0;
}

static struct json_object *content_of(struct json_object *obj) {
   struct json_object *content = NULL;
   return (obj && json_object_object_get_ex(obj, "content", &content)) ? content : NULL;
}

static bool is_array(struct json_object *obj) {
   return obj && json_object_is_type(obj, json_type_array);
}

bool llm_part_is_image(struct json_object *part) {
   const char *type = str_of(part, "type");
   return is(type, "image_url") || is(type, "image");
}

/* @p part's image as a data URI: its own (image_url), or built from a Claude
 * base64 source.  Caller frees; NULL when it has none. */
static char *data_url_of(struct json_object *part) {
   if (is(str_of(part, "type"), "image")) {
      return llm_claude_image_data_url(part);
   }
   struct json_object *image_url = NULL;
   if (!json_object_object_get_ex(part, "image_url", &image_url)) {
      return NULL;
   }
   const char *url = str_of(image_url, "url");
   return url ? strdup(url) : NULL;
}

/* The bytes @p part's image is sent as. */
static size_t image_bytes(struct json_object *part) {
   struct json_object *holder = NULL;
   if (is(str_of(part, "type"), "image")) {
      json_object_object_get_ex(part, "source", &holder);
      json_object_object_get_ex(holder, "data", &holder);
   } else {
      json_object_object_get_ex(part, "image_url", &holder);
      json_object_object_get_ex(holder, "url", &holder);
   }
   return json_object_is_type(holder, json_type_string) ? (size_t)json_object_get_string_len(holder)
                                                        : 0;
}

/* Count @p parts' images, a tool_result's nested ones too. */
static void count_parts(struct json_object *parts, int *count, int64_t *bytes) {
   const size_t n = is_array(parts) ? json_object_array_length(parts) : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(parts, i);
      if (llm_part_is_image(part)) {
         (*count)++;
         *bytes += (int64_t)image_bytes(part);
      } else if (is(str_of(part, "type"), "tool_result")) {
         count_parts(content_of(part), count, bytes);
      }
   }
}

void llm_history_image_totals(struct json_object *history,
                              int start,
                              int end,
                              int *count_out,
                              int64_t *bytes_out) {
   int count = 0;
   int64_t bytes = 0;
   const int len = is_array(history) ? (int)json_object_array_length(history) : 0;
   if (end > len || end < 0) {
      end = len;
   }
   for (int i = start < 0 ? 0 : start; i < end; i++) {
      count_parts(content_of(json_object_array_get_idx(history, i)), &count, &bytes);
   }
   if (count_out) {
      *count_out = count;
   }
   if (bytes_out) {
      *bytes_out = bytes;
   }
}

/* A copy of @p obj with its content replaced by @p content (taken). */
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

static struct json_object *text_part(const char *text) {
   struct json_object *part = json_object_new_object();
   if (part) {
      json_object_object_add(part, "type", json_object_new_string("text"));
      json_object_object_add(part, "text", json_object_new_string(text));
   }
   return part;
}

bool llm_parts_have_image(struct json_object *parts) {
   int count = 0;
   int64_t bytes = 0;
   count_parts(parts, &count, &bytes);
   return count > 0;
}

/* Whether a tool_result among @p parts holds an image. */
static bool results_have_image(struct json_object *parts) {
   const size_t n = json_object_array_length(parts);
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(parts, i);
      if (is(str_of(part, "type"), "tool_result") && llm_parts_have_image(content_of(part))) {
         return true;
      }
   }
   return false;
}

/* @p parts with each image inside a tool_result a no-vision text part, and
 * each of its own when @p images_too.  New reference, or NULL on out of
 * memory. */
static struct json_object *parts_without(struct json_object *parts, bool images_too) {
   const size_t n = json_object_array_length(parts);
   struct json_object *out = json_object_new_array_ext((int)n);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *part = json_object_array_get_idx(parts, i);
      struct json_object *kept = NULL;
      if (images_too && llm_part_is_image(part)) {
         kept = text_part(LLM_TOOL_IMAGES_NO_VISION_TEXT);
      } else if (is(str_of(part, "type"), "tool_result") && is_array(content_of(part)) &&
                 llm_parts_have_image(content_of(part))) {
         struct json_object *inner = parts_without(content_of(part), true);
         kept = inner ? with_content(part, inner) : NULL;
      } else {
         kept = json_object_get(part);
      }
      if (!kept) {
         json_object_put(out);
         return NULL;
      }
      json_object_array_add(out, kept);
   }
   return out;
}

struct json_object *llm_tool_images_without(struct json_object *history) {
   if (!is_array(history)) {
      return NULL;
   }
   const size_t n = json_object_array_length(history);
   struct json_object *out = json_object_new_array_ext((int)n);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content = content_of(msg);
      struct json_object *kept = NULL;
      const bool tool = is(str_of(msg, "role"), "tool");
      /* A tool message: its images go.  Another message: only those inside
       * its tool results (a question's own images are its provider's to
       * judge). */
      if (is_array(content) &&
          (tool ? llm_parts_have_image(content) : results_have_image(content))) {
         struct json_object *parts = parts_without(content, tool);
         kept = parts ? with_content(msg, parts) : NULL;
      } else {
         kept = json_object_get(msg);
      }
      if (!kept) {
         json_object_put(out);
         return NULL;
      }
      json_object_array_add(out, kept);
   }
   return out;
}

/* @p msg's content joined as its text (a string, or its text parts). */
static char *text_of(struct json_object *msg) {
   return llm_claude_tool_result_text(msg);
}

/* Append @p msg's images (image_url parts) to @p images, after its label.
 * False on out of memory. */
static bool add_images(struct json_object *msg, struct json_object *images) {
   struct json_object *content = content_of(msg);
   const size_t n = json_object_array_length(content);
   bool labelled = false;
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      if (!llm_part_is_image(part)) {
         continue;
      }
      if (!labelled) {
         char label[160];
         const char *id = str_of(msg, "tool_call_id");
         snprintf(label, sizeof(label), LLM_TOOL_IMAGES_CHAT_LABEL_FMT, id ? id : "");
         struct json_object *t = text_part(label);
         if (!t) {
            return false;
         }
         json_object_array_add(images, t);
         labelled = true;
      }
      /* An image_url part's own {url} object is shared, not copied (a data
       * URI runs to megabytes); only a Claude image is built into one. */
      struct json_object *image_url = NULL;
      if (is(str_of(part, "type"), "image_url")) {
         struct json_object *own = NULL;
         if (json_object_object_get_ex(part, "image_url", &own) && str_of(own, "url")) {
            image_url = json_object_get(own);
         }
      } else {
         char *url = data_url_of(part);
         image_url = url ? json_object_new_object() : NULL;
         if (url && !image_url) {
            free(url);
            return false;
         }
         if (image_url) {
            json_object_object_add(image_url, "url", json_object_new_string(url));
         }
         free(url);
      }
      if (!image_url) {
         continue; /* an image no provider takes (llm_claude_image_data_url) */
      }
      struct json_object *image = json_object_new_object();
      if (!image) {
         json_object_put(image_url);
         return false;
      }
      json_object_object_add(image, "type", json_object_new_string("image_url"));
      json_object_object_add(image, "image_url", image_url);
      json_object_array_add(images, image);
   }
   return true;
}

/* The user message carrying a run's @p images (taken), or NULL when none. */
static struct json_object *images_message(struct json_object *images, bool *oom) {
   if (json_object_array_length(images) == 0) {
      json_object_put(images);
      return NULL;
   }
   struct json_object *msg = json_object_new_object();
   if (!msg) {
      json_object_put(images);
      *oom = true;
      return NULL;
   }
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", images);
   return msg;
}

struct json_object *llm_tool_images_render_chat(struct json_object *history) {
   if (!is_array(history)) {
      return NULL;
   }
   const size_t n = json_object_array_length(history);
   struct json_object *out = json_object_new_array_ext((int)n);
   struct json_object *images = NULL; /* the current run's, while in one */
   bool oom = !out;
   for (size_t i = 0; !oom && i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      const bool tool = is(str_of(msg, "role"), "tool");
      if (!tool && images) {
         struct json_object *m = images_message(images, &oom);
         images = NULL;
         if (m) {
            json_object_array_add(out, m);
         }
      }
      if (!tool || !is_array(content_of(msg))) {
         json_object_array_add(out, json_object_get(msg));
         continue;
      }
      if (!images && !(images = json_object_new_array())) {
         oom = true;
         break;
      }
      char *text = text_of(msg);
      struct json_object *flat = text ? with_content(msg, json_object_new_string(text)) : NULL;
      free(text);
      if (!flat || !add_images(msg, images)) {
         json_object_put(flat);
         oom = true;
         break;
      }
      json_object_array_add(out, flat);
   }
   if (!oom && images) {
      struct json_object *m = images_message(images, &oom);
      images = NULL;
      if (m) {
         json_object_array_add(out, m);
      }
   }
   json_object_put(images);
   if (oom) {
      json_object_put(out);
      return NULL;
   }
   return out;
}

struct json_object *llm_tool_images_responses_output(struct json_object *content) {
   if (!is_array(content)) {
      const char *s = content ? json_object_get_string(content) : NULL;
      return json_object_new_string(s ? s : "");
   }
   if (!llm_parts_have_image(content)) {
      struct json_object *holder = json_object_new_object();
      if (!holder) {
         return NULL;
      }
      json_object_object_add(holder, "content", json_object_get(content));
      char *text = text_of(holder);
      json_object_put(holder);
      struct json_object *out = text ? json_object_new_string(text) : NULL;
      free(text);
      return out;
   }
   const size_t n = json_object_array_length(content);
   struct json_object *out = json_object_new_array_ext((int)n);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      struct json_object *item = NULL;
      if (llm_part_is_image(part)) {
         /* An image_url part's URI string is shared, not copied. */
         struct json_object *own = NULL;
         struct json_object *uri = NULL;
         if (is(str_of(part, "type"), "image_url") &&
             json_object_object_get_ex(part, "image_url", &own) &&
             json_object_object_get_ex(own, "url", &uri) &&
             json_object_is_type(uri, json_type_string)) {
            uri = json_object_get(uri);
         } else {
            char *url = data_url_of(part);
            if (!url) {
               continue; /* an image no provider takes */
            }
            uri = json_object_new_string(url);
            free(url);
         }
         if (!uri) {
            json_object_put(out);
            return NULL;
         }
         item = json_object_new_object();
         if (item) {
            json_object_object_add(item, "type", json_object_new_string("input_image"));
            json_object_object_add(item, "image_url", uri);
         } else {
            json_object_put(uri);
         }
      } else if (is(str_of(part, "type"), "text") && str_of(part, "text")) {
         item = json_object_new_object();
         if (item) {
            json_object_object_add(item, "type", json_object_new_string("input_text"));
            json_object_object_add(item, "text", json_object_new_string(str_of(part, "text")));
         }
      } else {
         continue;
      }
      if (!item) {
         json_object_put(out);
         return NULL;
      }
      json_object_array_add(out, item);
   }
   return out;
}

bool llm_tool_images_range_over(struct json_object *history,
                                int start,
                                int end,
                                const llm_image_limit_t *limit,
                                float fraction) {
   if (!history || !limit) {
      return false;
   }
   int have = 0;
   int64_t have_bytes = 0;
   llm_history_image_totals(history, start, end, &have, &have_bytes);
   if (have == 0) {
      return false;
   }
   /* At the seam, room for one more image is the least a turn needs. */
   return (float)(have + 1) > (float)limit->count * fraction ||
          (float)have_bytes > (float)limit->bytes * fraction;
}

bool llm_tool_images_history_over(struct json_object *history,
                                  const llm_image_limit_t *limit,
                                  float fraction) {
   return llm_tool_images_range_over(history, 0, -1, limit, fraction);
}

/* ---- a history without its images, for a model that takes none (moved from
 * llm_tools.c) ---- */

#define VISION_STRIP_TEXT_BUF_MAX 8192

/* Whether @p msg's own content (not a tool result's) holds an image. */
static bool has_own_image(struct json_object *msg) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return false;
   }
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n; i++) {
      if (llm_part_is_image(json_object_array_get_idx(content, i))) {
         return true;
      }
   }
   return false;
}

/* @p msg (whose own content holds an image) as text: its text parts joined,
 * then a note that an image was there.  Every other key kept (a tool
 * message's call id).  NULL on out of memory. */
static struct json_object *flatten_vision_message(struct json_object *msg) {
   struct json_object *content = json_object_object_get(msg, "content");
   const size_t n = json_object_array_length(content);
   char text_buffer[VISION_STRIP_TEXT_BUF_MAX] = "";
   size_t text_len = 0;
   for (size_t j = 0; j < n; j++) {
      struct json_object *elem = json_object_array_get_idx(content, j);
      struct json_object *type_obj = NULL;
      struct json_object *text_obj = NULL;
      if (!json_object_object_get_ex(elem, "type", &type_obj) ||
          strcmp(json_object_get_string(type_obj), "text") != 0 ||
          !json_object_object_get_ex(elem, "text", &text_obj)) {
         continue;
      }
      const char *text = json_object_get_string(text_obj);
      if (text && text_len < sizeof(text_buffer) - 1) {
         if (text_len > 0) {
            text_buffer[text_len++] = ' ';
         }
         size_t copy_len = strlen(text);
         if (text_len + copy_len >= sizeof(text_buffer)) {
            copy_len = sizeof(text_buffer) - text_len - 1;
         }
         memcpy(text_buffer + text_len, text, copy_len);
         text_len += copy_len;
         text_buffer[text_len] = '\0';
      }
   }
   utf8_trim_incomplete(text_buffer);

   struct json_object *new_msg = json_object_new_object();
   if (!new_msg) {
      return NULL;
   }
   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") != 0) {
         json_object_object_add(new_msg, key, json_object_get(val));
      }
   }
   if (text_len > 0) {
      char combined[VISION_STRIP_TEXT_BUF_MAX + 128];
      snprintf(combined, sizeof(combined), "%s [An image was shared earlier]", text_buffer);
      json_object_object_add(new_msg, "content", json_object_new_string(combined));
   } else {
      json_object_object_add(new_msg, "content",
                             json_object_new_string("[An image was shared here]"));
   }
   return new_msg;
}

struct json_object *llm_history_strip_vision_content(struct json_object *history) {
   if (!history || json_object_get_type(history) != json_type_array) {
      return NULL;
   }
   /* A tool result's images, at any depth: the no-vision text in their place,
    * the result's shape kept (llm_tool_images_render.h). */
   struct json_object *without = llm_tool_images_without(history);
   if (!without) {
      return NULL;
   }
   const size_t len = json_object_array_length(without);
   bool has_vision = false;
   for (size_t i = 0; i < len && !has_vision; i++) {
      has_vision = has_own_image(json_object_array_get_idx(without, i));
   }
   if (!has_vision) {
      return without;
   }

   OLOG_INFO("Stripping vision content from history");
   struct json_object *sanitized = json_object_new_array_ext((int)len);
   for (size_t i = 0; sanitized && i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(without, i);
      struct json_object *kept = has_own_image(msg) ? flatten_vision_message(msg)
                                                    : json_object_get(msg);
      if (!kept) {
         json_object_put(sanitized);
         sanitized = NULL;
         break;
      }
      json_object_array_add(sanitized, kept);
   }
   json_object_put(without);
   return sanitized;
}
