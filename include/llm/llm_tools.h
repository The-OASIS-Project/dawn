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
 * Native LLM Tool/Function Calling Support
 *
 * This module provides native tool calling support for OpenAI, Claude, and
 * local LLMs (via llama.cpp with --jinja flag). Tools are defined once and
 * converted to provider-specific formats (OpenAI functions vs Claude tools).
 *
 * Tool calling reduces system prompt size by ~70% and improves reliability
 * by using structured responses instead of parsing <command> tags from text.
 */

#ifndef LLM_TOOLS_H
#define LLM_TOOLS_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/command_registry.h"
#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define LLM_TOOLS_MAX_TOOLS 32         /* Maximum number of tools */
#define LLM_TOOLS_MAX_PARAMS 8         /* Maximum parameters per tool */
#define LLM_TOOLS_MAX_ENUM_VALUES 16   /* Maximum enum values per parameter */
#define LLM_TOOLS_MAX_PARALLEL_CALLS 8 /* Maximum parallel tool calls */
#define LLM_TOOLS_MAX_ITERATIONS 5     /* Maximum tool execution loop iterations */

#define LLM_TOOLS_NAME_LEN 64
#define LLM_TOOLS_DESC_LEN 512
#define LLM_TOOLS_ID_LEN 64
#define LLM_TOOLS_ARGS_LEN 4096
#define LLM_TOOLS_RESULT_LEN 8192

/* =============================================================================
 * Tool Definition Structures
 *
 * Note: Parameter types use cmd_param_type_t from command_registry.h
 * ============================================================================= */

/**
 * @brief Tool parameter definition
 *
 * Uses cmd_param_type_t from command_registry.h for type field to maintain
 * a single source of truth for parameter types.
 */
typedef struct {
   char name[LLM_TOOLS_NAME_LEN];                   /**< Parameter name */
   char description[256];                           /**< Parameter description */
   cmd_param_type_t type;                           /**< Parameter type (from command_registry.h) */
   bool required;                                   /**< Is this parameter required? */
   char enum_values[LLM_TOOLS_MAX_ENUM_VALUES][64]; /**< Allowed values for ENUM type */
   int enum_count;                                  /**< Number of enum values */
} tool_param_t;

/**
 * @brief Tool definition
 *
 * Defines a tool that can be called by the LLM. Maps to existing
 * deviceCallback functions in mosquitto_comms.c.
 */
typedef struct {
   char name[LLM_TOOLS_NAME_LEN];                 /**< Tool name (e.g., "weather") */
   char description[LLM_TOOLS_DESC_LEN];          /**< Tool description for LLM */
   tool_param_t parameters[LLM_TOOLS_MAX_PARAMS]; /**< Parameter definitions */
   int param_count;                               /**< Number of parameters */
   bool enabled;                                  /**< Runtime enable/disable */
   const char *device_name;                       /**< Mapped device name for callback */
} tool_definition_t;

/* =============================================================================
 * Tool Call Structures (from LLM response)
 * ============================================================================= */

/**
 * @brief Single tool call from LLM response
 *
 * Represents a tool invocation requested by the LLM. The id is used to
 * correlate results back to the correct tool call (important for parallel calls).
 */
typedef struct {
   char id[LLM_TOOLS_ID_LEN];          /**< Tool call ID (for response correlation) */
   char name[LLM_TOOLS_NAME_LEN];      /**< Tool name (maps to device type) */
   char arguments[LLM_TOOLS_ARGS_LEN]; /**< JSON arguments string */
} tool_call_t;

/**
 * @brief List of tool calls (for parallel invocation)
 *
 * LLMs can request multiple tool calls in a single response. This structure
 * holds all pending calls that need to be executed.
 */
typedef struct {
   tool_call_t calls[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int count;
} tool_call_list_t;

/**
 * @brief Tool execution result
 *
 * Contains the result of executing a tool, to be sent back to the LLM.
 */
typedef struct {
   char tool_call_id[LLM_TOOLS_ID_LEN]; /**< ID from original tool_call_t */
   char result[LLM_TOOLS_RESULT_LEN];   /**< Execution result text */
   bool success;                        /**< true if execution succeeded */
   bool skip_followup;       /**< If true, return result directly without LLM follow-up */
   char *vision_image;       /**< Base64 vision image (caller must free) */
   size_t vision_image_size; /**< Size of vision image data */
} tool_result_t;

/**
 * @brief List of tool results (for parallel execution)
 */
typedef struct {
   tool_result_t results[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int count;
} tool_result_list_t;

/* =============================================================================
 * LLM Response with Tool Calls
 * ============================================================================= */

/**
 * @brief Extended LLM response that may contain tool calls
 *
 * When the LLM decides to use tools, the response contains tool_calls
 * instead of (or in addition to) text content.
 */
typedef struct {
   char *text;                  /**< Text content (may be NULL if only tools) */
   tool_call_list_t tool_calls; /**< Tool calls requested by LLM */
   bool has_tool_calls;         /**< true if tool_calls.count > 0 */
   char finish_reason[32];      /**< "stop", "tool_calls", "tool_use", etc. */
} llm_tool_response_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize tool definitions
 *
 * Registers all available tools based on the deviceCallback system.
 * Should be called during llm_init() after config is loaded.
 */
void llm_tools_init(void);

/**
 * @brief Refresh tool availability based on current config
 *
 * Call when capabilities change at runtime (e.g., SmartThings authenticates,
 * search endpoint configured, etc.) to update which tools are enabled.
 */
void llm_tools_refresh(void);

/**
 * @brief Clean up tool resources
 */
void llm_tools_cleanup(void);

/* =============================================================================
 * Tool Schema Generation
 * ============================================================================= */

/**
 * @brief Generate tools array in OpenAI format
 *
 * Creates a JSON array suitable for the OpenAI API "tools" parameter.
 * Only includes enabled tools.
 *
 * @return JSON array (caller must json_object_put), or NULL if no tools
 *
 * Example output:
 * [
 *   {
 *     "type": "function",
 *     "function": {
 *       "name": "weather",
 *       "description": "Get weather forecast",
 *       "parameters": {
 *         "type": "object",
 *         "properties": {...},
 *         "required": [...]
 *       }
 *     }
 *   }
 * ]
 */
struct json_object *llm_tools_get_openai_format(void);

/**
 * @brief Generate tools array in Claude format
 *
 * Creates a JSON array suitable for the Claude API "tools" parameter.
 * Only includes enabled tools.
 *
 * @return JSON array (caller must json_object_put), or NULL if no tools
 *
 * Example output:
 * [
 *   {
 *     "name": "weather",
 *     "description": "Get weather forecast",
 *     "input_schema": {
 *       "type": "object",
 *       "properties": {...},
 *       "required": [...]
 *     }
 *   }
 * ]
 */
struct json_object *llm_tools_get_claude_format(void);

/* =============================================================================
 * Tool Execution
 * ============================================================================= */

/**
 * @brief Execute a single tool call
 *
 * Maps the tool call to the appropriate deviceCallback and executes it.
 * The result is formatted for returning to the LLM.
 *
 * @param call Tool call to execute
 * @param result Output: execution result
 * @return 0 on success, non-zero on error
 */
int llm_tools_execute(const tool_call_t *call, tool_result_t *result);

/**
 * @brief Execute multiple tool calls
 *
 * Executes all tool calls in the list. Currently sequential, but could
 * be parallelized in the future for independent tools.
 *
 * @param calls List of tool calls to execute
 * @param results Output: execution results
 * @return 0 if all succeeded, non-zero if any failed
 */
int llm_tools_execute_all(const tool_call_list_t *calls, tool_result_list_t *results);

/**
 * @brief Check if follow-up LLM call should be skipped
 *
 * Some tool executions (like switching LLM providers) should not trigger
 * a follow-up call because the credentials have changed.
 *
 * @param results Tool execution results to check
 * @return true if any result has skip_followup set
 */
bool llm_tools_should_skip_followup(const tool_result_list_t *results);

/**
 * @brief Get the result text from tool results for direct response
 *
 * When skip_followup is set, this formats the tool results as a direct
 * response to the user instead of sending to the LLM.
 *
 * @param results Tool execution results
 * @return Allocated string (caller must free), or NULL on error
 */
char *llm_tools_get_direct_response(const tool_result_list_t *results);

/* =============================================================================
 * Tool Result Formatting (for conversation history)
 * ============================================================================= */

/**
 * @brief Add tool results to conversation history (OpenAI format)
 *
 * Adds tool role messages to the conversation for OpenAI API.
 *
 * @param history Conversation history array
 * @param results Tool execution results
 * @return 0 on success
 */
int llm_tools_add_results_openai(struct json_object *history, const tool_result_list_t *results);

/**
 * @brief Add tool results to conversation history (Claude format)
 *
 * Adds tool_result content blocks to the conversation for Claude API.
 *
 * @param history Conversation history array
 * @param results Tool execution results
 * @return 0 on success
 */
int llm_tools_add_results_claude(struct json_object *history, const tool_result_list_t *results);

/* =============================================================================
 * Response Parsing
 * ============================================================================= */

/**
 * @brief Parse tool calls from OpenAI response
 *
 * Extracts tool_calls array from an OpenAI API response.
 *
 * @param response JSON response object
 * @param out Output: parsed tool calls
 * @return 0 if tool calls found, 1 if no tool calls, -1 on error
 */
int llm_tools_parse_openai_response(struct json_object *response, tool_call_list_t *out);

/**
 * @brief Parse tool calls from Claude response
 *
 * Extracts tool_use content blocks from a Claude API response.
 *
 * @param response JSON response object
 * @param out Output: parsed tool calls
 * @return 0 if tool calls found, 1 if no tool calls, -1 on error
 */
int llm_tools_parse_claude_response(struct json_object *response, tool_call_list_t *out);

/* =============================================================================
 * Capability Checking
 * ============================================================================= */

/**
 * @brief Check if native tool calling is enabled and supported
 *
 * Checks:
 * 1. Config setting: g_config.llm.tools.native_tools_enabled
 * 2. Provider support (OpenAI, Claude, or local with --jinja)
 *
 * @param config Resolved LLM config (NULL = use global)
 * @return true if native tools should be used, false for <command> fallback
 */
bool llm_tools_enabled(const llm_resolved_config_t *config);

/**
 * @brief Get count of currently enabled tools
 *
 * @return Number of enabled tools
 */
int llm_tools_get_enabled_count(void);

/**
 * @brief Free an llm_tool_response_t structure
 *
 * @param response Response to free (text field is freed if not NULL)
 */
void llm_tool_response_free(llm_tool_response_t *response);

/* =============================================================================
 * Common Tool Execution Helper
 *
 * Provides common context gathering for LLM tool execution loops.
 * ============================================================================= */

/**
 * @brief Context returned from tool execution for follow-up decisions
 */
typedef struct {
   bool skip_followup;         /**< True if follow-up should be skipped */
   bool has_pending_vision;    /**< True if viewing tool captured an image */
   const char *pending_vision; /**< Base64 vision data (if any) */
   size_t pending_vision_size; /**< Size of pending vision */
   char *direct_response;      /**< Response for skip_followup (caller must free) */
} tool_followup_context_t;

/**
 * @brief Prepare follow-up context after tool execution
 *
 * Gathers all context needed to make follow-up call decisions.
 * Call this after executing tools and updating conversation history.
 *
 * @param results The executed tool results
 * @param ctx Output context structure
 */
void llm_tools_prepare_followup(const tool_result_list_t *results, tool_followup_context_t *ctx);

/* =============================================================================
 * Tool Execution Notification Callback
 *
 * Allows external modules (like WebUI) to receive notifications when tools
 * are executed. Used for debug display in UI.
 * ============================================================================= */

/**
 * @brief Callback function type for tool execution notifications
 *
 * @param session Opaque session pointer (may be NULL)
 * @param tool_name Name of the tool being executed
 * @param tool_args JSON arguments string
 * @param result Result of execution (after execution complete)
 * @param success Whether execution succeeded
 */
typedef void (*tool_execution_callback_fn)(void *session,
                                           const char *tool_name,
                                           const char *tool_args,
                                           const char *result,
                                           bool success);

/**
 * @brief Register a callback for tool execution notifications
 *
 * @param callback Function to call when tools are executed
 */
void llm_tools_set_execution_callback(tool_execution_callback_fn callback);

/* =============================================================================
 * Pending Vision Data (DEPRECATED for multi-session use)
 *
 * These global pending vision functions are DEPRECATED for new code.
 * They exist only for the voice command path (viewingCallback -> dawn.c main loop).
 *
 * For session-isolated vision handling, use tool_result_t.vision_image instead.
 * The native tool path (execute_viewing_sync) stores vision directly in tool results.
 * ============================================================================= */

/**
 * @brief Check if pending vision data is available
 * @deprecated Use tool_result_t.vision_image for session isolation
 * @return true if vision data was captured and is waiting to be sent to LLM
 */
bool llm_tools_has_pending_vision(void);

/**
 * @brief Get pending vision data for LLM follow-up
 * @deprecated Use tool_result_t.vision_image for session isolation
 * @param size_out Output: size of vision data
 * @return Base64-encoded image data, or NULL if none pending
 * @note Does NOT clear the pending data - call llm_tools_clear_pending_vision()
 */
const char *llm_tools_get_pending_vision(size_t *size_out);

/**
 * @brief Clear pending vision data after it has been used
 * @deprecated Use tool_result_t.vision_image for session isolation
 */
void llm_tools_clear_pending_vision(void);

/**
 * @brief Set pending vision data from external source
 * @deprecated Use tool_result_t.vision_image for session isolation
 *
 * Used by viewingCallback for voice command path only.
 *
 * @param base64_image Base64-encoded image data (will be copied)
 * @param size Size of the base64 data including null terminator
 * @return true on success, false on allocation failure
 */
bool llm_tools_set_pending_vision(const char *base64_image, size_t size);

/**
 * @brief Process vision data from either base64 or file path
 * @deprecated Use tool_result_t.vision_image for session isolation
 *
 * Used by viewingCallback for voice command path only.
 * Stores result in global pending vision (not session-isolated).
 *
 * @param data Vision data - either base64-encoded image or a file path
 * @param error_buf Output buffer for error messages (can be NULL)
 * @param error_len Size of error buffer
 * @return true on success (image stored in pending vision), false on failure
 */
bool llm_tools_process_vision_data(const char *data, char *error_buf, size_t error_len);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOLS_H */
