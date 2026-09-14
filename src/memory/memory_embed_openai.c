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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * OpenAI-compatible Embedding Provider
 *
 * Uses the /v1/embeddings endpoint (compatible with OpenAI, Azure, etc.).
 * Default model: text-embedding-3-small (1536 dimensions).
 */

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "core/curl_buffer.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "utils/string_utils.h"

/* =============================================================================
 * State
 * ============================================================================= */

static char s_endpoint[256];
static char s_model[64];
static char s_api_key[256];

/* =============================================================================
 * Provider Implementation
 * ============================================================================= */

static int openai_init(const char *endpoint, const char *model, const char *api_key) {
   if (!api_key || api_key[0] == '\0') {
      OLOG_ERROR("memory_embed_openai: API key required");
      return FAILURE;
   }

   if (endpoint && endpoint[0]) {
      safe_strscpy(s_endpoint, endpoint);
   } else {
      safe_strscpy(s_endpoint, "https://api.openai.com");
   }
   s_endpoint[sizeof(s_endpoint) - 1] = '\0';

   if (model && model[0]) {
      safe_strscpy(s_model, model);
   } else {
      safe_strscpy(s_model, "text-embedding-3-small");
   }
   s_model[sizeof(s_model) - 1] = '\0';

   safe_strscpy(s_api_key, api_key);

   OLOG_INFO("memory_embed_openai: initialized (endpoint: %s, model: %s)", s_endpoint, s_model);
   return 0;
}

static void openai_cleanup(void) {
   /* Clear API key from memory */
   memset(s_api_key, 0, sizeof(s_api_key));
   OLOG_INFO("memory_embed_openai: cleanup");
}

static int openai_embed(const char *text, float *out, int max_dims, int *out_dims) {
   if (!text || !out || !out_dims)
      return FAILURE;

   *out_dims = 0;

   /* Build URL: {endpoint}/v1/embeddings */
   char url[320];
   snprintf(url, sizeof(url), "%s/v1/embeddings", s_endpoint);

   /* Build JSON request */
   struct json_object *req = json_object_new_object();
   json_object_object_add(req, "model", json_object_new_string(s_model));
   json_object_object_add(req, "input", json_object_new_string(text));
   const char *json_str = json_object_to_json_string(req);

   /* HTTP POST */
   CURL *curl = curl_easy_init();
   if (!curl) {
      json_object_put(req);
      return FAILURE;
   }

   curl_buffer_t buf;
   curl_buffer_init(&buf);

   char auth_header[300];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", s_api_key);

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");
   headers = curl_slist_append(headers, auth_header);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

   CURLcode res = curl_easy_perform(curl);

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);
   json_object_put(req);

   if (res != CURLE_OK) {
      OLOG_ERROR("memory_embed_openai: HTTP request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      return FAILURE;
   }

   /* Parse response: {"data": [{"embedding": [0.1, 0.2, ...]}]} */
   struct json_object *resp = json_tokener_parse(buf.data);
   curl_buffer_free(&buf);

   if (!resp) {
      OLOG_ERROR("memory_embed_openai: failed to parse response");
      return FAILURE;
   }

   struct json_object *data_arr;
   if (!json_object_object_get_ex(resp, "data", &data_arr)) {
      /* Check for error */
      struct json_object *err_obj;
      if (json_object_object_get_ex(resp, "error", &err_obj)) {
         struct json_object *msg;
         if (json_object_object_get_ex(err_obj, "message", &msg)) {
            OLOG_ERROR("memory_embed_openai: API error: %s", json_object_get_string(msg));
         }
      }
      json_object_put(resp);
      return FAILURE;
   }

   struct json_object *first = json_object_array_get_idx(data_arr, 0);
   if (!first) {
      json_object_put(resp);
      return FAILURE;
   }

   struct json_object *emb_arr;
   if (!json_object_object_get_ex(first, "embedding", &emb_arr)) {
      json_object_put(resp);
      return FAILURE;
   }

   int dims = json_object_array_length(emb_arr);
   if (dims > max_dims)
      dims = max_dims;

   for (int i = 0; i < dims; i++) {
      struct json_object *val = json_object_array_get_idx(emb_arr, i);
      out[i] = (float)json_object_get_double(val);
   }

   *out_dims = dims;
   json_object_put(resp);
   return 0;
}

/* =============================================================================
 * Provider Registration
 * ============================================================================= */

const embedding_provider_t embedding_provider_openai = {
   .name = "openai",
   .init = openai_init,
   .cleanup = openai_cleanup,
   .embed = openai_embed,
};
