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

#include "llm/llm_openai.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_claude.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
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
 * @brief Check if current session is remote (WebUI, DAP, etc.)
 *
 * Used to select the appropriate filtered tool set.
 * Local = SESSION_TYPE_LOCAL (helmet mic)
 * Remote = all other session types (WebSocket, DAP, etc.)
 */
static bool is_current_session_remote(void) {
   session_t *session = session_get_command_context();
   if (!session) {
      return false; /* No session context = local */
   }
   return (session->type != SESSION_TYPE_LOCAL);
}

/**
 * @brief Convert Claude-format tool content to text summary
 *
 * Claude uses content arrays with type "tool_use" or "tool_result" blocks.
 * This converts them to readable text summaries.
 *
 * @param msg JSON message object
 * @param summary_out Buffer to write summary (must be at least 2048 bytes)
 * @return 1 if message was converted, 0 if not a Claude tool message
 */
static int convert_claude_tool_to_summary(struct json_object *msg,
                                          char *summary_out,
                                          size_t summary_size) {
   struct json_object *content_obj;
   if (!json_object_object_get_ex(msg, "content", &content_obj)) {
      return 0;
   }

   // Claude tool messages have content as an array
   if (json_object_get_type(content_obj) != json_type_array) {
      return 0;
   }

   int arr_len = json_object_array_length(content_obj);
   size_t offset = 0;
   int found_tool = 0;
   char text_parts[2048] = "";
   size_t text_offset = 0;

   for (int i = 0; i < arr_len; i++) {
      struct json_object *elem = json_object_array_get_idx(content_obj, i);
      struct json_object *type_obj;

      if (!json_object_object_get_ex(elem, "type", &type_obj)) {
         continue;
      }

      const char *type_str = json_object_get_string(type_obj);

      if (strcmp(type_str, "tool_use") == 0) {
         // Claude tool_use block
         found_tool = 1;
         struct json_object *name_obj, *input_obj;
         const char *name = "unknown";
         const char *input_str = "{}";

         if (json_object_object_get_ex(elem, "name", &name_obj)) {
            name = json_object_get_string(name_obj);
         }
         if (json_object_object_get_ex(elem, "input", &input_obj)) {
            input_str = json_object_to_json_string(input_obj);
         }

         if (offset == 0) {
            offset += snprintf(summary_out + offset, summary_size - offset, "[Called tools: ");
         } else {
            offset += snprintf(summary_out + offset, summary_size - offset, ", ");
         }
         offset += snprintf(summary_out + offset, summary_size - offset, "%s(%.200s)", name,
                            input_str);

      } else if (strcmp(type_str, "tool_result") == 0) {
         // Claude tool_result block
         found_tool = 1;
         struct json_object *result_content_obj;
         const char *result = "";

         if (json_object_object_get_ex(elem, "content", &result_content_obj)) {
            result = json_object_get_string(result_content_obj);
         }

         if (offset > 0) {
            offset += snprintf(summary_out + offset, summary_size - offset, " ");
         }
         offset += snprintf(summary_out + offset, summary_size - offset, "[Tool result: %.900s]",
                            result ? result : "");

      } else if (strcmp(type_str, "text") == 0) {
         // Regular text content - preserve it
         struct json_object *text_obj;
         if (json_object_object_get_ex(elem, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            if (text && strlen(text) > 0) {
               if (text_offset > 0) {
                  text_offset += snprintf(text_parts + text_offset,
                                          sizeof(text_parts) - text_offset, "\n\n");
               }
               text_offset += snprintf(text_parts + text_offset, sizeof(text_parts) - text_offset,
                                       "%s", text);
            }
         }
      }
   }

   if (!found_tool) {
      return 0;
   }

   // Close the tool call bracket if we started one
   if (offset > 0 && summary_out[0] == '[' && summary_out[1] == 'C') {
      offset += snprintf(summary_out + offset, summary_size - offset, "]");
   }

   // Prepend any text content
   if (text_offset > 0) {
      char temp[8192];
      snprintf(temp, sizeof(temp), "%s\n\n%s", text_parts, summary_out);
      strncpy(summary_out, temp, summary_size - 1);
      summary_out[summary_size - 1] = '\0';
   }

   return 1;
}

/**
 * @brief Convert Claude image content block to OpenAI image_url format
 *
 * Claude format: {"type": "image", "source": {"type": "base64", "media_type": "...", "data":
 * "..."}} OpenAI format: {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}}
 *
 * @param block Content block to convert (or copy if not Claude image)
 * @return New json_object in OpenAI format (caller must json_object_put), or NULL on error
 */
static struct json_object *convert_content_block_to_openai(struct json_object *block) {
   if (!block) {
      return NULL;
   }

   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      // No type field - just copy the block
      return json_object_get(block);
   }

   const char *block_type = json_object_get_string(type_obj);
   if (!block_type || strcmp(block_type, "image") != 0) {
      // Not Claude image format - just copy the block
      return json_object_get(block);
   }

   // This is a Claude image block - convert to OpenAI format
   struct json_object *source_obj;
   if (!json_object_object_get_ex(block, "source", &source_obj)) {
      LOG_WARNING("OpenAI: Claude image block missing source field");
      return NULL;
   }

   struct json_object *media_type_obj, *data_obj;
   const char *media_type = "image/jpeg";  // default
   const char *data = NULL;

   if (json_object_object_get_ex(source_obj, "media_type", &media_type_obj)) {
      media_type = json_object_get_string(media_type_obj);
   }
   if (json_object_object_get_ex(source_obj, "data", &data_obj)) {
      data = json_object_get_string(data_obj);
   }

   if (!data) {
      LOG_WARNING("OpenAI: Claude image block missing data");
      return NULL;
   }

   // Build data URL
   size_t url_len = strlen("data:") + strlen(media_type) + strlen(";base64,") + strlen(data) + 1;
   char *data_url = malloc(url_len);
   if (!data_url) {
      return NULL;
   }
   snprintf(data_url, url_len, "data:%s;base64,%s", media_type, data);

   // Create OpenAI-format image_url block
   struct json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

   struct json_object *url_obj = json_object_new_object();
   json_object_object_add(url_obj, "url", json_object_new_string(data_url));
   json_object_object_add(image_obj, "image_url", url_obj);

   free(data_url);

   LOG_INFO("OpenAI: Converted Claude image to OpenAI image_url (media_type=%s)", media_type);
   return image_obj;
}

/**
 * @brief Check if a content block is a vision/image block (any format)
 */
static bool is_vision_content_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   // OpenAI format: "image_url", Claude format: "image"
   return (strcmp(type_str, "image_url") == 0 || strcmp(type_str, "image") == 0);
}

/**
 * @brief Convert conversation history, summarizing Claude-format tool messages
 *
 * Creates a new array with Claude tool messages converted to text summaries.
 * Caller must free with json_object_put().
 *
 * @param history Original conversation history
 * @return Converted conversation history (new object, caller frees)
 */

/**
 * @brief Check if a content block is a Claude-format image that needs conversion
 */
static bool is_claude_image_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   return (strcmp(type_str, "image") == 0);
}

/**
 * @brief Filter orphaned tool messages from conversation history.
 *
 * When conversations are restored from the database, tool messages lose their
 * tool_call_id and assistant messages lose their tool_calls array. OpenAI API
 * requires tool messages to reference a preceding tool_calls, so orphaned
 * messages cause HTTP 400 errors.
 *
 * This function:
 * - Removes tool messages without tool_call_id
 * - Removes assistant messages with null content and no tool_calls
 * - Converts orphaned tool content to assistant summaries to preserve context
 *
 * @param history The conversation history JSON array
 * @return Filtered history (new object with incremented refcount, caller must put)
 */
static struct json_object *filter_orphaned_tool_messages(struct json_object *history) {
   int len = json_object_array_length(history);
   bool needs_filtering = false;

   // First pass: check if filtering is needed
   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         // Check if tool message has tool_call_id
         struct json_object *tool_call_id_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            needs_filtering = true;
            break;
         }
      } else if (strcmp(role, "assistant") == 0) {
         // Check for assistant message with null content and no tool_calls
         struct json_object *content_obj, *tool_calls_obj;
         json_object_object_get_ex(msg, "content", &content_obj);
         bool has_tool_calls = json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj);

         if (!has_tool_calls &&
             (content_obj == NULL || json_object_get_string(content_obj) == NULL ||
              strlen(json_object_get_string(content_obj)) == 0)) {
            needs_filtering = true;
            break;
         }
      }
   }

   if (!needs_filtering) {
      return json_object_get(history);
   }

   LOG_WARNING("OpenAI: Filtering orphaned tool messages from restored conversation");

   struct json_object *filtered = json_object_new_array();
   int orphan_count = 0;

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj)) {
         json_object_array_add(filtered, json_object_get(msg));
         continue;
      }
      const char *role = json_object_get_string(role_obj);

      if (strcmp(role, "tool") == 0) {
         struct json_object *tool_call_id_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            // Orphaned tool message - convert to assistant summary
            struct json_object *content_obj;
            if (json_object_object_get_ex(msg, "content", &content_obj)) {
               const char *content = json_object_get_string(content_obj);
               if (content && strlen(content) > 0) {
                  // Create assistant message with tool result summary
                  struct json_object *summary_msg = json_object_new_object();
                  json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));

                  char summary[2048];
                  snprintf(summary, sizeof(summary), "[Previous tool result: %.1900s%s]", content,
                           strlen(content) > 1900 ? "..." : "");
                  json_object_object_add(summary_msg, "content", json_object_new_string(summary));
                  json_object_array_add(filtered, summary_msg);
               }
            }
            orphan_count++;
            continue;
         }
         // Valid tool message - keep it
         json_object_array_add(filtered, json_object_get(msg));
      } else if (strcmp(role, "assistant") == 0) {
         struct json_object *content_obj, *tool_calls_obj;
         json_object_object_get_ex(msg, "content", &content_obj);
         bool has_tool_calls = json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj);

         // Skip assistant messages with no content and no tool_calls (empty placeholders)
         if (!has_tool_calls &&
             (content_obj == NULL || json_object_get_string(content_obj) == NULL ||
              strlen(json_object_get_string(content_obj)) == 0)) {
            orphan_count++;
            continue;
         }
         json_object_array_add(filtered, json_object_get(msg));
      } else {
         // Keep all other messages (system, user)
         json_object_array_add(filtered, json_object_get(msg));
      }
   }

   if (orphan_count > 0) {
      LOG_INFO("OpenAI: Filtered %d orphaned tool-related messages", orphan_count);
   }

   return filtered;
}

static struct json_object *convert_claude_tool_messages(struct json_object *history) {
   int len = json_object_array_length(history);

   // Early-exit: check if any messages need conversion
   // This avoids creating new objects when no Claude-specific content is present
   bool needs_conversion = false;
   for (int i = 0; i < len && !needs_conversion; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         // Check if array contains tool_use, tool_result, or Claude image blocks
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "tool_use") == 0 || strcmp(type_str, "tool_result") == 0 ||
                   strcmp(type_str, "image") == 0) {
                  needs_conversion = true;
                  break;
               }
            }
         }
      }
   }

   // If no conversion needed, just increment refcount and return original
   if (!needs_conversion) {
      return json_object_get(history);
   }

   struct json_object *converted = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      char summary[4096];

      // Try to convert Claude tool message to summary
      if (convert_claude_tool_to_summary(msg, summary, sizeof(summary))) {
         // Create new message with summary text
         struct json_object *new_msg = json_object_new_object();
         struct json_object *role_obj;

         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            json_object_object_add(new_msg, "role", json_object_get(role_obj));
         }
         json_object_object_add(new_msg, "content", json_object_new_string(summary));
         json_object_array_add(converted, new_msg);
      } else {
         // Check if message has array content that may contain Claude images
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj) &&
             json_object_get_type(content_obj) == json_type_array) {
            // Check if any blocks need conversion
            int arr_len = json_object_array_length(content_obj);
            bool has_claude_images = false;
            for (int j = 0; j < arr_len && !has_claude_images; j++) {
               struct json_object *elem = json_object_array_get_idx(content_obj, j);
               if (is_claude_image_block(elem)) {
                  has_claude_images = true;
               }
            }

            if (has_claude_images) {
               // Create new message with converted content
               struct json_object *new_msg = json_object_new_object();
               struct json_object *role_obj;
               if (json_object_object_get_ex(msg, "role", &role_obj)) {
                  json_object_object_add(new_msg, "role", json_object_get(role_obj));
               }

               // Convert content array
               struct json_object *new_content = json_object_new_array();
               for (int j = 0; j < arr_len; j++) {
                  struct json_object *elem = json_object_array_get_idx(content_obj, j);
                  struct json_object *converted_elem = convert_content_block_to_openai(elem);
                  if (converted_elem) {
                     json_object_array_add(new_content, converted_elem);
                  }
               }
               json_object_object_add(new_msg, "content", new_content);
               json_object_array_add(converted, new_msg);
            } else {
               // No Claude images, copy as-is
               json_object_array_add(converted, json_object_get(msg));
            }
         } else {
            // Not array content, copy as-is
            json_object_array_add(converted, json_object_get(msg));
         }
      }
   }

   return converted;
}

/**
 * @brief Strip vision (image_url) content from conversation history
 *
 * When switching from a vision-capable LLM to one without vision support,
 * the history may contain image_url entries that would cause errors.
 * This function replaces image content with text placeholders.
 *
 * @param history Conversation history (already converted)
 * @return Sanitized history with vision content removed (new object, caller frees)
 */
static struct json_object *strip_vision_content(struct json_object *history) {
   int len = json_object_array_length(history);

   /* Check if any messages have image content (OpenAI or Claude format) */
   bool has_vision = false;
   for (int i = 0; i < len && !has_vision; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            if (is_vision_content_block(elem)) {
               has_vision = true;
               break;
            }
         }
      }
   }

   /* If no vision content, return original with incremented refcount */
   if (!has_vision) {
      return json_object_get(history);
   }

   LOG_INFO("Stripping vision content from history (target LLM doesn't support vision)");

   struct json_object *sanitized = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;

      /* Check if this message has array content with images */
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         /* Extract text parts, skip image parts (both OpenAI and Claude formats) */
         int arr_len = json_object_array_length(content_obj);
         char text_buffer[8192] = "";
         size_t text_len = 0;
         bool found_image = false;

         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "text") == 0) {
                  struct json_object *text_obj;
                  if (json_object_object_get_ex(elem, "text", &text_obj)) {
                     const char *text = json_object_get_string(text_obj);
                     if (text && text_len < sizeof(text_buffer) - 1) {
                        if (text_len > 0) {
                           text_buffer[text_len++] = ' ';
                        }
                        size_t copy_len = strlen(text);
                        if (text_len + copy_len >= sizeof(text_buffer)) {
                           copy_len = sizeof(text_buffer) - text_len - 1;
                        }
                        memcpy(text_buffer + text_len, text, copy_len);
                        text_len += copy_len;
                        text_buffer[text_len] = '\0';
                     }
                  }
               } else if (is_vision_content_block(elem)) {
                  found_image = true;
               }
            }
         }

         /* Create new message with text content only */
         struct json_object *new_msg = json_object_new_object();
         struct json_object *role_obj;
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            json_object_object_add(new_msg, "role", json_object_get(role_obj));
         }

         /* Add placeholder if image was removed */
         if (found_image) {
            if (text_len > 0) {
               char combined[8300];
               snprintf(combined, sizeof(combined), "%s [An image was shared earlier]",
                        text_buffer);
               json_object_object_add(new_msg, "content", json_object_new_string(combined));
            } else {
               json_object_object_add(new_msg, "content",
                                      json_object_new_string("[An image was shared here]"));
            }
         } else {
            json_object_object_add(new_msg, "content", json_object_new_string(text_buffer));
         }

         json_object_array_add(sanitized, new_msg);
      } else {
         /* Message doesn't have array content, copy as-is */
         json_object_array_add(sanitized, json_object_get(msg));
      }
   }

   return sanitized;
}

/**
 * @brief Build HTTP headers for OpenAI API request
 *
 * @param api_key API key (NULL for local LLM, required for cloud)
 * @return CURL header list (caller must free with curl_slist_free_all)
 */
static struct curl_slist *build_openai_headers(const char *api_key) {
   struct curl_slist *headers = NULL;

   headers = curl_slist_append(headers, "Content-Type: application/json");

   if (api_key != NULL) {
      // Cloud OpenAI - needs auth header
      char auth_header[512];
      snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
      headers = curl_slist_append(headers, auth_header);
   }
   // Local LLM doesn't need auth header

   return headers;
}

char *llm_openai_chat_completion(struct json_object *conversation_history,
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
   json_object *user_message = NULL;

   json_object *parsed_json = NULL;
   json_object *choices = NULL;
   json_object *first_choice = NULL;
   json_object *message = NULL;
   json_object *content = NULL;
   json_object *finish_reason = NULL;
   json_object *usage_obj = NULL;
   json_object *total_tokens_obj = NULL;

   // Filter orphaned tool messages (from restored conversations)
   json_object *filtered_history = filter_orphaned_tool_messages(conversation_history);

   // Convert Claude-format tool messages to text summaries
   json_object *converted_history = convert_claude_tool_messages(filtered_history);
   json_object_put(filtered_history);

   // Strip vision content if target LLM doesn't support vision
   if (!is_vision_enabled_for_current_llm()) {
      json_object *sanitized = strip_vision_content(converted_history);
      json_object_put(converted_history);
      converted_history = sanitized;
   }

   // Root JSON Container
   root = json_object_new_object();

   // Model: use passed model, or fall back to config default
   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }
   if (model_name && model_name[0] != '\0') {
      json_object_object_add(root, "model", json_object_new_string(model_name));
   }

   // User message is now added by dawn.c before calling this function
   // If vision is provided, modify the last user message to include images
   if (vision_images != NULL && vision_image_count > 0) {
      int msg_count = json_object_array_length(converted_history);
      if (msg_count > 0) {
         json_object *last_msg = json_object_array_get_idx(converted_history, msg_count - 1);
         json_object *role_obj;
         if (json_object_object_get_ex(last_msg, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0) {
            // Last message is user message - add vision content
            json_object *content_array = json_object_new_array();
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text", json_object_new_string(input_text));
            json_object_array_add(content_array, text_obj);

            // Add each image to the content array
            for (int i = 0; i < vision_image_count; i++) {
               if (!vision_images[i] || (vision_image_sizes && vision_image_sizes[i] == 0)) {
                  continue;
               }

               json_object *image_obj = json_object_new_object();
               json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

               json_object *image_url_obj = json_object_new_object();
               const char *data_uri_prefix = "data:image/jpeg;base64,";
               size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_images[i]) + 1;
               char *data_uri = malloc(data_uri_length);
               if (data_uri != NULL) {
                  snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_images[i]);
                  json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
                  free(data_uri);
               }
               json_object_object_add(image_obj, "image_url", image_url_obj);
               json_object_array_add(content_array, image_obj);
            }

            json_object_object_add(last_msg, "content", content_array);
         }
      }
   }

   // Use filtered history (already our own copy, no need to increment refcount)
   json_object_object_add(root, "messages", converted_history);

   // Max Tokens - cloud OpenAI uses max_completion_tokens, local llama.cpp uses max_tokens
   if (api_key == NULL) {
      // Local llama.cpp uses the older max_tokens parameter
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      // Cloud OpenAI (gpt-4o, o1, o3, etc.) uses max_completion_tokens
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   // Add tools if native tool calling is enabled
   if (llm_tools_enabled(NULL)) {
      bool is_remote = is_current_session_remote();
      struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         LOG_INFO("OpenAI: Added %d tools to request (%s session)",
                  llm_tools_get_enabled_count_filtered(is_remote), is_remote ? "remote" : "local");
      }
   }

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   LOG_INFO("OpenAI request payload: %zu bytes (~%zu tokens est)", strlen(payload),
            strlen(payload) / 4);

   curl_buffer_init(&chunk);

   // Check connection (handled in llm_interface.c for fallback)
   if (!llm_check_connection(base_url, 4)) {
      LOG_ERROR("URL did not return. Unavailable.");
      json_object_put(root);
      return NULL;
   }

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = build_openai_headers(api_key);

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);
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
            LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
         }
         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         curl_buffer_free(&chunk);
         json_object_put(root);
         return NULL;
      }

      // Check HTTP status code
      long http_code = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

      if (http_code != 200) {
         if (http_code == 401) {
            LOG_ERROR("OpenAI API: Invalid or missing API key (HTTP 401)");
         } else if (http_code == 403) {
            LOG_ERROR("OpenAI API: Access forbidden (HTTP 403) - check API key permissions");
         } else if (http_code == 429) {
            LOG_ERROR("OpenAI API: Rate limit exceeded (HTTP 429)");
         } else if (http_code >= 500) {
            LOG_ERROR("OpenAI API: Server error (HTTP %ld)", http_code);
         } else if (http_code != 0) {
            LOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
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
      LOG_ERROR("Failed to parse JSON response.");
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   if (!json_object_object_get_ex(parsed_json, "choices", &choices) ||
       json_object_get_type(choices) != json_type_array || json_object_array_length(choices) < 1) {
      LOG_ERROR("Error in parsing response: 'choices' missing or invalid.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   first_choice = json_object_array_get_idx(choices, 0);
   if (!first_choice) {
      LOG_ERROR("Error: 'choices' array is empty.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   if (!json_object_object_get_ex(first_choice, "message", &message) ||
       !json_object_object_get_ex(message, "content", &content)) {
      LOG_ERROR("Error: 'message' or 'content' field missing.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }

   // Optional: Safely access 'finish_reason'
   json_object_object_get_ex(first_choice, "finish_reason", &finish_reason);

   // Check for usage and cache information
   int input_tokens = 0;
   int output_tokens = 0;
   int cached_tokens = 0;
   if (json_object_object_get_ex(parsed_json, "usage", &usage_obj)) {
      if (json_object_object_get_ex(usage_obj, "total_tokens", &total_tokens_obj)) {
         total_tokens = json_object_get_int(total_tokens_obj);
         LOG_WARNING("Total tokens: %d", total_tokens);
      }

      // Extract input/output tokens
      json_object *prompt_tokens_obj = NULL;
      json_object *completion_tokens_obj = NULL;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens", &prompt_tokens_obj)) {
         input_tokens = json_object_get_int(prompt_tokens_obj);
      }
      if (json_object_object_get_ex(usage_obj, "completion_tokens", &completion_tokens_obj)) {
         output_tokens = json_object_get_int(completion_tokens_obj);
      }

      // Log OpenAI automatic caching info (if available)
      json_object *prompt_tokens_details = NULL;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens_details", &prompt_tokens_details)) {
         json_object *cached_tokens_obj = NULL;
         if (json_object_object_get_ex(prompt_tokens_details, "cached_tokens",
                                       &cached_tokens_obj)) {
            cached_tokens = json_object_get_int(cached_tokens_obj);
            if (cached_tokens > 0) {
               LOG_INFO("OpenAI cache hit: %d tokens cached (50%% savings)", cached_tokens);
            }
         }
      }

      // Record metrics - determine type based on whether we have an API key
      llm_type_t token_type = (api_key != NULL) ? LLM_CLOUD : LLM_LOCAL;
      metrics_record_llm_tokens(token_type, CLOUD_PROVIDER_OPENAI, input_tokens, output_tokens,
                                cached_tokens);

      // Update context usage tracking with actual session ID
      session_t *session = session_get_command_context();
      uint32_t session_id = session ? session->session_id : 0;
      llm_context_update_usage(session_id, input_tokens, output_tokens, cached_tokens);
   }

   // Duplicate the response content string safely
   const char *content_str = json_object_get_string(content);
   if (!content_str) {
      // Content is null - check if this is a tool call response
      json_object *tool_calls = NULL;
      if (json_object_object_get_ex(message, "tool_calls", &tool_calls) &&
          json_object_get_type(tool_calls) == json_type_array &&
          json_object_array_length(tool_calls) > 0) {
         LOG_WARNING("OpenAI: LLM returned tool call instead of content (non-streaming API "
                     "doesn't support tool execution)");
         // Return a fallback message instead of failing
         response = strdup("I apologize, but I was unable to complete that request directly. "
                           "Please try rephrasing your question.");
      } else {
         LOG_ERROR("Error: 'content' field is empty or null with no tool calls.");
         json_object_put(parsed_json);
         curl_buffer_free(&chunk);
         json_object_put(root);
         return NULL;
      }
   } else {
      response = strdup(content_str);
   }

   if ((finish_reason != NULL) && (strcmp(json_object_get_string(finish_reason), "stop") != 0)) {
      LOG_WARNING("OpenAI returned with finish_reason: %s", json_object_get_string(finish_reason));
   } else {
      LOG_INFO("Response finished properly.");
   }

   json_object_put(parsed_json);
   curl_buffer_free(&chunk);
   json_object_put(root);

   return response;
}

/**
 * @brief Context for streaming callbacks
 */
typedef struct {
   sse_parser_t *sse_parser;
   llm_stream_context_t *stream_ctx;
   char *raw_buffer;    /* Captures raw response for error debugging */
   size_t raw_size;     /* Current size of raw buffer */
   size_t raw_capacity; /* Allocated capacity */
} openai_streaming_context_t;

/**
 * @brief CURL write callback for streaming responses
 *
 * Feeds incoming SSE data to the SSE parser, which processes it
 * and calls the LLM streaming callbacks. Also captures raw data
 * for error debugging.
 */
static size_t streaming_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
   size_t realsize = size * nmemb;
   openai_streaming_context_t *ctx = (openai_streaming_context_t *)userp;

   // Also capture raw data for error debugging (limit to first 4KB)
   if (ctx->raw_buffer && ctx->raw_size < ctx->raw_capacity - 1) {
      size_t space_left = ctx->raw_capacity - ctx->raw_size - 1;
      size_t to_copy = realsize < space_left ? realsize : space_left;
      memcpy(ctx->raw_buffer + ctx->raw_size, contents, to_copy);
      ctx->raw_size += to_copy;
      ctx->raw_buffer[ctx->raw_size] = '\0';
   }

   // Feed data to SSE parser
   sse_parser_feed(ctx->sse_parser, contents, realsize);

   return realsize;
}

/**
 * @brief SSE event callback that forwards events to LLM stream handler
 */
static void openai_sse_event_handler(const char *event_type,
                                     const char *event_data,
                                     void *userdata) {
   openai_streaming_context_t *ctx = (openai_streaming_context_t *)userdata;

   // Forward event data to LLM streaming parser
   llm_stream_handle_event(ctx->stream_ctx, event_data);
}

/* Maximum tool call iterations to prevent infinite loops */
#define MAX_TOOL_ITERATIONS 5

/* Maximum messages to check for duplicate tool calls (performance optimization) */
#define DUPLICATE_CHECK_LOOKBACK 10

/**
 * @brief Check if a tool call is a duplicate of a previous call in conversation history
 *
 * Prevents infinite loops where the LLM keeps making the same tool call repeatedly.
 * Compares tool name and arguments against previous tool calls in the history.
 * Only checks the last DUPLICATE_CHECK_LOOKBACK messages for efficiency.
 *
 * @param history Conversation history (JSON array)
 * @param tool_name Name of the tool being called
 * @param tool_args Arguments for the tool call
 * @return true if this exact call was already made, false otherwise
 */
static bool is_duplicate_tool_call(struct json_object *history,
                                   const char *tool_name,
                                   const char *tool_args) {
   if (!history || !tool_name)
      return false;

   int len = json_object_array_length(history);

   /* Limit lookback for efficiency - duplicate loops happen within a few iterations */
   int min_idx = len - DUPLICATE_CHECK_LOOKBACK;
   if (min_idx < 0) {
      min_idx = 0;
   }

   // Search backwards through recent history for assistant messages with tool_calls
   for (int i = len - 1; i >= min_idx; i--) {
      json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (!role || strcmp(role, "assistant") != 0)
         continue;

      // Check for tool_calls array
      json_object *tool_calls;
      if (!json_object_object_get_ex(msg, "tool_calls", &tool_calls))
         continue;
      if (!json_object_is_type(tool_calls, json_type_array))
         continue;

      // Compare each tool call in this message
      int tc_len = json_object_array_length(tool_calls);
      for (int j = 0; j < tc_len; j++) {
         json_object *tc = json_object_array_get_idx(tool_calls, j);
         if (!tc)
            continue;

         json_object *func;
         if (!json_object_object_get_ex(tc, "function", &func))
            continue;

         json_object *name_obj, *args_obj;
         if (!json_object_object_get_ex(func, "name", &name_obj))
            continue;

         const char *prev_name = json_object_get_string(name_obj);
         if (!prev_name || strcmp(prev_name, tool_name) != 0)
            continue;

         // Name matches - check arguments
         if (json_object_object_get_ex(func, "arguments", &args_obj)) {
            const char *prev_args = json_object_get_string(args_obj);
            // Compare arguments (both NULL/empty counts as match)
            bool args_match = false;
            if ((!prev_args || prev_args[0] == '\0') && (!tool_args || tool_args[0] == '\0')) {
               args_match = true;
            } else if (prev_args && tool_args && strcmp(prev_args, tool_args) == 0) {
               args_match = true;
            }

            if (args_match) {
               LOG_INFO("Duplicate tool call detected: %s with args %s", tool_name,
                        tool_args ? tool_args : "(none)");
               return true;
            }
         }
      }
   }

   return false;
}

/**
 * @brief Extract error message from OpenAI/compatible API error response
 *
 * Parses JSON like: {"error": {"message": "...", "code": "..."}}
 * Returns a formatted error message or a default message if parsing fails.
 * The returned string is thread-local and should not be freed.
 */
static const char *parse_api_error_message(const char *response_body, long http_code) {
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

   // SSE and streaming contexts
   sse_parser_t *sse_parser = NULL;
   llm_stream_context_t *stream_ctx = NULL;
   openai_streaming_context_t streaming_ctx;

   // Filter orphaned tool messages (from restored conversations)
   json_object *filtered_history = filter_orphaned_tool_messages(conversation_history);

   // Convert Claude-format tool messages to text summaries
   json_object *converted_history = convert_claude_tool_messages(filtered_history);
   json_object_put(filtered_history);

   // Strip vision content if target LLM doesn't support vision
   if (!is_vision_enabled_for_current_llm()) {
      json_object *sanitized = strip_vision_content(converted_history);
      json_object_put(converted_history);
      converted_history = sanitized;
   }

   // Root JSON Container
   root = json_object_new_object();

   // Model: use passed model, or fall back to config default
   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = (api_key == NULL) ? g_config.llm.local.model : llm_get_default_openai_model();
   }
   if (model_name && model_name[0] != '\0') {
      json_object_object_add(root, "model", json_object_new_string(model_name));
   }

   // Enable streaming with usage reporting
   json_object_object_add(root, "stream", json_object_new_boolean(1));
   json_object *stream_opts = json_object_new_object();
   json_object_object_add(stream_opts, "include_usage", json_object_new_boolean(1));
   json_object_object_add(root, "stream_options", stream_opts);

   // For local LLM (llama.cpp), request per-chunk timing metrics
   // This enables real-time tokens/sec display during streaming
   if (api_key == NULL) {
      json_object_object_add(root, "timings_per_token", json_object_new_boolean(1));

      // Add extended thinking for local LLM (llama.cpp with DeepSeek-R1, QwQ, etc.)
      // Skip for internal utility calls (search summarization, context compaction)
      // Use session-specific thinking mode if available, otherwise global config
      const char *thinking_mode = llm_get_current_thinking_mode();
      if (strcmp(thinking_mode, "disabled") != 0 && !llm_tools_suppressed()) {
         json_object *thinking = json_object_new_object();
         json_object_object_add(thinking, "type", json_object_new_string("enabled"));

         int budget = llm_get_effective_budget_tokens();
         json_object_object_add(thinking, "budget_tokens", json_object_new_int(budget));

         json_object_object_add(root, "thinking", thinking);

         // Force reasoning output even when tools are present (llama.cpp specific)
         // Without this, models may skip reasoning and go straight to tool selection
         json_object_object_add(root, "thinking_forced_open", json_object_new_boolean(1));

         // Pass enable_thinking=true to the chat template (Qwen3 requirement)
         // This sets the template variable that controls thinking behavior
         json_object *template_kwargs = json_object_new_object();
         json_object_object_add(template_kwargs, "enable_thinking", json_object_new_boolean(1));
         json_object_object_add(root, "chat_template_kwargs", template_kwargs);

         LOG_INFO("Local LLM: Extended thinking enabled (budget: %d tokens, forced_open: true, "
                  "chat_template_kwargs.enable_thinking: true)",
                  budget);
      } else if (strcmp(thinking_mode, "disabled") == 0) {
         // Explicitly disable reasoning for llama.cpp reasoning models.
         // NOTE: reasoning_budget is a llama.cpp-specific parameter (not OpenAI API).
         // Setting to 0 disables thinking output for models like DeepSeek-R1.
         json_object_object_add(root, "reasoning_budget", json_object_new_int(0));

         // Pass enable_thinking=false to the chat template (Qwen3 requirement)
         json_object *template_kwargs = json_object_new_object();
         json_object_object_add(template_kwargs, "enable_thinking", json_object_new_boolean(0));
         json_object_object_add(root, "chat_template_kwargs", template_kwargs);

         LOG_INFO("Local LLM: Reasoning explicitly disabled (reasoning_budget: 0)");
      }
   } else {
      // For cloud OpenAI reasoning models, add reasoning_effort parameter
      // Supported models: o1, o3 series and GPT-5 family
      // Skip for internal utility calls (search summarization, context compaction)
      // Note: use model_name (set from session override or config) not g_config directly
      const char *thinking_mode = llm_get_current_thinking_mode();

      // Check if model supports reasoning_effort
      // OpenAI: o1, o3 series and GPT-5 family
      // Gemini: 2.5+ and 3.x models (via OpenAI-compatible API)
      bool is_o_series = (strncmp(model_name, "o1", 2) == 0 || strncmp(model_name, "o3", 2) == 0);
      bool is_gpt5 = (strncmp(model_name, "gpt-5", 5) == 0);
      bool is_gemini_thinking = (strncmp(model_name, "gemini-2.5", 10) == 0 ||
                                 strncmp(model_name, "gemini-3", 8) == 0);
      bool supports_reasoning = is_o_series || is_gpt5 || is_gemini_thinking;

      if (supports_reasoning && !llm_tools_suppressed()) {
         const char *effort = NULL;

         if (strcmp(thinking_mode, "disabled") == 0) {
            // GPT-5 family: disable/minimize reasoning
            if (is_gpt5) {
               // GPT-5.2 supports "none", others use "minimal" for lowest reasoning
               if (strncmp(model_name, "gpt-5.2", 7) == 0) {
                  effort = "none";
               } else {
                  effort = "minimal";
               }
            }
            // Gemini 2.5+/3.x: reasoning cannot be fully disabled, use "low" as minimum
            else if (is_gemini_thinking) {
               effort = "low";
            }
            // For o-series with disabled, don't send the parameter (they always reason)
         } else {
            // Session thinking_mode may be low/medium/high for reasoning models
            effort = thinking_mode;
            // Map enabled/auto to config default or medium
            if (strcmp(effort, "enabled") == 0 || strcmp(effort, "auto") == 0) {
               effort = g_config.llm.thinking.reasoning_effort;
               if (effort[0] == '\0')
                  effort = "medium";
            }
         }

         if (effort != NULL) {
            // Both OpenAI and Gemini (via OpenAI-compatible API) use reasoning_effort
            // Note: Gemini's OpenAI-compatible endpoint doesn't support google.thinking_config
            // extension, so we can't get visible thinking content. The model still does
            // internal reasoning based on reasoning_effort level.
            json_object_object_add(root, "reasoning_effort", json_object_new_string(effort));
            LOG_INFO("Cloud LLM: Reasoning effort set to '%s' for model %s", effort, model_name);
         }
      }
   }

   // Handle vision if provided
   if (vision_images != NULL && vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < vision_image_count; i++) {
         if (vision_image_sizes && vision_images[i]) {
            total_bytes += vision_image_sizes[i];
         }
      }
      LOG_INFO("OpenAI streaming: %d vision images provided (%zu total bytes)", vision_image_count,
               total_bytes);
      int msg_count = json_object_array_length(converted_history);
      if (msg_count > 0) {
         json_object *last_msg = json_object_array_get_idx(converted_history, msg_count - 1);
         json_object *role_obj;
         if (json_object_object_get_ex(last_msg, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0) {
            LOG_INFO("OpenAI streaming: Adding %d images to last user message", vision_image_count);
            // Last message is user message - add vision content
            json_object *content_array = json_object_new_array();
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text", json_object_new_string(input_text));
            json_object_array_add(content_array, text_obj);

            // Add each image
            for (int i = 0; i < vision_image_count; i++) {
               if (!vision_images[i] || (vision_image_sizes && vision_image_sizes[i] == 0)) {
                  continue;
               }
               json_object *image_obj = json_object_new_object();
               json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

               json_object *image_url_obj = json_object_new_object();
               const char *data_uri_prefix = "data:image/jpeg;base64,";
               size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_images[i]) + 1;
               char *data_uri = malloc(data_uri_length);
               if (data_uri != NULL) {
                  snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_images[i]);
                  json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
                  free(data_uri);
               }
               json_object_object_add(image_obj, "image_url", image_url_obj);
               json_object_array_add(content_array, image_obj);
            }

            json_object_object_add(last_msg, "content", content_array);
         } else {
            /* Last message is not user (e.g., tool result after viewing command).
             * Create a new user message with the vision images. */
            LOG_INFO("OpenAI streaming: Adding vision as new user message (last was role=%s)",
                     role_obj ? json_object_get_string(role_obj) : "null");

            json_object *new_user_msg = json_object_new_object();
            json_object_object_add(new_user_msg, "role", json_object_new_string("user"));

            json_object *content_array = json_object_new_array();

            /* Add text instruction */
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text",
                                   json_object_new_string("Here are the captured images. "
                                                          "Please respond to the user's request."));
            json_object_array_add(content_array, text_obj);

            /* Add each image */
            for (int i = 0; i < vision_image_count; i++) {
               if (!vision_images[i] || (vision_image_sizes && vision_image_sizes[i] == 0)) {
                  continue;
               }
               json_object *image_obj = json_object_new_object();
               json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

               json_object *image_url_obj = json_object_new_object();
               const char *data_uri_prefix = "data:image/jpeg;base64,";
               size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_images[i]) + 1;
               char *data_uri = malloc(data_uri_length);
               if (data_uri != NULL) {
                  snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_images[i]);
                  json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
                  free(data_uri);
               }
               json_object_object_add(image_obj, "image_url", image_url_obj);
               json_object_array_add(content_array, image_obj);
            }

            json_object_object_add(new_user_msg, "content", content_array);

            /* Append the new user message to history */
            json_object_array_add(converted_history, new_user_msg);
            LOG_INFO("OpenAI streaming: Vision user message added to history");
         }
      }
   }

   // Use filtered history (already our own copy, no need to increment refcount)
   json_object_object_add(root, "messages", converted_history);

   // Max Tokens - cloud OpenAI uses max_completion_tokens, local llama.cpp uses max_tokens
   if (api_key == NULL) {
      // Local llama.cpp uses the older max_tokens parameter
      json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));
   } else {
      // Cloud OpenAI (gpt-4o, o1, o3, etc.) uses max_completion_tokens
      json_object_object_add(root, "max_completion_tokens",
                             json_object_new_int(g_config.llm.max_tokens));
   }

   // Add tools if native tool calling is enabled
   // Skip tools at max iterations (used by duplicate detection to force text response)
   if (llm_tools_enabled(NULL) && iteration < MAX_TOOL_ITERATIONS) {
      bool is_remote = is_current_session_remote();
      struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote);
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         LOG_INFO("OpenAI streaming: Added %d tools to request (%s session)",
                  llm_tools_get_enabled_count_filtered(is_remote), is_remote ? "remote" : "local");
      }
   } else if (iteration >= MAX_TOOL_ITERATIONS) {
      LOG_INFO("OpenAI streaming: Skipping tools (forcing text response at iteration %d)",
               iteration);
   }

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   LOG_INFO("OpenAI streaming request payload: %zu bytes (~%zu tokens est)", strlen(payload),
            strlen(payload) / 4);

   // Log request details for debugging (use actual model_name that was added to JSON)
   LOG_INFO("OpenAI streaming iter %d: url=%s model=%s api_key=%s", iteration, base_url,
            (model_name && model_name[0]) ? model_name : "(server default)",
            LOG_CREDENTIAL_STATUS(api_key));

   // Debug: Log conversation state on follow-up iterations
   if (iteration > 0) {
      int msg_count = json_object_array_length(converted_history);
      LOG_INFO("OpenAI streaming iter %d: %d messages in history", iteration, msg_count);
      // Log last 3 messages (roles and content types)
      for (int i = msg_count > 3 ? msg_count - 3 : 0; i < msg_count; i++) {
         json_object *msg = json_object_array_get_idx(converted_history, i);
         json_object *role_obj, *content_obj;
         const char *role = "unknown";
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            role = json_object_get_string(role_obj);
         }
         if (json_object_object_get_ex(msg, "content", &content_obj)) {
            const char *content = json_object_get_string(content_obj);
            LOG_INFO("  [%d] role=%s content=%.100s%s", i, role, content ? content : "(null)",
                     (content && strlen(content) > 100) ? "..." : "");
         } else {
            // Check for tool_calls (assistant message)
            json_object *tc_obj;
            if (json_object_object_get_ex(msg, "tool_calls", &tc_obj)) {
               LOG_INFO("  [%d] role=%s (has tool_calls)", i, role);
            } else {
               LOG_INFO("  [%d] role=%s (no content)", i, role);
            }
         }
      }
   }

   // Check connection
   if (!llm_check_connection(base_url, 4)) {
      LOG_ERROR("URL did not return. Unavailable.");
      json_object_put(root);
      return NULL;
   }

   // Create streaming context
   // Determine LLM type based on whether we have an API key (cloud) or not (local)
   llm_type_t stream_llm_type = (api_key != NULL) ? LLM_CLOUD : LLM_LOCAL;

   // Get cloud provider from session context (preferred) or fall back to URL detection
   cloud_provider_t stream_provider = CLOUD_PROVIDER_OPENAI;
   session_t *ctx_session = session_get_command_context();
   if (ctx_session && stream_llm_type == LLM_CLOUD) {
      session_llm_config_t session_config;
      session_get_llm_config(ctx_session, &session_config);
      stream_provider = session_config.cloud_provider;
   } else if (base_url && strstr(base_url, "generativelanguage.googleapis.com")) {
      // Fallback: detect from URL for cases without session context
      stream_provider = CLOUD_PROVIDER_GEMINI;
   }
   stream_ctx = llm_stream_create(stream_llm_type, stream_provider, chunk_callback,
                                  callback_userdata);
   if (!stream_ctx) {
      LOG_ERROR("Failed to create LLM stream context");
      json_object_put(root);
      return NULL;
   }

   // Create SSE parser
   sse_parser = sse_parser_create(openai_sse_event_handler, &streaming_ctx);
   if (!sse_parser) {
      LOG_ERROR("Failed to create SSE parser");
      llm_stream_free(stream_ctx);
      json_object_put(root);
      return NULL;
   }

   // Setup streaming context
   streaming_ctx.sse_parser = sse_parser;
   streaming_ctx.stream_ctx = stream_ctx;
   streaming_ctx.raw_capacity = 4096; /* 4KB for error debugging */
   streaming_ctx.raw_buffer = malloc(streaming_ctx.raw_capacity);
   streaming_ctx.raw_size = 0;
   if (streaming_ctx.raw_buffer) {
      streaming_ctx.raw_buffer[0] = '\0';
   }

   curl_handle = curl_easy_init();
   if (curl_handle) {
      headers = build_openai_headers(api_key);

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_CHAT_ENDPOINT);

      curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, streaming_write_callback);
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

      // No hard timeout for streaming - rely on low-speed detection instead.
      // The llm_timeout_ms config is only used for non-streaming requests.

      res = curl_easy_perform(curl_handle);
      if (res != CURLE_OK) {
         const char *error_code = "LLM_ERROR";
         const char *error_msg = NULL;

         if (res == CURLE_ABORTED_BY_CALLBACK) {
            LOG_INFO("LLM transfer interrupted by user");
            /* User cancellation - don't send as error */
         } else if (res == CURLE_OPERATION_TIMEDOUT) {
            LOG_ERROR("LLM stream timed out (no data for 60 seconds)");
            error_code = "LLM_TIMEOUT";
            error_msg = "Request timed out - AI server may be overloaded";
         } else {
            LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
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
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         free(streaming_ctx.raw_buffer);
         return NULL;
      }

      // Check HTTP status code
      long http_code = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

      if (http_code != 200) {
         /* Determine error code based on HTTP status */
         const char *error_code;
         if (http_code == 401) {
            LOG_ERROR("OpenAI API: Invalid or missing API key (HTTP 401)");
            error_code = "LLM_AUTH_ERROR";
         } else if (http_code == 403) {
            LOG_ERROR("OpenAI API: Access forbidden (HTTP 403) - check API key permissions");
            error_code = "LLM_ACCESS_ERROR";
         } else if (http_code == 429) {
            LOG_ERROR("OpenAI API: Rate limit exceeded (HTTP 429)");
            error_code = "LLM_RATE_LIMIT";
         } else if (http_code >= 500) {
            LOG_ERROR("OpenAI API: Server error (HTTP %ld)", http_code);
            error_code = "LLM_SERVER_ERROR";
         } else {
            LOG_ERROR("OpenAI API: Request failed (HTTP %ld)", http_code);
            error_code = "LLM_ERROR";
         }

         /* Log raw response body for debugging */
         if (streaming_ctx.raw_buffer && streaming_ctx.raw_size > 0) {
            LOG_ERROR("OpenAI API error response: %s", streaming_ctx.raw_buffer);
         }

#ifdef ENABLE_WEBUI
         /* Send error to WebUI client if connected */
         session_t *session = session_get_command_context();
         if (session && session->type == SESSION_TYPE_WEBUI) {
            const char *error_msg = parse_api_error_message(streaming_ctx.raw_buffer, http_code);
            webui_send_error(session, error_code, error_msg);
         }
#endif

         curl_easy_cleanup(curl_handle);
         curl_slist_free_all(headers);
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         free(streaming_ctx.raw_buffer);
         return NULL;
      }

      curl_easy_cleanup(curl_handle);
      curl_slist_free_all(headers);
   }

   // Check for tool calls
   if (llm_stream_has_tool_calls(stream_ctx)) {
      const tool_call_list_t *tool_calls = llm_stream_get_tool_calls(stream_ctx);
      if (tool_calls && tool_calls->count > 0) {
         LOG_INFO("OpenAI streaming: Executing %d tool call(s) at iteration %d", tool_calls->count,
                  iteration);

         // Debug: Log the tool calls OpenAI is requesting
         for (int i = 0; i < tool_calls->count; i++) {
            LOG_INFO("  Tool call [%d]: id=%s name=%s args=%s", i, tool_calls->calls[i].id,
                     tool_calls->calls[i].name, tool_calls->calls[i].arguments);
         }

         // Check for duplicate tool calls (prevents infinite loops with weak models)
         // Only check the first/primary tool call - if it's a duplicate, the LLM is stuck
         if (tool_calls->count > 0 &&
             is_duplicate_tool_call(conversation_history, tool_calls->calls[0].name,
                                    tool_calls->calls[0].arguments)) {
            LOG_WARNING("OpenAI streaming: Duplicate tool call detected, forcing text response");

            // Add a system hint to use existing results and make one more call without tools
            json_object *hint_msg = json_object_new_object();
            json_object_object_add(hint_msg, "role", json_object_new_string("user"));
            json_object_object_add(
                hint_msg, "content",
                json_object_new_string(
                    "[System: You already performed this search. Use the search results you "
                    "already have to answer the question. Do not search again.]"));
            json_object_array_add(conversation_history, hint_msg);

            // Cleanup current stream context
            sse_parser_free(sse_parser);
            llm_stream_free(stream_ctx);
            json_object_put(root);
            free(streaming_ctx.raw_buffer);

            // Make one more call with tools disabled to force a text response
            LOG_INFO("OpenAI streaming: Making final call without tools to force text response");

            // Get fresh config
            llm_resolved_config_t config;
            char model_buf[LLM_MODEL_NAME_MAX] = "";
            bool config_ok = (llm_get_current_resolved_config(&config) == 0);
            if (config_ok && config.model && config.model[0] != '\0') {
               strncpy(model_buf, config.model, sizeof(model_buf) - 1);
            }

            const char *fresh_url = config_ok ? config.endpoint : base_url;
            const char *fresh_key = config_ok ? config.api_key : api_key;
            const char *fresh_model = model_buf[0] ? model_buf : model;

            // Recursive call with MAX_TOOL_ITERATIONS to skip further tool processing
            return llm_openai_streaming_internal(conversation_history, "", NULL, NULL, 0, fresh_url,
                                                 fresh_key, fresh_model, chunk_callback,
                                                 callback_userdata, MAX_TOOL_ITERATIONS);
         }

         // Execute tools (heap-allocated: ~66KB, too large for 512KB satellite worker stack
         // with up to MAX_TOOL_ITERATIONS levels of recursion)
         tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
         if (!results) {
            LOG_ERROR("OpenAI streaming: Failed to allocate tool results");
            sse_parser_free(sse_parser);
            llm_stream_free(stream_ctx);
            json_object_put(root);
            free(streaming_ctx.raw_buffer);
            return NULL;
         }
         llm_tools_execute_all(tool_calls, results);

         // Add assistant message with tool_calls to conversation history
         json_object *assistant_msg = json_object_new_object();
         json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));
         // Use empty string instead of NULL for Gemini API compatibility
         json_object_object_add(assistant_msg, "content", json_object_new_string(""));

         // Add tool_calls array
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

            // Gemini 3+ models: Include thought_signature in first tool call
            // Required for follow-up requests when reasoning mode is enabled
            if (i == 0 && tool_calls->thought_signature[0] != '\0') {
               json_object *extra_content = json_object_new_object();
               json_object *google_obj = json_object_new_object();
               json_object_object_add(google_obj, "thought_signature",
                                      json_object_new_string(tool_calls->thought_signature));
               json_object_object_add(extra_content, "google", google_obj);
               json_object_object_add(tc, "extra_content", extra_content);
               LOG_INFO("OpenAI streaming: Including thought_signature in follow-up request");
            }

            json_object_array_add(tc_array, tc);
         }
         json_object_object_add(assistant_msg, "tool_calls", tc_array);
         json_object_array_add(conversation_history, assistant_msg);

         // Add tool results to conversation history
         llm_tools_add_results_openai(conversation_history, results);

         // Cleanup current stream context
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         free(streaming_ctx.raw_buffer);

         // Check if we should skip follow-up (e.g., LLM was switched)
         if (llm_tools_should_skip_followup(results)) {
            LOG_INFO("OpenAI streaming: Skipping follow-up call (tool requested no follow-up)");
            char *direct_response = llm_tools_get_direct_response(results);

            // Add synthetic assistant message to complete the tool call sequence
            // This prevents HTTP 400 on subsequent requests due to incomplete history
            if (direct_response) {
               json_object *closing_msg = json_object_new_object();
               json_object_object_add(closing_msg, "role", json_object_new_string("assistant"));
               json_object_object_add(closing_msg, "content",
                                      json_object_new_string(direct_response));
               json_object_array_add(conversation_history, closing_msg);
               LOG_INFO("OpenAI streaming: Added closing assistant message to complete history");
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
            LOG_WARNING("OpenAI streaming: Max tool iterations (%d) reached, returning without "
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

         // Make another call to get final response
         LOG_INFO("OpenAI streaming: Making follow-up call after tool execution (iteration %d/%d)",
                  iteration + 1, MAX_TOOL_ITERATIONS);

         // Debug: Log tool results that were added
         LOG_INFO("OpenAI streaming: Tool results added to history:");
         for (int i = 0; i < results->count; i++) {
            LOG_INFO("  [%d] id=%s result=%.200s%s", i, results->results[i].tool_call_id,
                     results->results[i].result,
                     strlen(results->results[i].result) > 200 ? "..." : "");
         }

         // Check for vision data in tool results (session-isolated)
         const char *result_vision = NULL;
         size_t result_vision_size = 0;
         for (int i = 0; i < results->count; i++) {
            if (results->results[i].vision_image && results->results[i].vision_image_size > 0) {
               result_vision = results->results[i].vision_image;
               result_vision_size = results->results[i].vision_image_size;
               LOG_INFO("OpenAI streaming: Including vision from tool result (%zu bytes)",
                        result_vision_size);
               break;
            }
         }

         // Check if provider changed (e.g., switch_llm was called)
         // Resolve config once and reuse for both provider check and credentials
         llm_resolved_config_t current_config;
         char *result = NULL;
         char model_buf_followup[LLM_MODEL_NAME_MAX] =
             "";  // Buffer for model (resolved ptr may dangle)
         bool config_valid = (llm_get_current_resolved_config(&current_config) == 0);

         // Copy model to local buffer immediately (current_config.model may be dangling pointer)
         if (config_valid && current_config.model && current_config.model[0] != '\0') {
            strncpy(model_buf_followup, current_config.model, sizeof(model_buf_followup) - 1);
            model_buf_followup[sizeof(model_buf_followup) - 1] = '\0';
         }

         if (config_valid && current_config.type != LLM_LOCAL &&
             current_config.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
            // Provider switched to Claude - hand off to Claude code path
            LOG_INFO("OpenAI streaming: Provider switched to Claude, handing off");

            // Convert conversation history from OpenAI to Claude format
            // Claude will handle the vision data if present
            // Use copied model buffer for Claude model
            const char *claude_model = model_buf_followup[0] ? model_buf_followup : NULL;
            // Pass tool result vision as a single-item array
            const char *result_vision_arr[1] = { result_vision };
            size_t result_vision_size_arr[1] = { result_vision_size };
            int result_vision_count = result_vision ? 1 : 0;
            result = llm_claude_chat_completion_streaming(
                conversation_history, "", result_vision_arr, result_vision_size_arr,
                result_vision_count, current_config.endpoint, current_config.api_key, claude_model,
                (llm_claude_text_chunk_callback)chunk_callback, callback_userdata);
         } else {
            // Still OpenAI-compatible (local or OpenAI cloud) - use resolved config or fallback
            const char *fresh_url = config_valid ? current_config.endpoint : base_url;
            const char *fresh_api_key = config_valid ? current_config.api_key : api_key;
            // Use copied model buffer, or fall back to original model param if not set
            const char *fresh_model = model_buf_followup[0] ? model_buf_followup : model;

            if (config_valid && fresh_url != base_url) {
               LOG_INFO("OpenAI streaming: Credentials refreshed to %s", fresh_url);
            }

            // Pass tool result vision as a single-item array
            const char *result_vision_arr[1] = { result_vision };
            size_t result_vision_size_arr[1] = { result_vision_size };
            int result_vision_count = result_vision ? 1 : 0;
            result = llm_openai_streaming_internal(conversation_history, "", result_vision_arr,
                                                   result_vision_size_arr, result_vision_count,
                                                   fresh_url, fresh_api_key, fresh_model,
                                                   chunk_callback, callback_userdata,
                                                   iteration + 1);
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

   // Send reasoning summary to WebUI if we had reasoning tokens (OpenAI o-series)
#ifdef ENABLE_WEBUI
   if (stream_ctx->reasoning_tokens > 0) {
      session_t *ws_session = session_get_command_context();
      if (ws_session && ws_session->type == SESSION_TYPE_WEBUI) {
         webui_send_reasoning_summary(ws_session, stream_ctx->reasoning_tokens);
      }
   }
#endif

   // Cleanup
   sse_parser_free(sse_parser);
   llm_stream_free(stream_ctx);
   json_object_put(root);
   free(streaming_ctx.raw_buffer);

   return response;
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
   return llm_openai_streaming_internal(conversation_history, input_text, vision_images,
                                        vision_image_sizes, vision_image_count, base_url, api_key,
                                        model, chunk_callback, callback_userdata, 0);
}
