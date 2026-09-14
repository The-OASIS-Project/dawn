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
 * OpenAI /v1/chat/completions API implementation. Non-streaming, streaming
 * (with recursive tool execution), and single-shot streaming paths including
 * vision handling and tool-call iteration. History conversion lives in
 * llm_openai_history.c.
 */

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_claude.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_openai.h"
#include "llm/llm_openai_cache.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
#include "llm/sse_parser.h"
#include "logging.h"
#include "ui/metrics.h"
#include "utils/string_utils.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

extern int llm_curl_progress_callback(void *clientp,
                                      curl_off_t dltotal,
                                      curl_off_t dlnow,
                                      curl_off_t ultotal,
                                      curl_off_t ulnow);

/* Maximum tool call iterations to prevent infinite loops (used by old recursive path) */
#define MAX_TOOL_ITERATIONS 8

/* A chat-template / Jinja render failure is DETERMINISTIC: the raise lives in the
 * model's GGUF chat template (e.g. Qwen 3.5/3.6's "System message must be at the
 * beginning"), so the identical request fails identically on every retry.  Detect
 * it from the server's error body so the tool loop's transient-5xx retry path
 * fails fast instead of burning ~7s on three hopeless exponential-backoff
 * attempts.  Genuine transient 5xx (no template signature) stay retryable. */
static bool llm_openai_is_deterministic_template_error(const char *body) {
   if (body == NULL) {
      return false;
   }
   /* Match specific chat-template raise signatures only.  The bare token "Jinja" is
    * deliberately NOT matched — a genuinely transient 5xx whose body merely mentions
    * Jinja must stay retryable; a real template raise carries one of these phrases. */
   return strstr(body, "System message must be at the beginning") != NULL ||
          strstr(body, "raise_exception") != NULL;
}

static bool is_current_session_remote(void) {
   session_t *session = session_get_command_context();
   if (!session) {
      return false;
   }
   return (session->type != SESSION_TYPE_LOCAL);
}

/* ── Streaming infrastructure ───────────────────────────────────────────── */

typedef struct {
   sse_parser_t *sse_parser;
   llm_stream_context_t *stream_ctx;
   curl_buffer_t raw_response; /**< Raw response for error diagnostics (cap 4KB) */
} openai_streaming_context_t;

#define OPENAI_RAW_BUFFER_CAP 4096

static size_t streaming_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
   size_t realsize = size * nmemb;
   openai_streaming_context_t *ctx = (openai_streaming_context_t *)userp;

   /* Capture raw response for error diagnostics (cap at OPENAI_RAW_BUFFER_CAP).
    * Matches Claude's streaming context pattern (see llm_claude.c). */
   if (ctx->raw_response.size < OPENAI_RAW_BUFFER_CAP) {
      size_t space = OPENAI_RAW_BUFFER_CAP - ctx->raw_response.size;
      size_t to_copy = realsize < space ? realsize : space;
      curl_buffer_write_callback(contents, 1, to_copy, &ctx->raw_response);
   }

   sse_parser_feed(ctx->sse_parser, contents, realsize);

   return realsize;
}

static void openai_sse_event_handler(const char *event_type,
                                     const char *event_data,
                                     void *userdata) {
   openai_streaming_context_t *ctx = (openai_streaming_context_t *)userdata;
   llm_stream_handle_event(ctx->stream_ctx, event_data);
}

/* ── Local LLM thinking parameters ─────────────────────────────────────── */

static void add_local_thinking_params(json_object *root) {
   const char *thinking_mode = llm_get_current_thinking_mode();
   local_provider_t provider = llm_local_get_provider();

   if (provider == LOCAL_PROVIDER_OLLAMA) {
      if (strcmp(thinking_mode, "disabled") != 0 && !llm_tools_suppressed()) {
         json_object_object_add(root, "think", json_object_new_boolean(1));
         OLOG_INFO("Local LLM (Ollama): Thinking enabled (think: true)");
      } else {
         json_object_object_add(root, "think", json_object_new_boolean(0));
         OLOG_INFO("Local LLM (Ollama): Thinking disabled (think: false)");
      }
   } else {
      if (strcmp(thinking_mode, "disabled") != 0 && !llm_tools_suppressed()) {
         json_object *thinking = json_object_new_object();
         json_object_object_add(thinking, "type", json_object_new_string("enabled"));

         int budget = llm_get_effective_budget_tokens();
         json_object_object_add(thinking, "budget_tokens", json_object_new_int(budget));

         json_object_object_add(root, "thinking", thinking);

         json_object_object_add(root, "thinking_forced_open", json_object_new_boolean(1));

         json_object *template_kwargs = json_object_new_object();
         json_object_object_add(template_kwargs, "enable_thinking", json_object_new_boolean(1));
         json_object_object_add(root, "chat_template_kwargs", template_kwargs);

         OLOG_INFO("Local LLM (llama.cpp): Extended thinking enabled (budget: %d tokens, "
                   "forced_open: true, chat_template_kwargs.enable_thinking: true)",
                   budget);
      } else if (strcmp(thinking_mode, "disabled") == 0) {
         json_object_object_add(root, "reasoning_budget", json_object_new_int(0));

         json_object *template_kwargs = json_object_new_object();
         json_object_object_add(template_kwargs, "enable_thinking", json_object_new_boolean(0));
         json_object_object_add(root, "chat_template_kwargs", template_kwargs);

         OLOG_INFO("Local LLM (llama.cpp): Reasoning explicitly disabled (reasoning_budget: 0)");
      }
   }
}

/* ── Cloud reasoning effort ─────────────────────────────────────────────── */

static void add_cloud_reasoning_effort(json_object *root,
                                       const char *model_name,
                                       const char *base_url) {
   const char *thinking_mode = llm_get_current_thinking_mode();

   /* OpenRouter: use the unified reasoning OBJECT (`reasoning: { effort }`), not OpenAI's
    * flat `reasoning_effort` string.  Gate on the gateway endpoint rather than model-name
    * heuristics — OpenRouter slugs are "vendor/model" (e.g. anthropic/claude-sonnet-4.6) so
    * the gpt-5/o-series/gemini prefix checks below never match.  OpenRouter ignores the
    * field for models that don't support reasoning, so it's safe to always send it when
    * thinking is on.  The streamed reasoning comes back in delta.reasoning_details[]
    * (parsed in llm_streaming.c). */
   if (base_url && strstr(base_url, "openrouter.ai") != NULL) {
      if (strcmp(thinking_mode, "disabled") == 0 || llm_tools_suppressed()) {
         return;
      }
      const char *effort = thinking_mode;
      if (strcmp(effort, "enabled") == 0 || strcmp(effort, "auto") == 0) {
         effort = g_config.llm.thinking.reasoning_effort;
         if (!effort || effort[0] == '\0') {
            effort = "medium";
         }
      }
      json_object *reasoning = json_object_new_object();
      json_object_object_add(reasoning, "effort", json_object_new_string(effort));
      json_object_object_add(root, "reasoning", reasoning);
      OLOG_INFO("OpenRouter: reasoning effort '%s' requested for model %s", effort, model_name);
      return;
   }

   bool is_o_series = (strncmp(model_name, "o1", 2) == 0 || strncmp(model_name, "o3", 2) == 0);
   bool is_gpt5 = (strncmp(model_name, "gpt-5", 5) == 0);
   bool is_gemini_thinking = (strncmp(model_name, "gemini-2.5", 10) == 0 ||
                              strncmp(model_name, "gemini-3", 8) == 0);
   bool supports_reasoning = is_o_series || is_gpt5 || is_gemini_thinking;

   if (supports_reasoning && !llm_tools_suppressed()) {
      const char *effort = NULL;

      if (strcmp(thinking_mode, "disabled") == 0) {
         if (is_gpt5) {
            effort = llm_openai_is_gpt5_base_family(model_name) ? "minimal" : "none";
         } else if (is_gemini_thinking) {
            effort = "low";
         }
      } else {
         effort = thinking_mode;
         if (strcmp(effort, "enabled") == 0 || strcmp(effort, "auto") == 0) {
            effort = g_config.llm.thinking.reasoning_effort;
            if (effort[0] == '\0')
               effort = "medium";
         }
      }

      if (effort != NULL) {
         effort = llm_openai_clamp_effort_for_model(model_name, effort);
         json_object_object_add(root, "reasoning_effort", json_object_new_string(effort));
         OLOG_INFO("Cloud LLM: Reasoning effort set to '%s' for model %s", effort, model_name);
      }
   }
}

/* ── Non-streaming chat completion ──────────────────────────────────────── */

char *llm_openai_cc_chat_completion(struct json_object *conversation_history,
                                    const char *input_text,
                                    const char **vision_images,
                                    const size_t *vision_image_sizes,
                                    int vision_image_count,
                                    const char *base_url,
                                    const char *api_key,
                                    const char *model) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";

   curl_buffer_t chunk;

   const char *payload = NULL;
   char *response = NULL;
   int total_tokens = 0;

   json_object *root = NULL;

   json_object *parsed_json = NULL;
   json_object *choices = NULL;
   json_object *first_choice = NULL;
   json_object *message = NULL;
   json_object *content = NULL;
   json_object *finish_reason = NULL;
   json_object *usage_obj = NULL;
   json_object *total_tokens_obj = NULL;

   json_object *converted_history = llm_openai_prepare_chat_history(conversation_history);

   root = json_object_new_object();

   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }
   if (model_name && model_name[0] != '\0') {
      json_object_object_add(root, "model", json_object_new_string(model_name));
   }

   if (vision_images != NULL && vision_image_count > 0) {
      /* Copy-on-write: converted_history may share message objects with the
       * caller's session history, so apply vision to a private copy. */
      json_object *with_vision = llm_openai_apply_vision_images(converted_history, input_text,
                                                                vision_images, vision_image_sizes,
                                                                vision_image_count);
      if (with_vision != NULL) {
         json_object_put(converted_history);
         converted_history = with_vision;
      }
   }

   json_object_object_add(root, "messages", converted_history);

   if (api_key == NULL) {
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   if (llm_tools_enabled(NULL)) {
      bool is_remote = is_current_session_remote();
      struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         OLOG_INFO("OpenAI: Added %d tools to request (%s session)",
                   llm_tools_get_enabled_count_filtered(is_remote), is_remote ? "remote" : "local");
      }
   }

   /* Finalize system-prompt shape for caching: keep the two-segment split for the
    * OpenRouter→Anthropic cache path, else collapse it into one system message so
    * strict local chat templates (Qwen 3.5/3.6 Jinja) don't reject the second. */
   llm_openai_apply_system_prompt_caching(root, model_name, base_url);

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   OLOG_INFO("OpenAI request payload: %zu bytes (~%zu tokens est)", strlen(payload),
             strlen(payload) / 4);

   curl_buffer_init(&chunk);

   if (!llm_check_connection(base_url, 4)) {
      llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      OLOG_ERROR("Pre-flight connection check failed (cloud unreachable)");
      json_object_put(root);
      return NULL;
   }

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = llm_openai_build_headers(api_key, base_url);

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);
      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);

      curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);

      int effective_timeout = llm_get_effective_timeout_ms();
      if (effective_timeout > 0) {
         curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, (long)effective_timeout);
      }

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
            OLOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
         }
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         curl_buffer_free(&chunk);
         json_object_put(root);
         return NULL;
      }

      long http_code = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

      if (http_code != 200) {
         if (http_code == 401) {
            OLOG_ERROR("OpenAI API: Invalid or missing API key (HTTP 401)");
         } else if (http_code == 403) {
            OLOG_ERROR("OpenAI API: Access forbidden (HTTP 403) - check API key permissions");
         } else if (http_code == 429) {
            OLOG_ERROR("OpenAI API: Rate limit exceeded (HTTP 429)");
            llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
         } else if (http_code >= 500 && http_code < 600) {
            OLOG_ERROR("OpenAI API: Server error (HTTP %ld)", http_code);
            if (llm_openai_is_deterministic_template_error(chunk.data)) {
               OLOG_ERROR("OpenAI API: deterministic chat-template error in response — "
                          "failing fast (not retrying)");
            } else {
               llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
            }
         } else if (http_code != 0) {
            OLOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
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
      OLOG_ERROR("Failed to parse JSON response.");
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   if (!json_object_object_get_ex(parsed_json, "choices", &choices) ||
       json_object_get_type(choices) != json_type_array || json_object_array_length(choices) < 1) {
      OLOG_ERROR("Error in parsing response: 'choices' missing or invalid.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   first_choice = json_object_array_get_idx(choices, 0);
   if (!first_choice) {
      OLOG_ERROR("Error: 'choices' array is empty.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   if (!json_object_object_get_ex(first_choice, "message", &message) ||
       !json_object_object_get_ex(message, "content", &content)) {
      OLOG_ERROR("Error: 'message' or 'content' field missing.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   json_object_object_get_ex(first_choice, "finish_reason", &finish_reason);

   int input_tokens = 0;
   int output_tokens = 0;
   int cached_tokens = 0;
   if (json_object_object_get_ex(parsed_json, "usage", &usage_obj)) {
      if (json_object_object_get_ex(usage_obj, "total_tokens", &total_tokens_obj)) {
         total_tokens = json_object_get_int(total_tokens_obj);
         OLOG_WARNING("Total tokens: %d", total_tokens);
      }

      json_object *prompt_tokens_obj = NULL;
      json_object *completion_tokens_obj = NULL;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens", &prompt_tokens_obj)) {
         input_tokens = json_object_get_int(prompt_tokens_obj);
      }
      if (json_object_object_get_ex(usage_obj, "completion_tokens", &completion_tokens_obj)) {
         output_tokens = json_object_get_int(completion_tokens_obj);
      }

      json_object *prompt_tokens_details = NULL;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens_details", &prompt_tokens_details)) {
         json_object *cached_tokens_obj = NULL;
         if (json_object_object_get_ex(prompt_tokens_details, "cached_tokens",
                                       &cached_tokens_obj)) {
            cached_tokens = json_object_get_int(cached_tokens_obj);
            if (cached_tokens > 0) {
               /* Both OpenAI and Gemini-2.5+ surface implicit prompt caching
                * via the same prompt_tokens_details.cached_tokens field
                * (Gemini routes through our OpenAI-compat shim).  Detect
                * which upstream we hit so the log line labels the provider
                * correctly.  Dropped the explicit savings percentage —
                * OpenAI's discount and Gemini's discount differ AND drift
                * with provider pricing changes; the "X tokens cached" count
                * is the unambiguous operator signal. */
               const char *provider_label = (base_url &&
                                             strstr(base_url, "generativelanguage.googleapis.com"))
                                                ? "Gemini"
                                                : "OpenAI";
               OLOG_INFO("%s cache hit: %d tokens cached", provider_label, cached_tokens);
            }
         }
      }

      llm_type_t token_type = (api_key != NULL) ? LLM_CLOUD : LLM_LOCAL;
      metrics_record_llm_tokens(token_type, CLOUD_PROVIDER_OPENAI, input_tokens, output_tokens,
                                cached_tokens);

      session_t *session = session_get_command_context();
      uint32_t session_id = session ? session->session_id : 0;
      llm_context_update_usage(session_id, input_tokens, output_tokens, cached_tokens);
   }

   const char *content_str = json_object_get_string(content);
   if (!content_str) {
      json_object *tool_calls = NULL;
      if (json_object_object_get_ex(message, "tool_calls", &tool_calls) &&
          json_object_get_type(tool_calls) == json_type_array &&
          json_object_array_length(tool_calls) > 0) {
         OLOG_WARNING("OpenAI: LLM returned tool call instead of content (non-streaming API "
                      "doesn't support tool execution)");
         response = strdup("I apologize, but I was unable to complete that request directly. "
                           "Please try rephrasing your question.");
      } else {
         OLOG_ERROR("Error: 'content' field is empty or null with no tool calls.");
         json_object_put(parsed_json);
         curl_buffer_free(&chunk);
         json_object_put(root);
         return NULL;
      }
   } else {
      response = strdup(content_str);
   }

   if ((finish_reason != NULL) && (strcmp(json_object_get_string(finish_reason), "stop") != 0)) {
      OLOG_WARNING("OpenAI returned with finish_reason: %s", json_object_get_string(finish_reason));
   } else {
      OLOG_INFO("Response finished properly.");
   }

   json_object_put(parsed_json);
   curl_buffer_free(&chunk);
   json_object_put(root);

   return response;
}

/* ── Streaming with recursive tool execution ────────────────────────────── */

static char *llm_openai_streaming_internal(struct json_object *conversation_history,
                                           const char *input_text,
                                           const char **vision_images,
                                           const size_t *vision_image_sizes,
                                           int vision_image_count,
                                           const char *base_url,
                                           const char *api_key,
                                           const char *model,
                                           llm_openai_text_chunk_callback chunk_callback,
                                           void *callback_userdata,
                                           int iteration) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";

   const char *payload = NULL;
   char *response = NULL;

   json_object *root = NULL;

   sse_parser_t *sse_parser = NULL;
   llm_stream_context_t *stream_ctx = NULL;
   openai_streaming_context_t streaming_ctx;

   json_object *converted_history = llm_openai_prepare_chat_history(conversation_history);

   root = json_object_new_object();

   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }
   if (model_name && model_name[0] != '\0') {
      json_object_object_add(root, "model", json_object_new_string(model_name));
   }

   json_object_object_add(root, "stream", json_object_new_boolean(1));
   json_object *stream_opts = json_object_new_object();
   json_object_object_add(stream_opts, "include_usage", json_object_new_boolean(1));
   json_object_object_add(root, "stream_options", stream_opts);

   if (api_key == NULL) {
      json_object_object_add(root, "timings_per_token", json_object_new_boolean(1));
      add_local_thinking_params(root);
   } else {
      add_cloud_reasoning_effort(root, model_name, base_url);
   }

   if (vision_images != NULL && vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < vision_image_count; i++) {
         if (vision_image_sizes && vision_images[i]) {
            total_bytes += vision_image_sizes[i];
         }
      }
      OLOG_INFO("OpenAI streaming: %d vision images provided (%zu total bytes)", vision_image_count,
                total_bytes);
      /* Copy-on-write: converted_history may share message objects with the
       * caller's session history, so apply vision to a private copy. */
      json_object *with_vision = llm_openai_apply_vision_images(converted_history, input_text,
                                                                vision_images, vision_image_sizes,
                                                                vision_image_count);
      if (with_vision != NULL) {
         json_object_put(converted_history);
         converted_history = with_vision;
      }
   }

   json_object_object_add(root, "messages", converted_history);

   if (api_key == NULL) {
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   if (llm_tools_enabled(NULL) && iteration < MAX_TOOL_ITERATIONS) {
      bool is_remote = is_current_session_remote();
      struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         OLOG_INFO("OpenAI streaming: Added %d tools to request (%s session)",
                   llm_tools_get_enabled_count_filtered(is_remote), is_remote ? "remote" : "local");
      }
   } else if (iteration >= MAX_TOOL_ITERATIONS) {
      OLOG_INFO("OpenAI streaming: Skipping tools (forcing text response at iteration %d)",
                iteration);
   }

   /* Finalize system-prompt shape for caching: keep the two-segment split for the
    * OpenRouter→Anthropic cache path, else collapse it into one system message so
    * strict local chat templates (Qwen 3.5/3.6 Jinja) don't reject the second. */
   llm_openai_apply_system_prompt_caching(root, model_name, base_url);

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   OLOG_INFO("OpenAI streaming request payload: %zu bytes (~%zu tokens est)", strlen(payload),
             strlen(payload) / 4);

   OLOG_INFO("OpenAI streaming iter %d: url=%s model=%s api_key=%s", iteration, base_url,
             (model_name && model_name[0]) ? model_name : "(server default)",
             LOG_CREDENTIAL_STATUS(api_key));

   if (iteration > 0) {
      int msg_count = json_object_array_length(converted_history);
      OLOG_INFO("OpenAI streaming iter %d: %d messages in history", iteration, msg_count);
      for (int i = msg_count > 3 ? msg_count - 3 : 0; i < msg_count; i++) {
         json_object *msg = json_object_array_get_idx(converted_history, i);
         json_object *role_obj, *content_obj;
         const char *role = "unknown";
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            role = json_object_get_string(role_obj);
         }
         if (json_object_object_get_ex(msg, "content", &content_obj)) {
            const char *content_str = json_object_get_string(content_obj);
            OLOG_INFO("  [%d] role=%s content=%.100s%s", i, role,
                      content_str ? content_str : "(null)",
                      (content_str && strlen(content_str) > 100) ? "..." : "");
         } else {
            json_object *tc_obj;
            if (json_object_object_get_ex(msg, "tool_calls", &tc_obj)) {
               OLOG_INFO("  [%d] role=%s (has tool_calls)", i, role);
            } else {
               OLOG_INFO("  [%d] role=%s (no content)", i, role);
            }
         }
      }
   }

   if (!llm_check_connection(base_url, 4)) {
      llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      OLOG_ERROR("Pre-flight connection check failed (cloud unreachable)");
      json_object_put(root);
      return NULL;
   }

   llm_type_t stream_llm_type = (api_key != NULL) ? LLM_CLOUD : LLM_LOCAL;

   cloud_provider_t stream_provider = CLOUD_PROVIDER_OPENAI;
   session_t *ctx_session = session_get_command_context();
   if (ctx_session && stream_llm_type == LLM_CLOUD) {
      session_llm_config_t session_config;
      session_get_llm_config(ctx_session, &session_config);
      stream_provider = session_config.cloud_provider;
   } else if (base_url && strstr(base_url, "generativelanguage.googleapis.com")) {
      stream_provider = CLOUD_PROVIDER_GEMINI;
   } else if (base_url && strstr(base_url, "openrouter.ai")) {
      stream_provider = CLOUD_PROVIDER_OPENROUTER;
   }
   stream_ctx = llm_stream_create(stream_llm_type, stream_provider, chunk_callback,
                                  callback_userdata);
   if (!stream_ctx) {
      OLOG_ERROR("Failed to create LLM stream context");
      json_object_put(root);
      return NULL;
   }

   sse_parser = sse_parser_create(openai_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      OLOG_ERROR("Failed to create SSE parser");
      llm_stream_free(stream_ctx);
      json_object_put(root);
      return NULL;
   }

   streaming_ctx.sse_parser = sse_parser;
   streaming_ctx.stream_ctx = stream_ctx;
   curl_buffer_init_with_max(&streaming_ctx.raw_response, OPENAI_RAW_BUFFER_CAP);

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = llm_openai_build_headers(api_key, base_url);

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);

      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, streaming_write_callback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&streaming_ctx);

      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);

      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 60L);

      curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         const char *error_code = "LLM_ERROR";
         const char *error_msg = NULL;

         if (res == CURLE_ABORTED_BY_CALLBACK) {
            OLOG_INFO("LLM transfer interrupted by user");
         } else if (res == CURLE_OPERATION_TIMEDOUT) {
            OLOG_ERROR("LLM stream timed out (no data for 60 seconds)");
            error_code = "LLM_TIMEOUT";
            error_msg = "Request timed out - AI server may be overloaded";
         } else {
            OLOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
            error_code = "LLM_CONNECTION_ERROR";
            error_msg = curl_easy_strerror(res);
         }

#ifdef ENABLE_WEBUI
         if (error_msg) {
            session_t *session = session_get_command_context();
            if (session && session->type == SESSION_TYPE_WEBUI) {
               webui_send_error(session, error_code, error_msg);
            }
         }
#endif

         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         curl_buffer_free(&streaming_ctx.raw_response);
         return NULL;
      }

      long http_code = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

      if (http_code != 200) {
         const char *error_code;
         if (http_code == 401) {
            OLOG_ERROR("OpenAI API: Invalid or missing API key (HTTP 401)");
            error_code = "LLM_AUTH_ERROR";
         } else if (http_code == 403) {
            OLOG_ERROR("OpenAI API: Access forbidden (HTTP 403) - check API key permissions");
            error_code = "LLM_ACCESS_ERROR";
         } else if (http_code == 429) {
            OLOG_ERROR("OpenAI API: Rate limit exceeded (HTTP 429)");
            error_code = "LLM_RATE_LIMIT";
            llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
         } else if (http_code >= 500 && http_code < 600) {
            OLOG_ERROR("OpenAI API: Server error (HTTP %ld)", http_code);
            error_code = "LLM_SERVER_ERROR";
            if (llm_openai_is_deterministic_template_error(streaming_ctx.raw_response.data)) {
               OLOG_ERROR("OpenAI API: deterministic chat-template error in response — "
                          "failing fast (not retrying)");
            } else {
               llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
            }
         } else {
            OLOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
            error_code = "LLM_ERROR";
         }

         if (streaming_ctx.raw_response.data && streaming_ctx.raw_response.size > 0) {
            OLOG_ERROR("OpenAI API error response: %s", streaming_ctx.raw_response.data);
         }

#ifdef ENABLE_WEBUI
         session_t *session = session_get_command_context();
         if (session && session->type == SESSION_TYPE_WEBUI) {
            const char *err_msg = llm_openai_parse_error_message(streaming_ctx.raw_response.data,
                                                                 http_code);
            webui_send_error(session, error_code, err_msg);
         }
#endif

         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         curl_buffer_free(&streaming_ctx.raw_response);
         return NULL;
      }

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
   }

   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         OLOG_INFO("OpenAI streaming: Executing %d tool call(s) at iteration %d", tool_calls->count,
                   iteration);

         for (int i = 0; i < tool_calls->count; i++) {
            OLOG_INFO("  Tool call [%d]: id=%s name=%s args=%s", i, tool_calls->calls[i].id,
                      tool_calls->calls[i].name, tool_calls->calls[i].arguments);
         }

         if (tool_calls->count > 0 &&
             llm_tools_is_duplicate_call(conversation_history, tool_calls->calls[0].name,
                                         tool_calls->calls[0].arguments, LLM_HISTORY_OPENAI)) {
            OLOG_WARNING("OpenAI streaming: Duplicate tool call detected, forcing text response");

            /* Plain instruction (no "[System:]" prefix) so a reasoning model
             * doesn't mistake this daemon control message for an injected
             * directive; tool-agnostic since it fires for any repeated tool. */
            json_object *hint_msg = json_object_new_object();
            json_object_object_add(hint_msg, "role", json_object_new_string("user"));
            json_object_object_add(
                hint_msg, "content",
                json_object_new_string(
                    "You already called that tool with identical arguments and have its result. "
                    "Answer using the information you already have — do not call it again."));
            json_object_array_add(conversation_history, hint_msg);

            sse_parser_free(sse_parser);
            llm_stream_free(stream_ctx);
            json_object_put(root);
            curl_buffer_free(&streaming_ctx.raw_response);

            OLOG_INFO("OpenAI streaming: Making final call without tools to force text response");

            llm_resolved_config_t config;
            char model_buf[LLM_MODEL_NAME_MAX] = "";
            bool config_ok = (llm_get_current_resolved_config(&config) == 0);
            if (config_ok && config.model && config.model[0] != '\0') {
               safe_strscpy(model_buf, config.model);
            }

            const char *fresh_url = config_ok ? config.endpoint : base_url;
            const char *fresh_key = config_ok ? config.api_key : api_key;
            const char *fresh_model = model_buf[0] ? model_buf : model;

            return llm_openai_streaming_internal(conversation_history, "", NULL, NULL, 0, fresh_url,
                                                 fresh_key, fresh_model, chunk_callback,
                                                 callback_userdata, MAX_TOOL_ITERATIONS);
         }

         tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
         if (!results) {
            OLOG_ERROR("OpenAI streaming: Failed to allocate tool results");
            sse_parser_free(sse_parser);
            llm_stream_free(stream_ctx);
            json_object_put(root);
            curl_buffer_free(&streaming_ctx.raw_response);
            return NULL;
         }
         llm_tools_execute_all(tool_calls, results);

         json_object *assistant_msg = json_object_new_object();
         json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));
         json_object_object_add(assistant_msg, "content", json_object_new_string(""));

         json_object *tc_array = json_object_new_array();
         for (int i = 0; i < tool_calls->count; i++) {
            json_object *tc = json_object_new_object();
            json_object_object_add(tc, "id", json_object_new_string(tool_calls->calls[i].id));
            json_object_object_add(tc, "type", json_object_new_string("function"));

            json_object *func = json_object_new_object();
            json_object_object_add(func, "name", json_object_new_string(tool_calls->calls[i].name));
            json_object_object_add(func, "arguments",
                                   json_object_new_string(tool_calls->calls[i].arguments));
            json_object_object_add(tc, "function", func);

            if (i == 0 && tool_calls->thought_signature[0] != '\0') {
               json_object *extra_content = json_object_new_object();
               json_object *google_obj = json_object_new_object();
               json_object_object_add(google_obj, "thought_signature",
                                      json_object_new_string(tool_calls->thought_signature));
               json_object_object_add(extra_content, "google", google_obj);
               json_object_object_add(tc, "extra_content", extra_content);
               OLOG_INFO("OpenAI streaming: Including thought_signature in follow-up request");
            }

            json_object_array_add(tc_array, tc);
         }
         json_object_object_add(assistant_msg, "tool_calls", tc_array);
         json_object_array_add(conversation_history, assistant_msg);

         llm_tools_add_results_openai(conversation_history, results);

         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         curl_buffer_free(&streaming_ctx.raw_response);

         if (llm_tools_should_skip_followup(results)) {
            OLOG_INFO("OpenAI streaming: Skipping follow-up call (tool requested no follow-up)");
            char *direct_response = llm_tools_get_direct_response(results);

            if (direct_response) {
               json_object *closing_msg = json_object_new_object();
               json_object_object_add(closing_msg, "role", json_object_new_string("assistant"));
               json_object_object_add(closing_msg, "content",
                                      json_object_new_string(direct_response));
               json_object_array_add(conversation_history, closing_msg);
               OLOG_INFO("OpenAI streaming: Added closing assistant message to complete history");
            }

            if (direct_response && chunk_callback) {
               chunk_callback(direct_response, callback_userdata);
            }

            for (int i = 0; i < results->count; i++) {
               if (results->results[i].vision_image) {
                  free(results->results[i].vision_image);
                  results->results[i].vision_image = NULL;
               }
            }
            free(results);
            return direct_response;
         }

         if (iteration >= MAX_TOOL_ITERATIONS) {
            OLOG_WARNING(
                "OpenAI streaming: Max tool iterations (%d) reached, forcing text response",
                MAX_TOOL_ITERATIONS);

            json_object *hint_msg = json_object_new_object();
            json_object_object_add(hint_msg, "role", json_object_new_string("user"));
            json_object_object_add(
                hint_msg, "content",
                json_object_new_string(
                    "[System: Maximum tool iterations reached. Respond to the user now with "
                    "the information you have gathered so far. Do not call any more tools.]"));
            json_object_array_add(conversation_history, hint_msg);

            for (int i = 0; i < results->count; i++) {
               if (results->results[i].vision_image) {
                  free(results->results[i].vision_image);
                  results->results[i].vision_image = NULL;
               }
            }
            free(results);

            OLOG_INFO("OpenAI streaming: Making final call without tools to present results");
            return llm_openai_streaming_internal(conversation_history, "", NULL, NULL, 0, base_url,
                                                 api_key, model, chunk_callback, callback_userdata,
                                                 MAX_TOOL_ITERATIONS);
         }

         OLOG_INFO("OpenAI streaming: Making follow-up call after tool execution (iteration %d/%d)",
                   iteration + 1, MAX_TOOL_ITERATIONS);

         OLOG_INFO("OpenAI streaming: Tool results added to history:");
         for (int i = 0; i < results->count; i++) {
            OLOG_INFO("  [%d] id=%s result=%.200s%s", i, results->results[i].tool_call_id,
                      results->results[i].result,
                      strlen(results->results[i].result) > 200 ? "..." : "");
         }

         const char *result_vision = NULL;
         size_t result_vision_size = 0;
         for (int i = 0; i < results->count; i++) {
            if (results->results[i].vision_image && results->results[i].vision_image_size > 0) {
               result_vision = results->results[i].vision_image;
               result_vision_size = results->results[i].vision_image_size;
               OLOG_INFO("OpenAI streaming: Including vision from tool result (%zu bytes)",
                         result_vision_size);
               break;
            }
         }

         llm_resolved_config_t current_config;
         char *result = NULL;
         char model_buf_followup[LLM_MODEL_NAME_MAX] = "";
         bool config_valid = (llm_get_current_resolved_config(&current_config) == 0);

         if (config_valid && current_config.model && current_config.model[0] != '\0') {
            safe_strscpy(model_buf_followup, current_config.model);
         }

         if (config_valid && current_config.type != LLM_LOCAL &&
             current_config.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
            OLOG_INFO("OpenAI streaming: Provider switched to Claude, handing off");

            const char *claude_model = model_buf_followup[0] ? model_buf_followup : NULL;
            const char *result_vision_arr[1] = { result_vision };
            size_t result_vision_size_arr[1] = { result_vision_size };
            int result_vision_count = result_vision ? 1 : 0;
            result = llm_claude_chat_completion_streaming(
                conversation_history, "", result_vision_arr, result_vision_size_arr,
                result_vision_count, current_config.endpoint, current_config.api_key, claude_model,
                (llm_claude_text_chunk_callback)chunk_callback, callback_userdata);
         } else {
            const char *fresh_url = config_valid ? current_config.endpoint : base_url;
            const char *fresh_api_key = config_valid ? current_config.api_key : api_key;
            const char *fresh_model = model_buf_followup[0] ? model_buf_followup : model;

            if (config_valid && fresh_url != base_url) {
               OLOG_INFO("OpenAI streaming: Credentials refreshed to %s", fresh_url);
            }

            const char *result_vision_arr[1] = { result_vision };
            size_t result_vision_size_arr[1] = { result_vision_size };
            int result_vision_count = result_vision ? 1 : 0;
            result = llm_openai_streaming_internal(conversation_history, "", result_vision_arr,
                                                   result_vision_size_arr, result_vision_count,
                                                   fresh_url, fresh_api_key, fresh_model,
                                                   chunk_callback, callback_userdata,
                                                   iteration + 1);
         }

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

   response = llm_stream_get_response(stream_ctx);

#ifdef ENABLE_WEBUI
   if (stream_ctx->reasoning_tokens > 0) {
      session_t *ws_session = session_get_command_context();
      if (ws_session && ws_session->type == SESSION_TYPE_WEBUI) {
         webui_send_reasoning_summary(ws_session, stream_ctx->reasoning_tokens);
      }
   }
#endif

   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(root);
   curl_buffer_free(&streaming_ctx.raw_response);

   return response;
}

/* ── Public streaming wrapper (legacy recursive path) ───────────────────── */

char *llm_openai_cc_streaming(struct json_object *conversation_history,
                              const char *input_text,
                              const char **vision_images,
                              const size_t *vision_image_sizes,
                              int vision_image_count,
                              const char *base_url,
                              const char *api_key,
                              const char *model,
                              llm_openai_text_chunk_callback chunk_callback,
                              void *callback_userdata) {
   return llm_openai_streaming_internal(conversation_history, input_text, vision_images,
                                        vision_image_sizes, vision_image_count, base_url, api_key,
                                        model, chunk_callback, callback_userdata, 0);
}

/* ── Single-shot streaming (no tool execution or recursion) ─────────────── */

int llm_openai_cc_streaming_single_shot(struct json_object *conversation_history,
                                        const char *input_text,
                                        const char **vision_images,
                                        const size_t *vision_image_sizes,
                                        int vision_image_count,
                                        const char *base_url,
                                        const char *api_key,
                                        const char *model,
                                        llm_openai_text_chunk_callback chunk_callback,
                                        void *callback_userdata,
                                        int iteration,
                                        llm_tool_response_t *result) {
   CURL *curl_handle = NULL;
   CURLcode res = -1;
   struct curl_slist *headers = NULL;
   char full_url[2048 + 20] = "";
   const char *payload = NULL;
   json_object *root = NULL;
   sse_parser_t *sse_parser = NULL;
   llm_stream_context_t *stream_ctx = NULL;
   openai_streaming_context_t streaming_ctx;

   if (!result) {
      return 1;
   }
   memset(result, 0, sizeof(*result));

   json_object *converted_history = llm_openai_prepare_chat_history(conversation_history);

   root = json_object_new_object();

   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }
   if (model_name && model_name[0] != '\0') {
      json_object_object_add(root, "model", json_object_new_string(model_name));
   }

   json_object_object_add(root, "stream", json_object_new_boolean(1));
   json_object *stream_opts = json_object_new_object();
   json_object_object_add(stream_opts, "include_usage", json_object_new_boolean(1));
   json_object_object_add(root, "stream_options", stream_opts);

   if (api_key == NULL) {
      json_object_object_add(root, "timings_per_token", json_object_new_boolean(1));
      add_local_thinking_params(root);
   } else {
      add_cloud_reasoning_effort(root, model_name, base_url);
   }

   if (vision_images != NULL && vision_image_count > 0) {
      /* Copy-on-write: converted_history may share message objects with the
       * caller's session history, so apply vision to a private copy. */
      json_object *with_vision = llm_openai_apply_vision_images(converted_history, input_text,
                                                                vision_images, vision_image_sizes,
                                                                vision_image_count);
      if (with_vision != NULL) {
         json_object_put(converted_history);
         converted_history = with_vision;
      }
   }

   json_object_object_add(root, "messages", converted_history);

   if (api_key == NULL) {
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   if (llm_tools_enabled(NULL) && iteration < LLM_TOOLS_MAX_ITERATIONS) {
      bool is_remote = is_current_session_remote();
      struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
      }
   }

   /* Finalize system-prompt shape for caching: keep the two-segment split for the
    * OpenRouter→Anthropic cache path, else collapse it into one system message so
    * strict local chat templates (Qwen 3.5/3.6 Jinja) don't reject the second. */
   llm_openai_apply_system_prompt_caching(root, model_name, base_url);

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   OLOG_INFO("OpenAI single-shot iter %d: url=%s model=%s", iteration, base_url,
             (model_name && model_name[0]) ? model_name : "(server default)");

   if (!llm_check_connection(base_url, 4)) {
      llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      OLOG_ERROR("Pre-flight connection check failed (cloud unreachable)");
      json_object_put(root);
      return 1;
   }

   llm_type_t stream_llm_type = (api_key != NULL) ? LLM_CLOUD : LLM_LOCAL;
   cloud_provider_t stream_provider = CLOUD_PROVIDER_OPENAI;
   session_t *ctx_session = session_get_command_context();
   if (ctx_session && stream_llm_type == LLM_CLOUD) {
      session_llm_config_t session_config;
      session_get_llm_config(ctx_session, &session_config);
      stream_provider = session_config.cloud_provider;
   } else if (base_url && strstr(base_url, "generativelanguage.googleapis.com")) {
      stream_provider = CLOUD_PROVIDER_GEMINI;
   } else if (base_url && strstr(base_url, "openrouter.ai")) {
      stream_provider = CLOUD_PROVIDER_OPENROUTER;
   }
   stream_ctx = llm_stream_create(stream_llm_type, stream_provider, chunk_callback,
                                  callback_userdata);
   if (!stream_ctx) {
      OLOG_ERROR("Failed to create LLM stream context");
      json_object_put(root);
      return 1;
   }

   sse_parser = sse_parser_create(openai_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      OLOG_ERROR("Failed to create SSE parser");
      llm_stream_free(stream_ctx);
      json_object_put(root);
      return 1;
   }

   streaming_ctx.sse_parser = sse_parser;
   streaming_ctx.stream_ctx = stream_ctx;
   curl_buffer_init_with_max(&streaming_ctx.raw_response, OPENAI_RAW_BUFFER_CAP);

   int max_attempts = 2;
   for (int attempt = 0; attempt < max_attempts; attempt++) {
      curl_handle = curl_easy_init();
      if (!curl_handle) {
         OLOG_ERROR("Failed to initialize CURL");
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         curl_buffer_free(&streaming_ctx.raw_response);
         return 1;
      }

      headers = llm_openai_build_headers(api_key, base_url);
      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);

      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, streaming_write_callback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&streaming_ctx);
      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, NULL);
      curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);
      {
         int eff_to = llm_get_effective_timeout_ms();
         long lspt = 60L;
         if (eff_to > 60000) {
            lspt = (long)(eff_to / 1000);
         }
         curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
         curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, lspt);
      }

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
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
            curl_buffer_reset(&streaming_ctx.raw_response);
            if (streaming_ctx.raw_response.data) {
               streaming_ctx.raw_response.data[0] = '\0';
            }
            sse_parser_reset(sse_parser);
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
            OLOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
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
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         curl_buffer_free(&streaming_ctx.raw_response);
         return 1;
      }

      break;
   }

   long http_code = 0;
   curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

   if (http_code != 200) {
      OLOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
      /* Log the provider's error body — the exact reason (e.g. a 400 "prompt is
       * too long" / context-overflow, or a malformed-request detail) is otherwise
       * invisible on this streaming path. */
      if (streaming_ctx.raw_response.data && streaming_ctx.raw_response.size > 0) {
         /* Bound the logged body: the actionable detail (e.g. "prompt is too long")
          * is early, and a full body could carry echoed request content. */
         OLOG_ERROR("OpenAI API error response: %.2048s", streaming_ctx.raw_response.data);
      }
      if (http_code == 429) {
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      } else if (http_code >= 500 && http_code < 600) {
         if (llm_openai_is_deterministic_template_error(streaming_ctx.raw_response.data)) {
            OLOG_ERROR("OpenAI API: deterministic chat-template error in response — "
                       "failing fast (not retrying)");
         } else {
            llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
         }
      }
#ifdef ENABLE_WEBUI
      session_t *session = session_get_command_context();
      if (session && session->type == SESSION_TYPE_WEBUI) {
         const char *err_msg = llm_openai_parse_error_message(streaming_ctx.raw_response.data,
                                                              http_code);
         webui_send_error(session, "LLM_ERROR", err_msg);
      }
#endif
      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      json_object_put(root);
      curl_buffer_free(&streaming_ctx.raw_response);
      return 1;
   }

   curl_easy_cleanup(curl_handle);
   curl_slist_free_all(headers);

   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         result->has_tool_calls = true;
         memcpy(&result->tool_calls, tool_calls, sizeof(tool_call_list_t));
      }
   }

   result->text = llm_stream_get_response(stream_ctx);
   result->thinking_content = llm_stream_get_thinking(stream_ctx);
   result->reasoning_tokens = stream_ctx->reasoning_tokens;

   if (stream_ctx->finish_reason[0] != '\0') {
      safe_strscpy(result->finish_reason, stream_ctx->finish_reason);
   }

#ifdef ENABLE_WEBUI
   if (stream_ctx->reasoning_tokens > 0) {
      session_t *ws_session = session_get_command_context();
      if (ws_session && ws_session->type == SESSION_TYPE_WEBUI) {
         webui_send_reasoning_summary(ws_session, stream_ctx->reasoning_tokens);
      }
   }
#endif

   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(root);
   curl_buffer_free(&streaming_ctx.raw_response);

   return 0;
}
