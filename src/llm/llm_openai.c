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
#include "dawn.h"
#include "llm/llm_claude.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
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
                                 char *vision_image,
                                 size_t vision_image_size,
                                 const char *base_url,
                                 const char *api_key) {
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

   // Convert Claude-format tool messages to text summaries
   json_object *converted_history = convert_claude_tool_messages(conversation_history);

   // Strip vision content if target LLM doesn't support vision
   if (!is_vision_enabled_for_current_llm()) {
      json_object *sanitized = strip_vision_content(converted_history);
      json_object_put(converted_history);
      converted_history = sanitized;
   }

   // Root JSON Container
   root = json_object_new_object();

   // Model from config
   json_object_object_add(root, "model", json_object_new_string(g_config.llm.cloud.openai_model));

   // User message is now added by dawn.c before calling this function
   // If vision is provided, modify the last user message to include image
   if (vision_image != NULL && vision_image_size > 0) {
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

            json_object *image_obj = json_object_new_object();
            json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

            json_object *image_url_obj = json_object_new_object();
            char *data_uri_prefix = "data:image/jpeg;base64,";
            size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_image) + 1;
            char *data_uri = malloc(data_uri_length);
            if (data_uri != NULL) {
               snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_image);
               json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
               free(data_uri);
            }
            json_object_object_add(image_obj, "image_url", image_url_obj);
            json_object_array_add(content_array, image_obj);

            json_object_object_add(last_msg, "content", content_array);
         }
      }
   }

   // Use filtered history (already our own copy, no need to increment refcount)
   json_object_object_add(root, "messages", converted_history);

   // Max Tokens
   json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));

   // Add tools if native tool calling is enabled
   if (llm_tools_enabled(NULL)) {
      struct json_object *tools = llm_tools_get_openai_format();
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         LOG_INFO("OpenAI: Added %d tools to request", llm_tools_get_enabled_count());
      }
   }

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

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
   }

   // Duplicate the response content string safely
   const char *content_str = json_object_get_string(content);
   if (!content_str) {
      LOG_ERROR("Error: 'content' field is empty or not a string.");
      json_object_put(parsed_json);
      curl_buffer_free(&chunk);
      json_object_put(root);
      return NULL;
   }
   response = strdup(content_str);

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

/**
 * @brief Internal streaming implementation with iteration tracking
 */
static char *llm_openai_streaming_internal(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
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

   // Convert Claude-format tool messages to text summaries
   json_object *converted_history = convert_claude_tool_messages(conversation_history);

   // Strip vision content if target LLM doesn't support vision
   if (!is_vision_enabled_for_current_llm()) {
      json_object *sanitized = strip_vision_content(converted_history);
      json_object_put(converted_history);
      converted_history = sanitized;
   }

   // Root JSON Container
   root = json_object_new_object();

   // Model from config
   json_object_object_add(root, "model", json_object_new_string(g_config.llm.cloud.openai_model));

   // Enable streaming with usage reporting
   json_object_object_add(root, "stream", json_object_new_boolean(1));
   json_object *stream_opts = json_object_new_object();
   json_object_object_add(stream_opts, "include_usage", json_object_new_boolean(1));
   json_object_object_add(root, "stream_options", stream_opts);

   // Handle vision if provided
   if (vision_image != NULL && vision_image_size > 0) {
      LOG_INFO("OpenAI streaming: Vision image provided (%zu bytes)", vision_image_size);
      int msg_count = json_object_array_length(converted_history);
      if (msg_count > 0) {
         json_object *last_msg = json_object_array_get_idx(converted_history, msg_count - 1);
         json_object *role_obj;
         if (json_object_object_get_ex(last_msg, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0) {
            LOG_INFO("OpenAI streaming: Adding vision to last user message");
            // Last message is user message - add vision content
            json_object *content_array = json_object_new_array();
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text", json_object_new_string(input_text));
            json_object_array_add(content_array, text_obj);

            json_object *image_obj = json_object_new_object();
            json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

            json_object *image_url_obj = json_object_new_object();
            char *data_uri_prefix = "data:image/jpeg;base64,";
            size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_image) + 1;
            char *data_uri = malloc(data_uri_length);
            if (data_uri != NULL) {
               snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_image);
               json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
               free(data_uri);
            }
            json_object_object_add(image_obj, "image_url", image_url_obj);
            json_object_array_add(content_array, image_obj);

            json_object_object_add(last_msg, "content", content_array);
         } else {
            /* Last message is not user (e.g., tool result after viewing command).
             * Create a new user message with the vision image. */
            LOG_INFO("OpenAI streaming: Adding vision as new user message (last was role=%s)",
                     role_obj ? json_object_get_string(role_obj) : "null");

            json_object *new_user_msg = json_object_new_object();
            json_object_object_add(new_user_msg, "role", json_object_new_string("user"));

            json_object *content_array = json_object_new_array();

            /* Add text instruction */
            json_object *text_obj = json_object_new_object();
            json_object_object_add(text_obj, "type", json_object_new_string("text"));
            json_object_object_add(text_obj, "text",
                                   json_object_new_string("Here is the captured image. "
                                                          "Please respond to the user's request."));
            json_object_array_add(content_array, text_obj);

            /* Add the image */
            json_object *image_obj = json_object_new_object();
            json_object_object_add(image_obj, "type", json_object_new_string("image_url"));

            json_object *image_url_obj = json_object_new_object();
            char *data_uri_prefix = "data:image/jpeg;base64,";
            size_t data_uri_length = strlen(data_uri_prefix) + strlen(vision_image) + 1;
            char *data_uri = malloc(data_uri_length);
            if (data_uri != NULL) {
               snprintf(data_uri, data_uri_length, "%s%s", data_uri_prefix, vision_image);
               json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
               free(data_uri);
            }
            json_object_object_add(image_obj, "image_url", image_url_obj);
            json_object_array_add(content_array, image_obj);

            json_object_object_add(new_user_msg, "content", content_array);

            /* Append the new user message to history */
            json_object_array_add(converted_history, new_user_msg);
            LOG_INFO("OpenAI streaming: Vision user message added to history");
         }
      }
   }

   // Use filtered history (already our own copy, no need to increment refcount)
   json_object_object_add(root, "messages", converted_history);

   // Max Tokens
   json_object_object_add(root, "max_tokens", json_object_new_int(g_config.llm.max_tokens));

   // Add tools if native tool calling is enabled
   if (llm_tools_enabled(NULL)) {
      struct json_object *tools = llm_tools_get_openai_format();
      if (tools) {
         json_object_object_add(root, "tools", tools);
         json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
         LOG_INFO("OpenAI streaming: Added %d tools to request", llm_tools_get_enabled_count());
      }
   }

   payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                      JSON_C_TO_STRING_NOSLASHESCAPE);

   // Log request details for debugging
   LOG_INFO("OpenAI streaming iter %d: url=%s model=%s api_key=%s", iteration, base_url,
            g_config.llm.cloud.openai_model, api_key ? "(present)" : "(null)");

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
   stream_ctx = llm_stream_create(stream_llm_type, CLOUD_PROVIDER_OPENAI, chunk_callback,
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

      // Set low-speed timeout: abort if transfer drops below 1 byte/sec for 30 seconds
      // This catches hung/stalled streams from local LLM servers
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
            LOG_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
         }
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
         // Log raw response body for debugging errors (especially 400)
         if (streaming_ctx.raw_buffer && streaming_ctx.raw_size > 0) {
            LOG_ERROR("OpenAI API error response: %s", streaming_ctx.raw_buffer);
         }
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

         // Execute tools
         tool_result_list_t results;
         llm_tools_execute_all(tool_calls, &results);

         // Add assistant message with tool_calls to conversation history
         json_object *assistant_msg = json_object_new_object();
         json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));
         json_object_object_add(assistant_msg, "content", NULL);  // No text content

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

            json_object_array_add(tc_array, tc);
         }
         json_object_object_add(assistant_msg, "tool_calls", tc_array);
         json_object_array_add(conversation_history, assistant_msg);

         // Add tool results to conversation history
         llm_tools_add_results_openai(conversation_history, &results);

         // Cleanup current stream context
         sse_parser_free(sse_parser);
         llm_stream_free(stream_ctx);
         json_object_put(root);
         free(streaming_ctx.raw_buffer);

         // Check if we should skip follow-up (e.g., LLM was switched)
         if (llm_tools_should_skip_followup(&results)) {
            LOG_INFO("OpenAI streaming: Skipping follow-up call (tool requested no follow-up)");
            char *direct_response = llm_tools_get_direct_response(&results);

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
            for (int i = 0; i < results.count; i++) {
               if (results.results[i].vision_image) {
                  free(results.results[i].vision_image);
                  results.results[i].vision_image = NULL;
               }
            }
            return strdup(error_msg);
         }

         // Make another call to get final response
         LOG_INFO("OpenAI streaming: Making follow-up call after tool execution (iteration %d/%d)",
                  iteration + 1, MAX_TOOL_ITERATIONS);

         // Debug: Log tool results that were added
         LOG_INFO("OpenAI streaming: Tool results added to history:");
         for (int i = 0; i < results.count; i++) {
            LOG_INFO("  [%d] id=%s result=%.200s%s", i, results.results[i].tool_call_id,
                     results.results[i].result,
                     strlen(results.results[i].result) > 200 ? "..." : "");
         }

         // Check for vision data in tool results (session-isolated)
         const char *result_vision = NULL;
         size_t result_vision_size = 0;
         for (int i = 0; i < results.count; i++) {
            if (results.results[i].vision_image && results.results[i].vision_image_size > 0) {
               result_vision = results.results[i].vision_image;
               result_vision_size = results.results[i].vision_image_size;
               LOG_INFO("OpenAI streaming: Including vision from tool result (%zu bytes)",
                        result_vision_size);
               break;
            }
         }

         // Check if provider changed (e.g., switch_llm was called)
         llm_resolved_config_t current_config;
         char *result = NULL;

         if (llm_get_current_resolved_config(&current_config) == 0 &&
             current_config.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
            // Provider switched to Claude - hand off to Claude code path
            LOG_INFO("OpenAI streaming: Provider switched to Claude, handing off");

            // Convert conversation history from OpenAI to Claude format
            // Claude will handle the vision data if present
            result = llm_claude_chat_completion_streaming(
                conversation_history, "", (char *)result_vision, result_vision_size,
                current_config.endpoint, current_config.api_key,
                (llm_claude_text_chunk_callback)chunk_callback, callback_userdata);
         } else {
            // Still OpenAI-compatible (local or OpenAI cloud) - refresh credentials and continue
            const char *fresh_url = base_url;
            const char *fresh_api_key = api_key;

            if (llm_get_current_resolved_config(&current_config) == 0) {
               fresh_url = current_config.endpoint;
               fresh_api_key = current_config.api_key;
               if (fresh_url != base_url) {
                  LOG_INFO("OpenAI streaming: Credentials refreshed to %s", fresh_url);
               }
            }

            result = llm_openai_streaming_internal(conversation_history, "", (char *)result_vision,
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
   json_object_put(root);
   free(streaming_ctx.raw_buffer);

   return response;
}

char *llm_openai_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
                                           llm_openai_text_chunk_callback chunk_callback,
                                           void *callback_userdata) {
   return llm_openai_streaming_internal(conversation_history, input_text, vision_image,
                                        vision_image_size, base_url, api_key, chunk_callback,
                                        callback_userdata, 0);
}
