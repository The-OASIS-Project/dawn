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
 * Local LLM Provider Detection and Management
 */

#include "llm/llm_local_provider.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "llm/llm_context.h"
#include "logging.h"
#include "tools/curl_buffer.h"

/* =============================================================================
 * Configuration Access
 * ============================================================================= */

extern dawn_config_t g_config;

/* =============================================================================
 * Module State (Static Allocation)
 * ============================================================================= */

static struct {
   bool initialized;
   pthread_mutex_t mutex;

   /* Provider detection */
   local_provider_t provider;
   bool provider_detected;

   /* Context size cache */
   int context_size;
   bool context_cached;
   char cached_model[LLM_LOCAL_MODEL_NAME_MAX];

   /* Model list cache */
   llm_local_model_t models[LLM_LOCAL_MAX_MODELS];
   size_t model_count;
   time_t models_fetched_at;

} s_state = {
   .initialized = false,
   .provider = LOCAL_PROVIDER_UNKNOWN,
   .provider_detected = false,
   .context_size = 0,
   .context_cached = false,
   .cached_model = { 0 },
   .model_count = 0,
   .models_fetched_at = 0,
};

/* =============================================================================
 * CURL Response Buffer - Uses shared curl_buffer.h
 * ============================================================================= */

#define LLM_LOCAL_MAX_RESPONSE_SIZE (64 * 1024) /* 64KB max response */

/* =============================================================================
 * HTTP Helper Functions
 * ============================================================================= */

/* Timeout bounds for HTTP requests */
#define HTTP_TIMEOUT_MIN_MS 100
#define HTTP_TIMEOUT_MAX_MS 30000

/**
 * @brief Perform HTTP GET request
 */
static int http_get(const char *url, int timeout_ms, curl_buffer_t *response) {
   CURL *curl = curl_easy_init();
   if (!curl) {
      return -1;
   }

   /* Clamp timeout to valid bounds */
   if (timeout_ms < HTTP_TIMEOUT_MIN_MS)
      timeout_ms = HTTP_TIMEOUT_MIN_MS;
   if (timeout_ms > HTTP_TIMEOUT_MAX_MS)
      timeout_ms = HTTP_TIMEOUT_MAX_MS;

   curl_buffer_init_with_max(response, LLM_LOCAL_MAX_RESPONSE_SIZE);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_ms / 2));
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      curl_buffer_free(response);
      return -1;
   }

   if (http_code >= 400) {
      curl_buffer_free(response);
      return (int)http_code;
   }

   return 0;
}

/**
 * @brief Perform HTTP POST request with JSON body
 */
static int http_post_json(const char *url,
                          const char *json_body,
                          int timeout_ms,
                          curl_buffer_t *response) {
   CURL *curl = curl_easy_init();
   if (!curl) {
      return -1;
   }

   /* Clamp timeout to valid bounds */
   if (timeout_ms < HTTP_TIMEOUT_MIN_MS)
      timeout_ms = HTTP_TIMEOUT_MIN_MS;
   if (timeout_ms > HTTP_TIMEOUT_MAX_MS)
      timeout_ms = HTTP_TIMEOUT_MAX_MS;

   curl_buffer_init_with_max(response, LLM_LOCAL_MAX_RESPONSE_SIZE);

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_ms / 2));
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      curl_buffer_free(response);
      return -1;
   }

   if (http_code >= 400) {
      curl_buffer_free(response);
      return (int)http_code;
   }

   return 0;
}

/**
 * @brief Probe endpoint with GET request (success = 200 OK)
 */
static bool probe_endpoint(const char *base_url, const char *path, int timeout_ms) {
   char url[512];
   snprintf(url, sizeof(url), "%s%s", base_url, path);

   curl_buffer_t response;
   int result = http_get(url, timeout_ms, &response);
   curl_buffer_free(&response);

   return (result == 0);
}

/* =============================================================================
 * URL Helpers
 * ============================================================================= */

/**
 * @brief Get clean base URL (remove any path suffixes like /v1/chat/completions)
 */
static void get_base_url(const char *endpoint, char *base_url, size_t base_url_len) {
   strncpy(base_url, endpoint, base_url_len - 1);
   base_url[base_url_len - 1] = '\0';

   /* Remove common path suffixes */
   char *v1_pos = strstr(base_url, "/v1/");
   if (v1_pos) {
      *v1_pos = '\0';
   }

   /* Remove trailing slash */
   size_t len = strlen(base_url);
   if (len > 0 && base_url[len - 1] == '/') {
      base_url[len - 1] = '\0';
   }
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int llm_local_provider_init(void) {
   if (s_state.initialized) {
      return 0;
   }

   if (pthread_mutex_init(&s_state.mutex, NULL) != 0) {
      LOG_ERROR("llm_local_provider: Failed to initialize mutex");
      return 1;
   }

   s_state.provider = LOCAL_PROVIDER_UNKNOWN;
   s_state.provider_detected = false;
   s_state.context_size = 0;
   s_state.context_cached = false;
   s_state.cached_model[0] = '\0';
   s_state.model_count = 0;
   s_state.models_fetched_at = 0;
   memset(s_state.models, 0, sizeof(s_state.models));

   s_state.initialized = true;
   LOG_INFO("llm_local_provider: Initialized");

   return 0;
}

void llm_local_provider_cleanup(void) {
   if (!s_state.initialized) {
      return;
   }

   pthread_mutex_destroy(&s_state.mutex);
   s_state.initialized = false;
   LOG_INFO("llm_local_provider: Cleaned up");
}

/* =============================================================================
 * Provider Detection Functions
 * ============================================================================= */

const char *llm_local_provider_name(local_provider_t provider) {
   switch (provider) {
      case LOCAL_PROVIDER_OLLAMA:
         return "Ollama";
      case LOCAL_PROVIDER_LLAMA_CPP:
         return "llama.cpp";
      case LOCAL_PROVIDER_GENERIC:
         return "Generic";
      case LOCAL_PROVIDER_UNKNOWN:
      default:
         return "Unknown";
   }
}

local_provider_t llm_local_get_provider(void) {
   pthread_mutex_lock(&s_state.mutex);
   local_provider_t provider = s_state.provider;
   pthread_mutex_unlock(&s_state.mutex);
   return provider;
}

void llm_local_invalidate_cache(void) {
   pthread_mutex_lock(&s_state.mutex);
   s_state.provider_detected = false;
   s_state.context_cached = false;
   pthread_mutex_unlock(&s_state.mutex);
   LOG_INFO("llm_local_provider: Cache invalidated");
}

local_provider_t llm_local_detect_provider(const char *endpoint) {
   if (!endpoint || endpoint[0] == '\0') {
      return LOCAL_PROVIDER_UNKNOWN;
   }

   pthread_mutex_lock(&s_state.mutex);

   /* Return cached result if available */
   if (s_state.provider_detected) {
      local_provider_t cached = s_state.provider;
      pthread_mutex_unlock(&s_state.mutex);
      return cached;
   }

   pthread_mutex_unlock(&s_state.mutex);

   /* Check config override first */
   const char *config_provider = g_config.llm.local.provider;
   if (config_provider[0] != '\0' && strcmp(config_provider, "auto") != 0) {
      local_provider_t override = LOCAL_PROVIDER_UNKNOWN;

      if (strcmp(config_provider, "ollama") == 0) {
         override = LOCAL_PROVIDER_OLLAMA;
      } else if (strcmp(config_provider, "llama_cpp") == 0) {
         override = LOCAL_PROVIDER_LLAMA_CPP;
      } else if (strcmp(config_provider, "generic") == 0) {
         override = LOCAL_PROVIDER_GENERIC;
      }

      if (override != LOCAL_PROVIDER_UNKNOWN) {
         pthread_mutex_lock(&s_state.mutex);
         s_state.provider = override;
         s_state.provider_detected = true;
         pthread_mutex_unlock(&s_state.mutex);

         LOG_INFO("llm_local_provider: Using config override: %s",
                  llm_local_provider_name(override));
         return override;
      }
   }

   /* Get clean base URL */
   char base_url[512];
   get_base_url(endpoint, base_url, sizeof(base_url));

   /* Probe endpoints to detect provider */
   local_provider_t detected = LOCAL_PROVIDER_GENERIC;

   /* Try llama.cpp /props first - this endpoint is unique to llama.cpp.
    * Must check before /api/tags because llama.cpp may have Ollama compatibility. */
   if (probe_endpoint(base_url, "/props", LLM_LOCAL_PROBE_TIMEOUT_MS)) {
      detected = LOCAL_PROVIDER_LLAMA_CPP;
      LOG_INFO("llm_local_provider: Detected llama.cpp at %s", base_url);
   }
   /* Try Ollama /api/tags */
   else if (probe_endpoint(base_url, "/api/tags", LLM_LOCAL_PROBE_TIMEOUT_MS)) {
      detected = LOCAL_PROVIDER_OLLAMA;
      LOG_INFO("llm_local_provider: Detected Ollama at %s", base_url);
   }
   /* Fallback to generic */
   else {
      LOG_INFO("llm_local_provider: Using generic OpenAI-compatible mode for %s", base_url);
   }

   /* Cache result */
   pthread_mutex_lock(&s_state.mutex);
   s_state.provider = detected;
   s_state.provider_detected = true;
   pthread_mutex_unlock(&s_state.mutex);

   return detected;
}

/* =============================================================================
 * Context Size Functions
 * ============================================================================= */

/**
 * @brief Try to get context size from Ollama /api/show endpoint
 */
static int try_ollama_context(const char *base_url, const char *model, int *ctx_out) {
   if (!model || model[0] == '\0') {
      return -1;
   }

   char url[512];
   snprintf(url, sizeof(url), "%s/api/show", base_url);

   /* Build JSON request body */
   struct json_object *req = json_object_new_object();
   json_object_object_add(req, "model", json_object_new_string(model));
   const char *json_body = json_object_to_json_string(req);

   curl_buffer_t response;
   int result = http_post_json(url, json_body, 3000, &response);
   json_object_put(req);

   if (result != 0 || !response.data) {
      curl_buffer_free(&response);
      return -1;
   }

   /* Parse response */
   struct json_object *root = json_tokener_parse(response.data);
   curl_buffer_free(&response);

   if (!root) {
      return -1;
   }

   int context_size = 0;

   /* Try model_info.context_length (newer Ollama versions) */
   struct json_object *model_info = NULL;
   struct json_object *ctx_obj = NULL;

   if (json_object_object_get_ex(root, "model_info", &model_info)) {
      /* Try various field names */
      if (json_object_object_get_ex(model_info, "context_length", &ctx_obj)) {
         context_size = json_object_get_int(ctx_obj);
      } else if (json_object_object_get_ex(model_info, "num_ctx", &ctx_obj)) {
         context_size = json_object_get_int(ctx_obj);
      }
   }

   /* Try parameters string (older format: "num_ctx 32768\n...") */
   if (context_size == 0) {
      struct json_object *params_obj = NULL;
      if (json_object_object_get_ex(root, "parameters", &params_obj)) {
         const char *params = json_object_get_string(params_obj);
         if (params) {
            const char *num_ctx = strstr(params, "num_ctx");
            if (num_ctx) {
               /* Skip "num_ctx " and parse number */
               const char *num_start = num_ctx + 7;
               while (*num_start && (*num_start == ' ' || *num_start == '\t')) {
                  num_start++;
               }
               if (*num_start) {
                  context_size = atoi(num_start);
               }
            }
         }
      }
   }

   json_object_put(root);

   if (context_size > 0) {
      *ctx_out = context_size;
      return 0;
   }

   return -1;
}

/**
 * @brief Try to get context size from llama.cpp /props endpoint
 */
static int try_llamacpp_context(const char *base_url, int *ctx_out) {
   char url[512];
   snprintf(url, sizeof(url), "%s/props", base_url);

   curl_buffer_t response;
   int result = http_get(url, 3000, &response);

   if (result != 0 || !response.data) {
      curl_buffer_free(&response);
      return -1;
   }

   struct json_object *root = json_tokener_parse(response.data);
   curl_buffer_free(&response);

   if (!root) {
      return -1;
   }

   int context_size = 0;
   struct json_object *settings = NULL;
   struct json_object *n_ctx_obj = NULL;

   /* Try default_generation_settings.n_ctx */
   if (json_object_object_get_ex(root, "default_generation_settings", &settings)) {
      if (json_object_object_get_ex(settings, "n_ctx", &n_ctx_obj)) {
         context_size = json_object_get_int(n_ctx_obj);
      }
   }

   /* Try top-level n_ctx */
   if (context_size == 0) {
      if (json_object_object_get_ex(root, "n_ctx", &n_ctx_obj)) {
         context_size = json_object_get_int(n_ctx_obj);
      }
   }

   json_object_put(root);

   if (context_size > 0) {
      *ctx_out = context_size;
      return 0;
   }

   return -1;
}

int llm_local_query_context_size(const char *endpoint, const char *model) {
   if (!endpoint || endpoint[0] == '\0') {
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   /* Get clean base URL */
   char base_url[512];
   get_base_url(endpoint, base_url, sizeof(base_url));

   /* Detect provider if not already done */
   local_provider_t provider = llm_local_detect_provider(endpoint);

   int context_size = 0;

   /* Try provider-specific query */
   if (provider == LOCAL_PROVIDER_OLLAMA && model && model[0] != '\0') {
      if (try_ollama_context(base_url, model, &context_size) == 0) {
         LOG_INFO("llm_local_provider: Ollama context size for %s: %d tokens", model, context_size);
         return context_size;
      }
   }

   /* Try llama.cpp /props (works for llama.cpp and as fallback) */
   if (try_llamacpp_context(base_url, &context_size) == 0) {
      LOG_INFO("llm_local_provider: llama.cpp context size: %d tokens", context_size);
      return context_size;
   }

   LOG_WARNING("llm_local_provider: Failed to query context size, using default %d",
               LLM_CONTEXT_DEFAULT_LOCAL);
   return LLM_CONTEXT_DEFAULT_LOCAL;
}

/* =============================================================================
 * Model Listing Functions
 * ============================================================================= */

bool llm_local_is_valid_model_name(const char *name) {
   if (!name || name[0] == '\0') {
      return false;
   }

   for (const char *p = name; *p; p++) {
      if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.' && *p != ':' &&
          *p != '/') {
         return false;
      }
   }

   return true;
}

void llm_local_invalidate_models_cache(void) {
   pthread_mutex_lock(&s_state.mutex);
   s_state.model_count = 0;
   s_state.models_fetched_at = 0;
   pthread_mutex_unlock(&s_state.mutex);
   LOG_INFO("llm_local_provider: Model cache invalidated");
}

/**
 * @brief Parse Ollama /api/tags response
 */
static int parse_ollama_models(const char *json_str, llm_local_model_t *models, size_t max_models) {
   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      return -1;
   }

   struct json_object *models_arr = NULL;
   if (!json_object_object_get_ex(root, "models", &models_arr)) {
      json_object_put(root);
      return -1;
   }

   int count = 0;
   int arr_len = json_object_array_length(models_arr);

   for (int i = 0; i < arr_len && count < (int)max_models; i++) {
      struct json_object *model_obj = json_object_array_get_idx(models_arr, i);
      struct json_object *name_obj = NULL;

      if (json_object_object_get_ex(model_obj, "name", &name_obj)) {
         const char *name = json_object_get_string(name_obj);
         if (name && llm_local_is_valid_model_name(name)) {
            strncpy(models[count].name, name, LLM_LOCAL_MODEL_NAME_MAX - 1);
            models[count].name[LLM_LOCAL_MODEL_NAME_MAX - 1] = '\0';
            models[count].loaded = false; /* Ollama doesn't indicate this in /api/tags */
            count++;
         }
      }
   }

   json_object_put(root);
   return count;
}

/**
 * @brief Parse OpenAI-compatible /v1/models response
 */
static int parse_openai_models(const char *json_str, llm_local_model_t *models, size_t max_models) {
   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      return -1;
   }

   struct json_object *data_arr = NULL;
   if (!json_object_object_get_ex(root, "data", &data_arr)) {
      json_object_put(root);
      return -1;
   }

   int count = 0;
   int arr_len = json_object_array_length(data_arr);

   for (int i = 0; i < arr_len && count < (int)max_models; i++) {
      struct json_object *model_obj = json_object_array_get_idx(data_arr, i);
      struct json_object *id_obj = NULL;

      if (json_object_object_get_ex(model_obj, "id", &id_obj)) {
         const char *id = json_object_get_string(id_obj);
         if (id && llm_local_is_valid_model_name(id)) {
            strncpy(models[count].name, id, LLM_LOCAL_MODEL_NAME_MAX - 1);
            models[count].name[LLM_LOCAL_MODEL_NAME_MAX - 1] = '\0';
            models[count].loaded = true; /* llama.cpp only shows loaded model */
            count++;
         }
      }
   }

   json_object_put(root);
   return count;
}

int llm_local_list_models(const char *endpoint,
                          llm_local_model_t *models,
                          size_t max_models,
                          size_t *out_count) {
   if (!endpoint || endpoint[0] == '\0' || !models || max_models == 0 || !out_count) {
      return 1;
   }

   *out_count = 0;

   /* Check cache first */
   pthread_mutex_lock(&s_state.mutex);
   time_t now = time(NULL);
   if (s_state.model_count > 0 && (now - s_state.models_fetched_at) < LLM_LOCAL_MODEL_CACHE_TTL) {
      /* Return cached models */
      size_t count = (s_state.model_count < max_models) ? s_state.model_count : max_models;
      memcpy(models, s_state.models, count * sizeof(llm_local_model_t));
      pthread_mutex_unlock(&s_state.mutex);
      *out_count = count;
      return 0;
   }
   pthread_mutex_unlock(&s_state.mutex);

   /* Get clean base URL */
   char base_url[512];
   get_base_url(endpoint, base_url, sizeof(base_url));

   /* Detect provider */
   local_provider_t provider = llm_local_detect_provider(endpoint);

   char url[544]; /* base_url + "/v1/models" suffix */
   curl_buffer_t response;
   int result;
   int count = -1;

   if (provider == LOCAL_PROVIDER_OLLAMA) {
      /* GET /api/tags for Ollama */
      snprintf(url, sizeof(url), "%s/api/tags", base_url);
      result = http_get(url, 5000, &response);

      if (result == 0 && response.data) {
         count = parse_ollama_models(response.data, models, max_models);
      }
   } else {
      /* GET /v1/models for llama.cpp and generic */
      snprintf(url, sizeof(url), "%s/v1/models", base_url);
      result = http_get(url, 5000, &response);

      if (result == 0 && response.data) {
         count = parse_openai_models(response.data, models, max_models);
      }
   }

   curl_buffer_free(&response);

   /* Update cache and return on success */
   if (count >= 0) {
      pthread_mutex_lock(&s_state.mutex);
      size_t cache_count = ((size_t)count < LLM_LOCAL_MAX_MODELS) ? (size_t)count
                                                                  : LLM_LOCAL_MAX_MODELS;
      memcpy(s_state.models, models, cache_count * sizeof(llm_local_model_t));
      s_state.model_count = cache_count;
      s_state.models_fetched_at = now;
      pthread_mutex_unlock(&s_state.mutex);

      LOG_INFO("llm_local_provider: Found %d models from %s", count,
               llm_local_provider_name(provider));

      *out_count = (size_t)count;
      return 0;
   }

   return 1;
}
