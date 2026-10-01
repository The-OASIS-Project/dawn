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
 * Images a tool returns: checked, stored and shown.  See llm_tool_images.h.
 */

#include "llm/llm_tool_images.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/image_rehydrate.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "image_store.h"
#include "llm/llm_context.h"
#include "llm/llm_model_family.h"
#include "llm/llm_tool_images_render.h"
#include "logging.h"
#include "utils/string_utils.h"

_Static_assert(LLM_TOOLS_IMAGE_ID_LEN == IMAGE_ID_LEN, "a result's image id is an image id");

/* The image's type by its first bytes; NULL when it is none DAWN shows. */
static const char *sniff_mime(const unsigned char *b, size_t n) {
   static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
      return "image/jpeg";
   }
   if (n >= sizeof(png) && memcmp(b, png, sizeof(png)) == 0) {
      return "image/png";
   }
   if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) {
      return "image/gif";
   }
   if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) {
      return "image/webp";
   }
   return NULL;
}

/* Whether one more image of @p add_bytes (as sent) would take a request
 * holding @p have images of @p have_bytes past @p limit. */
static bool over_request_limit(int have,
                               int64_t have_bytes,
                               int64_t add_bytes,
                               const llm_image_limit_t *limit) {
   return have + 1 > limit->count || have_bytes + add_bytes > limit->bytes;
}

/* The user a capture in this call is kept for: the user the turn's
 * conversation is saved under.  0: none (a guest, or no session: kept in
 * memory only). */
static int capture_owner(void) {
   session_t *session = session_get_command_context();
   return session ? session_effective_user_id(session) : 0;
}

/* The result's text says why it holds no image. */
static void refuse(tool_result_t *result, const char *fmt, const char *why) {
   snprintf(result->result, LLM_TOOLS_RESULT_LEN, fmt, why);
}

bool llm_tool_images_ingest(char *base64, tool_result_t *result) {
   if (!result) {
      free(base64);
      return false;
   }
   result->vision_image = NULL;
   result->vision_image_size = 0;
   result->vision_image_id[0] = '\0';
   result->vision_image_owner = 0;
   if (!base64) {
      refuse(result, "Error: %s", "no image data");
      return false;
   }

   /* The configured upload cap applies to what arrives (an MQTT producer, or
    * something posing as one, controls these bytes). */
   const size_t b64_len = strlen(base64);
   if (b64_len > (size_t)g_config.vision.max_image_size_kb * 1024) {
      OLOG_WARNING("Tool image exceeds max_image_size_kb (%d KB, %zu bytes received): refused",
                   g_config.vision.max_image_size_kb, b64_len);
      free(base64);
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Image exceeds maximum size (%d KB)",
               g_config.vision.max_image_size_kb);
      return false;
   }

   size_t raw_len = 0;
   unsigned char *raw = ocp_base64_decode(base64, &raw_len);
   free(base64);
   base64 = NULL;
   const char *mime = raw ? sniff_mime(raw, raw_len) : NULL;
   if (!mime) {
      free(raw);
      refuse(result, "Error: %s", "the capture is not a JPEG, PNG, GIF or WebP image");
      return false;
   }
   if (raw_len > LLM_TOOL_IMAGE_MAX_BYTES) {
      OLOG_WARNING("Tool image of %zu bytes over the %d-byte limit: refused", raw_len,
                   LLM_TOOL_IMAGE_MAX_BYTES);
      free(raw);
      snprintf(result->result, LLM_TOOLS_RESULT_LEN,
               "Error: capture refused: the image is %zu KB, over the %d KB limit", raw_len / 1024,
               LLM_TOOL_IMAGE_MAX_BYTES / 1024);
      return false;
   }
   const int owner = capture_owner();
   if (owner > 0) {
      char id[IMAGE_ID_LEN] = "";
      const int rc = image_store_save_ex(owner, raw, raw_len, mime, IMAGE_SOURCE_CAPTURE,
                                         IMAGE_RETAIN_UNBOUND, id);
      if (rc != IMAGE_STORE_SUCCESS) {
         free(raw);
         OLOG_WARNING("Tool image for user %d not stored (%d)", owner, rc);
         refuse(result, "Image captured but not kept: %s.",
                rc == IMAGE_STORE_LIMIT_EXCEEDED ? "the user's stored captures are at their limit "
                                                   "(deleting conversations that hold captures "
                                                   "frees room)"
                : rc == IMAGE_STORE_TOO_LARGE    ? "the image is too large to keep"
                                                 : "the image store failed");
         return false;
      }
      safe_strscpy(result->vision_image_id, id);
      result->vision_image_owner = owner;
   }

   /* The bytes as checked, re-encoded (a guest's request shows these). */
   result->vision_image = ocp_base64_encode(raw, raw_len);
   free(raw);
   if (!result->vision_image) {
      refuse(result, "Error: %s", "memory allocation failed");
      result->vision_image_id[0] = '\0'; /* unbound: the store reclaims it */
      return false;
   }
   result->vision_image_size = strlen(result->vision_image) + 1;
   OLOG_INFO("Tool image: %zu bytes %s, %s%s", raw_len, mime,
             result->vision_image_id[0] ? "stored as " : "kept in memory", result->vision_image_id);
   return true;
}

/* Base64 characters that decode to the longest signature sniff_mime() reads
 * (12 bytes: WebP's RIFF....WEBP). */
#define SNIFF_BASE64_CHARS 16

/* @p base64's type by its first bytes, decoding only those. */
static const char *sniff_base64(const char *base64) {
   char head[SNIFF_BASE64_CHARS + 1];
   size_t n = strnlen(base64, SNIFF_BASE64_CHARS);
   n -= n % 4; /* whole base64 quanta only */
   memcpy(head, base64, n);
   head[n] = '\0';
   size_t raw_len = 0;
   unsigned char *raw = n ? ocp_base64_decode(head, &raw_len) : NULL;
   const char *mime = raw ? sniff_mime(raw, raw_len) : NULL;
   free(raw);
   return mime;
}

/* An image_url part of the image the result holds in memory, marked with the
 * stored image it is (@p id) when it was stored: the bytes the turn stored,
 * in the form a reload builds from the file (image_rehydrate_parts). */
static struct json_object *memory_part(const char *base64, const char *id) {
   const char *mime = sniff_base64(base64);
   if (!mime) {
      return NULL;
   }
   const size_t len = strlen("data:;base64,") + strlen(mime) + strlen(base64) + 1;
   char *uri = malloc(len);
   if (!uri) {
      return NULL;
   }
   snprintf(uri, len, "data:%s;base64,%s", mime, base64);
   struct json_object *part = json_object_new_object();
   struct json_object *image_url = json_object_new_object();
   if (part && image_url) {
      json_object_object_add(image_url, "url", json_object_new_string(uri));
      json_object_object_add(part, "type", json_object_new_string("image_url"));
      json_object_object_add(part, "image_url", image_url);
      if (id && id[0]) {
         json_object_object_add(part, IMAGE_PART_ID_KEY, json_object_new_string(id));
      }
   } else {
      json_object_put(part);
      json_object_put(image_url);
      part = NULL;
   }
   free(uri);
   return part;
}

struct json_object *llm_tool_images_result_content(const tool_result_t *result) {
   const char *text = tool_result_content(result);
   struct json_object *parts = NULL;
   /* From memory: the turn holds the bytes it stored (no re-read). */
   if (result && result->vision_image) {
      struct json_object *part = memory_part(result->vision_image, result->vision_image_id);
      parts = part ? json_object_new_array() : NULL;
      if (parts) {
         json_object_array_add(parts, part);
      } else {
         json_object_put(part);
      }
   }
   if (!parts) {
      return json_object_new_string(text);
   }

   struct json_object *content = json_object_new_array();
   struct json_object *text_part = json_object_new_object();
   if (!content || !text_part) {
      json_object_put(content);
      json_object_put(text_part);
      json_object_put(parts);
      return NULL;
   }
   json_object_object_add(text_part, "type", json_object_new_string("text"));
   json_object_object_add(text_part, "text", json_object_new_string(text));
   json_object_array_add(content, text_part);
   const size_t n = json_object_array_length(parts);
   for (size_t i = 0; i < n; i++) {
      json_object_array_add(content, json_object_get(json_object_array_get_idx(parts, i)));
   }
   json_object_put(parts);
   return content;
}

bool llm_tool_images_request_limit(llm_type_t type,
                                   cloud_provider_t provider,
                                   const char *model,
                                   llm_image_limit_t *out) {
   char id[LLM_MODEL_NAME_MAX];
   const char *key = "other";
   switch (llm_model_route(type, provider, model, id, sizeof(id))) {
      case LLM_FAMILY_LOCAL:
         key = "local";
         break;
      case LLM_FAMILY_ANTHROPIC:
         key = llm_context_get_size(type, provider, model) <= LLM_TOOL_IMAGES_SMALL_WINDOW
                   ? "anthropic_200k"
                   : "anthropic";
         break;
      case LLM_FAMILY_OPENAI:
         key = "openai";
         break;
      case LLM_FAMILY_GEMINI:
         key = "gemini";
         break;
      case LLM_FAMILY_OTHER:
         key = "other";
         break;
   }
   if (llm_capabilities_image_limit(key, out)) {
      return true;
   }
   out->count = LLM_TOOL_IMAGES_DEFAULT_COUNT;
   out->bytes = LLM_TOOL_IMAGES_DEFAULT_BYTES;
   return false;
}

/* The bytes @p r's image is sent as: its data URI's length. */
static int64_t sent_bytes(const tool_result_t *r) {
   return (int64_t)strlen(r->vision_image) + (int64_t)strlen("data:image/jpeg;base64,");
}

int llm_tool_images_cap_batch(struct json_object *history,
                              tool_result_list_t *results,
                              const llm_image_limit_t *limit) {
   if (!results || !limit) {
      return 0;
   }
   int have = 0;
   int64_t have_bytes = 0;
   llm_history_image_totals(history, 0, -1, &have, &have_bytes);
   int refused = 0;
   for (int i = 0; i < results->count; i++) {
      tool_result_t *r = &results->results[i];
      if (!r->vision_image) {
         continue;
      }
      const int64_t add = sent_bytes(r);
      if (!over_request_limit(have, have_bytes, add, limit)) {
         have++;
         have_bytes += add;
         continue;
      }
      OLOG_WARNING("Tool image refused: the request would carry %d image(s), %lld bytes, past "
                   "the model's limit (%d, %lld)",
                   have + 1, (long long)(have_bytes + add), limit->count, (long long)limit->bytes);
      if (r->vision_image_id[0] && r->vision_image_owner > 0) {
         (void)image_store_delete(r->vision_image_id, r->vision_image_owner); /* never bound */
      }
      free(r->vision_image);
      r->vision_image = NULL;
      r->vision_image_size = 0;
      r->vision_image_id[0] = '\0';
      r->vision_image_owner = 0;
      refuse(r, "Error: %s", LLM_TOOL_IMAGES_REFUSED_TEXT);
      refused++;
   }
   return refused;
}
