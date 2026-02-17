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
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_interface.h"
#include "llm/llm_openai.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
#include "llm/sse_parser.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "ui/metrics.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

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


char *llm_claude_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 const char **vision_images,
                                 const size_t *vision_image_sizes,
                                 int vision_image_count,
                                 const char *base_url,
                                 const char *api_key,
                                 const char *model) {
   CURL *curl_handle = NULL;
   CURLcode res;
   struct curl_slist *headers = NULL;
   char full_url[2048];
   curl_buffer_t chunk;
   char *response = NULL;

   // Convert OpenAI format to Claude format.
   // Always iteration 0: non-streaming does not support tool execution loops,
   // so orphaned tool_use filtering is always needed to clean up any history artifacts.
   json_object *request = convert_to_claude_format(conversation_history, input_text, vision_images,
                                                   vision_image_sizes, vision_image_count, model,
                                                   0);

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

      // Record metrics - Claude is always cloud
      metrics_record_llm_tokens(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, input_tokens, output_tokens,
                                cached_tokens);
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
   curl_buffer_t raw_response; /**< Raw response for error logging */
} claude_streaming_context_t;

/**
 * @brief CURL write callback for streaming responses
 *
 * Feeds incoming SSE data to the SSE parser, which processes it
 * and calls the LLM streaming callbacks. Also captures raw response
 * for error debugging.
 */
static size_t claude_streaming_write_callback(void *contents,
                                              size_t size,
                                              size_t nmemb,
                                              void *userp) {
   size_t realsize = size * nmemb;
   claude_streaming_context_t *ctx = (claude_streaming_context_t *)userp;

   // Capture raw response for error debugging (limit to 4KB)
   if (ctx->raw_response.size < 4096) {
      size_t space = 4096 - ctx->raw_response.size;
      size_t to_copy = realsize < space ? realsize : space;
      curl_buffer_write_callback(contents, 1, to_copy, &ctx->raw_response);
   }

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

/* Maximum tool call iterations to prevent infinite loops */
#define MAX_TOOL_ITERATIONS 5

/**
 * @brief Extract error message from Claude API error response
 *
 * Parses JSON like: {"type": "error", "error": {"type": "...", "message": "..."}}
 * Returns a formatted error message or a default message if parsing fails.
 * The returned string is static and should not be freed.
 */
static const char *parse_claude_error_message(const char *response_body, long http_code) {
   static _Thread_local char error_msg[512];

   if (!response_body || response_body[0] == '\0') {
      snprintf(error_msg, sizeof(error_msg), "API request failed (HTTP %ld)", http_code);
      return error_msg;
   }

   struct json_object *root = json_tokener_parse(response_body);
   if (!root) {
      snprintf(error_msg, sizeof(error_msg), "API request failed (HTTP %ld)", http_code);
      return error_msg;
   }

   /* Claude format: {"type": "error", "error": {"type": "...", "message": "..."}} */
   struct json_object *error_obj;
   if (!json_object_object_get_ex(root, "error", &error_obj)) {
      json_object_put(root);
      snprintf(error_msg, sizeof(error_msg), "API request failed (HTTP %ld)", http_code);
      return error_msg;
   }

   struct json_object *message_obj;
   const char *message = NULL;
   if (json_object_object_get_ex(error_obj, "message", &message_obj)) {
      message = json_object_get_string(message_obj);
   }

   if (message && message[0] != '\0') {
      snprintf(error_msg, sizeof(error_msg), "%s", message);
   } else {
      snprintf(error_msg, sizeof(error_msg), "API request failed (HTTP %ld)", http_code);
   }

   json_object_put(root);
   return error_msg;
}

/**
 * @brief Internal streaming implementation with iteration tracking
 */
static char *llm_claude_streaming_internal(struct json_object *conversation_history,
                                           const char *input_text,
                                           const char **vision_images,
                                           const size_t *vision_image_sizes,
                                           int vision_image_count,
                                           const char *base_url,
                                           const char *api_key,
                                           const char *model,
                                           llm_claude_text_chunk_callback chunk_callback,
                                           void *callback_userdata,
                                           int iteration) {
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
   request = convert_to_claude_format(conversation_history, input_text, vision_images,
                                      vision_image_sizes, vision_image_count, model, iteration);
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
   curl_buffer_init(&streaming_ctx.raw_response);

   curl_handle = curl_easy_init();
   if (!curl_handle) {
      LOG_ERROR("Failed to initialize CURL");
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      curl_buffer_free(&streaming_ctx.raw_response);
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
      const char *error_code = "LLM_ERROR";
      const char *error_msg = NULL;

      if (res == CURLE_ABORTED_BY_CALLBACK) {
         LOG_INFO("LLM transfer interrupted by user");
         /* User cancellation - don't send as error */
      } else if (res == CURLE_OPERATION_TIMEDOUT) {
         LOG_ERROR("LLM stream timed out (limit: %dms)", g_config.network.llm_timeout_ms);
         error_code = "LLM_TIMEOUT";
         error_msg = "Request timed out - AI server may be overloaded";
      } else {
         LOG_ERROR("CURL failed: %s", curl_easy_strerror(res));
         error_code = "LLM_CONNECTION_ERROR";
         error_msg = curl_easy_strerror(res);
      }

#ifdef ENABLE_WEBUI
      /* Send error to WebUI client if connected (except for user cancellation) */
      if (error_msg) {
         session_t *session = session_get_command_context();
         if (session && session->type == SESSION_TYPE_WEBUI) {
            webui_send_error(session, error_code, error_msg);
         }
      }
#endif

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      json_object_put(request);
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      curl_buffer_free(&streaming_ctx.raw_response);
      return NULL;
   }

   // Check HTTP status code
   long http_code = 0;
   curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

   if (http_code != 200) {
      /* Determine error code based on HTTP status */
      const char *error_code;
      if (http_code == 401) {
         LOG_ERROR("Claude API: Invalid or missing API key (HTTP 401)");
         error_code = "LLM_AUTH_ERROR";
      } else if (http_code == 403) {
         LOG_ERROR("Claude API: Access forbidden (HTTP 403) - check API key permissions");
         error_code = "LLM_ACCESS_ERROR";
      } else if (http_code == 429) {
         LOG_ERROR("Claude API: Rate limit exceeded (HTTP 429)");
         error_code = "LLM_RATE_LIMIT";
      } else if (http_code >= 500) {
         LOG_ERROR("Claude API: Server error (HTTP %ld)", http_code);
         error_code = "LLM_SERVER_ERROR";
      } else if (http_code == 400) {
         LOG_ERROR("Claude API: Bad request (HTTP 400) - check tool format and message structure");
         error_code = "LLM_BAD_REQUEST";
         /* Log the raw response which contains error details */
         if (streaming_ctx.raw_response.data && streaming_ctx.raw_response.size > 0) {
            LOG_ERROR("Claude error response: %s", streaming_ctx.raw_response.data);
         }
         /* Log a sample of the request for debugging */
         LOG_WARNING("Claude request payload (first 1000 chars): %.1000s", payload);
      } else {
         LOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
         error_code = "LLM_ERROR";
      }

#ifdef ENABLE_WEBUI
      /* Send error to WebUI client if connected */
      session_t *session = session_get_command_context();
      if (session && session->type == SESSION_TYPE_WEBUI) {
         const char *error_msg = parse_claude_error_message(streaming_ctx.raw_response.data,
                                                            http_code);
         webui_send_error(session, error_code, error_msg);
      }
#endif

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      json_object_put(request);
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      curl_buffer_free(&streaming_ctx.raw_response);
      return NULL;
   }

   curl_easy_cleanup(curl_handle);
   curl_slist_free_all(headers);

   // Debug: Log raw response info before cleanup
   if (streaming_ctx.raw_response.size == 0) {
      LOG_WARNING("Claude: No data received from API (raw_response empty)");
   }

   curl_buffer_free(&streaming_ctx.raw_response);

   // Check for tool calls
   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         LOG_INFO("Claude streaming: Executing %d tool call(s)", tool_calls->count);

         // Execute tools (heap-allocated: ~66KB, too large for 512KB satellite worker stack
         // with up to MAX_TOOL_ITERATIONS levels of recursion)
         tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
         if (!results) {
            LOG_ERROR("Claude streaming: Failed to allocate tool results");
            sse_parser_free(sse_parser);
            llm_stream_free(stream_ctx);
            json_object_put(request);
            return NULL;
         }
         llm_tools_execute_all(tool_calls, results);

         // Add assistant message with tool_use blocks to conversation history
         // Claude format: content is an array of content blocks
         json_object *assistant_msg = json_object_new_object();
         json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

         json_object *content_array = json_object_new_array();

         // If thinking was enabled, add the thinking block first (required by Claude API)
         char *thinking_content = llm_stream_get_thinking(stream_ctx);
         if (thinking_content) {
            json_object *thinking_block = json_object_new_object();
            json_object_object_add(thinking_block, "type", json_object_new_string("thinking"));
            json_object_object_add(thinking_block, "thinking",
                                   json_object_new_string(thinking_content));

            // Signature is required when sending thinking content back to Claude
            char *thinking_signature = llm_stream_get_thinking_signature(stream_ctx);
            if (thinking_signature) {
               json_object_object_add(thinking_block, "signature",
                                      json_object_new_string(thinking_signature));
               free(thinking_signature);
            }

            json_object_array_add(content_array, thinking_block);
            free(thinking_content);
         }

         for (int i = 0; i < tool_calls->count; i++) {
            json_object *tool_use = json_object_new_object();
            json_object_object_add(tool_use, "type", json_object_new_string("tool_use"));
            json_object_object_add(tool_use, "id", json_object_new_string(tool_calls->calls[i].id));
            json_object_object_add(tool_use, "name",
                                   json_object_new_string(tool_calls->calls[i].name));

            // Parse arguments JSON
            json_object *args = json_tokener_parse(tool_calls->calls[i].arguments);
            if (args) {
               json_object_object_add(tool_use, "input", args);
            } else {
               json_object_object_add(tool_use, "input", json_object_new_object());
            }

            json_object_array_add(content_array, tool_use);
         }
         json_object_object_add(assistant_msg, "content", content_array);
         json_object_array_add(conversation_history, assistant_msg);

         // Add tool results to conversation history (Claude format)
         llm_tools_add_results_claude(conversation_history, results);

         // Cleanup current stream context
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(request);

         // Check if we should skip follow-up (e.g., LLM was switched)
         if (llm_tools_should_skip_followup(results)) {
            LOG_INFO("Claude streaming: Skipping follow-up call (tool requested no follow-up)");
            char *direct_response = llm_tools_get_direct_response(results);

            // Add synthetic assistant message to complete the tool call sequence
            // This prevents errors on subsequent requests due to incomplete history
            if (direct_response) {
               json_object *closing_msg = json_object_new_object();
               json_object_object_add(closing_msg, "role", json_object_new_string("assistant"));
               // Claude format: content is an array of content blocks
               json_object *content_array = json_object_new_array();
               json_object *text_block = json_object_new_object();
               json_object_object_add(text_block, "type", json_object_new_string("text"));
               json_object_object_add(text_block, "text", json_object_new_string(direct_response));
               json_object_array_add(content_array, text_block);
               json_object_object_add(closing_msg, "content", content_array);
               json_object_array_add(conversation_history, closing_msg);
               LOG_INFO("Claude streaming: Added closing assistant message to complete history");
            }

            // Send through chunk callback so TTS receives it
            if (direct_response && chunk_callback) {
               chunk_callback(direct_response, callback_userdata);
            }

            // Free any vision data from tool results
            for (int i = 0; i < results->count; i++) {
               if (results->results[i].vision_image) {
                  free(results->results[i].vision_image);
                  results->results[i].vision_image = NULL;
               }
            }
            free(results);
            return direct_response;
         }

         // Check iteration limit
         if (iteration >= MAX_TOOL_ITERATIONS) {
            LOG_WARNING("Claude streaming: Max tool iterations (%d) reached, returning without "
                        "final response",
                        MAX_TOOL_ITERATIONS);
            const char *error_msg = "I apologize, but I wasn't able to complete that request after "
                                    "several attempts. Could you try rephrasing your question?";
            // Send through chunk callback so TTS receives it
            if (chunk_callback) {
               chunk_callback(error_msg, callback_userdata);
            }
            // Free any vision data from tool results
            for (int i = 0; i < results->count; i++) {
               if (results->results[i].vision_image) {
                  free(results->results[i].vision_image);
                  results->results[i].vision_image = NULL;
               }
            }
            free(results);
            return strdup(error_msg);
         }

         // Check for vision data in tool results (session-isolated)
         const char *result_vision = NULL;
         size_t result_vision_size = 0;
         for (int i = 0; i < results->count; i++) {
            if (results->results[i].vision_image && results->results[i].vision_image_size > 0) {
               result_vision = results->results[i].vision_image;
               result_vision_size = results->results[i].vision_image_size;
               LOG_INFO("Claude streaming: Including vision from tool result (%zu bytes)",
                        result_vision_size);
               break;
            }
         }

         // Check if provider changed (e.g., switch_llm was called)
         llm_resolved_config_t current_config;
         char *result = NULL;
         char model_buf_followup[LLM_MODEL_NAME_MAX] =
             "";  // Buffer for model (resolved ptr may dangle)

         LOG_INFO("Claude streaming: Making follow-up call after tool execution (iteration %d/%d)",
                  iteration + 1, MAX_TOOL_ITERATIONS);

         // Resolve config once and reuse for both provider check and credentials
         bool config_valid = (llm_get_current_resolved_config(&current_config) == 0);

         // Copy model to local buffer immediately (current_config.model may be dangling pointer)
         if (config_valid && current_config.model && current_config.model[0] != '\0') {
            strncpy(model_buf_followup, current_config.model, sizeof(model_buf_followup) - 1);
            model_buf_followup[sizeof(model_buf_followup) - 1] = '\0';
         }

         // Create single-item array for tool result vision
         const char *result_vision_arr[1] = { result_vision };
         size_t result_vision_size_arr[1] = { result_vision_size };
         int result_vision_count = result_vision ? 1 : 0;

         if (config_valid && (current_config.type == LLM_LOCAL ||
                              current_config.cloud_provider == CLOUD_PROVIDER_OPENAI)) {
            // Provider switched to OpenAI or local - hand off to OpenAI code path
            LOG_INFO("Claude streaming: Provider switched to OpenAI/local, handing off");

            // OpenAI will handle the vision data if present
            // Use copied model buffer to avoid dangling pointer
            result = llm_openai_chat_completion_streaming(
                conversation_history, "", result_vision_arr, result_vision_size_arr,
                result_vision_count, current_config.endpoint, current_config.api_key,
                model_buf_followup[0] ? model_buf_followup : NULL,
                (llm_openai_text_chunk_callback)chunk_callback, callback_userdata);
         } else {
            // Still Claude - use resolved config or fallback to original
            const char *fresh_url = config_valid ? current_config.endpoint : base_url;
            const char *fresh_api_key = config_valid ? current_config.api_key : api_key;

            result = llm_claude_streaming_internal(conversation_history, "", result_vision_arr,
                                                   result_vision_size_arr, result_vision_count,
                                                   fresh_url, fresh_api_key, model, chunk_callback,
                                                   callback_userdata, iteration + 1);
         }

         // Free vision data from tool results after use
         for (int i = 0; i < results->count; i++) {
            if (results->results[i].vision_image) {
               free(results->results[i].vision_image);
               results->results[i].vision_image = NULL;
            }
         }
         free(results);

         return result;
      }
   }

   // Get accumulated response
   response = llm_stream_get_response(stream_ctx);

   // Debug: Log if response is empty (helps diagnose streaming issues)
   if (!response || !*response) {
      LOG_WARNING(
          "Claude: Stream completed but response is empty (no text content, no tool calls)");
      LOG_WARNING("Claude: has_tool_calls=%d", llm_stream_has_tool_calls(stream_ctx) ? 1 : 0);
   }

   // Cleanup
   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(request);

   return response;
}

char *llm_claude_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           const char **vision_images,
                                           const size_t *vision_image_sizes,
                                           int vision_image_count,
                                           const char *base_url,
                                           const char *api_key,
                                           const char *model,
                                           llm_claude_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   return llm_claude_streaming_internal(conversation_history, input_text, vision_images,
                                        vision_image_sizes, vision_image_count, base_url, api_key,
                                        model, chunk_callback, callback_userdata, 0);
}
