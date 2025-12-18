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

#include "llm/llm_claude.h"

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
 * @brief Build HTTP headers for Claude API request
 *
 * @param api_key Anthropic API key (required)
 * @return CURL header list (caller must free with curl_slist_free_all)
 */
static struct curl_slist *build_claude_headers(const char *api_key) {
   struct curl_slist *headers = NULL;
   char api_key_header[512];
   char version_header[128];

   headers = curl_slist_append(headers, "Content-Type: application/json");

   // Claude uses x-api-key instead of Authorization
   snprintf(api_key_header, sizeof(api_key_header), "x-api-key: %s", api_key);
   headers = curl_slist_append(headers, api_key_header);

   // Claude requires API version header
   snprintf(version_header, sizeof(version_header), "anthropic-version: %s", CLAUDE_API_VERSION);
   headers = curl_slist_append(headers, version_header);

   return headers;
}

/**
 * @brief Convert OpenAI-format conversation to Claude format
 *
 * Claude format differences:
 * - System messages go in separate "system" array, not "messages"
 * - System messages support cache_control markers
 * - Messages array contains only user/assistant exchanges
 *
 * @param openai_conversation OpenAI format conversation history
 * @param input_text Current user input
 * @param vision_image Optional base64 vision image
 * @param vision_image_size Size of vision image
 * @return Claude-format request object (caller must free with json_object_put)
 */
static json_object *convert_to_claude_format(struct json_object *openai_conversation,
                                             const char *input_text,
                                             char *vision_image,
                                             size_t vision_image_size) {
   json_object *claude_request = json_object_new_object();

   // Model from config
   json_object_object_add(claude_request, "model",
                          json_object_new_string(g_config.llm.cloud.claude_model));

   // Max tokens from config
   json_object_object_add(claude_request, "max_tokens",
                          json_object_new_int(g_config.llm.max_tokens));

   // Extract system message and user/assistant messages
   json_object *system_array = json_object_new_array();
   json_object *messages_array = json_object_new_array();

   int conv_len = json_object_array_length(openai_conversation);
   const char *last_role = NULL;
   json_object *last_message = NULL;

   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(openai_conversation, i);
      json_object *role_obj, *content_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj) ||
          !json_object_object_get_ex(msg, "content", &content_obj)) {
         continue;
      }

      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "system") == 0) {
         // System message goes in separate "system" array with cache control
         json_object *system_block = json_object_new_object();
         json_object_object_add(system_block, "type", json_object_new_string("text"));
         json_object_object_add(system_block, "text", json_object_get(content_obj));

#if CLAUDE_ENABLE_PROMPT_CACHING
         // Add cache control to system prompt for 90% cost savings
         json_object *cache_control = json_object_new_object();
         json_object_object_add(cache_control, "type", json_object_new_string("ephemeral"));
         json_object_object_add(system_block, "cache_control", cache_control);
#endif

         json_object_array_add(system_array, system_block);
      } else {
         // Claude requires strict role alternation - consolidate consecutive same-role messages
         if (last_role != NULL && strcmp(last_role, role) == 0 && last_message != NULL) {
            // Same role as previous - concatenate content
            json_object *last_content_obj;
            if (json_object_object_get_ex(last_message, "content", &last_content_obj)) {
               const char *last_content = json_object_get_string(last_content_obj);
               const char *current_content = json_object_get_string(content_obj);

               // Build concatenated content
               size_t new_len = strlen(last_content) + strlen(current_content) +
                                4;  // +4 for "\n\n" and null
               char *combined = malloc(new_len);
               if (combined) {
                  snprintf(combined, new_len, "%s\n\n%s", last_content, current_content);
                  json_object_object_add(last_message, "content", json_object_new_string(combined));
                  free(combined);
               }
            }
         } else {
            // Different role or first message - add new message
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string(role));
            json_object_object_add(last_message, "content", json_object_get(content_obj));
            json_object_array_add(messages_array, last_message);
            last_role = role;
         }
      }
   }

   // Add system array if not empty
   if (json_object_array_length(system_array) > 0) {
      json_object_object_add(claude_request, "system", system_array);
   } else {
      json_object_put(system_array);
   }

   // User message is now added by dawn.c before calling this function
   // If vision is provided, modify the last user message to include image
   if (vision_image != NULL && vision_image_size > 0) {
      int msg_count = json_object_array_length(messages_array);
      if (msg_count > 0) {
         json_object *last_msg = json_object_array_get_idx(messages_array, msg_count - 1);
         json_object *role_obj;
         if (json_object_object_get_ex(last_msg, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0) {
            // Last message is user message - add vision content in Claude format
            json_object *content_array = json_object_new_array();

            // Text content
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text", json_object_new_string(input_text));
            json_object_array_add(content_array, text_obj);

            // Image content (Claude format)
            json_object *image_obj = json_object_new_object();
            json_object_object_add(image_obj, "type", json_object_new_string("image"));

            json_object *source_obj = json_object_new_object();
            json_object_object_add(source_obj, "type", json_object_new_string("base64"));
            json_object_object_add(source_obj, "media_type", json_object_new_string("image/jpeg"));
            json_object_object_add(source_obj, "data", json_object_new_string(vision_image));
            json_object_object_add(image_obj, "source", source_obj);

            json_object_array_add(content_array, image_obj);
            json_object_object_add(last_msg, "content", content_array);
         }
      }
   }

   json_object_object_add(claude_request, "messages", messages_array);

   return claude_request;
}

char *llm_claude_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 char *vision_image,
                                 size_t vision_image_size,
                                 const char *base_url,
                                 const char *api_key) {
   CURL *curl_handle = NULL;
   CURLcode res;
   struct curl_slist *headers = NULL;
   char full_url[2048];
   curl_buffer_t chunk;
   char *response = NULL;

   // Convert OpenAI format to Claude format
   json_object *request = convert_to_claude_format(conversation_history, input_text, vision_image,
                                                   vision_image_size);

   const char *payload = json_object_to_json_string_ext(
       request, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);

   // Initialize response buffer
   curl_buffer_init(&chunk);

   // Check connection
   if (!llm_check_connection(base_url, 4)) {
      LOG_ERROR("Cannot reach Claude API");
      json_object_put(request);
      return NULL;
   }

   // Setup CURL
   curl_handle = curl_easy_init();
   if (!curl_handle) {
      LOG_ERROR("Failed to initialize CURL");
      json_object_put(request);
      return NULL;
   }

   headers = build_claude_headers(api_key);
   snprintf(full_url, sizeof(full_url), "%s%s", base_url, CLAUDE_MESSAGES_ENDPOINT);

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
         LOG_ERROR("CURL failed: %s", curl_easy_strerror(res));
      }
      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      curl_buffer_free(&chunk);
      json_object_put(request);
      return NULL;
   }

   // Check HTTP status code
   long http_code = 0;
   curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

   if (http_code != 200) {
      if (http_code == 401) {
         LOG_ERROR("Claude API: Invalid or missing API key (HTTP 401)");
      } else if (http_code == 403) {
         LOG_ERROR("Claude API: Access forbidden (HTTP 403) - check API key permissions");
      } else if (http_code == 429) {
         LOG_ERROR("Claude API: Rate limit exceeded (HTTP 429)");
      } else if (http_code >= 500) {
         LOG_ERROR("Claude API: Server error (HTTP %ld)", http_code);
      } else if (http_code != 0) {
         LOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
      }
      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      curl_buffer_free(&chunk);
      json_object_put(request);
      return NULL;
   }

   curl_easy_cleanup(curl_handle);
   curl_slist_free_all(headers);
   json_object_put(request);

   // Parse Claude response
   json_object *parsed = json_tokener_parse(chunk.data);
   if (!parsed) {
      LOG_ERROR("Failed to parse Claude response");
      curl_buffer_free(&chunk);
      return NULL;
   }

   // Extract text from response.content[0].text
   json_object *content_array, *first_content, *text_obj, *type_obj;
   if (!json_object_object_get_ex(parsed, "content", &content_array) ||
       json_object_get_type(content_array) != json_type_array ||
       json_object_array_length(content_array) < 1) {
      LOG_ERROR("Invalid Claude response format: missing content array");
      json_object_put(parsed);
      curl_buffer_free(&chunk);
      return NULL;
   }

   first_content = json_object_array_get_idx(content_array, 0);
   if (!first_content) {
      LOG_ERROR("Empty content array in Claude response");
      json_object_put(parsed);
      curl_buffer_free(&chunk);
      return NULL;
   }

   // Verify it's a text block
   if (json_object_object_get_ex(first_content, "type", &type_obj)) {
      const char *content_type = json_object_get_string(type_obj);
      if (strcmp(content_type, "text") != 0) {
         LOG_ERROR("First content block is not text: %s", content_type);
         json_object_put(parsed);
         curl_buffer_free(&chunk);
         return NULL;
      }
   }

   if (!json_object_object_get_ex(first_content, "text", &text_obj)) {
      LOG_ERROR("No text in Claude response");
      json_object_put(parsed);
      curl_buffer_free(&chunk);
      return NULL;
   }

   response = strdup(json_object_get_string(text_obj));

   // Log cache usage (important for cost monitoring)
   json_object *usage_obj, *cache_creation_obj, *cache_read_obj;
   int input_tokens = 0;
   int output_tokens = 0;
   int cached_tokens = 0;
   if (json_object_object_get_ex(parsed, "usage", &usage_obj)) {
      // Log total tokens
      json_object *input_tokens_obj, *output_tokens_obj;
      if (json_object_object_get_ex(usage_obj, "input_tokens", &input_tokens_obj) &&
          json_object_object_get_ex(usage_obj, "output_tokens", &output_tokens_obj)) {
         input_tokens = json_object_get_int(input_tokens_obj);
         output_tokens = json_object_get_int(output_tokens_obj);
         LOG_WARNING("Total tokens: %d input + %d output = %d", input_tokens, output_tokens,
                     input_tokens + output_tokens);
      }

      // Log cache creation
      if (json_object_object_get_ex(usage_obj, "cache_creation_input_tokens",
                                    &cache_creation_obj)) {
         int cache_created = json_object_get_int(cache_creation_obj);
         if (cache_created > 0) {
            LOG_INFO("Claude cache created: %d tokens", cache_created);
         }
      }

      // Log cache hits (this is where we save money!)
      if (json_object_object_get_ex(usage_obj, "cache_read_input_tokens", &cache_read_obj)) {
         cached_tokens = json_object_get_int(cache_read_obj);
         if (cached_tokens > 0) {
            LOG_INFO("Claude cache hit: %d tokens (90%% cost savings!)", cached_tokens);
         }
      }

      // Record metrics
      metrics_record_llm_tokens(LLM_CLOUD, input_tokens, output_tokens, cached_tokens);
   }

   // Check stop reason
   json_object *stop_reason_obj;
   if (json_object_object_get_ex(parsed, "stop_reason", &stop_reason_obj)) {
      const char *stop_reason = json_object_get_string(stop_reason_obj);
      if (strcmp(stop_reason, "end_turn") == 0) {
         LOG_INFO("Response finished properly.");
      } else {
         LOG_WARNING("Claude stopped with reason: %s", stop_reason);
      }
   }

   json_object_put(parsed);
   curl_buffer_free(&chunk);

   return response;
}

/**
 * @brief Context for streaming callbacks
 */
typedef struct {
   sse_parser_t *sse_parser;
   llm_stream_context_t *stream_ctx;
} claude_streaming_context_t;

/**
 * @brief CURL write callback for streaming responses
 *
 * Feeds incoming SSE data to the SSE parser, which processes it
 * and calls the LLM streaming callbacks.
 */
static size_t claude_streaming_write_callback(void *contents,
                                              size_t size,
                                              size_t nmemb,
                                              void *userp) {
   size_t realsize = size * nmemb;
   claude_streaming_context_t *ctx = (claude_streaming_context_t *)userp;

   // Feed data to SSE parser
   sse_parser_feed(ctx->sse_parser, contents, realsize);

   return realsize;
}

/**
 * @brief SSE event callback that forwards events to LLM stream handler
 */
static void claude_sse_event_handler(const char *event_type,
                                     const char *event_data,
                                     void *userdata) {
   claude_streaming_context_t *ctx = (claude_streaming_context_t *)userdata;

   // Forward event data to LLM streaming parser
   llm_stream_handle_event(ctx->stream_ctx, event_data);
}

char *llm_claude_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
                                           llm_claude_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";

   const char *payload = NULL;
   char *response = NULL;

   json_object *request = NULL;

   // SSE and streaming contexts
   sse_parser_t *sse_parser = NULL;
   llm_stream_context_t *stream_ctx = NULL;
   claude_streaming_context_t streaming_ctx;

   if (!api_key) {
      LOG_ERROR("Claude API key is required");
      return NULL;
   }

   // Check connection
   if (!llm_check_connection(base_url, 4)) {
      LOG_ERROR("URL did not return. Unavailable.");
      return NULL;
   }

   // Convert OpenAI format to Claude format
   request = convert_to_claude_format(conversation_history, input_text, vision_image,
                                      vision_image_size);
   if (!request) {
      LOG_ERROR("Failed to convert conversation to Claude format");
      return NULL;
   }

   // Enable streaming
   json_object_object_add(request, "stream", json_object_new_boolean(1));

   payload = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN |
                                                         JSON_C_TO_STRING_NOSLASHESCAPE);

   // Create streaming context
   stream_ctx = llm_stream_create(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, chunk_callback,
                                  callback_userdata);
   if (!stream_ctx) {
      LOG_ERROR("Failed to create LLM stream context");
      json_object_put(request);
      return NULL;
   }

   // Create SSE parser
   sse_parser = sse_parser_create(claude_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      LOG_ERROR("Failed to create SSE parser");
      llm_stream_free(stream_ctx);
      json_object_put(request);
      return NULL;
   }

   // Setup streaming context
   streaming_ctx.sse_parser = sse_parser;
   streaming_ctx.stream_ctx = stream_ctx;

   curl_handle = curl_easy_init();
   if (!curl_handle) {
      LOG_ERROR("Failed to initialize CURL");
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      json_object_put(request);
      return NULL;
   }

   headers = build_claude_headers(api_key);
   snprintf(full_url, sizeof(full_url), "%s%s", base_url, CLAUDE_MESSAGES_ENDPOINT);

   curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
   curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
   curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, claude_streaming_write_callback);
   curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&streaming_ctx);

   // Enable progress callback for interruption support
   curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);  // Enable progress callback
   curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
   curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);

   // Set low-speed timeout: abort if transfer drops below 1 byte/sec for 30 seconds
   // This catches hung/stalled streams from LLM servers
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
         LOG_ERROR("CURL failed: %s", curl_easy_strerror(res));
      }
      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      json_object_put(request);
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      return NULL;
   }

   // Check HTTP status code
   long http_code = 0;
   curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

   if (http_code != 200) {
      if (http_code == 401) {
         LOG_ERROR("Claude API: Invalid or missing API key (HTTP 401)");
      } else if (http_code == 403) {
         LOG_ERROR("Claude API: Access forbidden (HTTP 403) - check API key permissions");
      } else if (http_code == 429) {
         LOG_ERROR("Claude API: Rate limit exceeded (HTTP 429)");
      } else if (http_code >= 500) {
         LOG_ERROR("Claude API: Server error (HTTP %ld)", http_code);
      } else if (http_code != 0) {
         LOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
      }
      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      json_object_put(request);
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      return NULL;
   }

   curl_easy_cleanup(curl_handle);
   curl_slist_free_all(headers);
   json_object_put(request);

   // Get accumulated response
   response = llm_stream_get_response(stream_ctx);

   // Cleanup
   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);

   return response;
}
