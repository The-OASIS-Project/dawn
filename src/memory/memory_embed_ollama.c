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
 * Ollama Embedding Provider
 *
 * Uses Ollama's /api/embed endpoint for embedding generation.
 * Default model: all-minilm (384 dimensions).
 */

#include <curl/curl.h>
#include <json-c/json.h>
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

/* =============================================================================
 * Provider Implementation
 * ============================================================================= */

static int ollama_init(const char *endpoint, const char *model, const char *api_key) {
   (void)api_key; /* Ollama doesn't use API keys */

   if (endpoint && endpoint[0]) {
      safe_strscpy(s_endpoint, endpoint);
   } else {
      safe_strscpy(s_endpoint, "http://localhost:11434");
   }
   s_endpoint[sizeof(s_endpoint) - 1] = '\0';

   if (model && model[0]) {
      safe_strscpy(s_model, model);
   } else {
      safe_strscpy(s_model, "all-minilm");
   }
   s_model[sizeof(s_model) - 1] = '\0';

   OLOG_INFO("memory_embed_ollama: initialized (endpoint: %s, model: %s)", s_endpoint, s_model);
   return 0;
}

static void ollama_cleanup(void) {
   OLOG_INFO("memory_embed_ollama: cleanup");
}

static int ollama_embed(const char *text, float *out, int max_dims, int *out_dims) {
   if (!text || !out || !out_dims)
      return FAILURE;

   *out_dims = 0;

   /* Build URL: {endpoint}/api/embed */
   char url[320];
   snprintf(url, sizeof(url), "%s/api/embed", s_endpoint);

   /* Build JSON request: {"model": "...", "input": "..."} */
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

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");

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
      OLOG_ERROR("memory_embed_ollama: HTTP request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      return FAILURE;
   }

   /* Parse response: {"embeddings": [[0.1, 0.2, ...]]} */
   struct json_object *resp = json_tokener_parse(buf.data);
   curl_buffer_free(&buf);

   if (!resp) {
      OLOG_ERROR("memory_embed_ollama: failed to parse response");
      return FAILURE;
   }

   struct json_object *embeddings_arr;
   if (!json_object_object_get_ex(resp, "embeddings", &embeddings_arr)) {
      OLOG_ERROR("memory_embed_ollama: no 'embeddings' in response");
      json_object_put(resp);
      return FAILURE;
   }

   /* Get first embedding vector */
   struct json_object *first = json_object_array_get_idx(embeddings_arr, 0);
   if (!first) {
      json_object_put(resp);
      return FAILURE;
   }

   int dims = json_object_array_length(first);
   if (dims > max_dims)
      dims = max_dims;

   for (int i = 0; i < dims; i++) {
      struct json_object *val = json_object_array_get_idx(first, i);
      out[i] = (float)json_object_get_double(val);
   }

   *out_dims = dims;
   json_object_put(resp);
   return 0;
}

/* =============================================================================
 * Provider Registration
 * ============================================================================= */

const embedding_provider_t embedding_provider_ollama = {
   .name = "ollama",
   .init = ollama_init,
   .cleanup = ollama_cleanup,
   .embed = ollama_embed,
};
