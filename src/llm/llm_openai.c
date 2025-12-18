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
 */

#include "llm/llm_openai.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn.h"
#include "llm/llm_interface.h"
#include "llm/llm_streaming.h"
#include "llm/sse_parser.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "ui/metrics.h"

// External CURL progress callback for interrupt support
extern int llm_curl_progress_callback(void *clientp,
                                      curl_off_t dltotal,
                                      curl_off_t dlnow,
                                      curl_off_t ultotal,
                                      curl_off_t ulnow);

/**
 * @brief Build HTTP headers for OpenAI API request
 *
 * @param api_key API key (NULL for local LLM, required for cloud)
 * @return CURL header list (caller must free with curl_slist_free_all)
 */
static struct curl_slist *build_openai_headers(const char *api_key) {
   struct curl_slist *headers = NULL;

   headers = curl_slist_append(headers, "Content-Type: application/json");

   if (api_key != NULL) {
      // Cloud OpenAI - needs auth header
      char auth_header[512];
      snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
      headers = curl_slist_append(headers, auth_header);
   }
   // Local LLM doesn't need auth header

   return headers;
}

char *llm_openai_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 char *vision_image,
                                 size_t vision_image_size,
                                 const char *base_url,
                                 const char *api_key) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";

   curl_buffer_t chunk;

   const char *payload = NULL;
   char *response = NULL;
   int total_tokens = 0;

   json_object *root = NULL;
   json_object *user_message = NULL;

   json_object *parsed_json = NULL;
   json_object *choices = NULL;
   json_object *first_choice = NULL;
   json_object *message = NULL;
   json_object *content = NULL;
   json_object *finish_reason = NULL;
   json_object *usage_obj = NULL;
   json_object *total_tokens_obj = NULL;

   // Root JSON Container
   root = json_object_new_object();

   // Model from config
   json_object_object_add(root, "model", json_object_new_string(g_config.llm.cloud.openai_model));

   // User message is now added by dawn.c before calling this function
   // If vision is provided, modify the last user message to include image
   if (vision_image != NULL && vision_image_size > 0) {
      int msg_count = json_object_array_length(conversation_history);
      if (msg_count > 0) {
         json_object *last_msg = json_object_array_get_idx(conversation_history, msg_count - 1);
         json_object *role_obj;
         if (json_object_object_get_ex(last_msg, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0) {
            // Last message is user message - add vision content
            json_object *content_array = json_object_new_array();
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text", json_object_new_string(input_text));
            json_object_array_add(content_array, text_obj);

            json_object *image_obj = json_object_new_object();
            json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

            json_object *image_url_obj = json_object_new_object();
            char *data_uri_prefix = "data:image/jpeg;base64,";
            size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_image) + 1;
            char *data_uri = malloc(data_uri_length);
            if (data_uri != NULL) {
               snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_image);
               json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
               free(data_uri);
            }
            json_object_object_add(image_obj, "image_url", image_url_obj);
            json_object_array_add(content_array, image_obj);

            json_object_object_add(last_msg, "content", content_array);
         }
      }
   }

   json_object_get(conversation_history);  // Increment refcount to prevent freeing caller's array
   json_object_object_add(root, "messages", conversation_history);

   // Max Tokens
   json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   curl_buffer_init(&chunk);

   // Check connection (handled in llm_interface.c for fallback)
   if (!llm_check_connection(base_url, 4)) {
      LOG_ERROR("URL did not return. Unavailable.");
      json_object_put(root);
      return NULL;
   }

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = build_openai_headers(api_key);

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);
      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

      // Enable progress callback for interruption support
      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);  // Enable progress callback
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);

      // Set low-speed timeout: abort if transfer drops below 1 byte/sec for 30 seconds
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 30L);

      // Set overall timeout from config (default 30000ms)
      if (g_config.network.llm_timeout_ms > 0) {
         curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, (long)g_config.network.llm_timeout_ms);
      }

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         if (res == CURLE_ABORTED_BY_CALLBACK) {
            LOG_INFO("LLM transfer interrupted by user");
         } else if (res == CURLE_OPERATION_TIMEDOUT) {
            LOG_ERROR("LLM request timed out (limit: %dms)", g_config.network.llm_timeout_ms);
         } else {
            LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
         }
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         curl_buffer_free(&chunk);
         json_object_put(root);
         return NULL;
      }

      // Check HTTP status code
      long http_code = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

      if (http_code != 200) {
         if (http_code == 401) {
            LOG_ERROR("OpenAI API: Invalid or missing API key (HTTP 401)");
         } else if (http_code == 403) {
            LOG_ERROR("OpenAI API: Access forbidden (HTTP 403) - check API key permissions");
         } else if (http_code == 429) {
            LOG_ERROR("OpenAI API: Rate limit exceeded (HTTP 429)");
         } else if (http_code >= 500) {
            LOG_ERROR("OpenAI API: Server error (HTTP %ld)", http_code);
         } else if (http_code != 0) {
            LOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
         }
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         curl_buffer_free(&chunk);
         json_object_put(root);
         return NULL;
      }

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
   }

   parsed_json = json_tokener_parse(chunk.data);
   if (!parsed_json) {
      LOG_ERROR("Failed to parse JSON response.");
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   if (!json_object_object_get_ex(parsed_json, "choices", &choices) ||
       json_object_get_type(choices) != json_type_array || json_object_array_length(choices) < 1) {
      LOG_ERROR("Error in parsing response: 'choices' missing or invalid.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   first_choice = json_object_array_get_idx(choices, 0);
   if (!first_choice) {
      LOG_ERROR("Error: 'choices' array is empty.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   if (!json_object_object_get_ex(first_choice, "message", &message) ||
       !json_object_object_get_ex(message, "content", &content)) {
      LOG_ERROR("Error: 'message' or 'content' field missing.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   // Optional: Safely access 'finish_reason'
   json_object_object_get_ex(first_choice, "finish_reason", &finish_reason);

   // Check for usage and cache information
   int input_tokens = 0;
   int output_tokens = 0;
   int cached_tokens = 0;
   if (json_object_object_get_ex(parsed_json, "usage", &usage_obj)) {
      if (json_object_object_get_ex(usage_obj, "total_tokens", &total_tokens_obj)) {
         total_tokens = json_object_get_int(total_tokens_obj);
         LOG_WARNING("Total tokens: %d", total_tokens);
      }

      // Extract input/output tokens
      json_object *prompt_tokens_obj = NULL;
      json_object *completion_tokens_obj = NULL;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens", &prompt_tokens_obj)) {
         input_tokens = json_object_get_int(prompt_tokens_obj);
      }
      if (json_object_object_get_ex(usage_obj, "completion_tokens", &completion_tokens_obj)) {
         output_tokens = json_object_get_int(completion_tokens_obj);
      }

      // Log OpenAI automatic caching info (if available)
      json_object *prompt_tokens_details = NULL;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens_details", &prompt_tokens_details)) {
         json_object *cached_tokens_obj = NULL;
         if (json_object_object_get_ex(prompt_tokens_details, "cached_tokens",
                                       &cached_tokens_obj)) {
            cached_tokens = json_object_get_int(cached_tokens_obj);
            if (cached_tokens > 0) {
               LOG_INFO("OpenAI cache hit: %d tokens cached (50%% savings)", cached_tokens);
            }
         }
      }

      // Record metrics
      metrics_record_llm_tokens(LLM_CLOUD, input_tokens, output_tokens, cached_tokens);
   }

   // Duplicate the response content string safely
   const char *content_str = json_object_get_string(content);
   if (!content_str) {
      LOG_ERROR("Error: 'content' field is empty or not a string.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }
   response = strdup(content_str);

   if ((finish_reason != NULL) && (strcmp(json_object_get_string(finish_reason), "stop") != 0)) {
      LOG_WARNING("OpenAI returned with finish_reason: %s", json_object_get_string(finish_reason));
   } else {
      LOG_INFO("Response finished properly.");
   }

   json_object_put(parsed_json);
   curl_buffer_free(&chunk);
   json_object_put(root);

   return response;
}

/**
 * @brief Context for streaming callbacks
 */
typedef struct {
   sse_parser_t *sse_parser;
   llm_stream_context_t *stream_ctx;
} openai_streaming_context_t;

/**
 * @brief CURL write callback for streaming responses
 *
 * Feeds incoming SSE data to the SSE parser, which processes it
 * and calls the LLM streaming callbacks.
 */
static size_t streaming_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
   size_t realsize = size * nmemb;
   openai_streaming_context_t *ctx = (openai_streaming_context_t *)userp;

   // Feed data to SSE parser
   sse_parser_feed(ctx->sse_parser, contents, realsize);

   return realsize;
}

/**
 * @brief SSE event callback that forwards events to LLM stream handler
 */
static void openai_sse_event_handler(const char *event_type,
                                     const char *event_data,
                                     void *userdata) {
   openai_streaming_context_t *ctx = (openai_streaming_context_t *)userdata;

   // Forward event data to LLM streaming parser
   llm_stream_handle_event(ctx->stream_ctx, event_data);
}

char *llm_openai_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
                                           llm_openai_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";

   const char *payload = NULL;
   char *response = NULL;

   json_object *root = NULL;

   // SSE and streaming contexts
   sse_parser_t *sse_parser = NULL;
   llm_stream_context_t *stream_ctx = NULL;
   openai_streaming_context_t streaming_ctx;

   // Root JSON Container
   root = json_object_new_object();

   // Model from config
   json_object_object_add(root, "model", json_object_new_string(g_config.llm.cloud.openai_model));

   // Enable streaming with usage reporting
   json_object_object_add(root, "stream", json_object_new_boolean(1));
   json_object *stream_opts = json_object_new_object();
   json_object_object_add(stream_opts, "include_usage", json_object_new_boolean(1));
   json_object_object_add(root, "stream_options", stream_opts);

   // Handle vision if provided
   if (vision_image != NULL && vision_image_size > 0) {
      int msg_count = json_object_array_length(conversation_history);
      if (msg_count > 0) {
         json_object *last_msg = json_object_array_get_idx(conversation_history, msg_count - 1);
         json_object *role_obj;
         if (json_object_object_get_ex(last_msg, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0) {
            // Last message is user message - add vision content
            json_object *content_array = json_object_new_array();
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text", json_object_new_string(input_text));
            json_object_array_add(content_array, text_obj);

            json_object *image_obj = json_object_new_object();
            json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

            json_object *image_url_obj = json_object_new_object();
            char *data_uri_prefix = "data:image/jpeg;base64,";
            size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_image) + 1;
            char *data_uri = malloc(data_uri_length);
            if (data_uri != NULL) {
               snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_image);
               json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
               free(data_uri);
            }
            json_object_object_add(image_obj, "image_url", image_url_obj);
            json_object_array_add(content_array, image_obj);

            json_object_object_add(last_msg, "content", content_array);
         }
      }
   }

   json_object_get(conversation_history);  // Increment refcount to prevent freeing caller's array
   json_object_object_add(root, "messages", conversation_history);

   // Max Tokens
   json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   // Check connection
   if (!llm_check_connection(base_url, 4)) {
      LOG_ERROR("URL did not return. Unavailable.");
      json_object_put(root);
      return NULL;
   }

   // Create streaming context
   stream_ctx = llm_stream_create(LLM_CLOUD, CLOUD_PROVIDER_OPENAI, chunk_callback,
                                  callback_userdata);
   if (!stream_ctx) {
      LOG_ERROR("Failed to create LLM stream context");
      json_object_put(root);
      return NULL;
   }

   // Create SSE parser
   sse_parser = sse_parser_create(openai_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      LOG_ERROR("Failed to create SSE parser");
      llm_stream_free(stream_ctx);
      json_object_put(root);
      return NULL;
   }

   // Setup streaming context
   streaming_ctx.sse_parser = sse_parser;
   streaming_ctx.stream_ctx = stream_ctx;

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = build_openai_headers(api_key);

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);

      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, streaming_write_callback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&streaming_ctx);

      // Enable progress callback for interruption support
      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);  // Enable progress callback
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);

      // Set low-speed timeout: abort if transfer drops below 1 byte/sec for 30 seconds
      // This catches hung/stalled streams from local LLM servers
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 30L);

      // Set overall timeout from config (default 30000ms)
      if (g_config.network.llm_timeout_ms > 0) {
         curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, (long)g_config.network.llm_timeout_ms);
      }

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         if (res == CURLE_ABORTED_BY_CALLBACK) {
            LOG_INFO("LLM transfer interrupted by user");
         } else if (res == CURLE_OPERATION_TIMEDOUT) {
            LOG_ERROR("LLM stream timed out (limit: %dms)", g_config.network.llm_timeout_ms);
         } else {
            LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
         }
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         return NULL;
      }

      // Check HTTP status code
      long http_code = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

      if (http_code != 200) {
         if (http_code == 401) {
            LOG_ERROR("OpenAI API: Invalid or missing API key (HTTP 401)");
         } else if (http_code == 403) {
            LOG_ERROR("OpenAI API: Access forbidden (HTTP 403) - check API key permissions");
         } else if (http_code == 429) {
            LOG_ERROR("OpenAI API: Rate limit exceeded (HTTP 429)");
         } else if (http_code >= 500) {
            LOG_ERROR("OpenAI API: Server error (HTTP %ld)", http_code);
         } else if (http_code != 0) {
            LOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
         }
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         return NULL;
      }

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
   }

   // Get accumulated response
   response = llm_stream_get_response(stream_ctx);

   // Cleanup
   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(root);

   return response;
}
