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
#include <strings.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_claude_betas.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_claude_parts.h"
#include "llm/llm_claude_route.h"
#include "llm/llm_interface.h"
#include "llm/llm_key_tag.h"
#include "llm/llm_openai.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
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
 * @param api_key The route's API key (required)
 * @param route Where the request goes (OpenRouter takes a Bearer key)
 * @param betas The betas the body carries (claude_betas_add)
 * @return CURL header list (caller must free with curl_slist_free_all)
 */
static struct curl_slist *build_claude_headers(const char *api_key,
                                               const llm_claude_route_t *route,
                                               const claude_betas_t *betas) {
   struct curl_slist *headers = NULL;
   char api_key_header[512];
   char version_header[128];

   headers = curl_slist_append(headers, "Content-Type: application/json");

   // Claude uses x-api-key instead of Authorization; OpenRouter its own Bearer key
   snprintf(api_key_header, sizeof(api_key_header),
            route->provider == CLOUD_PROVIDER_OPENROUTER ? "Authorization: Bearer %s"
                                                         : "x-api-key: %s",
            api_key);
   headers = curl_slist_append(headers, api_key_header);

   // Claude requires API version header
   snprintf(version_header, sizeof(version_header), "anthropic-version: %s", CLAUDE_API_VERSION);
   headers = curl_slist_append(headers, version_header);
   if (route->provider == CLOUD_PROVIDER_OPENROUTER) {
      /* OpenRouter's app attribution, as on its Chat Completions route */
      headers = curl_slist_append(headers,
                                  "HTTP-Referer: https://github.com/The-OASIS-Project/dawn");
      headers = curl_slist_append(headers, "X-Title: DAWN");
   }
   headers = claude_betas_header(headers, betas);

   return headers;
}


/* OpenRouter's own fields: served by Anthropic only (a conversation's cache and
 * its reasoning's binding are per platform, so a Bedrock or Vertex fallback
 * would be a silent full miss), and the conversation as its sticky-routing
 * key. */
static void add_route_fields(json_object *request, const llm_claude_route_t *route) {
   if (route->provider != CLOUD_PROVIDER_OPENROUTER) {
      return;
   }
   json_object *provider = json_object_new_object();
   json_object *order = json_object_new_array();
   json_object_array_add(order, json_object_new_string("anthropic"));
   json_object_object_add(provider, "order", order);
   json_object_object_add(provider, "allow_fallbacks", json_object_new_boolean(0));
   json_object_object_add(request, "provider", provider);
   session_t *session = llm_cache_monitor_in_side_call() ? NULL : session_get_command_context();
   const int64_t conv = session ? atomic_load(&session->stream_conversation_id) : 0;
   if (conv > 0) {
      char key[64];
      snprintf(key, sizeof(key), "dawn-conv-%lld", (long long)conv);
      json_object_object_add(request, "session_id", json_object_new_string(key));
   }
}

static char *claude_chat_completion_once(struct json_object *conversation_history,
                                         const char *input_text,
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
   char carrier[LLM_CARRIER_MAX];
   llm_request_carrier(base_url, api_key, carrier, sizeof(carrier));
   json_object *request = convert_to_claude_format(conversation_history, input_text, model, carrier,
                                                   0, claude_betas_render_inline(base_url));
   if (!request) {
      OLOG_ERROR("Failed to convert conversation to Claude format");
      return NULL;
   }

   const llm_claude_route_t route = llm_claude_route(base_url);
   add_route_fields(request, &route);
   claude_betas_t betas;
   claude_betas_add(request, base_url, &betas);
   llm_cache_monitor_note_request(request); /* for this call's "LLM cache:" line */
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

   headers = build_claude_headers(api_key, &route, &betas);
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
      if (claude_betas_rejected(http_code, chunk.data, &betas)) {
         /* retried without them by llm_claude_chat_completion */
         if (betas.inline_rejected) {
            llm_note_inline_tools_rejected();
         }
      } else if (http_code == 401) {
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

   // Log cache usage (important for cost monitoring)
   json_object *usage_obj, *cache_creation_obj, *cache_read_obj;
   int input_tokens = 0;
   int output_tokens = 0;
   int cached_tokens = 0;
   int cache_created = 0;
   if (json_object_object_get_ex(parsed, "usage", &usage_obj)) {
      // Log total tokens
      json_object *input_tokens_obj, *output_tokens_obj;
      if (json_object_object_get_ex(usage_obj, "input_tokens", &input_tokens_obj) &&
          json_object_object_get_ex(usage_obj, "output_tokens", &output_tokens_obj)) {
         input_tokens = json_object_get_int(input_tokens_obj);
         output_tokens = json_object_get_int(output_tokens_obj);
         OLOG_DEBUG("Total tokens: %d input + %d output = %d", input_tokens, output_tokens,
                    input_tokens + output_tokens);
      }

      // Log cache creation
      if (json_object_object_get_ex(usage_obj, "cache_creation_input_tokens",
                                    &cache_creation_obj)) {
         cache_created = json_object_get_int(cache_creation_obj);
         if (cache_created > 0) {
            OLOG_DEBUG("Claude cache created: %d tokens", cache_created);
         }
      }

      // Log cache hits (this is where we save money!)
      if (json_object_object_get_ex(usage_obj, "cache_read_input_tokens", &cache_read_obj)) {
         cached_tokens = json_object_get_int(cache_read_obj);
         if (cached_tokens > 0) {
            OLOG_DEBUG("Claude cache hit: %d tokens", cached_tokens);
         }
      }

      // Record metrics under the route's provider (Claude, or OpenRouter)
      metrics_record_llm_tokens(LLM_CLOUD, route.provider, input_tokens, output_tokens,
                                cached_tokens);

      /* Usage tracking + the per-call cache record, as the streaming path does.
       * Anthropic's input_tokens is the UNCACHED part: the prompt is its sum
       * with the cache read and write. */
      session_t *session = session_get_command_context();
      char message_id[64] = "";
      char miss_reason[32] = "";
      int missed_tokens = 0;
      json_object *v = NULL;
      json_object *diag = NULL;
      json_object *miss = NULL;
      if (json_object_object_get_ex(parsed, "id", &v)) {
         safe_strscpy(message_id, json_object_get_string(v));
      }
      if (json_object_object_get_ex(parsed, "diagnostics", &diag) &&
          json_object_object_get_ex(diag, "cache_miss_reason", &miss)) {
         if (json_object_object_get_ex(miss, "type", &v)) {
            safe_strscpy(miss_reason, json_object_get_string(v));
         }
         if (json_object_object_get_ex(miss, "cache_missed_input_tokens", &v)) {
            missed_tokens = json_object_get_int(v);
         }
      }
      llm_claude_drops_t drops;
      memset(&drops, 0, sizeof(drops));
      llm_claude_drops_from_message(parsed, &drops);
      llm_usage_report_t usage = {
         .prompt_tokens = input_tokens + cached_tokens + cache_created,
         .completion_tokens = output_tokens,
         .cached_tokens = cached_tokens,
         .cache_write_tokens = cache_created,
         .type = LLM_CLOUD,
         .provider = route.provider,
         .message_id = message_id,
         .cache_miss_reason = miss_reason,
         .cache_missed_tokens = missed_tokens,
         .drops = &drops,
      };
      llm_context_update_usage(session ? session->session_id : 0, &usage);
   }

   /* The reply is its text blocks; a model that can't turn thinking off may put
    * a thinking block first. */
   json_object *content_array = NULL;
   if (json_object_object_get_ex(parsed, "content", &content_array) &&
       json_object_is_type(content_array, json_type_array)) {
      response = llm_claude_content_text(parsed);
   }
   if (response && !response[0]) {
      free(response);
      response = NULL;
   }
   if (!response) {
      OLOG_ERROR("Claude response has no text");
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

#ifdef ENABLE_WEBUI
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
   /* OpenRouter wraps the upstream error ("Provider returned error") and keeps
    * Anthropic's own body in error.metadata.raw: say what Anthropic said. */
   struct json_object *metadata = NULL;
   struct json_object *raw = NULL;
   struct json_object *upstream = NULL;
   struct json_object *up_error = NULL;
   if (json_object_object_get_ex(error_obj, "metadata", &metadata) &&
       json_object_object_get_ex(metadata, "raw", &raw) &&
       json_object_is_type(raw, json_type_string) &&
       (upstream = json_tokener_parse(json_object_get_string(raw))) != NULL &&
       json_object_object_get_ex(upstream, "error", &up_error) &&
       json_object_object_get_ex(up_error, "message", &message_obj) &&
       json_object_is_type(message_obj, json_type_string)) {
      message = json_object_get_string(message_obj);
   }

   if (message && message[0] != '\0') {
      snprintf(error_msg, sizeof(error_msg), "%s", message);
   } else {
      snprintf(error_msg, sizeof(error_msg), "API request failed (HTTP %ld)", http_code);
   }

   json_object_put(upstream);
   json_object_put(root);
   return error_msg;
}
#endif /* ENABLE_WEBUI */

static int claude_single_shot_once(struct json_object *conversation_history,
                                   const char *input_text,
                                   const char *base_url,
                                   const char *api_key,
                                   const char *model,
                                   llm_claude_text_chunk_callback chunk_callback,
                                   void *callback_userdata,
                                   int iteration,
                                   llm_tool_response_t *result) {
   CURL *curl_handle = NULL;
   CURLcode res = CURLE_FAILED_INIT;
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
   /* Each attempt's own outcome: the tool loop retries on what this call sets,
    * never on what an earlier attempt left. */
   llm_set_last_error(LLM_ERR_NONE);

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
   /* Whose stored reasoning this request may send back, and whose this turn's is. */
   char carrier[LLM_CARRIER_MAX];
   llm_request_carrier(base_url, api_key, carrier, sizeof(carrier));
   request = convert_to_claude_format(conversation_history, input_text, model, carrier, iteration,
                                      claude_betas_render_inline(base_url));
   if (!request) {
      OLOG_ERROR("Failed to convert conversation to Claude format");
      return 1;
   }

   json_object_object_add(request, "stream", json_object_new_boolean(1));

   const llm_claude_route_t route = llm_claude_route(base_url);
   add_route_fields(request, &route);
   claude_betas_t betas;
   claude_betas_add(request, base_url, &betas);
   llm_cache_monitor_note_request(request); /* for this call's "LLM cache:" line */
   payload = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN |
                                                         JSON_C_TO_STRING_NOSLASHESCAPE);

   OLOG_INFO("Claude single-shot iter %d: url=%s", iteration, base_url);

   /* Create streaming context */
   stream_ctx = llm_stream_create_messages(route.provider, chunk_callback, callback_userdata);
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

      headers = build_claude_headers(api_key, &route, &betas);
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
      const bool retrying = claude_betas_rejected(http_code, streaming_ctx.raw_response.data,
                                                  &betas);
      if (betas.inline_rejected) {
         llm_note_inline_tools_rejected(); /* the turn's caller records it */
      }
      OLOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
      if (http_code == 429 || (http_code >= 500 && http_code < 600)) {
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      }
      if (http_code == 400 && streaming_ctx.raw_response.data) {
         OLOG_ERROR("Claude error response: %s", streaming_ctx.raw_response.data);
      }
#ifdef ENABLE_WEBUI
      session_t *session = session_get_command_context();
      if (!retrying && session && session->type == SESSION_TYPE_WEBUI) {
         const char *error_msg = parse_claude_error_message(streaming_ctx.raw_response.data,
                                                            http_code);
         webui_send_error(session, "LLM_ERROR", error_msg);
      }
#else
      (void)retrying; /* the call above still drops a rejected beta for the retry */
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

   if (llm_stream_check_finished(stream_ctx, "Claude API") != 0) {
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      json_object_put(request);
      return 1;
   }

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
   /* The turn's blocks exactly as sent: what the next request replays. */
   struct json_object *native = llm_stream_take_claude_content(stream_ctx);
   result->blocks = llm_turn_blocks_from_claude(native, carrier, model);
   json_object_put(native);
   result->reasoning_tokens = stream_ctx->reasoning_tokens;

   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(request);

   return 0;
}

/* The public entry points: a request is sent again, without the beta, when
 * Anthropic rejects one (see claude_betas_rejected). */
char *llm_claude_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 const char *base_url,
                                 const char *api_key,
                                 const char *model) {
   char *response = NULL;
   for (int attempt = 0; attempt < CLAUDE_BETA_ATTEMPTS; attempt++) {
      response = claude_chat_completion_once(conversation_history, input_text, base_url, api_key,
                                             model);
      if (response || !claude_betas_take_retry()) {
         break;
      }
   }
   claude_betas_take_retry(); /* no mark outlives this call */
   return response;
}

int llm_claude_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_claude_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     llm_tool_response_t *result) {
   int rc = 1;
   for (int attempt = 0; attempt < CLAUDE_BETA_ATTEMPTS; attempt++) {
      rc = claude_single_shot_once(conversation_history, input_text, base_url, api_key, model,
                                   chunk_callback, callback_userdata, iteration, result);
      if (rc == 0 || !claude_betas_take_retry()) {
         break;
      }
   }
   claude_betas_take_retry(); /* no mark outlives this call */
   return rc;
}
