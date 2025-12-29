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

#define _GNU_SOURCE
#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* JSON-C */
#include <json-c/json.h>

/* OpenSSL */
#include <openssl/buffer.h>
#include <openssl/evp.h>

/* Local */
#include "audio/flac_playback.h"
#include "audio/mic_passthrough.h"
#include "config/dawn_config.h"
#include "conversation_manager.h"
#include "core/command_router.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/calculator.h"
#include "tools/search_summarizer.h"
#include "tools/smartthings_service.h"
#include "tools/string_utils.h"
#include "tools/url_fetcher.h"
#include "tools/weather_service.h"
#include "tools/web_search.h"
#include "tts/text_to_speech.h"
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"
#include "word_to_number.h"

#define MAX_FILENAME_LENGTH 1024
#define MAX_PLAYLIST_LENGTH 100

/* Forward declaration for switchLlmCallback (defined later in file) */
char *switchLlmCallback(const char *actionName, char *value, int *should_respond);

/**
 * Array of device callbacks associating device types with their respective handling functions.
 * This facilitates dynamic invocation of actions based on the device type, enhancing the
 * application's modularity and scalability.
 *
 * CALLBACK RETURN VALUE CONTRACT:
 * - All callbacks MUST return heap-allocated strings (via malloc/strdup) or NULL
 * - Caller is responsible for freeing the returned value
 * - This makes callbacks thread-safe (no static buffers)
 *
 * TODO: should_respond is not being used consistently. This needs review.
 */
static deviceCallback deviceCallbackArray[] = { { AUDIO_PLAYBACK_DEVICE, setPcmPlaybackDevice },
                                                { AUDIO_CAPTURE_DEVICE, setPcmCaptureDevice },
                                                { TEXT_TO_SPEECH, textToSpeechCallback },
                                                { DATE, dateCallback },
                                                { TIME, timeCallback },
                                                { MUSIC, musicCallback },
                                                { VOICE_AMPLIFIER, voiceAmplifierCallback },
                                                { SHUTDOWN, shutdownCallback },
                                                { VOLUME, volumeCallback },
                                                { LOCAL_LLM_SWITCH, localLLMCallback },
                                                { CLOUD_LLM_SWITCH, cloudLLMCallback },
                                                { RESET_CONVERSATION, resetConversationCallback },
                                                { SEARCH, searchCallback },
                                                { WEATHER, weatherCallback },
                                                { CALCULATOR, calculatorCallback },
                                                { URL_FETCH, urlFetchCallback },
                                                { LLM_STATUS, llmStatusCallback },
                                                { CLOUD_PROVIDER, cloudProviderCallback },
                                                { SMARTTHINGS, smartThingsCallback },
                                                { SWITCH_LLM, switchLlmCallback } };

/**
 * @brief Look up a callback function by device name string
 *
 * Searches deviceCallbackArray for a matching device name and returns
 * the associated callback function pointer.
 *
 * @param device_name The device name string (e.g., "weather", "date", "search")
 * @return Callback function pointer, or NULL if not found
 */
device_callback_fn get_device_callback(const char *device_name) {
   if (!device_name) {
      return NULL;
   }

   for (int i = 0; i < MAX_DEVICE_TYPES; i++) {
      if (strcmp(device_name, deviceTypeStrings[i]) == 0) {
         return deviceCallbackArray[i].callback;
      }
   }

   return NULL;
}

static pthread_t music_thread = -1;
static pthread_t voice_thread = -1;
static char *pending_command_result = NULL;

// Custom music directory path (if set via command line)
// If NULL, uses default MUSIC_DIR from dawn.h
static char *custom_music_dir = NULL;

/**
 * Retrieves the current user's home directory.
 *
 * @return A pointer to a string containing the path to the home directory. This string
 *         should not be modified or freed by the caller, as it points to an environment variable.
 */
const char *getUserHomeDirectory() {
   const char *homeDir = getenv("HOME");
   if (!homeDir) {
      LOG_ERROR("Error: HOME environment variable not set.");
      return NULL;
   }

   return homeDir;
}

/**
 * Appends a specified subdirectory to the user's home directory to construct a path.
 *
 * @param subdirectory The subdirectory to append to the home directory.
 * @return A dynamically allocated string containing the full path. The caller is responsible
 *         for freeing this memory using free().
 */
char *constructPathWithSubdirectory(const char *subdirectory) {
   const char *homeDir = getUserHomeDirectory();
   if (!homeDir) {
      // getUserHomeDirectory already prints an error message if needed.

      return NULL;
   }

   // Calculate the size needed for the full path, including null terminator
   size_t fullPathSize = strlen(homeDir) + strlen(subdirectory) + 1;

   // Allocate memory for the full path
   char *fullPath = (char *)malloc(fullPathSize);
   if (!fullPath) {
      LOG_ERROR("Error: Memory allocation failed for full path.");

      return NULL;
   }

   // Construct the full path
   snprintf(fullPath, fullPathSize, "%s%s", homeDir, subdirectory);

   return fullPath;
}

/**
 * @struct  Playlist
 * @brief   Structure to hold the list of matching filenames.
 */
typedef struct {
   char filenames[MAX_PLAYLIST_LENGTH][MAX_FILENAME_LENGTH]; /**< Array of matching filenames */
   int count;                                                /**< Number of matching filenames */
} Playlist;

static Playlist playlist = { .count = 0 };
static int current_track = 0;

/**
 * @brief Recursively searches a directory for files matching a pattern and adds them to a playlist.
 *
 * This function searches through the given root directory and all its subdirectories
 * for files that match the provided filename pattern. It adds matching files to the playlist.
 *
 * @param rootDir The root directory where the search begins.
 * @param pattern The pattern to match filenames (supports wildcards and case folding).
 * @param playlist Pointer to the Playlist structure where matching files will be added.
 *
 * @note The function stops adding files if the playlist reaches its maximum length.
 * @note Uses the `fnmatch()` function to match filenames.
 */
void searchDirectory(const char *rootDir, const char *pattern, Playlist *playlist) {
   DIR *dir = opendir(rootDir);
   if (!dir) {
      LOG_ERROR("Error opening directory: %s", rootDir);
      return;
   }

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      // Check if playlist is full before processing
      if (playlist->count >= MAX_PLAYLIST_LENGTH) {
         LOG_WARNING("Playlist is full.");
         closedir(dir);
         return;
      }

      if (entry->d_type == DT_REG) {
         char filePath[MAX_FILENAME_LENGTH];
         int written = snprintf(filePath, sizeof(filePath), "%s/%s", rootDir, entry->d_name);

         // Check if path was truncated
         if (written >= MAX_FILENAME_LENGTH) {
            LOG_WARNING("Path too long, skipping: %s/%s", rootDir, entry->d_name);
            continue;
         }

         if (fnmatch(pattern, entry->d_name, FNM_CASEFOLD) == 0) {
            strncpy(playlist->filenames[playlist->count], filePath, MAX_FILENAME_LENGTH - 1);
            playlist->filenames[playlist->count][MAX_FILENAME_LENGTH - 1] = '\0';
            playlist->count++;
         }
      } else if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 &&
                 strcmp(entry->d_name, "..") != 0) {
         char subPath[MAX_FILENAME_LENGTH];
         int written = snprintf(subPath, sizeof(subPath), "%s/%s", rootDir, entry->d_name);

         // Check if path was truncated
         if (written >= MAX_FILENAME_LENGTH) {
            LOG_WARNING("Path too long, skipping directory: %s/%s", rootDir, entry->d_name);
            continue;
         }

         searchDirectory(subPath, pattern, playlist);
      }
   }

   closedir(dir);
}

#define GPT_RESPONSE_BUFFER_SIZE 512

/**
 * Execute a parsed JSON command (internal implementation)
 *
 * @param parsedJson Already-parsed JSON object (caller retains ownership)
 * @param mosq MQTT client handle
 */
static void executeJsonCommand(struct json_object *parsedJson, struct mosquitto *mosq) {
   struct json_object *deviceObject = NULL;
   struct json_object *actionObject = NULL;
   struct json_object *valueObject = NULL;

   extern struct json_object *conversation_history;
   char *response_text = NULL;
   char gpt_response[GPT_RESPONSE_BUFFER_SIZE];

   const char *deviceName = NULL;
   const char *actionName = NULL;
   const char *value = NULL;

   char *callback_result = NULL;
   int should_respond = 0;

   int i = 0;

   // Get the "device" object from the JSON
   if (json_object_object_get_ex(parsedJson, "device", &deviceObject)) {
      // Extract the text value as a C string
      deviceName = json_object_get_string(deviceObject);
      if (deviceName == NULL) {
         LOG_ERROR("Error: Unable to get device name from json command.");
         return;
      }
   } else {
      LOG_ERROR("Error: 'device' field not found in JSON.");
      return;
   }

   // Get the "action" object from the JSON
   if (json_object_object_get_ex(parsedJson, "action", &actionObject)) {
      // Extract the text value as a C string
      actionName = json_object_get_string(actionObject);
      if (actionName == NULL) {
         LOG_ERROR("Error: Unable to get action name from json command.");
         return;
      }
   } else {
      LOG_ERROR("Error: 'action' field not found in JSON.");
      return;
   }

   // Get the "value" object from the JSON, not required for all commands
   if (json_object_object_get_ex(parsedJson, "value", &valueObject)) {
      // Extract the text value as a C string
      value = json_object_get_string(valueObject);
      if (value == NULL) {
         LOG_WARNING("Notice: Unable to get value name from json command.");
      }
   }

   /* Before we process, make sure nothing's left over. */
   if (pending_command_result != NULL) {
      free(pending_command_result);
      pending_command_result = NULL;
   }

   /* Loop through device names for device types. */
   for (i = 0; i < MAX_DEVICE_TYPES; i++) {
      if (strcmp(deviceName, deviceTypeStrings[i]) == 0) {
         if (deviceCallbackArray[i].callback != NULL) {
            callback_result = deviceCallbackArray[i].callback(actionName, (char *)value,
                                                              &should_respond);

            // If in AI mode and callback returned data, store it for AI response
            if (callback_result != NULL && should_respond &&
                (command_processing_mode == CMD_MODE_LLM_ONLY ||
                 command_processing_mode == CMD_MODE_DIRECT_FIRST)) {
               size_t dest_len = (pending_command_result == NULL) ? 0
                                                                  : strlen(pending_command_result);
               size_t src_len = strlen(callback_result);

               // Resize memory to fit both strings plus space and null terminator
               char *temp = pending_command_result = realloc(pending_command_result,
                                                             dest_len + src_len + 2);
               if (temp == NULL) {
                  free(pending_command_result);
                  pending_command_result = NULL;
                  free(callback_result);
                  continue;
               }
               pending_command_result = temp;

               // Copy the new string to the end
               strcpy(pending_command_result + dest_len, " ");
               strcpy(pending_command_result + dest_len + 1, callback_result);
            }

            // Free callback result (callbacks return heap-allocated strings)
            if (callback_result) {
               free(callback_result);
               callback_result = NULL;
            }
         } else {
            LOG_WARNING("Skipping callback, value NULL.");
         }
      }
   }

   LOG_INFO("Command result for AI: %s",
            pending_command_result ? pending_command_result : "(null)");

   // Log device callback data to TUI for debugging (sanitized for display)
   if (pending_command_result) {
      size_t data_len = strlen(pending_command_result);
      char sanitized[100];
      size_t max_display = sizeof(sanitized) - 16;  // Room for "... (XXXXb)"

      // Copy up to max_display chars, replacing newlines with spaces
      size_t i, j;
      for (i = 0, j = 0; j < max_display && pending_command_result[i]; i++) {
         char c = pending_command_result[i];
         if (c == '\n' || c == '\r') {
            if (j > 0 && sanitized[j - 1] != ' ') {
               sanitized[j++] = ' ';
            }
         } else {
            sanitized[j++] = c;
         }
      }
      sanitized[j] = '\0';

      // Add truncation indicator with total size
      if (data_len > max_display) {
         snprintf(sanitized + j, sizeof(sanitized) - j, "... (%zub)", data_len);
      }

      metrics_log_activity("DATA: %s", sanitized);
   }

   if (pending_command_result == NULL) {
      // This is normal for commands that don't return data (e.g., TTS, volume, etc.)
      // Only commands that set should_respond=1 will have pending results
      return;
   }

   // Format system data with clear instruction to speak it to the user
   // Using "system" role so LLM knows this is data to relay, not user input
   snprintf(gpt_response, sizeof(gpt_response),
            "[DEVICE DATA] Speak this information naturally to the user: %s",
            pending_command_result);

   // Add as system message so LLM knows to relay it, not just confirm it
   struct json_object *system_response_message = json_object_new_object();
   json_object_object_add(system_response_message, "role", json_object_new_string("system"));
   json_object_object_add(system_response_message, "content", json_object_new_string(gpt_response));
   json_object_array_add(conversation_history, system_response_message);

   response_text = llm_chat_completion(conversation_history, gpt_response, NULL, 0);
   if (response_text != NULL) {
      // AI returned successfully, vocalize response.
      LOG_WARNING("AI: %s\n", response_text);
      char *match = NULL;
      if ((match = strstr(response_text, "<end_of_turn>")) != NULL) {
         *match = '\0';
         LOG_INFO("AI: %s\n", response_text);
      }

      // Process any commands in the LLM follow-up response (chained commands)
      if (command_processing_mode == CMD_MODE_LLM_ONLY ||
          command_processing_mode == CMD_MODE_DIRECT_FIRST) {
         int cmds_processed = parse_llm_response_for_commands(response_text, mosq);
         if (cmds_processed > 0) {
            LOG_INFO("Processed %d chained commands from LLM follow-up", cmds_processed);
         }
      }

      // Skip TTS if response is pure JSON (no conversational text)
      // Check by trying to parse as JSON - if valid JSON object/array, skip TTS
      const char *p = response_text;
      while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
         p++;  // Skip whitespace
      int is_pure_json = 0;
      if (*p == '{' || *p == '[') {
         struct json_object *test_json = json_tokener_parse(response_text);
         if (test_json) {
            is_pure_json = 1;
            json_object_put(test_json);
         }
      }

      if (!is_pure_json) {
         // Create cleaned version for TTS (remove command tags)
         char *tts_response = strdup(response_text);
         if (tts_response) {
            char *cmd_start, *cmd_end;
            while ((cmd_start = strstr(tts_response, "<command>")) != NULL) {
               cmd_end = strstr(cmd_start, "</command>");
               if (cmd_end) {
                  cmd_end += strlen("</command>");
                  memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
               } else {
                  break;
               }
            }
            // Remove emojis before TTS to prevent them from being read aloud
            remove_emojis(tts_response);
            text_to_speech(tts_response);
            free(tts_response);
         } else {
            // Fallback: need to copy for emoji removal since remove_emojis modifies in-place
            char *fallback = strdup(response_text);
            if (fallback) {
               remove_emojis(fallback);
               text_to_speech(fallback);
               free(fallback);
            } else {
               text_to_speech(response_text);  // Last resort: skip emoji removal
            }
         }

         // Update TUI with the AI response (full response including commands)
         metrics_set_last_ai_response(response_text);

         // Add the successful AI response to the conversation.
         struct json_object *ai_message = json_object_new_object();
         json_object_object_add(ai_message, "role", json_object_new_string("assistant"));
         json_object_object_add(ai_message, "content", json_object_new_string(response_text));
         json_object_array_add(conversation_history, ai_message);
      }

      free(response_text);
      response_text = NULL;
   } else {
      // Error on AI response
      LOG_ERROR("GPT error.\n");
      text_to_speech("I'm sorry but I'm currently unavailable boss.");
   }
   free(pending_command_result);
   pending_command_result = NULL;
   // Note: parsedJson is owned by caller, do not free here
}

/**
 * Parse and execute a JSON command string (legacy API wrapper)
 *
 * This function parses the input string as JSON and executes the command.
 * For callers that already have parsed JSON, use executeJsonCommand() directly
 * to avoid double parsing.
 *
 * @param input JSON string to parse and execute
 * @param mosq MQTT client handle
 */
void parseJsonCommandandExecute(const char *input, struct mosquitto *mosq) {
   struct json_object *parsedJson = json_tokener_parse(input);
   if (parsedJson == NULL) {
      // Log first 200 chars of malformed payload for debugging
      char preview[201];
      size_t len = strlen(input);
      if (len > 200) {
         strncpy(preview, input, 200);
         preview[200] = '\0';
      } else {
         strncpy(preview, input, len + 1);
      }
      LOG_ERROR("Unable to parse MQTT JSON command. Payload preview: %.200s%s", preview,
                len > 200 ? "..." : "");
      return;
   }

   executeJsonCommand(parsedJson, mosq);
   json_object_put(parsedJson);
}

/* Mosquitto */
/* Callback called when the client receives a CONNACK message from the broker. */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code) {
   int rc;

   LOG_INFO("MQTT Connecting.");

   if (reason_code != 0) {
      LOG_WARNING("MQTT disconnecting?");
      mosquitto_disconnect(mosq);
      return;
   }

   // Subscribe in the on_connect callback
   rc = mosquitto_subscribe(mosq, NULL, APPLICATION_NAME, 0);
   if (rc != MOSQ_ERR_SUCCESS) {
      LOG_ERROR("Error on mosquitto_subscribe(): %s", mosquitto_strerror(rc));
   } else {
      LOG_INFO("Subscribed to \"%s\" MQTT.", APPLICATION_NAME);
   }
}

/* Callback called when the broker sends a SUBACK in response to a SUBSCRIBE. */
void on_subscribe(struct mosquitto *mosq,
                  void *obj,
                  int mid,
                  int qos_count,
                  const int *granted_qos) {
   int i;
   bool have_subscription = false;

   LOG_INFO("MQTT subscribed.");

   for (i = 0; i < qos_count; i++) {
      if (granted_qos[i] <= 2) {
         have_subscription = true;
      }
   }
   if (have_subscription == false) {
      LOG_ERROR("Error: All subscriptions rejected.");
      mosquitto_disconnect(mosq);
   }
}

/**
 * @brief Execute command for a worker thread and deliver result
 *
 * This is called when a command has a request_id, indicating it came from
 * a worker thread that is waiting for the result.
 *
 * CALLBACK RETURN VALUE CONTRACT:
 * - Callbacks MUST return heap-allocated strings (via malloc/strdup) or NULL
 * - Caller (this function) is responsible for freeing the returned value
 * - Legacy callbacks (date, time, etc.) use static buffers - these should be
 *   migrated to heap allocation for consistency (Phase 4 cleanup)
 * - New callbacks (weather, search) already follow heap allocation pattern
 *
 * Thread safety:
 * - All MQTT message processing happens in main thread's on_message callback
 * - command_router_deliver() copies the result before returning
 *
 * @param parsed_json Parsed JSON command object
 * @param request_id Request ID to deliver result to
 */
static void execute_command_for_worker(struct json_object *parsed_json, const char *request_id) {
   struct json_object *deviceObject = NULL;
   struct json_object *actionObject = NULL;
   struct json_object *valueObject = NULL;

   const char *deviceName = NULL;
   const char *actionName = NULL;
   const char *value = NULL;

   char *callback_result = NULL;
   int should_respond = 0;

   // Get the "device" object from the JSON
   if (!json_object_object_get_ex(parsed_json, "device", &deviceObject)) {
      LOG_ERROR("Worker command missing 'device' field");
      command_router_deliver(request_id, "");
      return;
   }
   deviceName = json_object_get_string(deviceObject);

   // Get the "action" object from the JSON
   if (!json_object_object_get_ex(parsed_json, "action", &actionObject)) {
      LOG_ERROR("Worker command missing 'action' field");
      command_router_deliver(request_id, "");
      return;
   }
   actionName = json_object_get_string(actionObject);

   // Get the "value" object (optional)
   if (json_object_object_get_ex(parsed_json, "value", &valueObject)) {
      value = json_object_get_string(valueObject);
   }

   LOG_INFO("Executing command for worker: device=%s, action=%s, request_id=%s", deviceName,
            actionName, request_id);

   /* OCP: Check status field for error responses */
   struct json_object *status_obj = NULL;
   if (json_object_object_get_ex(parsed_json, "status", &status_obj)) {
      const char *status = json_object_get_string(status_obj);
      if (status && strcmp(status, "error") == 0) {
         /* Extract error details from error object */
         struct json_object *error_obj = NULL;
         const char *error_code = "UNKNOWN";
         const char *error_message = "Unknown error occurred";

         if (json_object_object_get_ex(parsed_json, "error", &error_obj)) {
            struct json_object *code_obj = NULL;
            struct json_object *msg_obj = NULL;

            if (json_object_object_get_ex(error_obj, "code", &code_obj)) {
               error_code = json_object_get_string(code_obj);
            }
            if (json_object_object_get_ex(error_obj, "message", &msg_obj)) {
               error_message = json_object_get_string(msg_obj);
            }
         }

         LOG_ERROR("OCP error response from %s: [%s] %s", deviceName, error_code, error_message);

         /* Deliver error to waiting worker with formatted error string */
         char error_result[512];
         snprintf(error_result, sizeof(error_result), "ERROR: %s - %s", error_code, error_message);
         command_router_deliver(request_id, error_result);
         return;
      }
   }

   /* Special handling for viewing responses with OCP inline data */
   if (strcmp(deviceName, "viewing") == 0) {
      struct json_object *data_obj = NULL;
      if (json_object_object_get_ex(parsed_json, "data", &data_obj)) {
         /* OCP inline data format: data.content contains base64 image */
         struct json_object *content_obj = NULL;
         if (json_object_object_get_ex(data_obj, "content", &content_obj)) {
            const char *base64_content = json_object_get_string(content_obj);
            if (base64_content && base64_content[0] != '\0') {
               /* OCP v1.1: Validate checksum if provided */
               struct json_object *checksum_obj = NULL;
               struct json_object *encoding_obj = NULL;
               const char *checksum = NULL;
               const char *encoding = "base64"; /* Default for images */

               if (json_object_object_get_ex(data_obj, "checksum", &checksum_obj)) {
                  checksum = json_object_get_string(checksum_obj);
               }
               if (json_object_object_get_ex(data_obj, "encoding", &encoding_obj)) {
                  encoding = json_object_get_string(encoding_obj);
               }

               /* OCP v1.1: Validate checksum - fail-closed policy */
               if (!ocp_validate_inline_checksum(base64_content, encoding, checksum)) {
                  LOG_ERROR("OCP: Rejecting viewing response due to checksum mismatch");
                  command_router_deliver(request_id, "");
                  return;
               }

               LOG_INFO("Viewing response contains inline data, delivering directly");
               command_router_deliver(request_id, base64_content);
               return;
            }
         }
      }

      /* Fall through to use file path if no inline data */
      /* OCP v1.1: Validate checksum for file reference if provided */
      if (value && value[0] != '\0') {
         struct json_object *checksum_obj = NULL;
         if (json_object_object_get_ex(parsed_json, "checksum", &checksum_obj)) {
            const char *checksum = json_object_get_string(checksum_obj);
            /* Validate checksum - fail-closed policy, no path restriction for viewing */
            if (!ocp_validate_file_checksum(value, checksum, NULL)) {
               LOG_ERROR("OCP: Rejecting viewing response due to file checksum mismatch");
               command_router_deliver(request_id, "");
               return;
            }
         }
      }
      LOG_INFO("Viewing response using file path: %s", value ? value : "(null)");
   }

   // Get session_id if present (for per-session LLM config)
   // Note: session_get() returns NULL for disconnected sessions, which means
   // commands from disconnected clients fall back to global config. This is
   // intentional - there's no value in changing config for a disconnected client,
   // and they can't see the result anyway.
   struct json_object *session_id_obj = NULL;
   session_t *session = NULL;
   if (json_object_object_get_ex(parsed_json, "session_id", &session_id_obj)) {
      uint32_t session_id = (uint32_t)json_object_get_int(session_id_obj);
      session = session_get(session_id);
      if (session) {
         session_set_command_context(session);
      }
   }

   // Loop through device names for device types and execute callback
   for (int i = 0; i < MAX_DEVICE_TYPES; i++) {
      if (strcmp(deviceName, deviceTypeStrings[i]) == 0) {
         if (deviceCallbackArray[i].callback != NULL) {
            callback_result = deviceCallbackArray[i].callback(actionName, (char *)value,
                                                              &should_respond);
         }
         break;
      }
   }

   // Clear command context and release session reference
   session_set_command_context(NULL);
   if (session) {
      session_release(session);
   }

   // Deliver result to waiting worker
   if (callback_result && should_respond) {
      command_router_deliver(request_id, callback_result);
      LOG_INFO("Delivered result to worker: %s", callback_result);
   } else {
      // Command executed but no data returned
      command_router_deliver(request_id, "");
      LOG_INFO("Delivered empty result to worker (command executed, no data)");
   }

   // Free callback result (callbacks return heap-allocated strings)
   if (callback_result) {
      free(callback_result);
   }
}

/* Callback called when the client receives a message. */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
   LOG_INFO("%s %d %s", msg->topic, msg->qos, (char *)msg->payload);

   // Parse the JSON to check for request_id
   struct json_object *parsed_json = json_tokener_parse((char *)msg->payload);
   if (parsed_json == NULL) {
      LOG_ERROR("Failed to parse MQTT message as JSON");
      return;
   }

   // Check if this is a worker request (has request_id)
   struct json_object *request_id_obj = NULL;
   if (json_object_object_get_ex(parsed_json, "request_id", &request_id_obj)) {
      const char *request_id = json_object_get_string(request_id_obj);

      // WORKER PATH: Execute callback and route result to worker
      execute_command_for_worker(parsed_json, request_id);
   } else {
      // LOCAL PATH: Pass already-parsed JSON to avoid double parsing
      executeJsonCommand(parsed_json, mosq);
   }

   json_object_put(parsed_json);
}

char *dateCallback(const char *actionName, char *value, int *should_respond) {
   time_t current_time;
   struct tm *time_info;
   char buffer[80];
   char *result = NULL;
   int choice;
   char *old_tz = NULL;

   *should_respond = 1;  // Default to responding

   time(&current_time);

   // Use configured timezone if set, otherwise use system default
   if (g_config.localization.timezone[0] != '\0') {
      // Save current TZ
      const char *current_tz = getenv("TZ");
      if (current_tz) {
         old_tz = strdup(current_tz);
      }
      // Set configured timezone
      setenv("TZ", g_config.localization.timezone, 1);
      tzset();
   }

   time_info = localtime(&current_time);

   // Format the date data
   strftime(buffer, sizeof(buffer), "%A, %B %d, %Y", time_info);

   // Restore original TZ if we changed it
   if (g_config.localization.timezone[0] != '\0') {
      if (old_tz) {
         setenv("TZ", old_tz, 1);
         free(old_tz);
      } else {
         unsetenv("TZ");
      }
      tzset();
   }

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      // Direct mode: use text-to-speech with personality
      srand(time(NULL));
      choice = rand() % 3;

      result = malloc(256);
      if (!result) {
         LOG_ERROR("dateCallback: malloc failed");
         *should_respond = 0;
         return NULL;
      }

      switch (choice) {
         case 0:
            snprintf(result, 256, "Today's date, dear Sir, is %s. You're welcome.", buffer);
            break;
         case 1:
            snprintf(result, 256, "In case you've forgotten, Sir, it's %s today.", buffer);
            break;
         case 2:
            snprintf(result, 256, "The current date is %s.", buffer);
            break;
      }

      int local_should_respond = 0;
      textToSpeechCallback(NULL, result, &local_should_respond);
      free(result);
      return NULL;  // Already handled
   } else {
      // AI modes: return the raw data for AI to process
      result = malloc(128);
      if (!result) {
         LOG_ERROR("dateCallback: malloc failed");
         *should_respond = 0;
         return NULL;
      }
      snprintf(result, 128, "The current date is %s", buffer);
      return result;
   }
}

char *timeCallback(const char *actionName, char *value, int *should_respond) {
   time_t current_time;
   struct tm *time_info;
   char buffer[80];
   char *result = NULL;
   int choice;
   char *old_tz = NULL;

   *should_respond = 1;

   time(&current_time);

   // Use configured timezone if set, otherwise use system default
   if (g_config.localization.timezone[0] != '\0') {
      // Save current TZ
      const char *current_tz = getenv("TZ");
      if (current_tz) {
         old_tz = strdup(current_tz);
      }
      // Set configured timezone
      setenv("TZ", g_config.localization.timezone, 1);
      tzset();
   }

   time_info = localtime(&current_time);

   // Format the time data with timezone
   strftime(buffer, sizeof(buffer), "%I:%M %p %Z", time_info);

   // Restore original TZ if we changed it
   if (g_config.localization.timezone[0] != '\0') {
      if (old_tz) {
         setenv("TZ", old_tz, 1);
         free(old_tz);
      } else {
         unsetenv("TZ");
      }
      tzset();
   }

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      // Direct mode: use text-to-speech with personality
      srand(time(NULL));
      choice = rand() % 4;

      result = malloc(256);
      if (!result) {
         LOG_ERROR("timeCallback: malloc failed");
         *should_respond = 0;
         return NULL;
      }

      switch (choice) {
         case 0:
            snprintf(result, 256,
                     "The current time, in case your wristwatch has failed you, is %s.", buffer);
            break;
         case 1:
            snprintf(result, 256, "I trust you have something important planned, Sir? It's %s.",
                     buffer);
            break;
         case 2:
            snprintf(result, 256,
                     "Oh, you want to know the time again? It's %s, not that I'm keeping track.",
                     buffer);
            break;
         case 3:
            snprintf(result, 256, "The time is %s.", buffer);
            break;
      }

      int local_should_respond = 0;
      textToSpeechCallback(NULL, result, &local_should_respond);
      free(result);
      return NULL;
   } else {
      // AI modes: return the raw data
      // buffer is up to 80 chars, plus "The time is " (12) + "." (1) + null (1) = 94
      result = malloc(96);
      if (!result) {
         LOG_ERROR("timeCallback: malloc failed");
         *should_respond = 0;
         return NULL;
      }
      snprintf(result, 96, "The time is %s.", buffer);
      return result;
   }
}

// Custom comparison function for qsort
int compare(const void *p1, const void *p2) {
   return strcmp((char *)p1, (char *)p2);
}

#define MUSIC_CALLBACK_BUFFER_SIZE 512

void set_music_directory(const char *path) {
   if (custom_music_dir) {
      free(custom_music_dir);
      custom_music_dir = NULL;
   }

   if (path) {
      custom_music_dir = strdup(path);
      if (!custom_music_dir) {
         LOG_ERROR("Failed to allocate memory for custom music directory");
      } else {
         LOG_INFO("Music directory set to: %s", custom_music_dir);
      }
   }
}

char *musicCallback(const char *actionName, char *value, int *should_respond) {
   PlaybackArgs *args = NULL;
   char strWildcards[MAX_FILENAME_LENGTH];
   char *result = NULL;
   int i = 0;

   *should_respond = 1;  // Default to responding

   if (strcmp(actionName, "play") == 0) {
      if (music_thread != -1) {
         setMusicPlay(0);
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
         }
      }

      playlist.count = 0;
      current_track = 0;

      if ((strlen(value) + 8) > MAX_FILENAME_LENGTH) {
         LOG_ERROR("\"%s\" is too long to search for.", value);

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
            if (result) {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Search term '%s' is too long", value);
            }
            return result;
         }
      }

      // Use custom music directory if set, otherwise use config default
      char *musicDir = NULL;
      if (custom_music_dir) {
         musicDir = strdup(custom_music_dir);
      } else {
         musicDir = constructPathWithSubdirectory(g_config.paths.music_dir);
      }

      if (!musicDir) {
         LOG_ERROR("Error constructing music path.");

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            return strdup("Failed to access music directory");
         }
      }

      strWildcards[0] = '*';
      for (i = 0; value[i] != '\0'; i++) {
         if (value[i] == ' ') {
            strWildcards[i + 1] = '*';
         } else {
            strWildcards[i + 1] = value[i];
         }
      }
      strWildcards[i + 1] = '*';
      strWildcards[i + 2] = '\0';
      strcat(strWildcards, ".flac");

      searchDirectory(musicDir, strWildcards, &playlist);

      free(musicDir);  // free the allocated memory

      // Sort the array using qsort
      qsort(playlist.filenames, playlist.count, MAX_FILENAME_LENGTH, compare);

      LOG_INFO("New playlist:");
      for (i = 0; i < playlist.count; i++) {
         LOG_INFO("\t%s", playlist.filenames[i]);
      }

      if (playlist.count > 0) {
         args = malloc(sizeof(PlaybackArgs));
         if (!args) {
            LOG_ERROR("Failed to allocate PlaybackArgs");
            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               return strdup("Failed to start music playback");
            }
         }
         args->sink_name = getPcmPlaybackDevice();
         args->file_name = playlist.filenames[current_track];
         args->start_time = 0; /* For now set to zero. We may support other modes later. */

         LOG_INFO("Playing: %s %s %d", args->sink_name, args->file_name, args->start_time);

         // Create the playback thread (thread takes ownership of args and will free it)
         if (pthread_create(&music_thread, NULL, playFlacAudio, args)) {
            LOG_ERROR("Error creating thread");
            free(args);

            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               return strdup("Failed to start music playback");
            }
         }

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;  // Music starts playing, no verbal response needed
            return NULL;
         } else {
            // Extract just the filename from the full path for display
            const char *filename = strrchr(playlist.filenames[current_track], '/');
            if (filename) {
               filename++;  // Skip the '/'
            } else {
               filename = playlist.filenames[current_track];
            }
            *should_respond = 1;
            result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
            if (result) {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE,
                        "Now playing: %s (track 1 of %d matching '%s')", filename, playlist.count,
                        value);
            }
            return result;
         }
      } else {
         LOG_WARNING("No music matching that description was found.");

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
            if (result) {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "No music found matching '%s'", value);
            }
            return result;
         }
      }
   } else if (strcmp(actionName, "stop") == 0) {
      LOG_INFO("Stopping music playback.");
      setMusicPlay(0);

      // Wait for the playback thread to finish
      if (music_thread != -1) {
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
            LOG_INFO("Music playback thread stopped successfully.");
         } else {
            LOG_ERROR("Failed to join music thread: %d", join_result);
         }
      }

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         *should_respond = 0;
         return NULL;
      } else {
         return strdup("Music playback stopped");
      }
   } else if (strcmp(actionName, "next") == 0) {
      if (music_thread != -1) {
         setMusicPlay(0);
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
         }
      }

      if (playlist.count > 0) {
         current_track++;
         if (current_track >= playlist.count) {
            current_track = 0;
         }

         args = malloc(sizeof(PlaybackArgs));
         if (!args) {
            LOG_ERROR("Failed to allocate PlaybackArgs");
            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               return strdup("Failed to play next track");
            }
         }
         args->sink_name = getPcmPlaybackDevice();
         args->file_name = playlist.filenames[current_track];
         args->start_time = 0; /* For now set to zero. We may support other modes later. */

         LOG_INFO("Playing: %s %s %d", args->sink_name, args->file_name, args->start_time);

         // Create the playback thread (thread takes ownership of args and will free it)
         if (pthread_create(&music_thread, NULL, playFlacAudio, args)) {
            LOG_ERROR("Error creating music thread");
            free(args);

            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               return strdup("Failed to play next track");
            }
         }

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            // Extract just the filename from the full path
            const char *filename = strrchr(playlist.filenames[current_track], '/');
            if (filename) {
               filename++;  // Skip the '/'
            } else {
               filename = playlist.filenames[current_track];
            }
            *should_respond = 1;
            result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
            if (result) {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing next track: %s", filename);
            }
            return result;
         }
      } else {
         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            *should_respond = 1;
            return strdup("No playlist available");
         }
      }
   } else if (strcmp(actionName, "previous") == 0) {
      if (music_thread != -1) {
         setMusicPlay(0);
         int join_result = pthread_join(music_thread, NULL);
         if (join_result == 0) {
            music_thread = -1;
         }
      }

      if (playlist.count > 0) {
         current_track--;
         if (current_track < 0) {
            current_track = playlist.count - 1;
         }

         args = malloc(sizeof(PlaybackArgs));
         if (!args) {
            LOG_ERROR("Failed to allocate PlaybackArgs");
            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               return strdup("Failed to play previous track");
            }
         }
         args->sink_name = getPcmPlaybackDevice();
         args->file_name = playlist.filenames[current_track];
         args->start_time = 0; /* For now set to zero. We may support other modes later. */

         LOG_INFO("Playing: %s %s %d", args->sink_name, args->file_name, args->start_time);

         // Create the playback thread (thread takes ownership of args and will free it)
         if (pthread_create(&music_thread, NULL, playFlacAudio, args)) {
            LOG_ERROR("Error creating music thread");
            free(args);

            if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
               *should_respond = 0;
               return NULL;
            } else {
               return strdup("Failed to play previous track");
            }
         }

         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            // Extract just the filename from the full path
            const char *filename = strrchr(playlist.filenames[current_track], '/');
            if (filename) {
               filename++;  // Skip the '/'
            } else {
               filename = playlist.filenames[current_track];
            }
            *should_respond = 1;
            result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
            if (result) {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing previous track: %s", filename);
            }
            return result;
         }
      } else {
         if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
            *should_respond = 0;
            return NULL;
         } else {
            *should_respond = 1;
            return strdup("No playlist available");
         }
      }
   }

   return NULL;
}

char *voiceAmplifierCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;

   if (strcmp(actionName, "enable") == 0) {
      if ((voice_thread != -1) && (pthread_kill(voice_thread, 0) == 0)) {
         LOG_WARNING("Voice amplification thread already running.");

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Voice amplifier is already enabled");
         }
         *should_respond = 0;
         return NULL;
      }

      // Create the playback thread
      if (pthread_create(&voice_thread, NULL, voiceAmplificationThread, NULL)) {
         LOG_ERROR("Error creating voice thread");

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Failed to enable voice amplifier");
         }
         *should_respond = 0;
         return NULL;
      }

      if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
         return strdup("Voice amplifier enabled");
      }
      *should_respond = 0;
      return NULL;

   } else if (strcmp(actionName, "disable") == 0) {
      if ((voice_thread != -1) && (pthread_kill(voice_thread, 0) == 0)) {
         setStopVA();

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Voice amplifier disabled");
         }
      } else {
         LOG_WARNING("Voice amplification thread not running.");

         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Voice amplifier was not running");
         }
      }
      *should_respond = 0;
      return NULL;
   }

   return NULL;
}

// Shutdown callback - requires explicit enable in config for security
char *shutdownCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;

   /* Security check 1: Must be explicitly enabled in config */
   if (!g_config.shutdown.enabled) {
      LOG_WARNING("Shutdown command rejected: shutdown.enabled = false in config");
      return strdup("Shutdown command is disabled. Enable it in settings first.");
   }

   /* Security check 2: If passphrase is configured, it must match */
   if (g_config.shutdown.passphrase[0] != '\0') {
      if (value == NULL || strcasecmp(value, g_config.shutdown.passphrase) != 0) {
         LOG_WARNING("Shutdown command rejected: incorrect or missing passphrase");
         return strdup("Shutdown command rejected: incorrect passphrase.");
      }
      LOG_INFO("Shutdown passphrase verified");
   }

   /* All security checks passed - execute shutdown */
   LOG_INFO("Shutdown command authorized, initiating system shutdown");

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      int local_should_respond = 0;
      textToSpeechCallback(NULL, "Shutdown authorized. Goodbye.", &local_should_respond);
      int ret = system("sudo shutdown -h now");
      if (ret != 0) {
         LOG_ERROR("Shutdown command failed with return code: %d", ret);
      }
      return NULL;
   } else {
      int ret = system("sudo shutdown -h now");
      if (ret != 0) {
         LOG_ERROR("Shutdown command failed with return code: %d", ret);
      }
      return strdup("Shutdown authorized. Initiating system shutdown. Goodbye.");
   }
}

/**
 * Reads the entire contents of a file into memory.
 *
 * @param filename The path to the file to be read.
 * @param length Pointer to a size_t variable where the size of the file will be stored.
 * @return A pointer to the allocated memory containing the file's contents. The caller
 *         is responsible for freeing this memory. Returns NULL on failure.
 */
unsigned char *read_file(const char *filename, size_t *length) {
   *length = 0;  // Ensure length is set to 0 initially
   FILE *file = fopen(filename, "rb");
   if (!file) {
      LOG_ERROR("File opening failed: %s", filename);
      return NULL;
   }

   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   if (size == -1) {
      LOG_ERROR("Failed to determine file size: %s", filename);
      fclose(file);
      return NULL;
   }
   *length = (size_t)size;
   LOG_INFO("The image file is %ld bytes.\n", *length);
   fseek(file, 0, SEEK_SET);

   unsigned char *content = malloc(*length);
   if (!content) {
      LOG_ERROR("Memory allocation failed");
      fclose(file);
      return NULL;
   }

   size_t read_length = fread(content, 1, *length, file);
   if (*length != read_length) {
      LOG_ERROR("Failed to read the total size. Expected: %ld, Read: %ld", *length, read_length);
      free(content);
      fclose(file);
      return NULL;
   }

   fclose(file);
   return content;
}

/* =============================================================================
 * Base64 Encoding
 * ============================================================================= */

/**
 * Encodes data using Base64 encoding.
 *
 * @param buffer Pointer to the data to be encoded.
 * @param length Length of the data to encode.
 * @return A pointer to the null-terminated Base64 encoded string, or NULL if an error occurred.
 *         The caller is responsible for freeing this memory.
 */
char *base64_encode(const unsigned char *buffer, size_t length) {
   if (buffer == NULL || length <= 0) {
      LOG_ERROR("Invalid input to base64_encode.");
      return NULL;
   }

   BIO *bio = NULL, *b64 = NULL;
   BUF_MEM *bufferPtr = NULL;

   // Create a new BIO for Base64 encoding.
   b64 = BIO_new(BIO_f_base64());
   if (b64 == NULL) {
      LOG_ERROR("Failed to create Base64 BIO.");
      return NULL;
   }

   // Create a new BIO that holds data in memory.
   bio = BIO_new(BIO_s_mem());
   if (bio == NULL) {
      LOG_ERROR("Failed to create memory BIO.");
      BIO_free_all(b64);  // Ensure cleanup
      return NULL;
   }

   // Chain the base64 BIO onto the memory BIO.
   // This means that data written to 'bio' will first be Base64 encoded, then stored in memory.
   bio = BIO_push(b64, bio);

   // Set the flag to not use newlines as BIO_FLAGS_BASE64_NO_NL implies.
   // This affects the Base64 encoding to output all data in one continuous line.
   BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

   // Write the input buffer into the BIO chain.
   // This data gets Base64 encoded by 'b64', then stored in the memory managed by 'bio'.
   if (BIO_write(bio, buffer, length) <= 0) {
      LOG_ERROR("Failed to write data to BIO.");
      BIO_free_all(bio);  // Also frees 'b64' since it's pushed onto 'bio'
      return NULL;
   }

   // Ensure all data is flushed through the BIO chain and any encoding is completed.
   if (BIO_flush(bio) <= 0) {
      LOG_ERROR("Failed to flush BIO.");
      BIO_free_all(bio);
      return NULL;
   }

   // Retrieve a pointer to the memory BIO's data.
   // This does not remove the data from the BIO, but allows access to it.
   BIO_get_mem_ptr(bio, &bufferPtr);
   if (bufferPtr == NULL || bufferPtr->data == NULL) {
      LOG_ERROR("Failed to get pointer to BIO memory.");
      BIO_free_all(bio);
      return NULL;
   }

   // Allocate memory for the output text and copy the Base64 encoded data.
   // Note: 'bufferPtr->length' contains the length of the Base64 encoded data.
   char *b64text = malloc(bufferPtr->length + 1);
   if (b64text == NULL) {
      LOG_ERROR("Memory allocation failed for Base64 text.");
      BIO_free_all(bio);
      return NULL;
   }

   memcpy(b64text, bufferPtr->data, bufferPtr->length);
   b64text[bufferPtr->length] = '\0';  // Null-terminate the Base64 encoded string.

   // Free the entire BIO chain, automatically freeing both 'b64' and 'bio'.
   BIO_free_all(bio);

   return b64text;
}

char *volumeCallback(const char *actionName, char *value, int *should_respond) {
   char *result = NULL;
   float floatVol = -1.0f;

   /* Try to parse as numeric string first (handles "100", "50", "0.5", etc) */
   char *endptr;
   double parsed = strtod(value, &endptr);
   if (endptr != value && *endptr == '\0') {
      /* Successfully parsed as number */
      floatVol = (float)parsed;
      /* If value > 2.0, assume it's a percentage (0-100) and convert to 0.0-1.0 */
      if (floatVol > 2.0f) {
         floatVol = floatVol / 100.0f;
      }
   } else {
      /* Fall back to word parsing ("fifty", "one hundred", etc) */
      floatVol = (float)wordToNumber(value);
      /* wordToNumber returns 0-100 for percentages, convert to 0.0-1.0 */
      if (floatVol > 2.0f) {
         floatVol = floatVol / 100.0f;
      }
   }

   LOG_INFO("Music volume: %s -> %.2f", value, floatVol);

   if (floatVol >= 0 && floatVol <= 2.0) {
      setMusicVolume(floatVol);

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         *should_respond = 0;  // No response in direct mode for volume
         return NULL;
      } else {
         // AI modes: return confirmation
         result = malloc(64);
         if (result) {
            snprintf(result, 64, "Music volume set to %.1f", floatVol);
         }
         *should_respond = 1;
         return result;
      }
   } else {
      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         int local_should_respond = 0;
         textToSpeechCallback(NULL, "Invalid volume level requested.", &local_should_respond);
         *should_respond = 0;
         return NULL;
      } else {
         result = malloc(128);
         if (result) {
            snprintf(result, 128,
                     "Invalid volume level %.1f requested. Volume must be between 0 and 2.",
                     floatVol);
         }
         *should_respond = 1;
         return result;
      }
   }
}

char *localLLMCallback(const char *actionName, char *value, int *should_respond) {
   LOG_INFO("Setting AI to local LLM.");
   *should_respond = 1;

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   session_llm_config_t config = { .type = LLM_LOCAL, .cloud_provider = CLOUD_PROVIDER_NONE };
   int result = session_set_llm_config(session, &config);
   if (result != 0) {
      return strdup("Failed to switch to local LLM");
   }
   LOG_INFO("Session %u switched to local LLM", session->session_id);
   return strdup("AI switched to local LLM");
}

char *cloudLLMCallback(const char *actionName, char *value, int *should_respond) {
   LOG_INFO("Setting AI to cloud LLM.");
   *should_respond = 1;

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   /* Use session's current provider preference, just switch to cloud type */
   session_llm_config_t config;
   session_get_llm_config(session, &config);
   config.type = LLM_CLOUD;

   /* If no provider is set (e.g., was using local LLM), default to OpenAI */
   if (config.cloud_provider == CLOUD_PROVIDER_NONE) {
      config.cloud_provider = CLOUD_PROVIDER_OPENAI;
      LOG_INFO("No cloud provider set, defaulting to OpenAI");
   }

   int result = session_set_llm_config(session, &config);
   if (result != 0) {
      return strdup("Failed to switch to cloud LLM. API key not configured in secrets.toml.");
   }
   LOG_INFO("Session %u switched to cloud LLM (provider=%d)", session->session_id,
            config.cloud_provider);
   return strdup("AI switched to cloud LLM");
}

char *llmStatusCallback(const char *actionName, char *value, int *should_respond) {
   char *result = NULL;
   *should_respond = 1;

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   // Handle "set" action to switch LLM type
   if (actionName && strcmp(actionName, "set") == 0) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);

      if (value && (strcasecmp(value, "local") == 0 || strcasecmp(value, "llama") == 0)) {
         LOG_INFO("Setting AI to local LLM via unified llm.set command.");
         config.type = LLM_LOCAL;
         config.cloud_provider = CLOUD_PROVIDER_NONE;
         if (session_set_llm_config(session, &config) != 0) {
            return strdup("Failed to switch to local LLM");
         }
         return strdup("AI switched to local LLM");
      } else if (value && strcasecmp(value, "cloud") == 0) {
         LOG_INFO("Setting AI to cloud LLM via unified llm.set command.");
         config.type = LLM_CLOUD;
         if (session_set_llm_config(session, &config) != 0) {
            return strdup("Failed to switch to cloud LLM. API key not configured.");
         }
         return strdup("AI switched to cloud LLM");
      } else if (value && strcasecmp(value, "openai") == 0) {
         LOG_INFO("Setting AI to OpenAI via unified llm.set command.");
         config.type = LLM_CLOUD;
         config.cloud_provider = CLOUD_PROVIDER_OPENAI;
         if (session_set_llm_config(session, &config) != 0) {
            return strdup("Failed to switch to OpenAI. API key not configured.");
         }
         return strdup("AI switched to OpenAI");
      } else if (value && strcasecmp(value, "claude") == 0) {
         LOG_INFO("Setting AI to Claude via unified llm.set command.");
         config.type = LLM_CLOUD;
         config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
         if (session_set_llm_config(session, &config) != 0) {
            return strdup("Failed to switch to Claude. API key not configured.");
         }
         return strdup("AI switched to Claude");
      } else {
         return strdup("Invalid LLM type. Use 'local', 'cloud', 'openai', or 'claude'.");
      }
   }

   // Handle "get" action (or default) to return current status
   session_llm_config_t session_config;
   llm_resolved_config_t resolved;
   session_get_llm_config(session, &session_config);
   llm_resolve_config(&session_config, &resolved);

   llm_type_t current = resolved.type;
   const char *provider = resolved.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "OpenAI"
                          : resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "Claude"
                                                                             : "None";
   const char *model = resolved.model;
   const char *type_str = (current == LLM_LOCAL) ? "local" : "cloud";

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      // Direct mode: use text-to-speech
      result = malloc(256);
      if (result) {
         if (current == LLM_CLOUD) {
            snprintf(result, 256, "Currently using %s LLM with %s, model %s.", type_str, provider,
                     model ? model : "unknown");
         } else {
            snprintf(result, 256, "Currently using %s LLM, model %s.", type_str,
                     model ? model : "unknown");
         }
         int local_should_respond = 0;
         textToSpeechCallback(NULL, result, &local_should_respond);
         free(result);
      }
      return NULL;
   } else {
      // AI modes: return the raw data for AI to process
      result = malloc(256);
      if (result) {
         if (current == LLM_CLOUD) {
            snprintf(result, 256, "Currently using %s LLM (%s, model: %s)", type_str, provider,
                     model ? model : "unknown");
         } else {
            snprintf(result, 256, "Currently using %s LLM (model: %s)", type_str,
                     model ? model : "unknown");
         }
      }
      return result;
   }
}

char *cloudProviderCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   // Handle "set" action to switch cloud provider
   if (actionName && strcmp(actionName, "set") == 0) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      config.type = LLM_CLOUD;  // Switching provider implies cloud mode

      if (value && strcasecmp(value, "openai") == 0) {
         LOG_INFO("Setting cloud provider to OpenAI.");
         config.cloud_provider = CLOUD_PROVIDER_OPENAI;
         if (session_set_llm_config(session, &config) != 0) {
            return strdup("Failed to switch to OpenAI. API key not configured.");
         }
         return strdup("Cloud provider switched to OpenAI");
      } else if (value && strcasecmp(value, "claude") == 0) {
         LOG_INFO("Setting cloud provider to Claude.");
         config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
         if (session_set_llm_config(session, &config) != 0) {
            return strdup("Failed to switch to Claude. API key not configured.");
         }
         return strdup("Cloud provider switched to Claude");
      } else {
         return strdup("Invalid cloud provider. Use 'openai' or 'claude'.");
      }
   }

   // Handle "get" action (or default) to return current provider
   session_llm_config_t session_config;
   llm_resolved_config_t resolved;
   session_get_llm_config(session, &session_config);
   llm_resolve_config(&session_config, &resolved);

   const char *provider = resolved.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "OpenAI"
                          : resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "Claude"
                                                                             : "None";
   char *result = malloc(128);
   if (result) {
      snprintf(result, 128, "Current cloud provider is %s", provider);
   }
   return result;
}

/**
 * @brief Unified callback for switching LLM mode/provider
 *
 * Routes to appropriate underlying callbacks based on action:
 * - "local": Switch to local LLM
 * - "cloud": Switch to cloud LLM (with current provider)
 * - "openai": Switch to cloud LLM with OpenAI provider
 * - "claude": Switch to cloud LLM with Claude provider
 *
 * This provides a single entry point for the switch_llm tool.
 */
char *switchLlmCallback(const char *actionName, char *value, int *should_respond) {
   extern struct json_object *conversation_history;
   *should_respond = 1;

   if (!actionName || actionName[0] == '\0') {
      return strdup("Error: No target specified for switch_llm");
   }

   LOG_INFO("switchLlmCallback: Switching to '%s'", actionName);

   /* Determine target LLM type and provider for pre-switch compaction check */
   llm_type_t target_type = LLM_LOCAL;
   cloud_provider_t target_provider = CLOUD_PROVIDER_OPENAI;
   const char *target_model = NULL;

   if (strcmp(actionName, "local") == 0) {
      target_type = LLM_LOCAL;
      target_model = g_config.llm.local.model;
   } else if (strcmp(actionName, "cloud") == 0 || strcmp(actionName, "openai") == 0) {
      target_type = LLM_CLOUD;
      target_provider = CLOUD_PROVIDER_OPENAI;
      target_model = g_config.llm.cloud.openai_model;
   } else if (strcmp(actionName, "claude") == 0) {
      target_type = LLM_CLOUD;
      target_provider = CLOUD_PROVIDER_CLAUDE;
      target_model = g_config.llm.cloud.claude_model;
   }

   /* Check if we need to compact before switching (especially cloud->local) */
   if (conversation_history &&
       llm_context_needs_compaction_for_switch(0, conversation_history, target_type,
                                               target_provider, target_model)) {
      /* Get current LLM settings to perform compaction */
      session_t *session = session_get_command_context();
      if (!session) {
         session = session_get_local();
      }
      session_llm_config_t current_config;
      session_get_llm_config(session, &current_config);

      llm_compaction_result_t compact_result;
      int rc = llm_context_compact_for_switch(0, conversation_history, current_config.type,
                                              current_config.cloud_provider, NULL, target_type,
                                              target_provider, target_model, &compact_result);
      if (rc == 0 && compact_result.performed) {
         LOG_INFO("Pre-switch compaction: %d messages summarized, %d -> %d tokens",
                  compact_result.messages_summarized, compact_result.tokens_before,
                  compact_result.tokens_after);
      }
   }

   if (strcmp(actionName, "local") == 0) {
      /* Route to local LLM callback */
      return localLLMCallback("switch", "", should_respond);
   } else if (strcmp(actionName, "cloud") == 0) {
      /* Route to cloud LLM callback with current provider */
      return cloudLLMCallback("switch", "", should_respond);
   } else if (strcmp(actionName, "openai") == 0) {
      /* Set provider to OpenAI, then switch to cloud */
      char *provider_result = cloudProviderCallback("set", "openai", should_respond);
      if (provider_result) {
         free(provider_result);
      }
      return cloudLLMCallback("switch", "", should_respond);
   } else if (strcmp(actionName, "claude") == 0) {
      /* Set provider to Claude, then switch to cloud */
      char *provider_result = cloudProviderCallback("set", "claude", should_respond);
      if (provider_result) {
         free(provider_result);
      }
      return cloudLLMCallback("switch", "", should_respond);
   } else {
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128, "Unknown LLM target: %s. Use local, cloud, openai, or claude.",
                  actionName);
      }
      return result;
   }
}

char *resetConversationCallback(const char *actionName, char *value, int *should_respond) {
   LOG_INFO("Resetting conversation context via command.");
   reset_conversation();

   *should_respond = 1;
   return strdup("Conversation context has been reset. Starting fresh.");
}

#define SEARCH_RESULT_BUFFER_SIZE 4096

/**
 * @brief Helper function to perform a typed search and format the response
 */
static char *perform_search(const char *value, search_type_t type, const char *type_name) {
   LOG_INFO("searchCallback: Performing %s search for '%s'", type_name, value);

   search_response_t *response = web_search_query_typed(value, SEARXNG_MAX_RESULTS, type);
   if (!response) {
      return strdup("Search request failed.");
   }

   if (response->error) {
      LOG_ERROR("searchCallback: Search error: %s", response->error);
      char *result = malloc(256);
      if (result) {
         snprintf(result, 256, "Search failed: %s", response->error);
      }
      web_search_free_response(response);
      return result ? result : strdup("Search failed.");
   }

   if (response->count > 0) {
      char *result = malloc(SEARCH_RESULT_BUFFER_SIZE);
      if (result) {
         web_search_format_for_llm(response, result, SEARCH_RESULT_BUFFER_SIZE);
         LOG_INFO("searchCallback: Returning %d %s results", response->count, type_name);

         // Run through summarizer if enabled and over threshold
         char *summarized = NULL;
         int sum_result = search_summarizer_process(result, value, &summarized);
         if (sum_result == SUMMARIZER_SUCCESS && summarized) {
            free(result);
            result = summarized;
         } else if (summarized) {
            // Summarizer returned something even on error (passthrough policy)
            free(result);
            result = summarized;
         }
         // If summarizer failed with no output, keep original result
      }

      // Sanitize result to remove invalid UTF-8/control chars before sending to LLM
      if (result) {
         sanitize_utf8_for_json(result);
      }

      web_search_free_response(response);
      return result ? result : strdup("Failed to format search results.");
   }

   web_search_free_response(response);
   char *msg = malloc(128);
   if (msg) {
      snprintf(msg, 128, "No %s results found for '%s'.", type_name, value);
   }
   return msg ? msg : strdup("No results found.");
}

char *searchCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;  // Always return results to LLM

   // Initialize web search module if needed
   if (!web_search_is_initialized()) {
      // Use config endpoint if set, otherwise web_search_init uses SEARXNG_DEFAULT_URL
      const char *endpoint = g_config.search.endpoint[0] != '\0' ? g_config.search.endpoint : NULL;
      if (web_search_init(endpoint) != 0) {
         LOG_ERROR("searchCallback: Failed to initialize web search module");
         return strdup("Web search service is not available.");
      }
   }

   // Determine search type from action name (default to "web" if not specified)
   if (actionName == NULL || actionName[0] == '\0' || strcmp(actionName, "web") == 0) {
      return perform_search(value, SEARCH_TYPE_WEB, "web");
   } else if (strcmp(actionName, "news") == 0) {
      return perform_search(value, SEARCH_TYPE_NEWS, "news");
   } else if (strcmp(actionName, "science") == 0) {
      return perform_search(value, SEARCH_TYPE_SCIENCE, "science");
   } else if (strcmp(actionName, "it") == 0 || strcmp(actionName, "tech") == 0) {
      return perform_search(value, SEARCH_TYPE_IT, "tech");
   } else if (strcmp(actionName, "social") == 0) {
      return perform_search(value, SEARCH_TYPE_SOCIAL, "social");
   } else if (strcmp(actionName, "define") == 0 || strcmp(actionName, "dictionary") == 0) {
      return perform_search(value, SEARCH_TYPE_DICTIONARY, "dictionary");
   } else if (strcmp(actionName, "papers") == 0 || strcmp(actionName, "academic") == 0) {
      return perform_search(value, SEARCH_TYPE_PAPERS, "papers");
   }

   // Fallback to web search for unknown categories (rather than failing)
   LOG_WARNING("searchCallback: Unknown category '%s', defaulting to web search", actionName);
   return perform_search(value, SEARCH_TYPE_WEB, "web");
}

#define WEATHER_RESULT_BUFFER_SIZE 2048  // Increased for week forecast

char *weatherCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;  // Always return results to LLM

   // Determine forecast type from action name
   forecast_type_t forecast = FORECAST_TODAY;  // Default
   int valid_action = 0;

   if (strcmp(actionName, "get") == 0 || strcmp(actionName, "today") == 0) {
      forecast = FORECAST_TODAY;
      valid_action = 1;
   } else if (strcmp(actionName, "tomorrow") == 0) {
      forecast = FORECAST_TOMORROW;
      valid_action = 1;
   } else if (strcmp(actionName, "week") == 0) {
      forecast = FORECAST_WEEK;
      valid_action = 1;
   }

   if (!valid_action) {
      return strdup("Unknown weather action. Use: 'today', 'tomorrow', or 'week' with a location.");
   }

   // Use provided location or fall back to config default
   const char *location = value;
   if (value == NULL || strlen(value) == 0) {
      if (g_config.localization.location[0] != '\0') {
         location = g_config.localization.location;
         LOG_INFO("weatherCallback: Using default location from config: %s", location);
      } else {
         LOG_WARNING("weatherCallback: No location provided and no default configured");
         return strdup("Please specify a location for the weather request.");
      }
   }

   LOG_INFO("weatherCallback: Fetching %s weather for '%s'",
            forecast == FORECAST_WEEK ? "week"
                                      : (forecast == FORECAST_TOMORROW ? "tomorrow" : "today"),
            location);

   weather_response_t *response = weather_get(location, forecast);
   if (response) {
      if (response->error) {
         LOG_ERROR("weatherCallback: Weather error: %s", response->error);
         char *result = malloc(256);
         if (result) {
            snprintf(result, 256, "Weather lookup failed: %s", response->error);
         }
         weather_free_response(response);
         return result ? result : strdup("Weather lookup failed.");
      }

      char *result = malloc(WEATHER_RESULT_BUFFER_SIZE);
      if (result) {
         int formatted_len = weather_format_for_llm(response, result, WEATHER_RESULT_BUFFER_SIZE);
         if (formatted_len < 0 || (size_t)formatted_len >= WEATHER_RESULT_BUFFER_SIZE) {
            LOG_ERROR("weatherCallback: Weather data truncated (needed %d bytes, have %d)",
                      formatted_len, WEATHER_RESULT_BUFFER_SIZE);
            free(result);
            weather_free_response(response);
            return strdup("Weather data too large to format.");
         }
         LOG_INFO("weatherCallback: Weather data retrieved successfully (%d bytes)", formatted_len);
      }
      weather_free_response(response);
      return result ? result : strdup("Failed to format weather data.");
   }

   return strdup("Weather request failed.");
}

char *calculatorCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;  // Always return results to LLM

   if (value == NULL || strlen(value) == 0) {
      LOG_WARNING("calculatorCallback: No value provided");
      return strdup("Please provide a value for the calculator.");
   }

   if (strcmp(actionName, "evaluate") == 0) {
      LOG_INFO("calculatorCallback: Evaluating '%s'", value);
      calc_result_t result = calculator_evaluate(value);
      char *formatted = calculator_format_result(&result);
      if (formatted) {
         LOG_INFO("calculatorCallback: Result = %s", formatted);
         return formatted;
      }
      return strdup("Failed to evaluate expression.");
   }

   if (strcmp(actionName, "convert") == 0) {
      LOG_INFO("calculatorCallback: Converting '%s'", value);
      char *result = calculator_convert(value);
      return result ? result : strdup("Failed to convert units.");
   }

   if (strcmp(actionName, "base") == 0) {
      LOG_INFO("calculatorCallback: Base converting '%s'", value);
      char *result = calculator_base_convert(value);
      return result ? result : strdup("Failed to convert base.");
   }

   if (strcmp(actionName, "random") == 0) {
      LOG_INFO("calculatorCallback: Random number '%s'", value);
      char *result = calculator_random(value);
      return result ? result : strdup("Failed to generate random number.");
   }

   return strdup("Unknown calculator action. Use: evaluate, convert, base, or random.");
}

/**
 * @brief Callback to fetch and extract content from a URL.
 *
 * Fetches the URL, extracts readable text (stripping HTML), and optionally
 * summarizes large content using the search summarizer.
 */
char *urlFetchCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;  // Always return results to LLM

   if (value == NULL || strlen(value) == 0) {
      LOG_WARNING("urlFetchCallback: No URL provided");
      return strdup("Please provide a URL to fetch.");
   }

   // Only accept "get" action (standard for getter-type tools)
   if (strcmp(actionName, "get") != 0) {
      LOG_WARNING("urlFetchCallback: Unknown action '%s'", actionName);
      return strdup("Unknown URL action. Use: get");
   }

   LOG_INFO("urlFetchCallback: Fetching URL '%s'", value);

   // Validate URL
   if (!url_is_valid(value)) {
      LOG_WARNING("urlFetchCallback: Invalid URL '%s'", value);
      return strdup("Invalid URL. Must start with http:// or https://");
   }

   // Fetch and extract content
   char *content = NULL;
   size_t content_size = 0;
   int result = url_fetch_content(value, &content, &content_size);

   if (result != URL_FETCH_SUCCESS) {
      const char *err = url_fetch_error_string(result);
      LOG_WARNING("urlFetchCallback: Fetch failed: %s", err);
      char *msg = malloc(256);
      if (msg) {
         snprintf(msg, 256, "Failed to fetch URL: %s", err);
         return msg;
      }
      return strdup("Failed to fetch URL.");
   }

   LOG_INFO("urlFetchCallback: Extracted %zu bytes of content", content_size);

   // Run through summarizer if enabled and over threshold
   char *summarized = NULL;
   int sum_result = search_summarizer_process(content, value, &summarized);
   if (sum_result == SUMMARIZER_SUCCESS && summarized) {
      free(content);
      content = summarized;
   } else if (summarized) {
      // Summarizer returned something even on error (passthrough policy)
      free(content);
      content = summarized;
   }
   // If summarizer failed with no output, keep original content

   // Hard limit on content size to prevent API errors (e.g., HTTP 400 from too-large requests)
   // Most LLM APIs have context limits; 8000 chars is a safe limit for tool results
   // This limit applies after summarization as a fallback safety measure
#define URL_CONTENT_MAX_CHARS 8000
   if (content && strlen(content) > URL_CONTENT_MAX_CHARS) {
      LOG_WARNING("urlFetchCallback: Content too large (%zu bytes), truncating to %d",
                  strlen(content), URL_CONTENT_MAX_CHARS);
      // Allocate space for truncated content + truncation notice
      char *truncated = malloc(URL_CONTENT_MAX_CHARS + 100);
      if (truncated) {
         strncpy(truncated, content, URL_CONTENT_MAX_CHARS - 50);
         truncated[URL_CONTENT_MAX_CHARS - 50] = '\0';
         strcat(truncated, "\n\n[Content truncated - original was too large]");
         free(content);
         content = truncated;
      } else {
         // If malloc fails, just truncate in place
         content[URL_CONTENT_MAX_CHARS] = '\0';
      }
   }

   // Sanitize content to remove invalid UTF-8/control chars before sending to LLM
   if (content) {
      sanitize_utf8_for_json(content);
   }

   return content;
}

/**
 * @brief SmartThings home automation callback
 *
 * Handles all SmartThings device control actions. Parses the action name
 * and value to determine what operation to perform.
 *
 * Actions:
 * - list: Returns list of all devices
 * - status: Get status of a device (value = device name)
 * - on/off: Turn device on/off (value = device name)
 * - brightness: Set brightness (value = "device name level")
 * - color: Set color (value = "device name color_name" or "device hue sat")
 * - temperature: Set thermostat (value = "device name temp")
 * - lock/unlock: Lock operations (value = device name)
 */
char *smartThingsCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;

   /* Check if service is configured */
   if (!smartthings_is_configured()) {
      return strdup("SmartThings is not configured. Please add client_id and client_secret to "
                    "secrets.toml.");
   }

   /* Check if authenticated */
   if (!smartthings_is_authenticated()) {
      return strdup("SmartThings is not connected. Please authorize via the WebUI settings.");
   }

   /* Action: list - List all devices */
   if (strcmp(actionName, "list") == 0) {
      const st_device_list_t *devices;
      st_error_t err = smartthings_list_devices(&devices);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to list devices: %s", smartthings_error_str(err));
         return msg;
      }

      /* Format device list for LLM */
      char *buf = malloc(8192);
      if (!buf)
         return strdup("Memory allocation failed");

      int len = snprintf(buf, 8192, "Found %d SmartThings devices:\n", devices->count);
      for (int i = 0; i < devices->count && len < 8000; i++) {
         const st_device_t *dev = &devices->devices[i];

         /* Build capability string */
         char caps[256] = "";
         int caps_len = 0;
         for (int j = 0; j < 15 && caps_len < 240; j++) {
            st_capability_t cap = (st_capability_t)(1 << j);
            if (dev->capabilities & cap) {
               if (caps_len > 0)
                  caps_len += snprintf(caps + caps_len, 256 - caps_len, ", ");
               caps_len += snprintf(caps + caps_len, 256 - caps_len, "%s",
                                    smartthings_capability_str(cap));
            }
         }

         len += snprintf(buf + len, 8192 - len, "- %s (%s)\n", dev->label, caps);
      }
      return buf;
   }

   /* Action: status - Get device status */
   if (strcmp(actionName, "status") == 0) {
      if (!value || !value[0])
         return strdup("Please specify a device name.");

      const st_device_t *device;
      st_error_t err = smartthings_find_device(value, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", value);
         return msg;
      }

      st_device_state_t state;
      err = smartthings_get_device_status(device->id, &state);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to get status: %s", smartthings_error_str(err));
         return msg;
      }

      char *buf = malloc(1024);
      int len = snprintf(buf, 1024, "Status of '%s':\n", device->label);

      if (device->capabilities & ST_CAP_SWITCH) {
         len += snprintf(buf + len, 1024 - len, "- Power: %s\n", state.switch_on ? "on" : "off");
      }
      if (device->capabilities & ST_CAP_SWITCH_LEVEL) {
         len += snprintf(buf + len, 1024 - len, "- Brightness: %d%%\n", state.level);
      }
      if (device->capabilities & ST_CAP_COLOR_CONTROL) {
         len += snprintf(buf + len, 1024 - len, "- Color: hue=%d, saturation=%d\n", state.hue,
                         state.saturation);
      }
      if (device->capabilities & ST_CAP_COLOR_TEMP) {
         len += snprintf(buf + len, 1024 - len, "- Color temp: %dK\n", state.color_temp);
      }
      if (device->capabilities & ST_CAP_TEMPERATURE) {
         len += snprintf(buf + len, 1024 - len, "- Temperature: %.1f\n", state.temperature);
      }
      if (device->capabilities & ST_CAP_HUMIDITY) {
         len += snprintf(buf + len, 1024 - len, "- Humidity: %.1f%%\n", state.humidity);
      }
      if (device->capabilities & ST_CAP_LOCK) {
         len += snprintf(buf + len, 1024 - len, "- Lock: %s\n",
                         state.locked ? "locked" : "unlocked");
      }
      if (device->capabilities & ST_CAP_BATTERY) {
         len += snprintf(buf + len, 1024 - len, "- Battery: %d%%\n", state.battery);
      }
      if (device->capabilities & ST_CAP_MOTION) {
         len += snprintf(buf + len, 1024 - len, "- Motion: %s\n",
                         state.motion_active ? "detected" : "none");
      }
      if (device->capabilities & ST_CAP_CONTACT) {
         len += snprintf(buf + len, 1024 - len, "- Contact: %s\n",
                         state.contact_open ? "open" : "closed");
      }
      return buf;
   }

   /* Action: on - Turn device on */
   if (strcmp(actionName, "on") == 0) {
      if (!value || !value[0])
         return strdup("Please specify a device name.");

      const st_device_t *device;
      st_error_t err = smartthings_find_device(value, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", value);
         return msg;
      }

      err = smartthings_switch_on(device->id);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to turn on: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Turned on '%s'", device->label);
      return msg;
   }

   /* Action: off - Turn device off */
   if (strcmp(actionName, "off") == 0) {
      if (!value || !value[0])
         return strdup("Please specify a device name.");

      const st_device_t *device;
      st_error_t err = smartthings_find_device(value, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", value);
         return msg;
      }

      err = smartthings_switch_off(device->id);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to turn off: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Turned off '%s'", device->label);
      return msg;
   }

   /* Action: brightness - Set brightness level */
   if (strcmp(actionName, "brightness") == 0) {
      if (!value || !value[0])
         return strdup("Please specify device name and brightness level (e.g., 'lamp 75').");

      /* Parse "device_name level" from value */
      char device_name[128];
      int level = -1;

      /* Find last number in string */
      char *last_space = strrchr(value, ' ');
      if (last_space && last_space[1]) {
         level = atoi(last_space + 1);
         size_t name_len = last_space - value;
         if (name_len >= sizeof(device_name))
            name_len = sizeof(device_name) - 1;
         strncpy(device_name, value, name_len);
         device_name[name_len] = '\0';
      }

      if (level < 0 || level > 100) {
         return strdup("Please specify device name and brightness (0-100).");
      }

      const st_device_t *device;
      st_error_t err = smartthings_find_device(device_name, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", device_name);
         return msg;
      }

      err = smartthings_set_level(device->id, level);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to set brightness: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Set '%s' brightness to %d%%", device->label, level);
      return msg;
   }

   /* Action: color - Set color */
   if (strcmp(actionName, "color") == 0) {
      if (!value || !value[0])
         return strdup("Please specify device name and color (e.g., 'lamp red' or 'lamp 50 100').");

      /* Try to parse color name or hue/sat values */
      char device_name[128];
      int hue = -1, sat = -1;

      /* Static color name to hue mappings (SmartThings uses 0-100 hue) */
      static const struct {
         const char *name;
         int hue;
         int sat;
      } colors[] = { { "red", 0, 100 },     { "orange", 8, 100 }, { "yellow", 17, 100 },
                     { "green", 33, 100 },  { "cyan", 50, 100 },  { "blue", 67, 100 },
                     { "purple", 75, 100 }, { "pink", 92, 80 },   { "white", 0, 0 } };

      /* Check for color name */
      char *last_word = strrchr(value, ' ');
      if (last_word) {
         for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
            if (strcasecmp(last_word + 1, colors[i].name) == 0) {
               hue = colors[i].hue;
               sat = colors[i].sat;
               size_t name_len = last_word - value;
               if (name_len >= sizeof(device_name))
                  name_len = sizeof(device_name) - 1;
               strncpy(device_name, value, name_len);
               device_name[name_len] = '\0';
               break;
            }
         }
      }

      if (hue < 0) {
         return strdup(
             "Unknown color. Try: red, orange, yellow, green, cyan, blue, purple, pink, white");
      }

      const st_device_t *device;
      st_error_t err = smartthings_find_device(device_name, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", device_name);
         return msg;
      }

      err = smartthings_set_color(device->id, hue, sat);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to set color: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Set '%s' color", device->label);
      return msg;
   }

   /* Action: temperature - Set thermostat */
   if (strcmp(actionName, "temperature") == 0) {
      if (!value || !value[0])
         return strdup("Please specify device name and temperature (e.g., 'thermostat 72').");

      char device_name[128];
      double temp = -1;

      char *last_space = strrchr(value, ' ');
      if (last_space && last_space[1]) {
         temp = atof(last_space + 1);
         size_t name_len = last_space - value;
         if (name_len >= sizeof(device_name))
            name_len = sizeof(device_name) - 1;
         strncpy(device_name, value, name_len);
         device_name[name_len] = '\0';
      }

      if (temp < 50 || temp > 90) {
         return strdup("Please specify a valid temperature (50-90F).");
      }

      const st_device_t *device;
      st_error_t err = smartthings_find_device(device_name, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", device_name);
         return msg;
      }

      err = smartthings_set_thermostat(device->id, temp);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to set temperature: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Set '%s' to %.0f°F", device->label, temp);
      return msg;
   }

   /* Action: lock - Lock a lock device */
   if (strcmp(actionName, "lock") == 0) {
      if (!value || !value[0])
         return strdup("Please specify a lock device name.");

      const st_device_t *device;
      st_error_t err = smartthings_find_device(value, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", value);
         return msg;
      }

      err = smartthings_lock(device->id);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to lock: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Locked '%s'", device->label);
      return msg;
   }

   /* Action: unlock - Unlock a lock device */
   if (strcmp(actionName, "unlock") == 0) {
      if (!value || !value[0])
         return strdup("Please specify a lock device name.");

      const st_device_t *device;
      st_error_t err = smartthings_find_device(value, &device);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Device '%s' not found", value);
         return msg;
      }

      err = smartthings_unlock(device->id);
      if (err != ST_OK) {
         char *msg = malloc(256);
         snprintf(msg, 256, "Failed to unlock: %s", smartthings_error_str(err));
         return msg;
      }

      char *msg = malloc(256);
      snprintf(msg, 256, "Unlocked '%s'", device->label);
      return msg;
   }

   /* Unknown action */
   char *msg = malloc(256);
   snprintf(msg, 256,
            "Unknown SmartThings action '%s'. Supported: list, status, on, off, brightness, "
            "color, temperature, lock, unlock",
            actionName);
   return msg;
}

/* End Mosquitto Stuff */
