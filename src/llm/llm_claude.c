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
#include "llm/llm_openai.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
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

/* =============================================================================
 * Claude Format Conversion Helpers
 * ============================================================================= */

/**
 * @brief Collect tool result IDs from conversation history
 *
 * Pre-scans conversation to find all tool call IDs that have matching results.
 * Used to filter orphaned tool_use blocks.
 *
 * @param conversation OpenAI-format conversation history
 * @param tool_result_ids Output array for tool IDs
 * @param max_results Maximum number of results to collect
 * @return Number of tool result IDs collected
 */
static int collect_tool_result_ids(struct json_object *conversation,
                                   char tool_result_ids[][LLM_TOOLS_ID_LEN],
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

/**
 * @brief Create a Claude-format image content block
 */
static json_object *create_claude_image_block(const char *vision_image) {
   json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image"));

   json_object *source_obj = json_object_new_object();
   json_object_object_add(source_obj, "type", json_object_new_string("base64"));
   json_object_object_add(source_obj, "media_type", json_object_new_string("image/jpeg"));
   json_object_object_add(source_obj, "data", json_object_new_string(vision_image));
   json_object_object_add(image_obj, "source", source_obj);

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
      // No type field - just copy the block
      return json_object_get(block);
   }

   const char *block_type = json_object_get_string(type_obj);
   if (!block_type || strcmp(block_type, "image_url") != 0) {
      // Not image_url - just copy the block
      return json_object_get(block);
   }

   // This is an OpenAI image_url block - convert to Claude format
   json_object *image_url_obj;
   if (!json_object_object_get_ex(block, "image_url", &image_url_obj)) {
      LOG_WARNING("Claude: image_url block missing image_url field");
      return NULL;
   }

   json_object *url_obj;
   if (!json_object_object_get_ex(image_url_obj, "url", &url_obj)) {
      LOG_WARNING("Claude: image_url block missing url field");
      return NULL;
   }

   const char *url = json_object_get_string(url_obj);
   if (!url) {
      return NULL;
   }

   // Parse data URL: data:image/jpeg;base64,<data>
   const char *data_prefix = "data:";
   if (strncmp(url, data_prefix, strlen(data_prefix)) != 0) {
      LOG_WARNING("Claude: image_url is not a data URL: %.50s...", url);
      return NULL;
   }

   // Find media type and base64 data
   const char *after_data = url + strlen(data_prefix);
   const char *semicolon = strchr(after_data, ';');
   const char *comma = strchr(after_data, ',');

   if (!semicolon || !comma || comma < semicolon) {
      LOG_WARNING("Claude: Invalid data URL format");
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

   LOG_INFO("Claude: Converted OpenAI image_url to Claude image (media_type=%s)", media_type);
   return image_obj;
}

/**
 * @brief Add vision image to Claude messages array
 *
 * Either modifies the last user message to include the image, or creates
 * a new user message if the last message isn't suitable.
 *
 * @param messages_array Claude messages array
 * @param input_text Text to include with the image
 * @param vision_image Base64-encoded image data
 */
static void add_vision_to_claude_messages(json_object *messages_array,
                                          const char *input_text,
                                          const char *vision_image) {
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

      json_object_array_add(content_array, create_claude_image_block(vision_image));
      json_object_object_add(last_msg, "content", content_array);
   } else {
      // Create a new user message with the vision image
      LOG_INFO("Claude: Adding vision as new user message (last message not suitable)");

      json_object *new_user_msg = json_object_new_object();
      json_object_object_add(new_user_msg, "role", json_object_new_string("user"));

      json_object *content_array = json_object_new_array();

      json_object *text_obj = json_object_new_object();
      json_object_object_add(text_obj, "type", json_object_new_string("text"));
      json_object_object_add(text_obj, "text",
                             json_object_new_string("Here is the captured image. "
                                                    "Please respond to the user's request."));
      json_object_array_add(content_array, text_obj);

      json_object_array_add(content_array, create_claude_image_block(vision_image));
      json_object_object_add(new_user_msg, "content", content_array);

      json_object_array_add(messages_array, new_user_msg);
   }
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

   // Add tools if native tool calling is enabled
   if (llm_tools_enabled(NULL)) {
      struct json_object *tools = llm_tools_get_claude_format();
      if (tools) {
         json_object_object_add(claude_request, "tools", tools);
         LOG_INFO("Claude: Added %d tools to request", llm_tools_get_enabled_count());
      }
   }

   // Extract system message and user/assistant messages
   json_object *system_array = json_object_new_array();
   json_object *messages_array = json_object_new_array();

   int conv_len = json_object_array_length(openai_conversation);
   const char *last_role = NULL;
   json_object *last_message = NULL;

// Pre-scan to collect tool result IDs for orphaned tool_use filtering
#define MAX_TRACKED_TOOL_RESULTS 16
   char tool_result_ids[MAX_TRACKED_TOOL_RESULTS][LLM_TOOLS_ID_LEN];
   int tool_result_count = collect_tool_result_ids(openai_conversation, tool_result_ids,
                                                   MAX_TRACKED_TOOL_RESULTS);

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
               // Keep blocks without type
               json_object_array_add(filtered_content, json_object_get(block));
               continue;
            }

            const char *block_type = json_object_get_string(type_obj);

            if (strcmp(block_type, "tool_use") == 0) {
               // Check if this tool_use has a matching result
               json_object *id_obj;
               if (json_object_object_get_ex(block, "id", &id_obj)) {
                  const char *tool_id = json_object_get_string(id_obj);
                  if (has_matching_tool_result(tool_id, tool_result_ids, tool_result_count)) {
                     json_object_array_add(filtered_content, json_object_get(block));
                     has_tool_use = 1;
                  } else {
                     LOG_WARNING("Claude: Skipping orphaned tool_use %s", tool_id);
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

            // Check if this tool call has a matching result
            if (!has_matching_tool_result(call_id, tool_result_ids, tool_result_count)) {
               LOG_WARNING("Claude: Skipping tool_use %s (no matching tool_result)", call_id);
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
         const char *tool_call_id = "";
         const char *result_content = json_object_get_string(content_obj);

         if (json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            tool_call_id = json_object_get_string(tool_call_id_obj);
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
               json_object_array_add(last_content, result_block);
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

   // If vision is provided, add it to a user message
   if (vision_image != NULL && vision_image_size > 0) {
      add_vision_to_claude_messages(messages_array, input_text, vision_image);
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
 * @brief Internal streaming implementation with iteration tracking
 */
static char *llm_claude_streaming_internal(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
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
      curl_buffer_free(&streaming_ctx.raw_response);
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
      } else if (http_code == 400) {
         LOG_ERROR("Claude API: Bad request (HTTP 400) - check tool format and message structure");
         // Log the raw response which contains error details
         if (streaming_ctx.raw_response.data && streaming_ctx.raw_response.size > 0) {
            LOG_ERROR("Claude error response: %s", streaming_ctx.raw_response.data);
         }
         // Log a sample of the request for debugging
         LOG_WARNING("Claude request payload (first 1000 chars): %.1000s", payload);
      } else if (http_code != 0) {
         LOG_ERROR("Claude API: Request failed (HTTP %ld)", http_code);
      }
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
   curl_buffer_free(&streaming_ctx.raw_response);

   // Check for tool calls
   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         LOG_INFO("Claude streaming: Executing %d tool call(s)", tool_calls->count);

         // Execute tools
         tool_result_list_t results;
         llm_tools_execute_all(tool_calls, &results);

         // Add assistant message with tool_use blocks to conversation history
         // Claude format: content is an array of content blocks
         json_object *assistant_msg = json_object_new_object();
         json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

         json_object *content_array = json_object_new_array();
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
         llm_tools_add_results_claude(conversation_history, &results);

         // Cleanup current stream context
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(request);

         // Check if we should skip follow-up (e.g., LLM was switched)
         if (llm_tools_should_skip_followup(&results)) {
            LOG_INFO("Claude streaming: Skipping follow-up call (tool requested no follow-up)");
            char *direct_response = llm_tools_get_direct_response(&results);

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
            for (int i = 0; i < results.count; i++) {
               if (results.results[i].vision_image) {
                  free(results.results[i].vision_image);
                  results.results[i].vision_image = NULL;
               }
            }
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
            for (int i = 0; i < results.count; i++) {
               if (results.results[i].vision_image) {
                  free(results.results[i].vision_image);
                  results.results[i].vision_image = NULL;
               }
            }
            return strdup(error_msg);
         }

         // Check for vision data in tool results (session-isolated)
         const char *result_vision = NULL;
         size_t result_vision_size = 0;
         for (int i = 0; i < results.count; i++) {
            if (results.results[i].vision_image && results.results[i].vision_image_size > 0) {
               result_vision = results.results[i].vision_image;
               result_vision_size = results.results[i].vision_image_size;
               LOG_INFO("Claude streaming: Including vision from tool result (%zu bytes)",
                        result_vision_size);
               break;
            }
         }

         // Check if provider changed (e.g., switch_llm was called)
         llm_resolved_config_t current_config;
         char *result = NULL;

         LOG_INFO("Claude streaming: Making follow-up call after tool execution (iteration %d/%d)",
                  iteration + 1, MAX_TOOL_ITERATIONS);

         // Resolve config once and reuse for both provider check and credentials
         bool config_valid = (llm_get_current_resolved_config(&current_config) == 0);

         if (config_valid && (current_config.type == LLM_LOCAL ||
                              current_config.cloud_provider == CLOUD_PROVIDER_OPENAI)) {
            // Provider switched to OpenAI or local - hand off to OpenAI code path
            LOG_INFO("Claude streaming: Provider switched to OpenAI/local, handing off");

            // OpenAI will handle the vision data if present
            result = llm_openai_chat_completion_streaming(
                conversation_history, "", (char *)result_vision, result_vision_size,
                current_config.endpoint, current_config.api_key,
                (llm_openai_text_chunk_callback)chunk_callback, callback_userdata);
         } else {
            // Still Claude - use resolved config or fallback to original
            const char *fresh_url = config_valid ? current_config.endpoint : base_url;
            const char *fresh_api_key = config_valid ? current_config.api_key : api_key;

            result = llm_claude_streaming_internal(conversation_history, "", (char *)result_vision,
                                                   result_vision_size, fresh_url, fresh_api_key,
                                                   chunk_callback, callback_userdata,
                                                   iteration + 1);
         }

         // Free vision data from tool results after use
         for (int i = 0; i < results.count; i++) {
            if (results.results[i].vision_image) {
               free(results.results[i].vision_image);
               results.results[i].vision_image = NULL;
            }
         }

         return result;
      }
   }

   // Get accumulated response
   response = llm_stream_get_response(stream_ctx);

   // Cleanup
   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(request);

   return response;
}

char *llm_claude_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
                                           llm_claude_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   return llm_claude_streaming_internal(conversation_history, input_text, vision_image,
                                        vision_image_size, base_url, api_key, chunk_callback,
                                        callback_userdata, 0);
}
