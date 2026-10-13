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
 * Unified Command Executor
 *
 * This module provides a single entry point for command execution across
 * all three command paths:
 * 1. Direct pattern matching (text_to_command_nuevo.c)
 * 2. <command> tag parsing (llm_command_parser.c)
 * 3. Native tool calling (llm_tools.c)
 *
 * Execution logic:
 * - If command has a callback in tool_registry, invoke it directly
 * - If command is MQTT-only (hardware), publish to configured topic
 * - If command requires sync_wait (viewing), use command_router for response
 */

#ifndef COMMAND_EXECUTOR_H
#define COMMAND_EXECUTOR_H

#include <json-c/json.h>
#include <mosquitto.h>
#include <stdbool.h>

#include "tools/tool_registry.h" /* for tool_metadata_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Execution Result
 * ============================================================================= */

/**
 * @brief Result of command execution
 *
 * TTS Feedback Pattern:
 * When should_respond is true and result is non-NULL, the CALLER is responsible
 * for providing TTS feedback (e.g., via text_to_speech(result)). Callbacks should
 * NOT call TTS directly - they return data via result and set should_respond=1.
 * This allows consistent handling across all command execution paths (direct
 * commands, LLM tool calls, MQTT commands).
 */
typedef struct {
   char *result;        /**< Execution result text (caller must free) */
   bool success;        /**< true if execution succeeded */
   bool should_respond; /**< true if caller should provide TTS feedback for result */
   bool skip_followup;  /**< true if LLM follow-up should be skipped */
   bool tool_error;     /**< The tool marked its result a hard failure
                             (TOOL_RESULT_ERROR_MARK, stripped from @ref result) */
} cmd_exec_result_t;

/* =============================================================================
 * Execution Functions
 * ============================================================================= */

/**
 * @brief Execute a command from any path
 *
 * This is the unified entry point for all command execution. It:
 * 1. Looks up the command in the registry
 * 2. If has_callback: invokes via tool_registry
 * 3. If mqtt_only: publishes to configured topic
 * 4. If sync_wait: uses command_router for synchronous response
 *
 * @param device Device name (from JSON, pattern match, or tool parameter)
 * @param action Action name (e.g., "get", "enable", "set")
 * @param value Optional value parameter (can be NULL)
 * @param mosq MQTT client (can be NULL for callback-only commands)
 * @param result Output execution result (caller must call cmd_exec_result_free)
 * @return 0 on success, non-zero on error
 */
int command_execute(const char *device,
                    const char *action,
                    const char *value,
                    struct mosquitto *mosq,
                    cmd_exec_result_t *result);

/**
 * @brief Publish an MQTT command using caller-provided tool metadata
 *
 * Unlike command_execute(), this skips the device-name registry lookup. Use
 * it when the caller has already resolved the outer tool (e.g., 'hud_control',
 * 'recording') and wants to pass a "device" value that is NOT itself a
 * registered tool — typically an element name or mode string that MIRAGE or
 * another subscriber will route on.
 *
 * Without this path, tools that use TOOL_MAPS_TO_DEVICE on a param whose
 * values aren't individually registered (e.g., hud_control.element =
 * "altitude", recording.mode = "record") fail with "Unknown device" even
 * though the wire contract expects the element/mode name in the `device`
 * field of the MQTT payload.
 *
 * @param tool   Already-resolved outer tool metadata (non-NULL). Provides
 *               the MQTT topic and default-action logic.
 * @param device String to place in the JSON `device` field (non-NULL).
 *               This is an opaque pass-through value, not a registry key.
 * @param action Action to place in the JSON `action` field (may be empty;
 *               default is derived from tool->device_type).
 * @param value  Optional value for the JSON `value` field (may be NULL/empty).
 * @param mosq   MQTT client (required).
 * @param result Output result (caller must call cmd_exec_result_free).
 * @return 0 on success, non-zero on error.
 */
int command_execute_mqtt_direct(const tool_metadata_t *tool,
                                const char *device,
                                const char *action,
                                const char *value,
                                struct mosquitto *mosq,
                                cmd_exec_result_t *result);

/**
 * @brief Execute from parsed JSON command
 *
 * Convenience wrapper that extracts device/action/value from JSON object.
 * Expected JSON format: {"device": "...", "action": "...", "value": "..."}
 *
 * @param cmd_json Parsed JSON command object
 * @param mosq MQTT client (can be NULL for callback-only commands)
 * @param result Output execution result
 * @return 0 on success, non-zero on error
 */
int command_execute_json(struct json_object *cmd_json,
                         struct mosquitto *mosq,
                         cmd_exec_result_t *result);

/**
 * @brief Free execution result resources
 *
 * Frees the result string if allocated. Safe to call with NULL result.
 *
 * @param result Result to free
 */
void cmd_exec_result_free(cmd_exec_result_t *result);

/* =============================================================================
 * Synchronous Execution (for sync_wait commands like viewing)
 * ============================================================================= */

/**
 * @brief Execute a command synchronously via MQTT with response wait
 *
 * Sends the command via MQTT and waits for a response using command_router.
 * Used for commands like "viewing" that need to wait for external data.
 *
 * @param device Device name
 * @param action Action name
 * @param value Optional value parameter
 * @param mosq MQTT client (required)
 * @param topic MQTT topic to publish to
 * @param result Output execution result
 * @param timeout_ms Timeout in milliseconds (0 = default)
 * @return 0 on success, non-zero on error or timeout
 */
int command_execute_sync(const char *device,
                         const char *action,
                         const char *value,
                         struct mosquitto *mosq,
                         const char *topic,
                         cmd_exec_result_t *result,
                         int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_EXECUTOR_H */
