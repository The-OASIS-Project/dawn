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
 * tool-call iteration. History conversion lives in
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
#include "llm/llm_cache_monitor.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_key_tag.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_openai.h"
#include "llm/llm_openai_cache.h"
#include "llm/llm_openai_internal.h"
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

extern int llm_curl_progress_callback(void *clientp,
                                      curl_off_t dltotal,
                                      curl_off_t dlnow,
                                      curl_off_t ultotal,
                                      curl_off_t ulnow);

/* The model a request uses: the one given, else the configured default (the
 * local model when there's no API key). */
static const char *resolve_model(const char *model, const char *api_key) {
   if (model && model[0] != '\0') {
      return model;
   }
   return (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
}

/* The cloud provider behind an OpenAI-compatible endpoint.  From the endpoint,
 * never the session config or the model name: the endpoint is what answered. */
static cloud_provider_t provider_for_endpoint(const char *base_url) {
   if (base_url && strstr(base_url, "generativelanguage.googleapis.com")) {
      return CLOUD_PROVIDER_GEMINI;
   }
   if (base_url && strstr(base_url, "openrouter.ai")) {
      return CLOUD_PROVIDER_OPENROUTER;
   }
   return CLOUD_PROVIDER_OPENAI;
}

/* The prompt-cache routing key, only where it is known to be taken: OpenAI and
 * OpenRouter (which passes it on).  A strict compatible endpoint (Gemini's, a
 * custom one) can reject a field it doesn't know. */
static void add_prompt_cache_key(json_object *root, const char *api_key, const char *base_url) {
   if (api_key && base_url &&
       (strstr(base_url, "api.openai.com") || strstr(base_url, "openrouter.ai"))) {
      llm_openai_add_prompt_cache_key(root);
   }
}

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

/* chat_template_kwargs for llama.cpp.  preserve_thinking keeps a template from
 * rendering an assistant message one way while it follows the latest question
 * (with a <think> block) and another once a newer question comes (without):
 * the Qwen 3.6 template does that unless asked not to, so every turn after a
 * tool call re-processed the previous turn instead of reusing its KV cache.
 * A template without the variable ignores it. */
static json_object *local_template_kwargs(bool thinking) {
   json_object *kwargs = json_object_new_object();
   json_object_object_add(kwargs, "enable_thinking", json_object_new_boolean(thinking));
   json_object_object_add(kwargs, "preserve_thinking", json_object_new_boolean(1));
   return kwargs;
}

static void add_local_thinking_params(json_object *root) {
   /* The session's mode and effort, resolved for the local provider in use
    * (llama.cpp: off or a fixed budget; Ollama: think on or off).  A utility
    * call resolves to off. */
   llm_thinking_resolved_t thinking;
   llm_thinking_resolve_current(LLM_LOCAL, CLOUD_PROVIDER_NONE, NULL, &thinking);
   const bool on = thinking.controllable && thinking.mode != LLM_THINK_DISABLED;
   local_provider_t provider = llm_local_get_provider();

   if (provider == LOCAL_PROVIDER_OLLAMA) {
      json_object_object_add(root, "think", json_object_new_boolean(on));
      OLOG_INFO("Local LLM (Ollama): Thinking %s (think: %s)", on ? "enabled" : "disabled",
                on ? "true" : "false");
      return;
   }
   if (on) {
      json_object *thinking_obj = json_object_new_object();
      json_object_object_add(thinking_obj, "type", json_object_new_string("enabled"));

      int budget = llm_budget_tokens_for_effort(thinking.effort);
      json_object_object_add(thinking_obj, "budget_tokens", json_object_new_int(budget));

      json_object_object_add(root, "thinking", thinking_obj);

      json_object_object_add(root, "thinking_forced_open", json_object_new_boolean(1));

      json_object_object_add(root, "chat_template_kwargs", local_template_kwargs(true));

      OLOG_INFO("Local LLM (llama.cpp): Extended thinking enabled (budget: %d tokens, "
                "forced_open: true, chat_template_kwargs.enable_thinking: true)",
                budget);
   } else {
      json_object_object_add(root, "reasoning_budget", json_object_new_int(0));

      json_object_object_add(root, "chat_template_kwargs", local_template_kwargs(false));

      OLOG_INFO("Local LLM (llama.cpp): Reasoning explicitly disabled (reasoning_budget: 0)");
   }
}

/* ── Cloud reasoning effort ─────────────────────────────────────────────── */

static void add_cloud_reasoning_effort(json_object *root,
                                       const char *model_name,
                                       const char *base_url) {
   /* The session's mode and effort, resolved against the model
    * (models.toml [thinking.*]; OpenRouter "vendor/model" by its vendor). */
   const cloud_provider_t provider = provider_for_endpoint(base_url);
   llm_thinking_resolved_t thinking;
   llm_thinking_resolve_current(LLM_CLOUD, provider, model_name, &thinking);
   if (!thinking.controllable) {
      return; /* the model has no reasoning control */
   }
   const char *note = thinking.clamped ? " (setting resolved to what the model accepts)" : "";

   /* OpenRouter: the unified reasoning OBJECT (`reasoning: { effort }`), not
    * OpenAI's flat `reasoning_effort`.  "disabled" sends nothing: OpenRouter's
    * off switch varies by upstream, and Anthropic models, the ones that can't
    * turn reasoning off, never resolve to it.  Streamed reasoning comes back in
    * delta.reasoning_details[] (llm_streaming.c). */
   if (provider == CLOUD_PROVIDER_OPENROUTER) {
      if (thinking.mode == LLM_THINK_DISABLED || !thinking.effort[0]) {
         return;
      }
      json_object *reasoning = json_object_new_object();
      json_object_object_add(reasoning, "effort", json_object_new_string(thinking.effort));
      json_object_object_add(root, "reasoning", reasoning);
      OLOG_INFO("OpenRouter: reasoning effort '%s' requested for model %s%s", thinking.effort,
                model_name, note);
      return;
   }

   /* OpenAI (o-series, gpt-5.0-5.3) and Gemini: "disabled" is effort "none". */
   const char *effort = thinking.mode == LLM_THINK_DISABLED ? "none" : thinking.effort;
   if (!effort[0]) {
      return;
   }
   json_object_object_add(root, "reasoning_effort", json_object_new_string(effort));
   OLOG_INFO("Cloud LLM: Reasoning effort set to '%s' for model %s%s", effort, model_name, note);
}

/* ── Non-streaming chat completion ──────────────────────────────────────── */

char *llm_openai_cc_chat_completion(struct json_object *conversation_history,
                                    const char *input_text,
                                    const char *base_url,
                                    const char *api_key,
                                    const char *model) {
   CURL *curl_handle = NULL;
   CURLcode res = CURLE_FAILED_INIT;
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

   /* The model, and whose reasoning this request may send back: its
    * endpoint and key (llm_turn_blocks_carrier). */
   const char *model_name = resolve_model(model, api_key);
   char carrier[LLM_CARRIER_MAX];
   llm_request_carrier(base_url, api_key, carrier, sizeof(carrier));

   json_object *converted_history = llm_openai_prepare_chat_history(conversation_history, carrier,
                                                                    model_name);
   if (!converted_history) {
      OLOG_ERROR("OpenAI: could not prepare the conversation history");
      return NULL;
   }

   root = json_object_new_object();


   if (model_name && model_name[0] != '\0') {
      json_object_object_add(root, "model", json_object_new_string(model_name));
   }

   json_object_object_add(root, "messages", converted_history);
   add_prompt_cache_key(root, api_key, base_url);

   if (api_key == NULL) {
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   if (llm_tools_enabled(NULL)) {
      /* The conversation's own tool set when it has one (llm_history_kind.h). */
      const char *source = NULL;
      struct json_object *tools = llm_tools_request_tools(conversation_history,
                                                          is_current_session_remote(), false, false,
                                                          &source);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         OLOG_INFO("OpenAI: Added %zu tools to request (%s)", json_object_array_length(tools),
                   source);
      }
   }

   /* One leading system message: strict local chat templates (Qwen 3.5/3.6
    * Jinja) reject a second. */
   llm_openai_merge_leading_system_messages(root);

   llm_cache_monitor_note_request(root); /* for this call's "LLM cache:" line */
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
   int cache_write_tokens = 0;
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
            /* OpenAI and Gemini 2.5+ both report implicit caching here; the
             * "LLM cache:" line labels it with the endpoint's provider. */
            cached_tokens = json_object_get_int(cached_tokens_obj);
         }
         /* Cache writes, where an upstream reports them (OpenRouter fronting
          * Anthropic, under either name). */
         json_object *write_obj = NULL;
         if (json_object_object_get_ex(prompt_tokens_details, "cache_write_tokens", &write_obj) ||
             json_object_object_get_ex(prompt_tokens_details, "cache_creation_input_tokens",
                                       &write_obj)) {
            cache_write_tokens = json_object_get_int(write_obj);
         }
      }

      llm_type_t token_type = (api_key != NULL) ? LLM_CLOUD : LLM_LOCAL;
      /* This OpenAI-compat path also fronts Gemini and OpenRouter: the endpoint
       * says which, so their tokens aren't booked as OpenAI's (cache-savings uses
       * each provider's own discount).  Local turns bill nothing regardless. */
      const cloud_provider_t token_provider = provider_for_endpoint(base_url);
      metrics_record_llm_tokens(token_type, token_provider, input_tokens, output_tokens,
                                cached_tokens);

      session_t *session = session_get_command_context();
      uint32_t session_id = session ? session->session_id : 0;
      llm_usage_report_t usage = { .prompt_tokens = input_tokens,
                                   .completion_tokens = output_tokens,
                                   .cached_tokens = cached_tokens,
                                   .cache_write_tokens = cache_write_tokens,
                                   .type = token_type,
                                   .provider = token_provider };
      llm_context_update_usage(session_id, &usage);
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

/* ── Single-shot streaming (no tool execution or recursion) ─────────────── */

int llm_openai_cc_streaming_single_shot(struct json_object *conversation_history,
                                        const char *input_text,
                                        const char *base_url,
                                        const char *api_key,
                                        const char *model,
                                        llm_openai_text_chunk_callback chunk_callback,
                                        void *callback_userdata,
                                        int iteration,
                                        llm_tool_response_t *result) {
   CURL *curl_handle = NULL;
   CURLcode res = CURLE_FAILED_INIT;
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

   /* The model, and whose reasoning this request may send back: its
    * endpoint and key (llm_turn_blocks_carrier). */
   const char *model_name = resolve_model(model, api_key);
   char carrier[LLM_CARRIER_MAX];
   llm_request_carrier(base_url, api_key, carrier, sizeof(carrier));

   json_object *converted_history = llm_openai_prepare_chat_history(conversation_history, carrier,
                                                                    model_name);
   if (!converted_history) {
      OLOG_ERROR("OpenAI: could not prepare the conversation history");
      return 1;
   }

   root = json_object_new_object();


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

   json_object_object_add(root, "messages", converted_history);
   add_prompt_cache_key(root, api_key, base_url);

   if (api_key == NULL) {
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   /* The loop's last call (iteration at the cap), for a text answer: the tools
    * stay (the request reads as every other did), none may be called. */
   if (llm_tools_enabled(NULL)) {
      struct json_object *tools = llm_tools_request_tools(conversation_history,
                                                          is_current_session_remote(), false, false,
                                                          NULL);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice",
                                json_object_new_string(
                                    iteration >= LLM_TOOLS_MAX_ITERATIONS ? "none" : "auto"));
      }
   }

   /* One leading system message: strict local chat templates (Qwen 3.5/3.6
    * Jinja) reject a second. */
   llm_openai_merge_leading_system_messages(root);

   llm_cache_monitor_note_request(root); /* for this call's "LLM cache:" line */
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
   const cloud_provider_t stream_provider = provider_for_endpoint(base_url);
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

   if (llm_stream_check_finished(stream_ctx, "OpenAI API") != 0) {
      sse_parser_free(sse_parser);
      llm_stream_free(stream_ctx);
      json_object_put(root);
      curl_buffer_free(&streaming_ctx.raw_response);
      return 1;
   }

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
   /* The turn's blocks: what the next request sends back to this endpoint. */
   result->blocks = llm_stream_chat_blocks(stream_ctx, carrier, model_name);

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
