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
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_interface.h"
#include "llm/llm_openai.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
#include "llm/sse_parser.h"
#include "logging.h"
#include "ui/metrics.h"
#include "utils/string_utils.h"
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
      llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      OLOG_ERROR("Pre-flight connection check failed (cloud unreachable)");
      json_object_put(request);
      return NULL;
   }

   // Setup CURL
   curl_handle = curl_easy_init();
   if (!curl_handle) {
      OLOG_ERROR("Failed to initialize CURL");
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

   // Set connect timeout: fail fast on unreachable hosts instead of waiting for overall timeout
   curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);

   // Set overall timeout from config (default 30000ms)
   int effective_timeout = llm_get_effective_timeout_ms();
   if (effective_timeout > 0) {
      curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, (long)effective_timeout);
   }

   // Set low-speed timeout — scale for long-running requests (see llm_openai.c)
   long low_speed_time = 30L;
   if (effective_timeout > 60000) {
      low_speed_time = (long)(effective_timeout / 1000);
   }
   curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
   curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, low_speed_time);

   res = curl_easy_perform(curl_handle);
   if (res != CURLE_OK) {
      if (res == CURLE_ABORTED_BY_CALLBACK) {
         OLOG_INFO("LLM transfer interrupted by user");
      } else if (res == CURLE_OPERATION_TIMEDOUT) {
         OLOG_ERROR("LLM request timed out (limit: %dms)", effective_timeout);
      } else {
         OLOG_ERROR("CURL failed: %s", curl_easy_strerror(res));
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
         OLOG_ERROR("Claude API: Invalid or missing API key (HTTP 401)");
      } else if (http_code == 403) {
         OLOG_ERROR("Claude API: Access forbidden (HTTP 403) - check API key permissions");
      } else if (http_code == 429) {
         OLOG_ERROR("Claude API: Rate limit exceeded (HTTP 429)");
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      } else if (http_code >= 500 && http_code < 600) {
         OLOG_ERROR("Claude API: Server error (HTTP %ld)", http_code);
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      } else if (http_code != 0) {
         OLOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
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
      OLOG_ERROR("Failed to parse Claude response");
      curl_buffer_free(&chunk);
      return NULL;
   }

   // Extract text from response.content[0].text
   json_object *content_array, *first_content, *text_obj, *type_obj;
   if (!json_object_object_get_ex(parsed, "content", &content_array) ||
       json_object_get_type(content_array) != json_type_array ||
       json_object_array_length(content_array) < 1) {
      OLOG_ERROR("Invalid Claude response format: missing content array");
      json_object_put(parsed);
      curl_buffer_free(&chunk);
      return NULL;
   }

   first_content = json_object_array_get_idx(content_array, 0);
   if (!first_content) {
      OLOG_ERROR("Empty content array in Claude response");
      json_object_put(parsed);
      curl_buffer_free(&chunk);
      return NULL;
   }

   // Verify it's a text block
   if (json_object_object_get_ex(first_content, "type", &type_obj)) {
      const char *content_type = json_object_get_string(type_obj);
      if (strcmp(content_type, "text") != 0) {
         OLOG_ERROR("First content block is not text: %s", content_type);
         json_object_put(parsed);
         curl_buffer_free(&chunk);
         return NULL;
      }
   }

   if (!json_object_object_get_ex(first_content, "text", &text_obj)) {
      OLOG_ERROR("No text in Claude response");
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
         OLOG_WARNING("Total tokens: %d input + %d output = %d", input_tokens, output_tokens,
                      input_tokens + output_tokens);
      }

      // Log cache creation
      if (json_object_object_get_ex(usage_obj, "cache_creation_input_tokens",
                                    &cache_creation_obj)) {
         int cache_created = json_object_get_int(cache_creation_obj);
         if (cache_created > 0) {
            OLOG_INFO("Claude cache created: %d tokens", cache_created);
         }
      }

      // Log cache hits (this is where we save money!)
      if (json_object_object_get_ex(usage_obj, "cache_read_input_tokens", &cache_read_obj)) {
         cached_tokens = json_object_get_int(cache_read_obj);
         if (cached_tokens > 0) {
            OLOG_INFO("Claude cache hit: %d tokens (90%% cost savings!)", cached_tokens);
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
         OLOG_INFO("Response finished properly.");
      } else {
         OLOG_WARNING("Claude stopped with reason: %s", stop_reason);
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
#define MAX_TOOL_ITERATIONS 8

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
      OLOG_ERROR("Claude API key is required");
      return NULL;
   }

   // Check connection
   if (!llm_check_connection(base_url, 4)) {
      llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      OLOG_ERROR("Pre-flight connection check failed (cloud unreachable)");
      return NULL;
   }

   // Convert OpenAI format to Claude format
   request = convert_to_claude_format(conversation_history, input_text, vision_images,
                                      vision_image_sizes, vision_image_count, model, iteration);
   if (!request) {
      OLOG_ERROR("Failed to convert conversation to Claude format");
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
      OLOG_ERROR("Failed to create LLM stream context");
      json_object_put(request);
      return NULL;
   }

   // Create SSE parser
   sse_parser = sse_parser_create(claude_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      OLOG_ERROR("Failed to create SSE parser");
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
      OLOG_ERROR("Failed to initialize CURL");
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

   // For streaming: use inactivity timeout instead of hard wall timeout.
   // Abort if transfer drops below 1 byte/sec for 60 seconds (no data flowing).
   // This allows long responses to complete while still catching hung connections.
   curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
   curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 60L);

   // Set connect timeout: fail fast on unreachable hosts instead of waiting for overall timeout
   curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);

   // No hard timeout for streaming - rely on low-speed detection instead.
   // The llm_timeout_ms config is only used for non-streaming requests.

   res = curl_easy_perform(curl_handle);
   if (res != CURLE_OK) {
      const char *error_code = "LLM_ERROR";
      const char *error_msg = NULL;

      if (res == CURLE_ABORTED_BY_CALLBACK) {
         OLOG_INFO("LLM transfer interrupted by user");
         /* User cancellation - don't send as error */
      } else if (res == CURLE_OPERATION_TIMEDOUT) {
         OLOG_ERROR("LLM stream timed out (no data for 60 seconds)");
         error_code = "LLM_TIMEOUT";
         error_msg = "Request timed out - AI server may be overloaded";
      } else {
         OLOG_ERROR("CURL failed: %s", curl_easy_strerror(res));
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
         OLOG_ERROR("Claude API: Invalid or missing API key (HTTP 401)");
         error_code = "LLM_AUTH_ERROR";
      } else if (http_code == 403) {
         OLOG_ERROR("Claude API: Access forbidden (HTTP 403) - check API key permissions");
         error_code = "LLM_ACCESS_ERROR";
      } else if (http_code == 429) {
         OLOG_ERROR("Claude API: Rate limit exceeded (HTTP 429)");
         error_code = "LLM_RATE_LIMIT";
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      } else if (http_code >= 500 && http_code < 600) {
         OLOG_ERROR("Claude API: Server error (HTTP %ld)", http_code);
         error_code = "LLM_SERVER_ERROR";
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      } else if (http_code == 400) {
         OLOG_ERROR("Claude API: Bad request (HTTP 400) - check tool format and message structure");
         error_code = "LLM_BAD_REQUEST";
         /* Log the raw response which contains error details */
         if (streaming_ctx.raw_response.data && streaming_ctx.raw_response.size > 0) {
            OLOG_ERROR("Claude error response: %s", streaming_ctx.raw_response.data);
         }
         /* Log a sample of the request for debugging */
         OLOG_WARNING("Claude request payload (first 1000 chars): %.1000s", payload);
      } else {
         OLOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
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
      OLOG_WARNING("Claude: No data received from API (raw_response empty)");
   }

   curl_buffer_free(&streaming_ctx.raw_response);

   // Check for tool calls
   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         OLOG_INFO("Claude streaming: Executing %d tool call(s)", tool_calls->count);

         // Execute tools (heap-allocated: ~66KB, too large for 512KB satellite worker stack
         // with up to MAX_TOOL_ITERATIONS levels of recursion)
         tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
         if (!results) {
            OLOG_ERROR("Claude streaming: Failed to allocate tool results");
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
            OLOG_INFO("Claude streaming: Skipping follow-up call (tool requested no follow-up)");
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
               OLOG_INFO("Claude streaming: Added closing assistant message to complete history");
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

         // Check iteration limit — force a final text response with what we have
         if (iteration >= MAX_TOOL_ITERATIONS) {
            OLOG_WARNING(
                "Claude streaming: Max tool iterations (%d) reached, forcing text response",
                MAX_TOOL_ITERATIONS);

            // Inject a system hint telling the LLM to respond with what it has
            json_object *hint_msg = json_object_new_object();
            json_object_object_add(hint_msg, "role", json_object_new_string("user"));
            json_object_object_add(
                hint_msg, "content",
                json_object_new_string(
                    "[System: Maximum tool iterations reached. Respond to the user now with "
                    "the information you have gathered so far. Do not call any more tools.]"));
            json_object_array_add(conversation_history, hint_msg);

            // Free any vision data from tool results
            for (int i = 0; i < results->count; i++) {
               if (results->results[i].vision_image) {
                  free(results->results[i].vision_image);
                  results->results[i].vision_image = NULL;
               }
            }
            free(results);

            // Make one final call with tools disabled
            OLOG_INFO("Claude streaming: Making final call without tools to present results");
            return llm_claude_streaming_internal(conversation_history, "", NULL, NULL, 0, base_url,
                                                 api_key, model, chunk_callback, callback_userdata,
                                                 MAX_TOOL_ITERATIONS);
         }

         // Check for vision data in tool results (session-isolated)
         const char *result_vision = NULL;
         size_t result_vision_size = 0;
         for (int i = 0; i < results->count; i++) {
            if (results->results[i].vision_image && results->results[i].vision_image_size > 0) {
               result_vision = results->results[i].vision_image;
               result_vision_size = results->results[i].vision_image_size;
               OLOG_INFO("Claude streaming: Including vision from tool result (%zu bytes)",
                         result_vision_size);
               break;
            }
         }

         // Check if provider changed (e.g., switch_llm was called)
         llm_resolved_config_t current_config;
         char *result = NULL;
         char model_buf_followup[LLM_MODEL_NAME_MAX] =
             "";  // Buffer for model (resolved ptr may dangle)

         OLOG_INFO("Claude streaming: Making follow-up call after tool execution (iteration %d/%d)",
                   iteration + 1, MAX_TOOL_ITERATIONS);

         // Resolve config once and reuse for both provider check and credentials
         bool config_valid = (llm_get_current_resolved_config(&current_config) == 0);

         // Copy model to local buffer immediately (current_config.model may be dangling pointer)
         if (config_valid && current_config.model && current_config.model[0] != '\0') {
            safe_strscpy(model_buf_followup, current_config.model);
         }

         // Create single-item array for tool result vision
         const char *result_vision_arr[1] = { result_vision };
         size_t result_vision_size_arr[1] = { result_vision_size };
         int result_vision_count = result_vision ? 1 : 0;

         if (config_valid && (current_config.type == LLM_LOCAL ||
                              current_config.cloud_provider == CLOUD_PROVIDER_OPENAI)) {
            // Provider switched to OpenAI or local - hand off to OpenAI code path
            OLOG_INFO("Claude streaming: Provider switched to OpenAI/local, handing off");

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
      OLOG_WARNING(
          "Claude: Stream completed but response is empty (no text content, no tool calls)");
      OLOG_WARNING("Claude: has_tool_calls=%d", llm_stream_has_tool_calls(stream_ctx) ? 1 : 0);
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

int llm_claude_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char **vision_images,
                                     const size_t *vision_image_sizes,
                                     int vision_image_count,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_claude_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     llm_tool_response_t *result) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";
   const char *payload = NULL;
   json_object *request = NULL;
   sse_parser_t *sse_parser = NULL;
   llm_stream_context_t *stream_ctx = NULL;
   claude_streaming_context_t streaming_ctx;

   if (!result) {
      return 1;
   }
   memset(result, 0, sizeof(*result));

   if (!api_key) {
      OLOG_ERROR("Claude API key is required");
      return 1;
   }

   if (!llm_check_connection(base_url, 4)) {
      llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      OLOG_ERROR("Pre-flight connection check failed (cloud unreachable)");
      return 1;
   }

   /* Convert to Claude format */
   request = convert_to_claude_format(conversation_history, input_text, vision_images,
                                      vision_image_sizes, vision_image_count, model, iteration);
   if (!request) {
      OLOG_ERROR("Failed to convert conversation to Claude format");
      return 1;
   }

   json_object_object_add(request, "stream", json_object_new_boolean(1));

   payload = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN |
                                                         JSON_C_TO_STRING_NOSLASHESCAPE);

   OLOG_INFO("Claude single-shot iter %d: url=%s", iteration, base_url);

   /* Create streaming context */
   stream_ctx = llm_stream_create(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, chunk_callback,
                                  callback_userdata);
   if (!stream_ctx) {
      OLOG_ERROR("Failed to create LLM stream context");
      json_object_put(request);
      return 1;
   }

   sse_parser = sse_parser_create(claude_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      OLOG_ERROR("Failed to create SSE parser");
      llm_stream_free(stream_ctx);
      json_object_put(request);
      return 1;
   }

   streaming_ctx.sse_parser = sse_parser;
   streaming_ctx.stream_ctx = stream_ctx;
   curl_buffer_init(&streaming_ctx.raw_response);

   /* Retry loop: attempt CURL request up to 2 times on transient connection failures */
   int max_attempts = 2;
   for (int attempt = 0; attempt < max_attempts; attempt++) {
      curl_handle = curl_easy_init();
      if (!curl_handle) {
         OLOG_ERROR("Failed to initialize CURL");
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         curl_buffer_free(&streaming_ctx.raw_response);
         json_object_put(request);
         return 1;
      }

      headers = build_claude_headers(api_key);
      snprintf(full_url, sizeof(full_url), "%s%s", base_url, CLAUDE_MESSAGES_ENDPOINT);

      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, claude_streaming_write_callback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&streaming_ctx);
      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);
      // For streaming: use inactivity timeout instead of hard wall timeout.
      // Abort if transfer drops below 1 byte/sec for 60 seconds (no data flowing).
      // This allows long responses to complete while still catching hung connections.
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 60L);
      curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);

      // No hard timeout for streaming - rely on low-speed detection instead.
      // The llm_timeout_ms config is only used for non-streaming requests.

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         /* Check if this is a retryable connection error.
          * Only retry CURLE_OPERATION_TIMEDOUT if no data was received (connect-phase
          * timeout). If data arrived, the server was processing — retrying could cause
          * duplicate side effects and would corrupt the accumulated stream_ctx state. */
         bool retryable = (res == CURLE_COULDNT_CONNECT || res == CURLE_COULDNT_RESOLVE_HOST);
         if (res == CURLE_OPERATION_TIMEDOUT && streaming_ctx.raw_response.size == 0) {
            retryable = true;
         }

         if (retryable && attempt < max_attempts - 1) {
            OLOG_WARNING("CURL connect failed (%s), retrying in 1s... (attempt %d/%d)",
                         curl_easy_strerror(res), attempt + 1, max_attempts);
            curl_easy_cleanup(curl_handle);
            curl_slist_free_all(headers);
            curl_handle = NULL;
            headers = NULL;
            /* Reset streaming context for retry */
            curl_buffer_free(&streaming_ctx.raw_response);
            curl_buffer_init(&streaming_ctx.raw_response);
            sse_parser_reset(sse_parser);
            /* Cancellation-aware 1s backoff before retry */
            for (int ms = 0; ms < 1000; ms += 100) {
               if (llm_is_interrupt_requested())
                  break;
               usleep(100000);
            }
            continue;
         }

         if (res == CURLE_ABORTED_BY_CALLBACK) {
            OLOG_INFO("LLM transfer interrupted by user");
         } else {
            OLOG_ERROR("CURL failed: %s", curl_easy_strerror(res));
         }
#ifdef ENABLE_WEBUI
         if (res != CURLE_ABORTED_BY_CALLBACK) {
            session_t *session = session_get_command_context();
            if (session && session->type == SESSION_TYPE_WEBUI) {
               const char *error_code = (res == CURLE_OPERATION_TIMEDOUT) ? "LLM_TIMEOUT"
                                                                          : "LLM_ERROR";
               webui_send_error(session, error_code, curl_easy_strerror(res));
            }
         }
#endif
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         json_object_put(request);
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         curl_buffer_free(&streaming_ctx.raw_response);
         return 1;
      }

      /* CURL succeeded - break out of retry loop */
      break;
   }

   long http_code = 0;
   curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

   if (http_code != 200) {
      OLOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
      if (http_code == 429 || (http_code >= 500 && http_code < 600)) {
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      }
      if (http_code == 400 && streaming_ctx.raw_response.data) {
         OLOG_ERROR("Claude error response: %s", streaming_ctx.raw_response.data);
      }
#ifdef ENABLE_WEBUI
      session_t *session = session_get_command_context();
      if (session && session->type == SESSION_TYPE_WEBUI) {
         const char *error_msg = parse_claude_error_message(streaming_ctx.raw_response.data,
                                                            http_code);
         webui_send_error(session, "LLM_ERROR", error_msg);
      }
#endif
      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      json_object_put(request);
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      curl_buffer_free(&streaming_ctx.raw_response);
      return 1;
   }

   curl_easy_cleanup(curl_handle);
   curl_slist_free_all(headers);
   curl_buffer_free(&streaming_ctx.raw_response);

   /* Populate result from stream context */
   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         result->has_tool_calls = true;
         memcpy(&result->tool_calls, tool_calls, sizeof(tool_call_list_t));
      }
   }

   /* Always capture streamed text — even when tool calls are present.
    * The LLM may stream text before tool_use blocks (e.g. "Let me check that...").
    * The tool loop needs this to include it in follow-up history so the LLM
    * doesn't repeat itself after the tool result comes back. */
   result->text = llm_stream_get_response(stream_ctx);

   if (stream_ctx->finish_reason[0] != '\0') {
      safe_strscpy(result->finish_reason, stream_ctx->finish_reason);
   }

   /* Extract thinking content and signature for follow-up history */
   result->thinking_content = llm_stream_get_thinking(stream_ctx);
   result->thinking_signature = llm_stream_get_thinking_signature(stream_ctx);
   result->reasoning_tokens = stream_ctx->reasoning_tokens;

   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(request);

   return 0;
}
