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
 * Tool results added to a conversation's history (each provider's shape,
 * with any image a tool captured) and tool calls parsed from a provider's
 * response.  Split from llm_tools.c.
 */

#include <limits.h>
#include <mosquitto.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/command_executor.h"
#include "core/component_status.h"
#include "core/hash_util.h"
#include "core/ocp_helpers.h"
#include "core/research_allowlist.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "dawn_error.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_context_text.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "llm/llm_tools_internal.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/hud_discovery.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Tool Result Formatting for Conversation History
 * ============================================================================= */

/**
 * @brief Build a standalone user message holding exactly one captured-vision image
 *
 * OpenAI-shape: {"role":"user","content":[{"type":"image_url","image_url":{"url":...}}]}
 * No accompanying text. Persists a tool-captured image (e.g. the `viewing`
 * camera tool) into conversation history so follow-up turns can still see
 * it, instead of the image only being visible for the single turn it was
 * captured in. is_capture_image_message() recognizes this exact
 * single-part shape for retention pruning.
 */
static struct json_object *build_openai_capture_image_message(const char *base64_data) {
   const char *media_type = llm_claude_detect_image_mime_type(base64_data);
   const char *prefix_fmt = "data:%s;base64,";
   size_t data_uri_len = strlen(prefix_fmt) + strlen(media_type) + strlen(base64_data) + 1;
   char *data_uri = malloc(data_uri_len);
   if (!data_uri) {
      OLOG_ERROR("Failed to allocate data URI for captured image (%zu bytes) — skipping persist",
                 data_uri_len);
      return NULL;
   }
   snprintf(data_uri, data_uri_len, "data:%s;base64,%s", media_type, base64_data);

   struct json_object *image_url_obj = json_object_new_object();
   json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
   free(data_uri);

   struct json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image_url"));
   json_object_object_add(image_obj, "image_url", image_url_obj);

   struct json_object *content_array = json_object_new_array();
   json_object_array_add(content_array, image_obj);

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);
   return msg;
}

/**
 * @brief Build a standalone user message holding exactly one captured-vision image (Claude shape)
 *
 * Mirrors build_openai_capture_image_message() but wraps the image with
 * llm_claude_create_image_block(), matching what convert_to_claude_format()
 * already produces ephemerally for a caller-supplied vision image.
 */
static struct json_object *build_claude_capture_image_message(const char *base64_data) {
   struct json_object *content_array = json_object_new_array();
   json_object_array_add(content_array, llm_claude_create_image_block(base64_data));

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);
   return msg;
}

/**
 * @brief True if msg is a lone captured-vision-image message
 *
 * Recognizes the exact shape built above: a "user" message whose content is
 * a single-element array containing only an image part ("image_url" for
 * OpenAI shape, "image" for Claude shape). This is distinct from a
 * WebUI-uploaded image (session_add_message_with_images() always pairs an
 * image with a text part, so its content array has length >= 2), so
 * retention pruning only ever touches ambient tool captures, never images
 * the user deliberately attached to a message.
 */
static bool is_capture_image_message(struct json_object *msg) {
   struct json_object *role_obj = NULL;
   struct json_object *content_obj = NULL;

   if (!msg || !json_object_object_get_ex(msg, "role", &role_obj) ||
       strcmp(json_object_get_string(role_obj), "user") != 0) {
      return false;
   }
   if (!json_object_object_get_ex(msg, "content", &content_obj) ||
       !json_object_is_type(content_obj, json_type_array) ||
       json_object_array_length(content_obj) != 1) {
      return false;
   }

   struct json_object *part = json_object_array_get_idx(content_obj, 0);
   struct json_object *type_obj = NULL;
   if (!part || !json_object_object_get_ex(part, "type", &type_obj)) {
      return false;
   }

   const char *type = json_object_get_string(type_obj);
   return type && (strcmp(type, "image_url") == 0 || strcmp(type, "image") == 0);
}

/**
 * @brief Keep only the most recent N tool-captured images in history
 *
 * Scans newest-to-oldest; the first retention_count capture-image messages
 * found are left intact, anything older is collapsed to a short text
 * placeholder. retention_count <= 0 means unlimited (no-op) — bounding is
 * then left entirely to normal context compaction.
 */
static void evict_old_capture_images(struct json_object *history, int retention_count) {
   if (!history || retention_count <= 0) {
      return;
   }

   int live = 0;
   for (int i = json_object_array_length(history) - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (!is_capture_image_message(msg)) {
         continue;
      }
      live++;
      if (live > retention_count) {
         json_object_object_add(msg, "content",
                                json_object_new_string(
                                    "[earlier camera capture - image no longer retained]"));
      }
   }
}

/**
 * @brief Persist the first tool result carrying a captured image, if any
 *
 * Appends an image-only message via the given builder and prunes older
 * captures per [vision] capture_history_count. Matches the prior ephemeral
 * behavior of surfacing at most one captured image per tool iteration.
 */
static void persist_capture_image_if_present(struct json_object *history,
                                             const tool_result_list_t *results,
                                             struct json_object *(*build_message)(const char *)) {
   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];
      if (r->vision_image && r->vision_image_size > 0) {
         struct json_object *msg = build_message(r->vision_image);
         if (msg) {
            session_history_append(history, msg);
            evict_old_capture_images(history, g_config.vision.capture_history_count);
         }
         return;
      }
   }
}

int llm_tools_add_results_openai(struct json_object *history, const tool_result_list_t *results) {
   if (!history || !results) {
      return 1;
   }

   /*
    * OpenAI format: Add a "tool" role message for each result
    * {
    *   "role": "tool",
    *   "tool_call_id": "call_xxx",
    *   "content": "result text"
    * }
    */
   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];

      struct json_object *msg = json_object_new_object();
      json_object_object_add(msg, "role", json_object_new_string("tool"));
      json_object_object_add(msg, "tool_call_id", json_object_new_string(r->tool_call_id));
      json_object_object_add(msg, "content", json_object_new_string(tool_result_content(r)));

      session_history_append(history, msg);
   }

   /* A tool that returned a captured image (e.g. `viewing`) only shows the
    * model that image for the turn it was captured in unless we persist it
    * here — see docs/arch/subsystems/llm.md vision notes. */
   persist_capture_image_if_present(history, results, build_openai_capture_image_message);

   return 0;
}

int llm_tools_add_results_claude(struct json_object *history, const tool_result_list_t *results) {
   if (!history || !results) {
      return 1;
   }

   /*
    * Claude format: Add a single "user" message with tool_result content blocks
    * {
    *   "role": "user",
    *   "content": [
    *     {
    *       "type": "tool_result",
    *       "tool_use_id": "toolu_xxx",
    *       "content": "result text"
    *     }
    *   ]
    * }
    */
   struct json_object *content_array = json_object_new_array();

   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];

      struct json_object *block = json_object_new_object();
      json_object_object_add(block, "type", json_object_new_string("tool_result"));
      json_object_object_add(block, "tool_use_id", json_object_new_string(r->tool_call_id));
      json_object_object_add(block, "content", json_object_new_string(tool_result_content(r)));

      json_object_array_add(content_array, block);
   }

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);

   session_history_append(history, msg);

   persist_capture_image_if_present(history, results, build_claude_capture_image_message);

   return 0;
}

/* =============================================================================
 * Response Parsing
 * ============================================================================= */

int llm_tools_parse_openai_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return FAILURE;
   }

   out->count = 0;

   /*
    * OpenAI response structure:
    * {
    *   "choices": [{
    *     "message": {
    *       "tool_calls": [{
    *         "id": "call_xxx",
    *         "function": {
    *           "name": "weather",
    *           "arguments": "{...}"
    *         }
    *       }]
    *     },
    *     "finish_reason": "tool_calls"
    *   }]
    * }
    */
   struct json_object *choices;
   if (!json_object_object_get_ex(response, "choices", &choices)) {
      return 1; /* No tool calls */
   }

   if (json_object_array_length(choices) == 0) {
      return 1;
   }

   struct json_object *first_choice = json_object_array_get_idx(choices, 0);
   struct json_object *message;
   if (!json_object_object_get_ex(first_choice, "message", &message)) {
      return 1;
   }

   struct json_object *tool_calls;
   if (!json_object_object_get_ex(message, "tool_calls", &tool_calls)) {
      return 1; /* No tool calls */
   }

   int len = json_object_array_length(tool_calls);
   for (int i = 0; i < len && out->count < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      struct json_object *tc = json_object_array_get_idx(tool_calls, i);
      struct json_object *id_obj, *function_obj;

      if (!json_object_object_get_ex(tc, "id", &id_obj) ||
          !json_object_object_get_ex(tc, "function", &function_obj)) {
         continue;
      }

      struct json_object *name_obj, *args_obj;
      if (!json_object_object_get_ex(function_obj, "name", &name_obj) ||
          !json_object_object_get_ex(function_obj, "arguments", &args_obj)) {
         continue;
      }

      tool_call_t *call = &out->calls[out->count++];
      safe_strncpy(call->id, json_object_get_string(id_obj), LLM_TOOLS_ID_LEN);
      safe_strncpy(call->name, json_object_get_string(name_obj), LLM_TOOLS_NAME_LEN);

      const char *args_str = json_object_get_string(args_obj);
      call->args_truncated = (args_str && strlen(args_str) >= LLM_TOOLS_ARGS_LEN);
      if (call->args_truncated) {
         OLOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                      strlen(args_str), LLM_TOOLS_ARGS_LEN - 1);
      }
      safe_strncpy(call->arguments, args_str ? args_str : "", LLM_TOOLS_ARGS_LEN);
   }

   return out->count > 0 ? 0 : 1;
}

int llm_tools_parse_claude_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return FAILURE;
   }

   out->count = 0;

   /*
    * Claude response structure:
    * {
    *   "content": [
    *     {
    *       "type": "tool_use",
    *       "id": "toolu_xxx",
    *       "name": "weather",
    *       "input": { ... }
    *     }
    *   ],
    *   "stop_reason": "tool_use"
    * }
    */
   struct json_object *content;
   if (!json_object_object_get_ex(response, "content", &content)) {
      return 1;
   }

   int len = json_object_array_length(content);
   for (int i = 0; i < len && out->count < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      struct json_object *block = json_object_array_get_idx(content, i);
      struct json_object *type_obj;

      if (!json_object_object_get_ex(block, "type", &type_obj)) {
         continue;
      }

      if (strcmp(json_object_get_string(type_obj), "tool_use") != 0) {
         continue;
      }

      struct json_object *id_obj, *name_obj, *input_obj;
      if (!json_object_object_get_ex(block, "id", &id_obj) ||
          !json_object_object_get_ex(block, "name", &name_obj) ||
          !json_object_object_get_ex(block, "input", &input_obj)) {
         continue;
      }

      tool_call_t *call = &out->calls[out->count++];
      safe_strncpy(call->id, json_object_get_string(id_obj), LLM_TOOLS_ID_LEN);
      safe_strncpy(call->name, json_object_get_string(name_obj), LLM_TOOLS_NAME_LEN);

      /* Claude sends input as object, we need it as string */
      const char *input_str = json_object_to_json_string(input_obj);
      call->args_truncated = (input_str && strlen(input_str) >= LLM_TOOLS_ARGS_LEN);
      if (call->args_truncated) {
         OLOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                      strlen(input_str), LLM_TOOLS_ARGS_LEN - 1);
      }
      safe_strncpy(call->arguments, input_str, LLM_TOOLS_ARGS_LEN);
   }

   return out->count > 0 ? 0 : 1;
}
