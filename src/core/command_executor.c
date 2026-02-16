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
 * Unified Command Executor Implementation
 *
 * Provides a single entry point for command execution across all three
 * command paths (direct, <command> tags, native tools).
 */

#include "core/command_executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command_router.h"
#include "core/ocp_helpers.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* Default timeout for sync_wait commands (e.g., viewing) */
#define DEFAULT_SYNC_TIMEOUT_MS 10000

/* Worker ID used for command executor sync requests */
#define EXECUTOR_WORKER_ID 99

/**
 * @brief Get default action based on tool's device_type
 *
 * Provides consistent default action derivation for both mqtt_only
 * and callback execution paths.
 *
 * @param tool Tool metadata
 * @return Default action string (static, do not free)
 */
static const char *get_default_action(const tool_metadata_t *tool) {
   if (!tool)
      return "get";

   switch (tool->device_type) {
      case TOOL_DEVICE_TYPE_BOOLEAN:
         return "toggle";
      case TOOL_DEVICE_TYPE_ANALOG:
         return "set";
      case TOOL_DEVICE_TYPE_GETTER:
         return "get";
      case TOOL_DEVICE_TYPE_TRIGGER:
         return "trigger";
      case TOOL_DEVICE_TYPE_MUSIC:
         return "play";
      default:
         return "get";
   }
}

/* =============================================================================
 * Synchronous Execution (for sync_wait commands)
 * ============================================================================= */

int command_execute_sync(const char *device,
                         const char *action,
                         const char *value,
                         struct mosquitto *mosq,
                         const char *topic,
                         cmd_exec_result_t *result,
                         int timeout_ms) {
   if (!mosq) {
      result->success = false;
      result->result = strdup("MQTT client not available for sync command");
      return 1;
   }

   if (timeout_ms <= 0) {
      timeout_ms = DEFAULT_SYNC_TIMEOUT_MS;
   }

   /* Register with command_router for response */
   pending_request_t *req = command_router_register(EXECUTOR_WORKER_ID);
   if (!req) {
      result->success = false;
      result->result = strdup("Could not register for response");
      return 1;
   }
   const char *request_id = command_router_get_id(req);

   /* Build the JSON command with request_id and timestamp (OCP format) */
   struct json_object *cmd = json_object_new_object();
   json_object_object_add(cmd, "device", json_object_new_string(device));
   json_object_object_add(cmd, "action", json_object_new_string(action ? action : "get"));
   if (value && value[0] != '\0') {
      json_object_object_add(cmd, "value", json_object_new_string(value));
   }
   json_object_object_add(cmd, "request_id", json_object_new_string(request_id));

   /* Add timestamp (OCP v1.1) */
   json_object_object_add(cmd, "timestamp", json_object_new_int64(ocp_get_timestamp_ms()));

   const char *cmd_str = json_object_to_json_string(cmd);
   int rc = mosquitto_publish(mosq, NULL, topic, strlen(cmd_str), cmd_str, 0, false);
   json_object_put(cmd);

   if (rc != MOSQ_ERR_SUCCESS) {
      command_router_cancel(req);
      result->success = false;
      char errbuf[256];
      snprintf(errbuf, sizeof(errbuf), "Failed to publish: %s", mosquitto_strerror(rc));
      result->result = strdup(errbuf);
      return 1;
   }

   LOG_INFO("command_executor: Sync command sent (request_id=%s), waiting...", request_id);

   /* Wait for response via command_router */
   char *response = command_router_wait(req, timeout_ms);
   if (!response || response[0] == '\0') {
      result->success = false;
      result->result = strdup("Timeout waiting for response");
      if (response) {
         free(response);
      }
      return 1;
   }

   result->success = true;
   result->result = response; /* Takes ownership */
   result->should_respond = true;

   LOG_INFO("command_executor: Sync response received for '%s'", device);
   return 0;
}

/* =============================================================================
 * Main Execution Function
 * ============================================================================= */

int command_execute(const char *device,
                    const char *action,
                    const char *value,
                    struct mosquitto *mosq,
                    cmd_exec_result_t *result) {
   if (!result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));

   if (!device || device[0] == '\0') {
      result->success = false;
      result->result = strdup("Device name is required");
      return 1;
   }

   /* Look up in tool_registry */
   const tool_metadata_t *tool = tool_registry_find(device);
   if (!tool) {
      LOG_WARNING("command_execute: Unknown device '%s'", device);
      result->success = false;
      char errbuf[256];
      snprintf(errbuf, sizeof(errbuf), "Unknown device: %s", device);
      result->result = strdup(errbuf);
      return 1;
   }

   result->skip_followup = tool->skip_followup;

   /* Check for sync_wait tools (need command_router for response) */
   if (tool->sync_wait && tool->topic) {
      cmd_exec_result_t sync_result;
      int rc = command_execute_sync(device, action, value, mosq, tool->topic, &sync_result, 0);
      *result = sync_result;
      return rc;
   }

   /* Check for mqtt_only tools - publish to MQTT instead of calling callback */
   if (tool->mqtt_only && tool->topic) {
      if (!mosq) {
         result->success = false;
         result->result = strdup("MQTT client not available for hardware command");
         return 1;
      }

      /* Determine default action based on device_type when action is empty */
      const char *effective_action = (action && action[0] != '\0') ? action
                                                                   : get_default_action(tool);

      struct json_object *cmd_json = json_object_new_object();
      json_object_object_add(cmd_json, "device", json_object_new_string(device));
      json_object_object_add(cmd_json, "action", json_object_new_string(effective_action));
      if (value && value[0] != '\0') {
         json_object_object_add(cmd_json, "value", json_object_new_string(value));
      }

      const char *cmd_str = json_object_to_json_string(cmd_json);
      int rc = mosquitto_publish(mosq, NULL, tool->topic, strlen(cmd_str), cmd_str, 0, false);
      json_object_put(cmd_json);

      if (rc != MOSQ_ERR_SUCCESS) {
         result->success = false;
         char errbuf[256];
         snprintf(errbuf, sizeof(errbuf), "MQTT publish failed: %s", mosquitto_strerror(rc));
         result->result = strdup(errbuf);
         return 1;
      }

      result->success = true;
      result->result = NULL; /* Fire-and-forget has no response */
      result->should_respond = false;

      LOG_INFO("command_execute: MQTT published to '%s' for tool '%s'", tool->topic, device);
      return 0;
   }

   /* Standard callback execution */
   if (tool->callback) {
      int should_respond = 0;
      /* Copy value to mutable buffer (callback signature requires char*, not const char*) */
      char value_buf[4096];
      safe_strncpy(value_buf, value ? value : "", sizeof(value_buf));
      const char *effective_action = (action && action[0] != '\0') ? action
                                                                   : get_default_action(tool);
      char *cb_result = tool->callback(effective_action, value_buf, &should_respond);

      result->success = true;
      result->should_respond = (should_respond != 0);
      result->result = cb_result; /* Takes ownership (may be NULL) */

      LOG_INFO("command_execute: Tool callback for '%s' -> %s", device,
               result->result ? result->result : "(no result)");
      return 0;
   }

   /* Tool has no callback and is not mqtt_only - this shouldn't happen */
   LOG_WARNING("command_execute: Tool '%s' has no callback or topic", device);
   result->success = false;
   result->result = strdup("Tool has no execution path configured");
   return 1;
}

/* =============================================================================
 * JSON Convenience Wrapper
 * ============================================================================= */

int command_execute_json(struct json_object *cmd_json,
                         struct mosquitto *mosq,
                         cmd_exec_result_t *result) {
   if (!cmd_json || !result) {
      if (result) {
         result->success = false;
         result->result = strdup("Invalid arguments");
      }
      return 1;
   }

   /* Extract device */
   struct json_object *device_obj = NULL;
   if (!json_object_object_get_ex(cmd_json, "device", &device_obj)) {
      result->success = false;
      result->result = strdup("Missing 'device' field in command");
      return 1;
   }
   const char *device = json_object_get_string(device_obj);

   /* Extract action (optional) */
   const char *action = NULL;
   struct json_object *action_obj = NULL;
   if (json_object_object_get_ex(cmd_json, "action", &action_obj)) {
      action = json_object_get_string(action_obj);
   }

   /* Extract value (optional) */
   const char *value = NULL;
   struct json_object *value_obj = NULL;
   if (json_object_object_get_ex(cmd_json, "value", &value_obj)) {
      value = json_object_get_string(value_obj);
   }

   return command_execute(device, action, value, mosq, result);
}

/* =============================================================================
 * Result Cleanup
 * ============================================================================= */

void cmd_exec_result_free(cmd_exec_result_t *result) {
   if (!result) {
      return;
   }
   if (result->result) {
      free(result->result);
      result->result = NULL;
   }
}
