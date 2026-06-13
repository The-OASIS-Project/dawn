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

#include "llm/llm_interface.h"
#include "tools/tool_registry.h"

/* Forward-declared (not #include "config/dawn_config.h") to avoid dragging the
 * global g_config declaration into every llm_tools.h consumer. The full
 * definition is in config/dawn_config.h, which llm_tools.c includes. */
typedef struct llm_tools_config llm_tools_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * History Format Hint (for dual-format duplicate detection)
 * ============================================================================= */

/**
 * @brief History format hint for functions that inspect conversation history
 *
 * Used by llm_tools_is_duplicate_call() to know which JSON structure to expect
 * when walking conversation history for tool call entries.
 */
typedef enum {
   LLM_HISTORY_OPENAI, /**< OpenAI format: assistant.tool_calls[].function.name */
   LLM_HISTORY_CLAUDE  /**< Claude format: assistant.content[].type=tool_use, .name */
} llm_history_format_t;

/* =============================================================================
 * Constants
 * ============================================================================= */

#define LLM_TOOLS_MAX_TOOLS TOOL_MAX_REGISTERED /* Derived from tool_registry.h */
#define LLM_TOOLS_MAX_PARAMS                                                             \
   16                                  /* Maximum parameters per tool. Was 8; the memory \
                                        * tool has 12, and params past this cap are      \
                                        * silently dropped from the LLM-callable schema  \
                                        * (llm_tools.c). Keep headroom above the largest \
                                        * tool's param_count. */
#define LLM_TOOLS_MAX_ENUM_VALUES 16   /* Maximum enum values per parameter */
#define LLM_TOOLS_MAX_PARALLEL_CALLS 8 /* Maximum parallel tool calls */
#define LLM_TOOLS_MAX_ITERATIONS 8     /* Maximum tool execution loop iterations */

#define LLM_TOOLS_NAME_LEN 64
#define LLM_TOOLS_DESC_LEN 512
#define LLM_TOOLS_ID_LEN 64
/* 16 KB — large enough for SVG/HTML code in render_visual tool arguments.
 * If tools need even larger arguments in the future, consider switching
 * tool_call_t.arguments from a fixed array to a heap-allocated char*
 * with dynamic sizing from the parsed JSON. */
#define LLM_TOOLS_ARGS_LEN 16384
#define LLM_TOOLS_RESULT_LEN 8192

/* =============================================================================
 * Tool Definition Structures
 * ============================================================================= */

/**
 * @brief Tool parameter definition
 *
 * Uses tool_param_type_t from tool_registry.h for type field.
 */
typedef struct {
   char name[LLM_TOOLS_NAME_LEN];                   /**< Parameter name */
   char description[256];                           /**< Parameter description */
   tool_param_type_t type;                          /**< Parameter type (from tool_registry.h) */
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
   bool enabled;                                  /**< Runtime enable (capability available) */
   bool enabled_local;                            /**< Enabled for local sessions */
   bool enabled_remote;                           /**< Enabled for remote sessions */
   bool armor_feature;                            /**< OASIS armor-specific feature */
   bool dangerous;                                /**< Requires explicit opt-in to enable */
   bool parallel_safe;                            /**< Safe for concurrent execution */
   const char *device_name;                       /**< Mapped device name for callback */
} tool_definition_t;

/**
 * @brief Tool info for WebUI configuration display
 */
typedef struct {
   char name[LLM_TOOLS_NAME_LEN];
   char description[LLM_TOOLS_DESC_LEN];
   bool enabled;        /**< Capability available (based on auth/config) */
   bool enabled_local;  /**< User setting for local sessions */
   bool enabled_remote; /**< User setting for remote sessions */
   bool armor_feature;  /**< OASIS armor-specific feature */
   bool dangerous;      /**< Requires explicit opt-in (TOOL_CAP_DANGEROUS) */
} tool_info_t;

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
   bool args_truncated;                /**< true if the provider's args exceeded the buffer and
                                        *   were clipped — the call must NOT execute on partial args */
} tool_call_t;

/**
 * Maximum length for Gemini thought signature.
 * Gemini 3+ models return a thought_signature with tool calls that must be
 * echoed back in tool results. Based on observed API behavior, signatures
 * are typically < 1KB but can grow with complex reasoning. 4KB provides
 * headroom. Truncation is logged as a warning if exceeded.
 */
#define LLM_TOOLS_THOUGHT_SIG_LEN 4096

/**
 * @brief List of tool calls (for parallel invocation)
 *
 * LLMs can request multiple tool calls in a single response. This structure
 * holds all pending calls that need to be executed.
 */
typedef struct {
   tool_call_t calls[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int count;
   char thought_signature[LLM_TOOLS_THOUGHT_SIG_LEN]; /**< Gemini 3+ thought signature (first call)
                                                       */
} tool_call_list_t;

/**
 * @brief Tool execution result
 *
 * Contains the result of executing a tool, to be sent back to the LLM.
 */
typedef struct {
   char tool_call_id[LLM_TOOLS_ID_LEN]; /**< ID from original tool_call_t */
   char result[LLM_TOOLS_RESULT_LEN];   /**< Execution result text (fixed buffer) */
   char *result_extended; /**< Heap-allocated large result (preferred over result[] if non-NULL) */
   bool success;          /**< true if execution succeeded */
   bool skip_followup;    /**< If true, return result directly without LLM follow-up */
   bool should_respond;   /**< If false, tool handled its own output — suppress follow-up */
   char *vision_image;    /**< Base64 vision image (caller must free) */
   size_t vision_image_size; /**< Size of vision image data */
} tool_result_t;

/**
 * @brief Get the effective result content from a tool result
 *
 * Returns result_extended if set, otherwise the fixed result[] buffer.
 * Use this whenever reading a tool result to ensure large results are
 * not silently truncated.
 */
static inline const char *tool_result_content(const tool_result_t *r) {
   return (r && r->result_extended) ? r->result_extended : (r ? r->result : "");
}

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
typedef struct llm_tool_response {
   char *text;                  /**< Text content (may be NULL if only tools) */
   tool_call_list_t tool_calls; /**< Tool calls requested by LLM */
   bool has_tool_calls;         /**< true if tool_calls.count > 0 */
   char finish_reason[32];      /**< "stop", "tool_calls", "tool_use", etc. */
   /* Extended thinking fields (Claude/Gemini) */
   char *thinking_content;   /**< Extended thinking text (caller must free) */
   char *thinking_signature; /**< Thinking signature for follow-up (caller must free) */
   int reasoning_tokens;     /**< Reasoning token count (OpenAI o-series / Responses); 0 if none */
   /* OpenAI Responses API round-trip fields (NULL for chat-completions / Claude paths) */
   char *response_id;         /**< response.id from /v1/responses (Mode A future use) */
   char *provider_state_json; /**< Opaque JSON blob to attach as _provider_state on the
                               *   resulting assistant message (e.g. encrypted reasoning
                               *   items for the next turn). Caller must free. */
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
 * Call when capabilities change at runtime (e.g., a tool's auth status
 * changes, search endpoint configured, etc.) to update which tools are enabled.
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
 * @brief Generate tools array in OpenAI format, filtered by session type
 *
 * @param is_remote_session true for WebUI/satellite sessions, false for local mic
 * @return JSON array (caller must json_object_put), or NULL if no tools
 */
struct json_object *llm_tools_get_openai_format_filtered(bool is_remote_session);

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

/**
 * @brief Generate tools array in Claude format, filtered by session type
 *
 * @param is_remote_session true for WebUI/satellite sessions, false for local mic
 * @return JSON array (caller must json_object_put), or NULL if no tools
 */
struct json_object *llm_tools_get_claude_format_filtered(bool is_remote_session);

/* =============================================================================
 * Tool Configuration API (for WebUI)
 * ============================================================================= */

/**
 * @brief Get all tools with their current enable states
 *
 * Populates an array of tool_info_t with name, description, and enable flags.
 *
 * @param out Output array (caller allocates)
 * @param max_tools Maximum entries to return
 * @return Number of tools populated
 */
int llm_tools_get_all(tool_info_t *out, int max_tools);

/**
 * @brief Set enable state for a specific tool
 *
 * Thread-safe. Invalidates cached token estimates.
 *
 * @param tool_name Name of the tool to configure
 * @param enabled_local Enable for local sessions
 * @param enabled_remote Enable for remote sessions
 * @return 0 on success, 1 on failure (tool not found or not initialized)
 */
int llm_tools_set_enabled(const char *tool_name, bool enabled_local, bool enabled_remote);

/**
 * @brief Check if a device/tool is enabled for a session type
 *
 * Used by legacy <command> tag prompt builder to filter commands
 * based on the same enabled state as native tools.
 *
 * Thread-safe.
 *
 * @param device_name Device or tool name to check
 * @param is_remote true for remote sessions, false for local
 * @return true if enabled, false if disabled or not found
 */
bool llm_tools_is_device_enabled(const char *device_name, bool is_remote);

/**
 * @brief Apply per-tool enable/disable configuration (per local + remote surface).
 *
 * Two models, chosen per surface by which list is configured:
 * - DISABLE list configured → BLOCKLIST: all non-dangerous tools on EXCEPT those
 *   listed (a newly-added tool is on by default). Precedence over the enable list.
 * - ENABLE list configured (no disable list) → WHITELIST (legacy): on only if listed.
 * - Neither configured → all non-dangerous tools on.
 * TOOL_CAP_DANGEROUS tools are NEVER auto-enabled under any model — they require
 * explicit membership in the enable list.
 *
 * Thread-safe. Must be called after llm_tools_init().
 *
 * @param cfg The [llm.tools] config (enable + disable lists for both surfaces).
 */
void llm_tools_apply_config(const llm_tools_config_t *cfg);

/**
 * @brief Get count of enabled tools for a session type
 *
 * @param is_remote_session true for remote, false for local
 * @return Number of enabled tools
 */
int llm_tools_get_enabled_count_filtered(bool is_remote_session);

/**
 * @brief Estimate token count for enabled tools
 *
 * Provides a rough estimate based on JSON size (~4 chars/token).
 *
 * @param is_remote_session true for remote, false for local
 * @return Estimated token count
 */
int llm_tools_estimate_tokens(bool is_remote_session);

/**
 * @brief Invalidate cached schemas (forces regeneration with current params)
 *
 * Call this when tool parameters are updated dynamically (e.g., by discovery).
 * This ensures the next schema generation reflects the current parameter values.
 */
void llm_tools_invalidate_cache(void);

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
 * @brief Execute multiple tool calls with parallel optimization
 *
 * Executes tool calls with automatic parallelization for independent tools.
 * Tools are classified as parallel-safe (HTTP calls, getters) or sequential
 * (state-modifying tools like switch_llm, reset_conversation). Parallel-safe
 * tools run concurrently via pthreads, while sequential tools run after.
 *
 * For single tool calls, executes directly without threading overhead.
 *
 * @param calls List of tool calls to execute
 * @param results Output: execution results (indexed to match input calls)
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
 * @return 0 if tool calls found, non-zero if no tool calls or error
 */
int llm_tools_parse_openai_response(struct json_object *response, tool_call_list_t *out);

/**
 * @brief Parse tool calls from Claude response
 *
 * Extracts tool_use content blocks from a Claude API response.
 *
 * @param response JSON response object
 * @param out Output: parsed tool calls
 * @return 0 if tool calls found, non-zero if no tool calls or error
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
 * @brief Temporarily suppress tools for the current request
 *
 * Used by subsystems (like the search summarizer) that need to make
 * LLM calls without tools being included in the request. Call
 * llm_tools_suppress_pop() when done.
 *
 * Thread-safe: Uses thread-local storage.
 */
void llm_tools_suppress_push(void);

/**
 * @brief Restore tools after suppression
 *
 * Must be called after llm_tools_suppress_push() to restore normal
 * tool behavior. Calls can be nested.
 */
void llm_tools_suppress_pop(void);

/**
 * @brief Check if tools are currently suppressed
 *
 * @return true if tools are suppressed (internal utility call in progress)
 */
bool llm_tools_suppressed(void);

/**
 * @brief Set current resolved config for thread-local tool_mode checking
 *
 * Called by llm_chat_completion_*_with_config() before invoking provider
 * functions, so that llm_tools_enabled() can check session-specific tool_mode.
 * Set to NULL after the LLM call completes.
 *
 * Thread-safe: Uses thread-local storage.
 *
 * @param config Resolved config (or NULL to clear)
 */
void llm_tools_set_current_config(const llm_resolved_config_t *config);

/**
 * @brief Get current session's thinking mode
 *
 * Returns the thinking_mode from the thread-local resolved config if set,
 * otherwise returns the global config thinking mode.
 *
 * @return Thinking mode string ("disabled", "auto", "enabled", "low", "medium", "high")
 */
const char *llm_get_current_thinking_mode(void);

/**
 * @brief Get current session's reasoning effort
 *
 * Returns the reasoning_effort from the thread-local resolved config if set,
 * otherwise returns the global config reasoning effort.
 *
 * @return Reasoning effort string ("low", "medium", "high")
 */
const char *llm_get_current_reasoning_effort(void);

/**
 * @brief Get effective budget tokens for thinking
 *
 * Maps reasoning_effort to token budget using the standard levels:
 *   low    = LLM_THINKING_BUDGET_LOW    (1024)
 *   medium = LLM_THINKING_BUDGET_MEDIUM (8192)
 *   high   = LLM_THINKING_BUDGET_HIGH   (16384)
 *
 * If global config budget_tokens > 0, uses that as an explicit override.
 *
 * @return Token budget for thinking/reasoning
 */
int llm_get_effective_budget_tokens(void);

/**
 * @brief Check if text contains thinking trigger phrases
 *
 * Scans the input text for phrases that should trigger extended thinking mode
 * for the current request (case-insensitive). Trigger phrases include:
 * "think carefully", "think step by step", "reason through", etc.
 *
 * @param text User command text to scan (can be NULL)
 * @return true if a thinking trigger phrase was found, false otherwise
 */
bool llm_check_thinking_trigger(const char *text);

/**
 * @brief Get count of currently enabled tools
 *
 * @return Number of enabled tools
 */
int llm_tools_get_enabled_count(void);

/**
 * @brief Build a per-session hint listing tools that are unavailable for the LLM
 *
 * Produces hint text that distinguishes two reasons a tool is unavailable:
 *   1. Capability not available (hardware offline, missing credentials, etc.)
 *   2. Available but disabled for this specific session type (local or remote)
 *
 * Tools that are fully usable in the given session are omitted from the hint.
 *
 * @param is_remote true for a remote-session prompt, false for a local-session prompt
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written (0 if no unavailable tools for this session)
 */
int llm_tools_build_disabled_hint(bool is_remote, char *buffer, size_t buffer_size);

/**
 * @brief Free an llm_tool_response_t structure
 *
 * @param response Response to free (text and thinking fields are freed if not NULL)
 */
void llm_tool_response_free(llm_tool_response_t *response);

/**
 * @brief Return a deep-copied history array with provider-private fields stripped.
 *
 * Currently strips `_provider_state` from each message — that's where the OpenAI
 * Responses path stashes encrypted reasoning blobs (and where future providers
 * will stash their own opaque per-turn state). Those blobs are session-bound,
 * provider-specific, and MUST NOT be:
 *   - Forwarded to a different LLM provider (e.g. memory extraction sending DAWN
 *     conversation history to a Claude/Gemini/local model for summarization).
 *   - Persisted to disk in pre-compact logs or conversation exports.
 *
 * The returned array is a new json_object with refcount 1; caller must
 * json_object_put it. Original history is unmodified. NULL on allocation failure.
 *
 * @param history JSON array of messages.
 * @return Sanitized deep-copy (caller json_object_put), or NULL on error.
 */
struct json_object *llm_history_strip_provider_state(struct json_object *history);

/**
 * @brief Check if a tool call is a duplicate of a previous call in conversation history
 *
 * Prevents infinite loops where the LLM keeps making the same tool call repeatedly.
 * Compares tool name and arguments against previous tool calls in the history.
 * Supports both OpenAI and Claude history formats via the format hint parameter.
 *
 * @param history Conversation history (JSON array)
 * @param tool_name Name of the tool being called
 * @param tool_args Arguments for the tool call
 * @param format History format hint (OpenAI or Claude)
 * @return true if this exact call was already made, false otherwise
 */
bool llm_tools_is_duplicate_call(struct json_object *history,
                                 const char *tool_name,
                                 const char *tool_args,
                                 llm_history_format_t format);

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
   bool all_silent;            /**< True if all tools set should_respond=false (history-safe) */
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

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOLS_H */
