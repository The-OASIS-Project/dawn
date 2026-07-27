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
 * Claude API format conversion utilities.
 * Converts OpenAI-format conversation history to Claude's native format.
 *
 * Extracted from llm_claude.c to reduce file size and improve maintainability.
 */

#include "llm/llm_claude_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "utils/string_utils.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/**
 * @brief Check if current session is remote (WebSocket or DAP)
 */
static bool is_current_session_remote(void) {
   session_t *session = session_get_command_context();
   if (!session) {
      return false; /* No session context = local */
   }
   return (session->type != SESSION_TYPE_LOCAL);
}

/**
 * @brief Count tool results in conversation history
 *
 * First pass to determine how many tool_result IDs exist, used to allocate
 * the correct size for the ID collection array.
 */
static int count_tool_results(struct json_object *conversation) {
   int count = 0;
   int conv_len = json_object_array_length(conversation);

   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(conversation, i);
      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      // OpenAI format: role="tool" with tool_call_id
      if (strcmp(role, "tool") == 0) {
         count++;
      }

      // Claude format: role="user" with content array containing tool_result blocks
      if (strcmp(role, "user") == 0) {
         json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj) &&
             json_object_is_type(content_obj, json_type_array)) {
            int content_len = json_object_array_length(content_obj);
            for (int j = 0; j < content_len; j++) {
               json_object *block = json_object_array_get_idx(content_obj, j);
               json_object *type_obj;
               if (json_object_object_get_ex(block, "type", &type_obj) &&
                   strcmp(json_object_get_string(type_obj), "tool_result") == 0) {
                  count++;
               }
            }
         }
      }
   }
   return count;
}

/**
 * @brief Collect tool result IDs from conversation history
 *
 * Pre-scans conversation to find all tool result IDs, used to filter
 * orphaned tool_use blocks that don't have matching results.
 *
 * @param conversation The conversation history to scan
 * @param tool_result_ids Dynamically allocated array to store IDs (caller allocates)
 * @param max_results Size of the tool_result_ids array
 * @return Number of IDs collected
 */
static int collect_tool_result_ids(struct json_object *conversation,
                                   char (*tool_result_ids)[LLM_TOOLS_ID_LEN],
                                   int max_results) {
   int count = 0;
   int conv_len = json_object_array_length(conversation);

   for (int i = 0; i < conv_len && count < max_results; i++) {
      json_object *msg = json_object_array_get_idx(conversation, i);
      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      // OpenAI format: role="tool" with tool_call_id
      if (strcmp(role, "tool") == 0) {
         json_object *tool_call_id_obj;
         if (json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            const char *id = json_object_get_string(tool_call_id_obj);
            if (id && count < max_results) {
               strncpy(tool_result_ids[count], id, LLM_TOOLS_ID_LEN - 1);
               tool_result_ids[count][LLM_TOOLS_ID_LEN - 1] = '\0';
               count++;
            }
         }
      }

      // Claude format: role="user" with content array containing tool_result blocks
      if (strcmp(role, "user") == 0) {
         json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj) &&
             json_object_is_type(content_obj, json_type_array)) {
            int content_len = json_object_array_length(content_obj);
            for (int j = 0; j < content_len && count < max_results; j++) {
               json_object *block = json_object_array_get_idx(content_obj, j);
               json_object *type_obj;
               if (json_object_object_get_ex(block, "type", &type_obj) &&
                   strcmp(json_object_get_string(type_obj), "tool_result") == 0) {
                  json_object *tool_use_id_obj;
                  if (json_object_object_get_ex(block, "tool_use_id", &tool_use_id_obj)) {
                     const char *id = json_object_get_string(tool_use_id_obj);
                     if (id && count < max_results) {
                        strncpy(tool_result_ids[count], id, LLM_TOOLS_ID_LEN - 1);
                        tool_result_ids[count][LLM_TOOLS_ID_LEN - 1] = '\0';
                        count++;
                     }
                  }
               }
            }
         }
      }
   }
   return count;
}

/**
 * @brief Check if a tool ID has a matching result
 */
static bool has_matching_tool_result(const char *tool_id,
                                     char tool_result_ids[][LLM_TOOLS_ID_LEN],
                                     int count) {
   for (int k = 0; k < count; k++) {
      if (strcmp(tool_id, tool_result_ids[k]) == 0) {
         return true;
      }
   }
   return false;
}

bool claude_history_has_tool_use_without_thinking(struct json_object *conversation) {
   if (!conversation) {
      return false;
   }

   int conv_len = json_object_array_length(conversation);
   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(conversation, i);
      json_object *role_obj, *content_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      // Check assistant messages with array content (Claude format with tool_use)
      if (strcmp(role, "assistant") == 0 &&
          json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_is_type(content_obj, json_type_array)) {
         int content_len = json_object_array_length(content_obj);
         bool has_tool_use = false;
         bool has_thinking = false;

         for (int j = 0; j < content_len; j++) {
            json_object *block = json_object_array_get_idx(content_obj, j);
            json_object *type_obj;
            if (json_object_object_get_ex(block, "type", &type_obj)) {
               const char *block_type = json_object_get_string(type_obj);
               if (strcmp(block_type, "tool_use") == 0) {
                  has_tool_use = true;
               } else if (strcmp(block_type, "thinking") == 0) {
                  has_thinking = true;
               }
            }
         }

         // If this message has tool_use but no thinking, history is incompatible
         if (has_tool_use && !has_thinking) {
            return true;
         }
      }

      // Check for OpenAI-format tool_calls (also incompatible - no thinking block format)
      json_object *tool_calls_obj;
      if (strcmp(role, "assistant") == 0 &&
          json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj) &&
          json_object_is_type(tool_calls_obj, json_type_array) &&
          json_object_array_length(tool_calls_obj) > 0) {
         // OpenAI-format tool_calls - these never have thinking blocks
         return true;
      }
   }

   return false;
}

bool claude_history_has_openai_tool_calls(struct json_object *conversation) {
   if (!conversation) {
      return false;
   }
   int conv_len = json_object_array_length(conversation);
   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(conversation, i);
      json_object *role_obj, *tool_calls_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      if (strcmp(json_object_get_string(role_obj), "assistant") != 0) {
         continue;
      }
      if (json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj) &&
          json_object_is_type(tool_calls_obj, json_type_array) &&
          json_object_array_length(tool_calls_obj) > 0) {
         return true;
      }
   }
   return false;
}

bool claude_history_has_thinking_blocks(struct json_object *conversation) {
   if (!conversation) {
      return false;
   }

   int conv_len = json_object_array_length(conversation);
   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(conversation, i);
      json_object *role_obj, *content_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      // Check assistant messages with array content
      if (strcmp(role, "assistant") == 0 &&
          json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_is_type(content_obj, json_type_array)) {
         int content_len = json_object_array_length(content_obj);

         for (int j = 0; j < content_len; j++) {
            json_object *block = json_object_array_get_idx(content_obj, j);
            json_object *type_obj;
            if (json_object_object_get_ex(block, "type", &type_obj)) {
               const char *block_type = json_object_get_string(type_obj);
               if (strcmp(block_type, "thinking") == 0) {
                  return true;
               }
            }
         }
      }
   }

   return false;
}

/**
 * @brief Detect MIME type from base64-encoded image data
 *
 * Checks the first few base64 characters which encode the magic bytes:
 * - PNG:  starts with "iVBORw0KGgo" (0x89 0x50 0x4E 0x47...)
 * - JPEG: starts with "/9j/"        (0xFF 0xD8 0xFF)
 * - GIF:  starts with "R0lGOD"      ("GIF8")
 * - WebP: starts with "UklGR"       ("RIFF")
 *
 * @param base64_data Base64-encoded image data
 * @return MIME type string (static, do not free)
 */
const char *llm_claude_detect_image_mime_type(const char *base64_data) {
   if (!base64_data || strlen(base64_data) < 8) {
      return "image/jpeg"; /* Default fallback */
   }

   /* PNG: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A -> "iVBORw0KGgo" */
   if (strncmp(base64_data, "iVBORw", 6) == 0) {
      return "image/png";
   }

   /* JPEG: 0xFF 0xD8 0xFF -> "/9j/" */
   if (strncmp(base64_data, "/9j/", 4) == 0) {
      return "image/jpeg";
   }

   /* GIF: "GIF8" -> "R0lGOD" */
   if (strncmp(base64_data, "R0lGOD", 6) == 0) {
      return "image/gif";
   }

   /* WebP: "RIFF" -> "UklGR" */
   if (strncmp(base64_data, "UklGR", 5) == 0) {
      return "image/webp";
   }

   return "image/jpeg"; /* Default fallback */
}

/**
 * @brief Create a Claude-format image content block
 *
 * Automatically detects the image MIME type from the base64 data.
 */
json_object *llm_claude_create_image_block(const char *vision_image) {
   json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image"));

   /* Detect actual MIME type from base64 data */
   const char *media_type = llm_claude_detect_image_mime_type(vision_image);

   json_object *source_obj = json_object_new_object();
   json_object_object_add(source_obj, "type", json_object_new_string("base64"));
   json_object_object_add(source_obj, "media_type", json_object_new_string(media_type));
   json_object_object_add(source_obj, "data", json_object_new_string(vision_image));
   json_object_object_add(image_obj, "source", source_obj);

   OLOG_INFO("Claude: Created image block with detected media_type=%s", media_type);

   return image_obj;
}

/**
 * @brief Convert OpenAI image_url content block to Claude image format
 *
 * OpenAI format: {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}}
 * Claude format: {"type": "image", "source": {"type": "base64", "media_type": "...", "data":
 * "..."}}
 *
 * @param block Content block to convert (or copy if not image_url)
 * @return New json_object in Claude format (caller must json_object_put), or NULL on error
 */
static json_object *convert_content_block_to_claude(json_object *block) {
   if (!block) {
      return NULL;
   }

   json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      // No type field - Claude requires type on all content blocks
      // Check if it has a "text" field (possible malformed text block)
      json_object *text_obj;
      if (json_object_object_get_ex(block, "text", &text_obj)) {
         // Convert to proper text block
         json_object *fixed_block = json_object_new_object();
         json_object_object_add(fixed_block, "type", json_object_new_string("text"));
         json_object_object_add(fixed_block, "text", json_object_get(text_obj));
         OLOG_WARNING("Claude: Fixed content block missing type (had text field)");
         return fixed_block;
      }
      // Unknown format - skip it to avoid Claude API errors
      OLOG_WARNING("Claude: Skipping content block with no type field");
      return NULL;
   }

   const char *block_type = json_object_get_string(type_obj);
   if (!block_type || strcmp(block_type, "image_url") != 0) {
      // Not image_url - just copy the block (already has type)
      return json_object_get(block);
   }

   // This is an OpenAI image_url block - convert to Claude format
   json_object *image_url_obj;
   if (!json_object_object_get_ex(block, "image_url", &image_url_obj)) {
      OLOG_WARNING("Claude: image_url block missing image_url field");
      return NULL;
   }

   json_object *url_obj;
   if (!json_object_object_get_ex(image_url_obj, "url", &url_obj)) {
      OLOG_WARNING("Claude: image_url block missing url field");
      return NULL;
   }

   const char *url = json_object_get_string(url_obj);
   if (!url) {
      return NULL;
   }

   // Parse data URL: data:image/jpeg;base64,<data>
   const char *data_prefix = "data:";
   if (strncmp(url, data_prefix, strlen(data_prefix)) != 0) {
      OLOG_WARNING("Claude: image_url is not a data URL: %.50s...", url);
      return NULL;
   }

   // Find media type and base64 data
   const char *after_data = url + strlen(data_prefix);
   const char *semicolon = strchr(after_data, ';');
   const char *comma = strchr(after_data, ',');

   if (!semicolon || !comma || comma < semicolon) {
      OLOG_WARNING("Claude: Invalid data URL format");
      return NULL;
   }

   // Extract media type
   size_t media_type_len = semicolon - after_data;
   char media_type[64];
   if (media_type_len >= sizeof(media_type)) {
      media_type_len = sizeof(media_type) - 1;
   }
   strncpy(media_type, after_data, media_type_len);
   media_type[media_type_len] = '\0';

   // The base64 data is after the comma
   const char *base64_data = comma + 1;

   // Create Claude-format image block
   json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image"));

   json_object *source_obj = json_object_new_object();
   json_object_object_add(source_obj, "type", json_object_new_string("base64"));
   json_object_object_add(source_obj, "media_type", json_object_new_string(media_type));
   json_object_object_add(source_obj, "data", json_object_new_string(base64_data));
   json_object_object_add(image_obj, "source", source_obj);

   OLOG_INFO("Claude: Converted OpenAI image_url to Claude image (media_type=%s)", media_type);
   return image_obj;
}

/**
 * @brief Add vision images to Claude messages array
 *
 * Either modifies the last user message to include the images, or creates
 * a new user message if the last message isn't suitable.
 *
 * @param messages_array Claude messages array
 * @param input_text Text to include with the images
 * @param vision_images Array of base64-encoded image data
 * @param vision_image_count Number of images in the array
 */
static void add_vision_to_claude_messages(json_object *messages_array,
                                          const char *input_text,
                                          const char **vision_images,
                                          int vision_image_count) {
   if (vision_image_count <= 0 || !vision_images) {
      return;
   }

   int msg_count = json_object_array_length(messages_array);
   json_object *last_msg = msg_count > 0 ? json_object_array_get_idx(messages_array, msg_count - 1)
                                         : NULL;
   json_object *role_obj = NULL;
   const char *last_role_str = NULL;

   if (last_msg && json_object_object_get_ex(last_msg, "role", &role_obj)) {
      last_role_str = json_object_get_string(role_obj);
   }

   // Check if last message is a user message with plain text content
   json_object *last_content = NULL;
   bool is_text_user_message = false;
   if (last_msg && last_role_str && strcmp(last_role_str, "user") == 0) {
      if (json_object_object_get_ex(last_msg, "content", &last_content)) {
         if (json_object_is_type(last_content, json_type_string)) {
            is_text_user_message = true;
         }
      }
   }

   if (is_text_user_message) {
      // Last message is user message with text - add vision content
      json_object *content_array = json_object_new_array();

      json_object *text_obj = json_object_new_object();
      json_object_object_add(text_obj, "type", json_object_new_string("text"));
      json_object_object_add(text_obj, "text", json_object_new_string(input_text));
      json_object_array_add(content_array, text_obj);

      // Add all images
      for (int i = 0; i < vision_image_count; i++) {
         if (vision_images[i]) {
            json_object_array_add(content_array, llm_claude_create_image_block(vision_images[i]));
         }
      }
      json_object_object_add(last_msg, "content", content_array);
   } else {
      // Create a new user message with the vision images
      OLOG_INFO("Claude: Adding vision as new user message (last message not suitable)");

      json_object *new_user_msg = json_object_new_object();
      json_object_object_add(new_user_msg, "role", json_object_new_string("user"));

      json_object *content_array = json_object_new_array();

      json_object *text_obj = json_object_new_object();
      json_object_object_add(text_obj, "type", json_object_new_string("text"));
      json_object_object_add(text_obj, "text",
                             json_object_new_string("Here are the captured images. "
                                                    "Please respond to the user's request."));
      json_object_array_add(content_array, text_obj);

      // Add all images
      for (int i = 0; i < vision_image_count; i++) {
         if (vision_images[i]) {
            json_object_array_add(content_array, llm_claude_create_image_block(vision_images[i]));
         }
      }
      json_object_object_add(new_user_msg, "content", content_array);

      json_object_array_add(messages_array, new_user_msg);
   }
}

/**
 * @brief Parse the leading version number out of a Claude model id.
 *
 * Takes the first numeric token that follows a delimiter, e.g.
 * "claude-opus-4-8" -> 4.8, "claude-sonnet-5" -> 5.0, "claude-haiku-4-5" -> 4.5,
 * "claude-fable-5" -> 5.0, "claude-3-5-sonnet-20241022" -> 3.5.  The version is
 * always the first version token in current Claude ids, so a trailing date
 * suffix does not interfere.
 */
static void claude_parse_model_version(const char *model_name, int *major_out, int *minor_out) {
   int major = 0, minor = 0;
   const char *p = model_name ? model_name : "";
   while (*p) {
      if ((*p >= '0' && *p <= '9') && (p == model_name || *(p - 1) == '-' || *(p - 1) == '.')) {
         major = atoi(p);
         while (*p >= '0' && *p <= '9') {
            p++;
         }
         if ((*p == '-' || *p == '.') && (*(p + 1) >= '0' && *(p + 1) <= '9')) {
            minor = atoi(p + 1);
         }
         break;
      }
      p++;
   }
   *major_out = major;
   *minor_out = minor;
}

/**
 * @brief Whether a Claude model requires adaptive thinking instead of the
 *        legacy `thinking.type=enabled` + `budget_tokens` shape.
 *
 * Anthropic removed `thinking.type=enabled` / `budget_tokens` on Opus 4.7+,
 * Sonnet 5, and Fable/Mythos 5 — those models reject it with a 400 and require
 * `thinking.type=adaptive` + `output_config.effort`.  Opus 4.6 / Sonnet 4.6,
 * Haiku 4.5, Sonnet 4.5, and older still accept the legacy enabled+budget shape.
 */
static bool claude_model_requires_adaptive_thinking(const char *model_name) {
   int major = 0, minor = 0;
   claude_parse_model_version(model_name, &major, &minor);
   if (major >= 5) {
      return true; /* Sonnet 5, Fable 5, Mythos 5, and future 5.x */
   }
   if (major == 4 && minor >= 7) {
      return true; /* Opus 4.7, 4.8 */
   }
   return false;
}

json_object *convert_to_claude_format(struct json_object *openai_conversation,
                                      const char *input_text,
                                      const char **vision_images,
                                      const size_t *vision_image_sizes,
                                      int vision_image_count,
                                      const char *model,
                                      int iteration) {
   (void)vision_image_sizes;  // Sizes not needed for base64 strings
   json_object *claude_request = json_object_new_object();

   // Model: use passed model, or fall back to config default
   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = llm_get_default_claude_model();
   }
   json_object_object_add(claude_request, "model", json_object_new_string(model_name));

   // Determine if extended thinking will be enabled
   // Use session-aware thinking mode (respects per-conversation settings)
   const char *thinking_mode = llm_get_current_thinking_mode();
   bool thinking_enabled = false;
   int thinking_budget = 0;
   OLOG_INFO("Claude: thinking_mode='%s', model='%s'", thinking_mode, model_name);

   if (strcmp(thinking_mode, "disabled") != 0) {
      // Check if model supports thinking (Claude 3.5+)
      // Extract version from model name and check if >= 3.5
      bool model_supports = false;
      int major = 0, minor = 0;
      const char *p = model_name;
      while (*p) {
         // Look for version patterns like "-3-5-", "-4-", "3.5", etc.
         if ((*p >= '0' && *p <= '9') && (p == model_name || *(p - 1) == '-' || *(p - 1) == '.')) {
            major = atoi(p);
            // Look for minor version after - or .
            while (*p >= '0' && *p <= '9')
               p++;
            if (*p == '-' || *p == '.') {
               p++;
               if (*p >= '0' && *p <= '9') {
                  minor = atoi(p);
               }
            }
            // Version >= 3.5 supports thinking
            if (major > 3 || (major == 3 && minor >= 5)) {
               model_supports = true;
               break;
            }
         }
         p++;
      }
      OLOG_INFO("Claude: version detected %d.%d, model_supports=%d", major, minor, model_supports);

      if (model_supports || strcmp(thinking_mode, "enabled") == 0) {
         thinking_enabled = true;
         thinking_budget = llm_get_effective_budget_tokens();
      }
   }

   // Disable thinking for internal utility calls (search summarization, context compaction)
   // These calls suppress tools and expect simple text responses
   if (thinking_enabled && llm_tools_suppressed()) {
      thinking_enabled = false;
   }

   // Check if conversation history is compatible with thinking mode
   // Claude requires assistant messages with tool_use to start with thinking blocks
   // IMPORTANT: If history already has thinking blocks, we CANNOT disable thinking
   // (Claude rejects "assistant message cannot contain thinking" when disabled)
   bool has_existing_thinking = claude_history_has_thinking_blocks(openai_conversation);

   if (thinking_enabled && claude_history_has_tool_use_without_thinking(openai_conversation)) {
      if (has_existing_thinking) {
         // History has both thinking blocks AND tool_use without thinking — keep
         // thinking enabled (disabling would cause "cannot contain thinking" error).
         //
         // Two sub-cases:
         //   (a) Pure Claude history where a follow-up turn after a tool_result
         //       legitimately omitted its thinking block. Normal behavior, silent.
         //   (b) OpenAI-format `tool_calls` field is present (provider switched
         //       mid-conversation). Real concern worth surfacing — INFO log so
         //       the operator can spot it without it screaming WARN.
         if (claude_history_has_openai_tool_calls(openai_conversation)) {
            OLOG_INFO("Claude: History has OpenAI-format tool_calls mixed with Claude "
                      "thinking blocks (provider switched mid-conversation); keeping "
                      "thinking enabled");
         }
      } else {
         // Safe to disable - no thinking blocks in history
         thinking_enabled = false;

#ifdef ENABLE_WEBUI
         /* Send notification to WebUI about thinking being disabled (once per session) */
         static uint32_t last_notified_session = 0;
         session_t *session = session_get_command_context();
         if (session && session->type == SESSION_TYPE_WEBUI &&
             session->session_id != last_notified_session) {
            last_notified_session = session->session_id;
            OLOG_INFO("Claude: Auto-disabled thinking for session %u (incompatible history)",
                      session->session_id);
            webui_send_error(session, "INFO_THINKING_DISABLED",
                             "Extended thinking auto-disabled: conversation history from "
                             "previous provider is incompatible. Start a new conversation "
                             "to use thinking with Claude.");
         }
#endif
      }
   }

   // Max tokens: Claude requires max_tokens > budget_tokens when thinking is enabled
   int max_tokens = g_config.llm.max_tokens;
   if (thinking_enabled && max_tokens <= thinking_budget) {
      // Ensure max_tokens > budget_tokens, add buffer for response
      max_tokens = thinking_budget + 4096;
      OLOG_INFO("Claude: Adjusted max_tokens to %d (budget %d + 4096 response buffer)", max_tokens,
                thinking_budget);
   }
   json_object_object_add(claude_request, "max_tokens", json_object_new_int(max_tokens));

   // Add extended thinking configuration.  Newer Claude models (Opus 4.7+,
   // Sonnet 5, Fable/Mythos 5) reject `thinking.type=enabled` + `budget_tokens`
   // with a 400 and require `thinking.type=adaptive` + `output_config.effort`;
   // older models still use the legacy enabled+budget shape.
   if (thinking_enabled) {
      json_object *thinking = json_object_new_object();
      if (claude_model_requires_adaptive_thinking(model_name)) {
         json_object_object_add(thinking, "type", json_object_new_string("adaptive"));
         /* Default display is "omitted" on these models (empty thinking text);
          * request "summarized" so DAWN's thinking UI still shows reasoning. */
         json_object_object_add(thinking, "display", json_object_new_string("summarized"));
         json_object_object_add(claude_request, "thinking", thinking);

         const char *effort = llm_get_current_reasoning_effort();
         if (!effort || effort[0] == '\0') {
            effort = "medium";
         }
         json_object *output_config = json_object_new_object();
         json_object_object_add(output_config, "effort", json_object_new_string(effort));
         json_object_object_add(claude_request, "output_config", output_config);
         OLOG_INFO("Claude: Adaptive thinking enabled (effort: %s)", effort);
      } else {
         json_object_object_add(thinking, "type", json_object_new_string("enabled"));
         json_object_object_add(thinking, "budget_tokens", json_object_new_int(thinking_budget));
         json_object_object_add(claude_request, "thinking", thinking);
         OLOG_INFO("Claude: Extended thinking enabled (budget: %d tokens)", thinking_budget);
      }
   }

   // Add tools if native tool calling is enabled
   if (llm_tools_enabled(NULL)) {
      bool is_remote = is_current_session_remote();
      struct json_object *tools = llm_tools_get_claude_format_filtered(is_remote);
      if (tools) {
         json_object_object_add(claude_request, "tools", tools);
         OLOG_INFO("Claude: Added %d tools to request (%s session)",
                   llm_tools_get_enabled_count_filtered(is_remote), is_remote ? "remote" : "local");

         /* Attach cache_control to the LAST tool so the entire tools
          * array is cacheable as its own prefix.  Anthropic caches
          * "everything up to and including the block that carries the
          * cache_control marker"; placing it on the final tool means
          * the full tools list becomes a single cache breakpoint
          * independent of the system block's cache breakpoint below.
          *
          * Tool schemas (~3-5 KB JSON across 27 tools today) change
          * only on tool registry / capability edits — settings-stable
          * across a session.  This saves another ~700-1300 tokens at
          * the 90% read discount per turn, on top of the system-block
          * cache.  Anthropic permits up to 4 cache_control blocks per
          * request; we use 2 (last tool + first system block).  Two
          * slots remain for future use. */
         int tool_count = json_object_array_length(tools);
         if (tool_count > 0) {
            json_object *last_tool = json_object_array_get_idx(tools, tool_count - 1);
            if (last_tool != NULL && json_object_is_type(last_tool, json_type_object)) {
               json_object *cache_control = json_object_new_object();
               json_object_object_add(cache_control, "type", json_object_new_string("ephemeral"));
               json_object_object_add(last_tool, "cache_control", cache_control);
            }
         }
      }
   }

   // Extract system message and user/assistant messages
   json_object *system_array = json_object_new_array();
   json_object *messages_array = json_object_new_array();

   int conv_len = json_object_array_length(openai_conversation);
   const char *last_role = NULL;
   json_object *last_message = NULL;

   // Pre-scan to collect tool result IDs for orphaned tool_use filtering.
   //
   // Runs on EVERY iteration, not just iteration 0.  The history can contain
   // orphaned tool_use blocks left over from an interrupted earlier turn (a
   // tool_use whose tool_result never landed — e.g. wake-word interrupt / rolled
   // back call).  Those orphans persist in conversation_history and must be
   // filtered out of every request, or Claude rejects the follow-up call with
   // "tool_use ids were found without tool_result blocks" (HTTP 400).  Filtering
   // is safe on follow-up iterations too: by the time each call is built, every
   // legitimate tool_use already has its paired tool_result appended, so only the
   // true orphans get dropped.
   //
   // Two-pass approach (count then allocate) is preferred over a growing array:
   // - Single exact-size malloc avoids fragmentation and realloc overhead
   // - Memory footprint is predictable (typically 64-320 bytes)
   // - CPU cost is acceptable since this runs once per provider call, not in audio loops
   //
   // `iteration` is no longer read (request-building is iteration-independent now
   // that filtering is unconditional); kept in the signature for caller stability.
   (void)iteration;
   char(*tool_result_ids)[LLM_TOOLS_ID_LEN] = NULL;
   int tool_result_count = 0;

   {
      // Count tool results first, then allocate exact size needed
      int result_count = count_tool_results(openai_conversation);

      // Sanity bound to prevent integer overflow in allocation calculation
      if (result_count > 10000) {
         OLOG_WARNING("Claude: Excessive tool results in history (%d), capping at 10000",
                      result_count);
         result_count = 10000;
      }

      if (result_count > 0) {
         tool_result_ids = malloc(result_count * LLM_TOOLS_ID_LEN);
         if (tool_result_ids) {
            tool_result_count = collect_tool_result_ids(openai_conversation, tool_result_ids,
                                                        result_count);
         }
      }
   }

   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(openai_conversation, i);
      json_object *role_obj, *content_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj) ||
          !json_object_object_get_ex(msg, "content", &content_obj)) {
         continue;
      }

      const char *role = json_object_get_string(role_obj);

      // Handle Claude-format assistant messages with tool_use content blocks
      // These need to be filtered to remove orphaned tool_use blocks
      if (strcmp(role, "assistant") == 0 && json_object_is_type(content_obj, json_type_array)) {
         int content_len = json_object_array_length(content_obj);
         json_object *filtered_content = json_object_new_array();
         int has_tool_use = 0;

         for (int j = 0; j < content_len; j++) {
            json_object *block = json_object_array_get_idx(content_obj, j);
            json_object *type_obj;

            if (!json_object_object_get_ex(block, "type", &type_obj)) {
               // Skip malformed blocks without type - Claude requires type field
               OLOG_WARNING("Claude: Skipping content block without type field");
               continue;
            }

            const char *block_type = json_object_get_string(type_obj);

            if (strcmp(block_type, "tool_use") == 0) {
               // Drop this tool_use if the history has no matching tool_result (orphan)
               json_object *id_obj;
               if (json_object_object_get_ex(block, "id", &id_obj)) {
                  const char *tool_id = json_object_get_string(id_obj);
                  // tool_result_ids is NULL only when there are no results at all
                  if (!tool_result_ids ||
                      has_matching_tool_result(tool_id, tool_result_ids, tool_result_count)) {
                     json_object_array_add(filtered_content, json_object_get(block));
                     has_tool_use = 1;
                  } else {
                     OLOG_WARNING("Claude: Skipping orphaned tool_use %s", tool_id);
                  }
               }
            } else {
               // Keep text and other blocks
               json_object_array_add(filtered_content, json_object_get(block));
            }
         }

         // Only add if we have content
         if (json_object_array_length(filtered_content) > 0) {
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string("assistant"));
            json_object_object_add(last_message, "content", filtered_content);
            json_object_array_add(messages_array, last_message);
            last_role = "assistant";
         } else {
            json_object_put(filtered_content);
         }
         continue;
      }

      // Convert assistant messages with tool_calls (OpenAI format) to Claude tool_use format
      json_object *tool_calls_obj;
      if (strcmp(role, "assistant") == 0 &&
          json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj)) {
         // Build Claude content array with tool_use blocks
         json_object *content_array = json_object_new_array();

         // Include any text content first
         const char *text_content = json_object_get_string(content_obj);
         if (text_content && strlen(text_content) > 0) {
            json_object *text_block = json_object_new_object();
            json_object_object_add(text_block, "type", json_object_new_string("text"));
            json_object_object_add(text_block, "text", json_object_new_string(text_content));
            json_object_array_add(content_array, text_block);
         }

         // Convert each tool call to Claude tool_use format
         // Only include tool calls that have matching results in the history
         int num_calls = json_object_array_length(tool_calls_obj);
         int added_tool_uses = 0;
         for (int j = 0; j < num_calls; j++) {
            json_object *call = json_object_array_get_idx(tool_calls_obj, j);
            json_object *func_obj, *id_obj, *name_obj, *args_obj;

            const char *call_id = "";
            const char *name = "";
            const char *args_str = "{}";

            if (json_object_object_get_ex(call, "id", &id_obj)) {
               call_id = json_object_get_string(id_obj);
            }

            // Skip this tool call if the history has no matching result (orphan)
            if (tool_result_ids &&
                !has_matching_tool_result(call_id, tool_result_ids, tool_result_count)) {
               OLOG_WARNING("Claude: Skipping tool_use %s (no matching tool_result)", call_id);
               continue;
            }

            if (json_object_object_get_ex(call, "function", &func_obj)) {
               if (json_object_object_get_ex(func_obj, "name", &name_obj)) {
                  name = json_object_get_string(name_obj);
               }
               if (json_object_object_get_ex(func_obj, "arguments", &args_obj)) {
                  args_str = json_object_get_string(args_obj);
               }
            }

            // Create tool_use block
            json_object *tool_use = json_object_new_object();
            json_object_object_add(tool_use, "type", json_object_new_string("tool_use"));
            json_object_object_add(tool_use, "id", json_object_new_string(call_id));
            json_object_object_add(tool_use, "name", json_object_new_string(name));

            // Parse args string to JSON object
            json_object *input = json_tokener_parse(args_str);
            if (input) {
               json_object_object_add(tool_use, "input", input);
            } else {
               json_object_object_add(tool_use, "input", json_object_new_object());
            }

            json_object_array_add(content_array, tool_use);
            added_tool_uses++;
         }

         // Silence unused variable warning
         (void)added_tool_uses;

         // Only add assistant message if we have content (text or tool_use blocks)
         if (json_object_array_length(content_array) > 0) {
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string("assistant"));
            json_object_object_add(last_message, "content", content_array);
            json_object_array_add(messages_array, last_message);
            last_role = "assistant";
         } else {
            json_object_put(content_array);  // Free unused array
         }
         continue;
      }

      // Convert tool role messages (OpenAI format) to Claude tool_result format
      if (strcmp(role, "tool") == 0) {
         json_object *tool_call_id_obj;
         const char *tool_call_id = NULL;
         const char *result_content = json_object_get_string(content_obj);

         if (json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            tool_call_id = json_object_get_string(tool_call_id_obj);
         }

         // Handle orphaned tool messages (restored from DB without tool_call_id)
         if (!tool_call_id || strlen(tool_call_id) == 0) {
            OLOG_WARNING(
                "Claude: Orphaned tool message without tool_call_id, converting to summary");
            // Convert to assistant summary to preserve context
            if (result_content && strlen(result_content) > 0) {
               char summary[2048];
               snprintf(summary, sizeof(summary), "[Previous tool result: %.1900s%s]",
                        result_content, strlen(result_content) > 1900 ? "..." : "");
               /* `%.1900s` is a BYTE precision, so it splits multi-byte characters —
                * and an invalid sequence here fails the ENTIRE provider request, not
                * just this message.  This is the more frequent of the two cuts in this
                * branch: it runs for every orphaned tool result, while the 4096-byte
                * combine below only runs when appending to an existing assistant turn.
                * Both are sanitized; neither is safe alone, since the combine can land
                * mid-codepoint on the concatenation even when its inputs are clean. */
               sanitize_utf8_for_json(summary);

               json_object *summary_msg = json_object_new_object();
               json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));
               json_object_object_add(summary_msg, "content", json_object_new_string(summary));

               // Handle role alternation
               if (last_role != NULL && strcmp(last_role, "assistant") == 0 &&
                   last_message != NULL) {
                  // Append to existing assistant content
                  json_object *last_content;
                  if (json_object_object_get_ex(last_message, "content", &last_content)) {
                     if (json_object_is_type(last_content, json_type_array)) {
                        // Content is an array - append a text block
                        json_object *text_block = json_object_new_object();
                        json_object_object_add(text_block, "type", json_object_new_string("text"));
                        json_object_object_add(text_block, "text", json_object_new_string(summary));
                        json_object_array_add(last_content, text_block);
                     } else {
                        // Content is a string - combine strings
                        const char *existing = json_object_get_string(last_content);
                        size_t existing_len = existing ? strlen(existing) : 0;
                        size_t summary_len = strlen(summary);
                        size_t needed = existing_len + summary_len + 2; /* +2 for \n and \0 */

                        char combined[4096];
                        if (needed > sizeof(combined)) {
                           OLOG_WARNING("Claude: Combined content truncated from %zu to %zu bytes",
                                        needed, sizeof(combined));
                        }
                        snprintf(combined, sizeof(combined), "%s\n%s", existing ? existing : "",
                                 summary);
                        /* snprintf cuts on a BYTE boundary, so a multi-byte character
                         * straddling the limit leaves a partial sequence — and the whole
                         * request then dies at the provider, not just this field:
                         * "The request body is not valid JSON: str is not valid UTF-8:
                         * surrogates not allowed" (HTTP 400).  Live-verified on a resumed
                         * research job (conv 1041), which is the likely victim: resume
                         * hydrates history whose tool rows arrive orphaned, this branch
                         * combines their summaries, and search results are full of
                         * non-ASCII, so the cut lands mid-codepoint often. */
                        sanitize_utf8_for_json(combined);
                        json_object_object_add(last_message, "content",
                                               json_object_new_string(combined));
                     }
                  }
                  json_object_put(summary_msg);
               } else {
                  json_object_array_add(messages_array, summary_msg);
                  last_message = summary_msg;
                  last_role = "assistant";
               }
            }
            continue;
         }

         // Create tool_result block in a user message (Claude requirement)
         json_object *result_array = json_object_new_array();
         json_object *result_block = json_object_new_object();
         json_object_object_add(result_block, "type", json_object_new_string("tool_result"));
         json_object_object_add(result_block, "tool_use_id", json_object_new_string(tool_call_id));
         json_object_object_add(result_block, "content",
                                json_object_new_string(result_content ? result_content : ""));
         json_object_array_add(result_array, result_block);

         // Tool results must be in user messages for Claude
         if (last_role != NULL && strcmp(last_role, "user") == 0 && last_message != NULL) {
            // Append to existing user message content array
            json_object *last_content;
            if (json_object_object_get_ex(last_message, "content", &last_content) &&
                json_object_is_type(last_content, json_type_array)) {
               /* result_block is already owned by result_array (added above); take a second
                * reference for last_content before freeing the wrapper, otherwise the put()
                * below frees result_block out from under last_content — a use-after-free /
                * double-free that crashes on consecutive tool results (e.g. parallel tool
                * calls producing several tool_result messages in a row). */
               json_object_array_add(last_content, json_object_get(result_block));
               json_object_put(result_array);  // Don't need the wrapper array
            } else {
               // Replace string content with array
               json_object_object_add(last_message, "content", result_array);
            }
         } else {
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string("user"));
            json_object_object_add(last_message, "content", result_array);
            json_object_array_add(messages_array, last_message);
            last_role = "user";
         }
         continue;
      }

      if (strcmp(role, "system") == 0) {
         // System message goes in separate "system" array with cache control
         json_object *system_block = json_object_new_object();
         json_object_object_add(system_block, "type", json_object_new_string("text"));
         json_object_object_add(system_block, "text", json_object_get(content_obj));

         // Cache control on first system block only (main prompt).
         // Claude allows max 4 cache_control blocks per request — injected
         // context messages (phone events, etc.) don't need caching.
         if (json_object_array_length(system_array) == 0) {
            json_object *cache_control = json_object_new_object();
            json_object_object_add(cache_control, "type", json_object_new_string("ephemeral"));
            json_object_object_add(system_block, "cache_control", cache_control);
         }

         json_object_array_add(system_array, system_block);
      } else {
         // Claude requires strict role alternation - consolidate consecutive same-role messages
         if (last_role != NULL && strcmp(last_role, role) == 0 && last_message != NULL) {
            // Same role as previous - need to consolidate
            json_object *last_content_obj;
            if (json_object_object_get_ex(last_message, "content", &last_content_obj)) {
               bool last_is_array = json_object_is_type(last_content_obj, json_type_array);
               bool curr_is_array = json_object_is_type(content_obj, json_type_array);

               if (last_is_array) {
                  // Last is array - append current content to it
                  if (curr_is_array) {
                     // Both arrays - copy all blocks from current to last
                     // Convert any OpenAI image_url blocks to Claude image format
                     int curr_len = json_object_array_length(content_obj);
                     for (int j = 0; j < curr_len; j++) {
                        json_object *block = json_object_array_get_idx(content_obj, j);
                        json_object *converted = convert_content_block_to_claude(block);
                        if (converted) {
                           json_object_array_add(last_content_obj, converted);
                        }
                     }
                  } else {
                     // Current is string - add as text block
                     const char *current_str = json_object_get_string(content_obj);
                     if (current_str && strlen(current_str) > 0) {
                        json_object *text_block = json_object_new_object();
                        json_object_object_add(text_block, "type", json_object_new_string("text"));
                        json_object_object_add(text_block, "text",
                                               json_object_new_string(current_str));
                        json_object_array_add(last_content_obj, text_block);
                     }
                  }
               } else if (curr_is_array) {
                  // Last is string, current is array - convert last to array and merge
                  const char *last_str = json_object_get_string(last_content_obj);
                  json_object *new_array = json_object_new_array();

                  // Add last string as text block first
                  if (last_str && strlen(last_str) > 0) {
                     json_object *text_block = json_object_new_object();
                     json_object_object_add(text_block, "type", json_object_new_string("text"));
                     json_object_object_add(text_block, "text", json_object_new_string(last_str));
                     json_object_array_add(new_array, text_block);
                  }

                  // Add all blocks from current array
                  // Convert any OpenAI image_url blocks to Claude image format
                  int curr_len = json_object_array_length(content_obj);
                  for (int j = 0; j < curr_len; j++) {
                     json_object *block = json_object_array_get_idx(content_obj, j);
                     json_object *converted = convert_content_block_to_claude(block);
                     if (converted) {
                        json_object_array_add(new_array, converted);
                     }
                  }

                  json_object_object_add(last_message, "content", new_array);
               } else {
                  // Both are strings - simple concatenation
                  const char *last_str = json_object_get_string(last_content_obj);
                  const char *curr_str = json_object_get_string(content_obj);
                  if (!last_str)
                     last_str = "";
                  if (!curr_str)
                     curr_str = "";

                  size_t new_len = strlen(last_str) + strlen(curr_str) + 4;
                  char *combined = malloc(new_len);
                  if (combined) {
                     snprintf(combined, new_len, "%s\n\n%s", last_str, curr_str);
                     json_object_object_add(last_message, "content",
                                            json_object_new_string(combined));
                     free(combined);
                  }
               }
            }
         } else {
            // Different role or first message - add new message
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string(role));

            // If content is an array, convert any image_url blocks to Claude format
            if (json_object_is_type(content_obj, json_type_array)) {
               json_object *converted_array = json_object_new_array();
               int content_len = json_object_array_length(content_obj);
               for (int j = 0; j < content_len; j++) {
                  json_object *block = json_object_array_get_idx(content_obj, j);
                  json_object *converted = convert_content_block_to_claude(block);
                  if (converted) {
                     json_object_array_add(converted_array, converted);
                  }
               }
               json_object_object_add(last_message, "content", converted_array);
            } else {
               json_object_object_add(last_message, "content", json_object_get(content_obj));
            }

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

   // If vision images are provided, add them to a user message
   if (vision_images != NULL && vision_image_count > 0) {
      add_vision_to_claude_messages(messages_array, input_text, vision_images, vision_image_count);
   }

   json_object_object_add(claude_request, "messages", messages_array);

   // Free dynamically allocated tool result IDs
   if (tool_result_ids) {
      free(tool_result_ids);
   }

   return claude_request;
}
