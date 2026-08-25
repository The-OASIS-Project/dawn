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
 * OpenAI provider — public entry points and shared helpers. Dispatches to
 * either the /v1/chat/completions implementation (llm_openai_chat_completions.c)
 * or the /v1/responses implementation (llm_openai_responses.c) based on model
 * and configuration.
 */

#include "llm/llm_openai.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_interface.h"
#include "llm/llm_model_version.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_openai_responses.h"
#include "llm/llm_tools.h"
#include "logging.h"

/* ── Routing ────────────────────────────────────────────────────────────── */

/**
 * @brief Decide whether this call should use /v1/responses instead of /v1/chat/completions.
 *
 * Triggers when api_key is set (cloud path only), and the configured mode permits it:
 *   - "auto"   (default) — route iff llm_openai_model_prefers_responses_api(model)
 *   - "always"           — route every cloud OpenAI call
 *   - "never"            — never route (gpt-5.4 will fail per OpenAI's HTTP 400)
 *
 * Local LLMs and non-OpenAI cloud providers (Gemini, OpenRouter, and other
 * OpenAI-compat proxies) never route — they only serve /v1/chat/completions.
 */
static bool should_dispatch_to_responses_api(const char *api_key,
                                             const char *base_url,
                                             const char *model_name) {
   if (!api_key)
      return false;
   if (!model_name || !*model_name)
      return false;
   if (base_url &&
       (strstr(base_url, "generativelanguage.googleapis.com") || strstr(base_url, "openrouter.ai")))
      return false;

   const char *mode = g_config.llm.cloud.openai_use_responses_api;
   if (!mode || !*mode)
      mode = "auto";

   if (strcmp(mode, "never") == 0)
      return false;
   if (strcmp(mode, "always") == 0)
      return true;
   /* auto */
   return llm_openai_model_prefers_responses_api(model_name);
}

/* ── Shared helpers (used by chat-completions and responses) ────────────── */

struct curl_slist *llm_openai_build_headers(const char *api_key, const char *base_url) {
   struct curl_slist *headers = NULL;

   headers = curl_slist_append(headers, "Content-Type: application/json");

   if (api_key != NULL) {
      char auth_header[512];
      snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
      headers = curl_slist_append(headers, auth_header);
   }

   /* OpenRouter optional attribution headers (used for their app rankings).
    * Harmless on other endpoints, but only sent when talking to OpenRouter. */
   if (base_url != NULL && strstr(base_url, "openrouter.ai") != NULL) {
      headers = curl_slist_append(headers,
                                  "HTTP-Referer: https://github.com/The-OASIS-Project/dawn");
      headers = curl_slist_append(headers, "X-Title: DAWN");
   }

   return headers;
}

const char *llm_openai_parse_error_message(const char *response_body, long http_code) {
   static _Thread_local char error_msg[512];

   if (response_body && *response_body) {
      struct json_object *parsed = json_tokener_parse(response_body);
      if (parsed) {
         struct json_object *error_obj, *msg_obj;
         if (json_object_object_get_ex(parsed, "error", &error_obj) &&
             json_object_object_get_ex(error_obj, "message", &msg_obj)) {
            const char *msg = json_object_get_string(msg_obj);
            if (msg && *msg) {
               snprintf(error_msg, sizeof(error_msg), "%s", msg);
               json_object_put(parsed);
               return error_msg;
            }
         }
         json_object_put(parsed);
      }
   }

   snprintf(error_msg, sizeof(error_msg), "Request failed with HTTP %ld", http_code);
   return error_msg;
}

bool llm_openai_is_gpt5_base_family(const char *model_name) {
   if (!model_name)
      return false;
   if (strncmp(model_name, "gpt-5", 5) != 0)
      return false;
   /* gpt-5, gpt-5-mini, gpt-5-nano — but NOT gpt-5.1, gpt-5.2, gpt-5.4* */
   if (model_name[5] == '\0' || model_name[5] == '-')
      return true;
   return false;
}

bool llm_openai_model_prefers_responses_api(const char *model_name) {
   if (!model_name)
      return false;
   /* Only versioned "gpt-<digit>..." ids qualify (gpt-5.x, gpt-6, gpt-4o...).
    * Gemini/OpenRouter are excluded upstream by base_url, but guard here too; and
    * require a digit right after "gpt-" so non-versioned families (gpt-oss-20b,
    * gpt-image-1) don't misparse a size token as a major version and misroute. */
   if (strncmp(model_name, "gpt-", 4) != 0)
      return false;
   if (model_name[4] < '0' || model_name[4] > '9')
      return false;

   /* From gpt-5.4 onward, /v1/chat/completions no longer supports tool calling with
    * reasoning_effort other than "none", and gpt-5.6+ hard-errors on it — those
    * models require /v1/responses. Match gpt-5.<minor> where minor >= 4 (covers
    * gpt-5.4/5.5/5.6 and suffixed variants like gpt-5.6-luna, gpt-5.4-mini), plus
    * any future gpt-6+; the older gpt-5 base family and gpt-5.1-5.3 stay on chat
    * completions. The [llm.cloud] openai_use_responses_api="never" escape hatch
    * remains if a future gpt-6 turns out not to need Responses. */
   int major = 0, minor = 0;
   llm_parse_model_version(model_name, &major, &minor);
   if (major > 5)
      return true;
   if (major == 5 && minor >= 4)
      return true;
   return false;
}

const char *llm_openai_clamp_effort_for_model(const char *model_name, const char *effort) {
   if (!effort || !model_name)
      return effort;

   bool is_o_series = (strncmp(model_name, "o1", 2) == 0 || strncmp(model_name, "o3", 2) == 0);
   bool is_gpt5_base = llm_openai_is_gpt5_base_family(model_name);
   bool is_gemini = (strncmp(model_name, "gemini-", 7) == 0);

   /* xhigh: only gpt-5.2+ supports it */
   if (strcmp(effort, "xhigh") == 0) {
      if (is_o_series || is_gpt5_base || is_gemini)
         return "high";
   }

   /* none: o-series and Gemini don't support it */
   if (strcmp(effort, "none") == 0) {
      if (is_o_series || is_gemini)
         return "low";
      if (is_gpt5_base)
         return "minimal";
   }

   /* minimal: only gpt-5 base family */
   if (strcmp(effort, "minimal") == 0) {
      if (!is_gpt5_base)
         return "low";
   }

   return effort;
}

/* ── Public entry points (dispatch to chat-completions or responses) ────── */

/* Discard sink for the bare-completion bridge below: /v1/responses only has a
 * streaming transport, and llm_stream_create() rejects a NULL callback, so a
 * non-streaming caller supplies this no-op and reads the assembled result.text. */
static void llm_openai_discard_text_chunk(const char *chunk, void *userdata) {
   (void)chunk;
   (void)userdata;
}

/* Run the streaming single-shot and return its assembled text (heap, caller frees),
 * or NULL. Bridges the non-streaming/legacy entry points onto the Responses path,
 * which has no non-streaming transport of its own. `sink` must be non-NULL. */
static char *llm_openai_single_shot_collect_text(struct json_object *conversation_history,
                                                 const char *input_text,
                                                 const char **vision_images,
                                                 const size_t *vision_image_sizes,
                                                 int vision_image_count,
                                                 const char *base_url,
                                                 const char *api_key,
                                                 const char *model,
                                                 llm_openai_text_chunk_callback sink,
                                                 void *sink_userdata) {
   llm_tool_response_t result = { 0 };
   int rc = llm_openai_streaming_single_shot(conversation_history, input_text, vision_images,
                                             vision_image_sizes, vision_image_count, base_url,
                                             api_key, model, sink, sink_userdata, 0, &result);
   char *text = (rc == 0 && result.text) ? strdup(result.text) : NULL;
   llm_tool_response_free(&result);
   return text;
}

char *llm_openai_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 const char **vision_images,
                                 const size_t *vision_image_sizes,
                                 int vision_image_count,
                                 const char *base_url,
                                 const char *api_key,
                                 const char *model) {
   if (should_dispatch_to_responses_api(api_key, base_url, model)) {
      /* Responses has no non-streaming transport; drive the streaming single-shot
       * with a discard sink so bare-completion callers (briefings, compaction,
       * memory extraction, summarizers) keep working on Responses-only models. */
      return llm_openai_single_shot_collect_text(conversation_history, input_text, vision_images,
                                                 vision_image_sizes, vision_image_count, base_url,
                                                 api_key, model, llm_openai_discard_text_chunk,
                                                 NULL);
   }

   return llm_openai_cc_chat_completion(conversation_history, input_text, vision_images,
                                        vision_image_sizes, vision_image_count, base_url, api_key,
                                        model);
}

char *llm_openai_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           const char **vision_images,
                                           const size_t *vision_image_sizes,
                                           int vision_image_count,
                                           const char *base_url,
                                           const char *api_key,
                                           const char *model,
                                           llm_openai_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   if (should_dispatch_to_responses_api(api_key, base_url, model)) {
      /* Legacy streaming entry (the Claude->OpenAI fallback path). Reuse the
       * streaming single-shot for Responses-only models, forwarding the real TTS
       * chunk sink so tokens still stream to the caller. */
      return llm_openai_single_shot_collect_text(conversation_history, input_text, vision_images,
                                                 vision_image_sizes, vision_image_count, base_url,
                                                 api_key, model, chunk_callback, callback_userdata);
   }
   return llm_openai_cc_streaming(conversation_history, input_text, vision_images,
                                  vision_image_sizes, vision_image_count, base_url, api_key, model,
                                  chunk_callback, callback_userdata);
}

int llm_openai_streaming_single_shot(struct json_object *conversation_history,
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
   if (!result) {
      return 1;
   }

   /* Resolve model for routing decision */
   const char *route_model = model;
   if (!route_model || !*route_model) {
      route_model = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }

   if (should_dispatch_to_responses_api(api_key, base_url, route_model)) {
      return llm_openai_responses_streaming_single_shot(
          conversation_history, input_text, vision_images, vision_image_sizes, vision_image_count,
          base_url, api_key, route_model, chunk_callback, callback_userdata, iteration, result);
   }

   return llm_openai_cc_streaming_single_shot(conversation_history, input_text, vision_images,
                                              vision_image_sizes, vision_image_count, base_url,
                                              api_key, model, chunk_callback, callback_userdata,
                                              iteration, result);
}
