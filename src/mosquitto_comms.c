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
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* JSON-C */
#include <json-c/json.h>

/* Local */
#include "config/dawn_config.h"
#include "conversation_manager.h"
#include "core/command_router.h"
#include "core/component_status.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "input_queue.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/hud_discovery.h"
#ifdef DAWN_ENABLE_PHONE_TOOL
#include "tools/phone_service.h"
#endif
#ifdef DAWN_ENABLE_STAT_TOOL
#include "core/stat_service.h"
#endif
#ifdef DAWN_ENABLE_SUIT_TOOL
#include "core/suit_service.h"
#endif
#include "tools/tool_registry.h"
#include "tts/text_to_speech.h"
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"
#include "utils/string_utils.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif


typedef char *(*device_callback_fn)(const char *actionName, char *value, int *should_respond);

static device_callback_fn get_device_callback(const char *device_name) {
   if (!device_name) {
      return NULL;
   }
   return (device_callback_fn)tool_registry_get_callback(device_name);
}

static char *pending_command_result = NULL;

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

   char gpt_response[GPT_RESPONSE_BUFFER_SIZE];

   const char *deviceName = NULL;
   const char *actionName = NULL;
   const char *value = NULL;

   char *callback_result = NULL;
   int should_respond = 0;

   // Get the "device" object from the JSON
   if (json_object_object_get_ex(parsedJson, "device", &deviceObject)) {
      // Extract the text value as a C string
      deviceName = json_object_get_string(deviceObject);
      if (deviceName == NULL) {
         OLOG_ERROR("Error: Unable to get device name from json command.");
         return;
      }
   } else {
      OLOG_ERROR("Error: 'device' field not found in JSON.");
      return;
   }

   // Get the "action" object from the JSON
   if (json_object_object_get_ex(parsedJson, "action", &actionObject)) {
      // Extract the text value as a C string
      actionName = json_object_get_string(actionObject);
      if (actionName == NULL) {
         OLOG_ERROR("Error: Unable to get action name from json command.");
         return;
      }
   } else {
      OLOG_ERROR("Error: 'action' field not found in JSON.");
      return;
   }

   // Get the "value" object from the JSON, not required for all commands
   if (json_object_object_get_ex(parsedJson, "value", &valueObject)) {
      // Extract the text value as a C string
      value = json_object_get_string(valueObject);
      if (value == NULL) {
         OLOG_WARNING("Notice: Unable to get value name from json command.");
      }
   }

   /* Before we process, make sure nothing's left over. */
   if (pending_command_result != NULL) {
      free(pending_command_result);
      pending_command_result = NULL;
   }

   /* Look up callback for this device type */
   device_callback_fn callback = get_device_callback(deviceName);
   if (callback) {
      callback_result = callback(actionName, (char *)value, &should_respond);
      /* Strip the opt-in tool error-marker before the result reaches the AI. */
      tool_result_strip_error_mark(callback_result);

      // If in AI mode and callback returned data, store it for AI response
      if (callback_result != NULL && should_respond &&
          (command_processing_mode == CMD_MODE_LLM_ONLY ||
           command_processing_mode == CMD_MODE_DIRECT_FIRST)) {
         size_t dest_len = (pending_command_result == NULL) ? 0 : strlen(pending_command_result);
         size_t src_len = strlen(callback_result);

         // Resize memory to fit both strings plus space and null terminator
         char *temp = realloc(pending_command_result, dest_len + src_len + 2);
         if (temp == NULL) {
            free(pending_command_result);
            pending_command_result = NULL;
            free(callback_result);
         } else {
            pending_command_result = temp;

            // Copy the new string to the end
            strcpy(pending_command_result + dest_len, " ");
            strcpy(pending_command_result + dest_len + 1, callback_result);
         }
      }

      // Free callback result (callbacks return heap-allocated strings)
      if (callback_result) {
         free(callback_result);
         callback_result = NULL;
      }
   }

   OLOG_INFO("Command result for AI: %s",
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

   /* Route the device-data relay through the main input queue rather than running
    * an LLM turn here on the MQTT background thread. This keeps the MQTT background
    * thread out of the local session's conversation_history — removing that
    * cross-thread writer. (The phone call-state broadcaster is a separate remaining
    * writer, closed by the tracked "session owns history under history_mutex"
    * refactor.) The main loop
    * drains the queue in SILENCE (promptly when idle, deferred until the current
    * turn finishes if one is in flight), then speaks the result via TTS.
    *
    * Behavior notes (relay now flows through the normal turn path as a user-role
    * input): it gains per-turn memory/focus context + native tool-calling, is
    * subject to utterance dedup, and lands in the session history that feeds
    * end-of-session memory extraction. Device data on MQTT is operator-trusted
    * (see the MQTT-auth hardening TODO); the explicit [DEVICE DATA] delimiter is
    * retained. A burst of relays during one long turn can exceed the 8-slot queue
    * (drop-oldest). Revisit tool-scoping / extraction-tagging for relays if the
    * MQTT LOCAL PATH sees heavy use. */
   (void)mosq; /* command chaining + TTS now handled by the main pipeline */
   snprintf(gpt_response, sizeof(gpt_response),
            "[DEVICE DATA] Speak this information naturally to the user: %s",
            pending_command_result);
   input_queue_push(INPUT_SOURCE_MQTT, gpt_response);

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
      safe_strscpy(preview, input);
      OLOG_ERROR("Unable to parse MQTT JSON command. Payload preview: %.200s%s", preview,
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

   OLOG_INFO("MQTT Connecting.");

   if (reason_code != 0) {
      OLOG_WARNING("MQTT disconnecting?");
      mosquitto_disconnect(mosq);
      return;
   }

   // Subscribe in the on_connect callback
   rc = mosquitto_subscribe(mosq, NULL, APPLICATION_NAME, 0);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("Error on mosquitto_subscribe(): %s", mosquitto_strerror(rc));
   } else {
      OLOG_INFO("Subscribed to \"%s\" MQTT.", APPLICATION_NAME);
   }

   /* Initialize component status (subscribes to hud/status, publishes dawn/status) */
   if (component_status_init(mosq) != 0) {
      OLOG_WARNING("Component status initialization failed");
   }

   /* Subscribe to ECHO modem topics for phone service */
#ifdef DAWN_ENABLE_PHONE_TOOL
   mosquitto_subscribe(mosq, NULL, "echo/events", 1);
   mosquitto_subscribe(mosq, NULL, "echo/response", 1);
   mosquitto_subscribe(mosq, NULL, "echo/status", 1);
   OLOG_INFO("Subscribed to echo/events, echo/response, echo/status");
#endif

   /* Subscribe to STAT telemetry (QoS 0, matches STAT's publish) + status
    * (QoS 1, retained/LWT).  Only when the service is active (enabled + inited). */
#ifdef DAWN_ENABLE_STAT_TOOL
   if (stat_service_is_active()) {
      mosquitto_subscribe(mosq, NULL, stat_service_telemetry_topic(), 0);
      mosquitto_subscribe(mosq, NULL, stat_service_status_topic(), 1);
      OLOG_INFO("Subscribed to %s (telemetry), %s (status)", stat_service_telemetry_topic(),
                stat_service_status_topic());
   }
#endif

   /* Subscribe to the AURA helmet + SPARK armor telemetry MIRAGE republishes
    * (QoS 0, matches the publisher).  Only when the service is active. */
#ifdef DAWN_ENABLE_SUIT_TOOL
   if (suit_service_is_active()) {
      mosquitto_subscribe(mosq, NULL, suit_service_helmet_topic(), 0);
      mosquitto_subscribe(mosq, NULL, suit_service_armor_topic(), 0);
      OLOG_INFO("Subscribed to %s (helmet), %s (armor)", suit_service_helmet_topic(),
                suit_service_armor_topic());
   }
#endif

   /* Initialize HUD discovery (subscribes to hud/discovery/# and requests state) */
   if (hud_discovery_init(mosq) != 0) {
      OLOG_WARNING("HUD discovery initialization failed - using defaults");
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

   OLOG_INFO("MQTT subscribed.");

   for (i = 0; i < qos_count; i++) {
      if (granted_qos[i] <= 2) {
         have_subscription = true;
      }
   }
   if (have_subscription == false) {
      OLOG_ERROR("Error: All subscriptions rejected.");
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
      OLOG_ERROR("Worker command missing 'device' field");
      command_router_deliver(request_id, "");
      return;
   }
   deviceName = json_object_get_string(deviceObject);

   // Get the "action" object from the JSON
   if (!json_object_object_get_ex(parsed_json, "action", &actionObject)) {
      OLOG_ERROR("Worker command missing 'action' field");
      command_router_deliver(request_id, "");
      return;
   }
   actionName = json_object_get_string(actionObject);

   // Get the "value" object (optional)
   if (json_object_object_get_ex(parsed_json, "value", &valueObject)) {
      value = json_object_get_string(valueObject);
   }

   OLOG_INFO("Executing command for worker: device=%s, action=%s, request_id=%s", deviceName,
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

         OLOG_ERROR("OCP error response from %s: [%s] %s", deviceName, error_code, error_message);

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
                  OLOG_ERROR("OCP: Rejecting viewing response due to checksum mismatch");
                  command_router_deliver(request_id, "");
                  return;
               }

               OLOG_INFO("Viewing response contains inline data, delivering directly");
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
               OLOG_ERROR("OCP: Rejecting viewing response due to file checksum mismatch");
               command_router_deliver(request_id, "");
               return;
            }
         }
      }
      OLOG_INFO("Viewing response using file path: %s", value ? value : "(null)");
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

   // Look up and execute callback for this device type
   device_callback_fn dev_callback = get_device_callback(deviceName);
   if (dev_callback) {
      callback_result = dev_callback(actionName, (char *)value, &should_respond);
      /* Strip the opt-in tool error-marker before the result reaches the AI. */
      tool_result_strip_error_mark(callback_result);
   }

   // Clear command context and release session reference
   session_set_command_context(NULL);
   if (session) {
      session_release(session);
   }

   // Deliver result to waiting worker
   if (callback_result && should_respond) {
      command_router_deliver(request_id, callback_result);
      OLOG_INFO("Delivered result to worker: %s", callback_result);
   } else {
      // Command executed but no data returned
      command_router_deliver(request_id, "");
      OLOG_INFO("Delivered empty result to worker (command executed, no data)");
   }

   // Free callback result (callbacks return heap-allocated strings)
   if (callback_result) {
      free(callback_result);
   }
}

/* Callback called when the client receives a message. */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
#ifdef DAWN_ENABLE_STAT_TOOL
   /* STAT telemetry is a multi-Hz firehose — consume it BEFORE the per-message
    * INFO log below so it does not flood the log file. */
   if (stat_service_handle_mqtt(msg->topic, (const char *)msg->payload, msg->payloadlen)) {
      return;
   }
#endif

#ifdef DAWN_ENABLE_SUIT_TOOL
   /* Suit telemetry (helmet/armor) — consume BEFORE the per-message INFO log so
    * the streaming feed does not flood the log file. */
   if (suit_service_handle_mqtt(msg->topic, (const char *)msg->payload, msg->payloadlen)) {
      return;
   }
#endif

   OLOG_INFO("%s %d %s", msg->topic, msg->qos, (char *)msg->payload);

   /* Check for component status messages (hud/status) */
   if (strcmp(msg->topic, STATUS_TOPIC_HUD) == 0) {
      component_status_handle_message(msg->topic, (const char *)msg->payload, msg->payloadlen);
      return;
   }

   /* Route ECHO modem daemon messages to phone service */
#ifdef DAWN_ENABLE_PHONE_TOOL
   if (strcmp(msg->topic, "echo/events") == 0 || strcmp(msg->topic, "echo/status") == 0) {
      phone_service_handle_event((const char *)msg->payload, msg->payloadlen);
      return;
   }
   if (strcmp(msg->topic, "echo/response") == 0) {
      /* Deliver response to waiting command_router slot by request_id */
      struct json_object *resp_json = json_tokener_parse((const char *)msg->payload);
      if (resp_json) {
         struct json_object *j_reqid, *j_status;
         if (json_object_object_get_ex(resp_json, "request_id", &j_reqid)) {
            const char *rid = json_object_get_string(j_reqid);
            const char *status = "success";
            if (json_object_object_get_ex(resp_json, "status", &j_status)) {
               status = json_object_get_string(j_status);
            }
            command_router_deliver(rid, status);
         }
         json_object_put(resp_json);
      }
      return;
   }
#endif

   /* Check for HUD discovery messages (hud/discovery/#) */
   if (strncmp(msg->topic, "hud/discovery/", 14) == 0) {
      /* Skip our own discovery requests (we publish these, don't need to process) */
      if (strcmp(msg->topic, "hud/discovery/request") == 0) {
         return;
      }
      hud_discovery_handle_message(msg->topic, (const char *)msg->payload, msg->payloadlen);
      return;
   }

   // Parse the JSON to check for request_id
   struct json_object *parsed_json = json_tokener_parse((char *)msg->payload);
   if (parsed_json == NULL) {
      OLOG_ERROR("Failed to parse MQTT message as JSON");
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

/* Legacy music code removed — all music playback handled by src/tools/music_tool.c */
