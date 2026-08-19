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
 * Tool Registry - Modular Tool Registration System
 *
 * This module provides a registration system for standalone tools. Each tool
 * registers its metadata (name, description, parameters), callback, and config
 * parser. This enables:
 * - Compile-time exclusion via CMake options (DAWN_ENABLE_X)
 * - Tools owning their own configuration and LLM schema
 * - Clean separation between core system and plugin tools
 */

#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Forward declaration for TOML table (avoid including toml.h everywhere) */
typedef struct toml_table_t toml_table_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TOOL_MAX_REGISTERED 64 /* Max tools in registry */
#define TOOL_NAME_MAX 64       /* Max length of tool name */
#define TOOL_DESC_MAX                                                            \
   2048                   /* Max length of an MCP-sourced tool/param description \
                           * (wrapped + sanitized at ingest in                   \
                           * mcp_schema_wrap_description).  Sized to fit verbose \
                           * MCP servers (e.g. cbm's ~1.7 KB tool docs) without  \
                           * truncation.  Compiled-in tools use a `const char *` \
                           * literal and are not bounded by this. */
#define TOOL_TOPIC_MAX 32 /* Max length of MQTT topic */
/* Max parameters per tool. No array is sized by this — it's a validation/hardening
 * cap (also the MCP bridge's property limit). 20 admits real MCP tools like cbm's
 * search_graph (14 params) while still bounding an untrusted upstream schema. */
#define TOOL_PARAM_MAX 20
#define TOOL_PARAM_ENUM_MAX 16        /* Max enum values per parameter */
#define TOOL_ALIAS_MAX 8              /* Max aliases per tool */
#define TOOL_DEVICE_MAP_MAX 8         /* Max device map entries for meta-tools */
#define TOOL_REPEATABLE_ACTIONS_MAX 4 /* Max non-deterministic actions per tool */
#define TOOL_SECRET_MAX 4             /* Max secret requirements per tool */

/* =============================================================================
 * Parameter Types and Mapping
 * ============================================================================= */

/**
 * @brief Parameter data types for tool definitions
 */
typedef enum {
   TOOL_PARAM_TYPE_STRING, /**< String parameter */
   TOOL_PARAM_TYPE_INT,    /**< Integer parameter */
   TOOL_PARAM_TYPE_NUMBER, /**< Floating-point parameter */
   TOOL_PARAM_TYPE_BOOL,   /**< Boolean parameter */
   TOOL_PARAM_TYPE_ENUM,   /**< Enumeration (string with allowed values) */
   TOOL_PARAM_TYPE_ARRAY,  /**< Array of strings (LLM emits a native JSON array). See note below. */
} tool_param_type_t;

/*
 * ARRAY param delivery contract:
 *   The LLM emits a native JSON array; the schema advertises
 *   {"type":"array","items":{"type":"string"}}. At encode time
 *   (llm_tools.c) json-c serializes the array to its compact JSON string,
 *   which rides the existing TOOL_MAPS_TO_CUSTOM "::field::value" packing.
 *   Because that packing is "::"-delimited and a serialized array (or an
 *   element) can itself contain "::", an ARRAY custom param MUST be the
 *   LAST-declared param in the tool's params[] so its value occupies the
 *   terminal slot, and the callback MUST decode it with
 *   tool_param_extract_custom_tail() (reads to end-of-string), not the
 *   plain tool_param_extract_custom(). A tool may declare at most one
 *   ARRAY param for this reason.
 */

/**
 * @brief How a parameter maps to the device/action/value model
 */
typedef enum {
   TOOL_MAPS_TO_VALUE,  /**< Parameter becomes "value" field */
   TOOL_MAPS_TO_ACTION, /**< Parameter becomes "action" field */
   TOOL_MAPS_TO_DEVICE, /**< Parameter becomes "device" field (for meta-tools) */
   TOOL_MAPS_TO_CUSTOM, /**< Custom field name (specified by field_name) */
} tool_param_mapping_t;

/**
 * @brief Device type (determines action_words pattern)
 */
typedef enum {
   TOOL_DEVICE_TYPE_BOOLEAN,    /**< enable/disable actions */
   TOOL_DEVICE_TYPE_ANALOG,     /**< set to value */
   TOOL_DEVICE_TYPE_GETTER,     /**< read-only query */
   TOOL_DEVICE_TYPE_MUSIC,      /**< play/pause/next/prev/stop */
   TOOL_DEVICE_TYPE_TRIGGER,    /**< single action */
   TOOL_DEVICE_TYPE_PASSPHRASE, /**< requires passphrase */
} tool_device_type_t;

/**
 * @brief Capability flags for tools
 *
 * Used for security decisions and runtime filtering.
 */
typedef enum {
   TOOL_CAP_NONE = 0,
   TOOL_CAP_DANGEROUS = (1 << 0),     /**< Requires explicit enable (e.g., shutdown) */
   TOOL_CAP_NETWORK = (1 << 1),       /**< Requires network access */
   TOOL_CAP_FILESYSTEM = (1 << 2),    /**< Accesses filesystem */
   TOOL_CAP_SECRETS = (1 << 3),       /**< Uses secrets.toml credentials */
   TOOL_CAP_ARMOR_FEATURE = (1 << 4), /**< OASIS armor-specific feature */
   TOOL_CAP_SCHEDULABLE = (1 << 5),   /**< Safe for scheduled task execution */
   /* Tool callback requires a non-empty value for ANY of its actions.  Set
    * ONLY on tools with no sensible default — e.g. search (no query =
    * nothing to search), url_fetch (no URL = nothing to fetch).  Do NOT set
    * on tools that fall back to config defaults like weather (uses
    * configured location when value is empty). */
   TOOL_CAP_REQUIRES_VALUE = (1 << 6),
   /* Read-only tool whose output is meant for the user to RECEIVE (weather,
    * search, url_fetch).  The scheduler uses this to auto-promote a scheduled
    * `task` on such a tool into a single-step briefing, so the result is
    * LLM-summarized and delivered (TTS / WebUI / deliver_to) instead of being
    * discarded.  Do NOT set on action tools (lights, send message) whose
    * scheduled result is just a status — "completed" is the right feedback
    * there.  Mixed read/write tools (calendar, email) are intentionally NOT
    * marked: their writes are legitimate tasks and a per-tool flag can't
    * distinguish a scheduled read from a scheduled write. */
   TOOL_CAP_INFORMATIONAL = (1 << 7),
} tool_capability_t;

/* =============================================================================
 * Parameter Definition
 * ============================================================================= */

/**
 * @brief Parameter definition for a tool
 *
 * Note: Named treg_param_t to avoid conflict with tool_param_t in llm_tools.h
 */
typedef struct {
   const char *name;                             /**< Parameter name */
   const char *description;                      /**< Description for LLM */
   tool_param_type_t type;                       /**< Parameter type */
   bool required;                                /**< Is this parameter required? */
   tool_param_mapping_t maps_to;                 /**< How to map to device/action/value */
   const char *field_name;                       /**< Custom field for MAPS_TO_CUSTOM */
   const char *enum_values[TOOL_PARAM_ENUM_MAX]; /**< Allowed values for ENUM type */
   int enum_count;                               /**< Number of enum values */
   const char *unit;                             /**< Unit for analog params (e.g., "pixels") */
} treg_param_t;

/**
 * @brief Device map entry for meta-tools
 *
 * Maps a parameter value to an actual device name for meta-tools
 * that dispatch to multiple underlying devices.
 */
typedef struct {
   const char *key;    /**< Parameter value (e.g., "capture") */
   const char *device; /**< Actual device name (e.g., "audio capture device") */
} tool_device_map_t;

/**
 * @brief Secret requirement declaration (security)
 *
 * Tools declare what secrets they need at compile time.
 * Registry validates TOOL_CAP_SECRETS matches declarations.
 */
typedef struct {
   const char *secret_name; /**< Key in secrets.toml (e.g., "openai_api_key") */
   bool required;           /**< Fail init if missing? */
} tool_secret_requirement_t;

/* =============================================================================
 * Function Pointer Types
 * ============================================================================= */

/**
 * @brief Tool config parser function type
 *
 * Called during config parsing to let tool parse its TOML section.
 *
 * @param table TOML table for the tool's section (may be NULL if not present)
 * @param config Pointer to tool's config struct
 */
typedef void (*tool_config_parser_fn)(toml_table_t *table, void *config);

/**
 * @brief Tool config writer function type
 *
 * Called during config save to let tool write its TOML section.
 * The section header (e.g. "[home_assistant]\n") is written by the caller.
 *
 * @param fp     File pointer (FILE*) to write TOML key-value pairs to (void* to avoid stdio.h)
 * @param config Pointer to tool's config struct
 */
typedef void (*tool_config_writer_fn)(void *fp, const void *config);

/**
 * @brief Tool initialization function type
 *
 * Called after config parsing. Tool should initialize resources.
 *
 * @return 0 on success, non-zero on error
 */
typedef int (*tool_init_fn)(void);

/**
 * @brief Tool cleanup function type
 *
 * Called at shutdown. Tool should free resources.
 */
typedef void (*tool_cleanup_fn)(void);

/**
 * @brief Tool callback function type
 *
 * Called to execute the tool's functionality.
 *
 * @param action The action/subcommand (from MAPS_TO_ACTION parameter)
 * @param value The primary value (from MAPS_TO_VALUE parameter)
 * @param should_respond Set to 1 to return result to LLM, 0 to handle directly
 * @return Heap-allocated response string, or NULL
 */
typedef char *(*tool_callback_fn)(const char *action, char *value, int *should_respond);

/* =============================================================================
 * Tool result error-marker convention (opt-in)
 *
 * The legacy `char *` callback contract carries no success/failure status, so
 * a result that is actually an error (e.g. "Weather lookup failed: HTTP 502")
 * is indistinguishable from a normal result — consumers that count "non-NULL
 * result == success" over-report.  A tool MAY prefix a result it considers a
 * HARD FAILURE with TOOL_RESULT_ERROR_MARK.  Consumers detect it via
 * tool_result_is_error() to keep their own accounting honest (e.g. the
 * scheduler briefing runner's per-step success count) and strip the marker
 * before the text reaches the LLM, so the human-readable error still survives.
 * Tools that don't use it behave exactly as before.
 * ============================================================================= */
#define TOOL_RESULT_ERROR_MARK "\x01"

/** @return true if @p result begins with the tool error marker. NULL-safe. */
static inline bool tool_result_is_error(const char *result) {
   return result != NULL && result[0] == TOOL_RESULT_ERROR_MARK[0];
}

/**
 * @brief Strip a leading TOOL_RESULT_ERROR_MARK from @p result in place.
 *
 * No-op when @p result is NULL or unmarked.  Same allocation (the string stays
 * free()-able).  Every callback-dispatch site that hands a result to the LLM
 * calls this so the marker never leaks; the one consumer that also needs the
 * failure signal (the scheduler briefing runner) uses tool_result_is_error()
 * directly before stripping, to keep its per-step accounting honest.
 */
static inline void tool_result_strip_error_mark(char *result) {
   if (tool_result_is_error(result)) {
      memmove(result, result + 1, strlen(result + 1) + 1);
   }
}

/* =============================================================================
 * Tool Metadata (Complete Definition)
 * ============================================================================= */

/**
 * @brief Complete tool metadata
 *
 * Contains all information needed to register, execute, and generate
 * LLM tool schemas for a tool. Replaces JSON device entries.
 */
typedef struct {
   /* Identity */
   const char *name;                    /**< API name (e.g., "search") */
   const char *device_string;           /**< Callback device name */
   const char *topic;                   /**< MQTT topic */
   const char *aliases[TOOL_ALIAS_MAX]; /**< Alternative names */
   int alias_count;                     /**< Number of aliases */

   /* LLM Tool Schema */
   const char *description;    /**< Tool description for LLM */
   const treg_param_t *params; /**< Parameter definitions */
   int param_count;            /**< Number of parameters */

   /* Actions exempt from duplicate-call detection because they are
    * non-deterministic — an identical-args repeat is expected to produce a
    * different result (e.g. calculator "random": "pick another number"), so the
    * anti-loop guard must NOT treat it as a duplicate.  Lists action values, not
    * param names.  Empty for ordinary deterministic tools. */
   const char *repeatable_actions[TOOL_REPEATABLE_ACTIONS_MAX];
   int repeatable_action_count;

   /* Device Mapping (for meta-tools) */
   const tool_device_map_t *device_map; /**< Maps param values to devices */
   int device_map_count;                /**< Number of device map entries */

   /* Behavior Flags */
   tool_device_type_t device_type; /**< boolean, analog, getter, etc. */
   tool_capability_t capabilities; /**< Capability flags */
   bool skip_followup;             /**< Skip LLM follow-up response (see guide for details) */
   bool mqtt_only;                 /**< Only available via MQTT */
   bool sync_wait;                 /**< Wait for MQTT response */
   bool default_local;             /**< Available to local sessions */
   bool default_remote;            /**< Available to remote sessions */

   /** Optional runtime availability check (NULL = always available) */
   bool (*is_available)(void);

   /** Optional per-action schedulability gate.  TOOL_CAP_SCHEDULABLE is a
    *  tool-level grant; a tool that is schedulable for some actions but not
    *  others (e.g. messaging: read_* yes, send no) implements this to reject
    *  the unsafe actions at BOTH create time (scheduler tool) and fire time.
    *  NULL = every action of a schedulable tool may be scheduled.
    *  @return SUCCESS if `action` may be scheduled, FAILURE otherwise (writes err_buf). */
   int (*validate_schedulable_action)(const char *action, char *err_buf, size_t err_buf_size);

   /* Config (optional - NULL if tool has no config) */
   void *config;                        /**< Pointer to tool's config struct */
   size_t config_size;                  /**< sizeof() the config struct */
   tool_config_parser_fn config_parser; /**< Parser for TOML section */
   tool_config_writer_fn config_writer; /**< Writer for TOML section (optional) */
   const char *config_section;          /**< TOML section name */

   /* Secret Requirements (security - NULL-terminated array or NULL) */
   const tool_secret_requirement_t *secret_requirements;

   /* Lifecycle (optional - NULL if not needed) */
   tool_init_fn init;       /**< Called after config parse */
   tool_cleanup_fn cleanup; /**< Called at shutdown */

   /* Callback (required) */
   tool_callback_fn callback; /**< Execute tool functionality */
} tool_metadata_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the tool registry
 *
 * Must be called before any other registry functions.
 * Does NOT call tool init functions - call tool_registry_init_tools() after
 * config parsing is complete.
 *
 * @return 0 on success, non-zero on error
 */
int tool_registry_init(void);

/**
 * @brief Initialize all registered tools
 *
 * Calls init() for each registered tool in registration order.
 * Should be called after config parsing is complete.
 *
 * @return 0 on success, non-zero if any tool init fails
 */
int tool_registry_init_tools(void);

/**
 * @brief Lock the registry to prevent further registrations
 *
 * Should be called after all tools are registered but before network
 * services start. Prevents registration race conditions.
 */
void tool_registry_lock(void);

/**
 * @brief Check if registry is locked
 *
 * @return true if locked, false if registrations still allowed
 */
bool tool_registry_is_locked(void);

/**
 * @brief Check if tool registry is available for use
 *
 * Returns false if tool_registry_init() failed, indicating
 * the system should operate in degraded mode without tool support.
 *
 * @return true if tools are available, false if degraded mode
 */
bool tool_registry_is_available(void);

/**
 * @brief Shutdown all tools and free registry resources
 *
 * Calls cleanup() for each tool in reverse registration order.
 */
void tool_registry_shutdown(void);

/* =============================================================================
 * Registration Functions
 * ============================================================================= */

/**
 * @brief Register a tool with the registry
 *
 * Tools call this during initialization to register themselves.
 * Registration fails if:
 * - Registry is locked
 * - Registry is full
 * - Tool name already registered
 * - TOOL_CAP_DANGEROUS tool doesn't have config with enabled field
 * - TOOL_CAP_SECRETS tool doesn't declare secret_requirements
 *
 * @param metadata Pointer to tool's static metadata (must remain valid)
 * @return 0 on success, non-zero on error
 */
int tool_registry_register(const tool_metadata_t *metadata);

/* =============================================================================
 * Lookup Functions
 * ============================================================================= */

/**
 * @brief Look up a tool by name
 *
 * @param name Tool name to look up
 * @return Pointer to metadata, or NULL if not found
 */
const tool_metadata_t *tool_registry_lookup(const char *name);

/**
 * @brief Look up a tool by alias
 *
 * @param alias Alias to look up
 * @return Pointer to metadata, or NULL if not found
 */
const tool_metadata_t *tool_registry_lookup_alias(const char *alias);

/**
 * @brief Look up a tool by name or alias
 *
 * Checks both name and aliases.
 *
 * @param name_or_alias Name or alias to look up
 * @return Pointer to metadata, or NULL if not found
 */
const tool_metadata_t *tool_registry_find(const char *name_or_alias);

/**
 * @brief Get a tool's callback function
 *
 * Convenience function for callback lookup.
 *
 * @param name Tool name
 * @return Callback function, or NULL if not found
 */
tool_callback_fn tool_registry_get_callback(const char *name);

/**
 * @brief Check if a tool is enabled
 *
 * For TOOL_CAP_DANGEROUS tools, checks the config enabled field.
 * For other tools, always returns true if registered.
 *
 * @param name Tool name
 * @return true if enabled, false if disabled or not found
 */
bool tool_registry_is_enabled(const char *name);

/**
 * @brief Validate a tool reference is safe to schedule / safe to fire
 *
 * Re-checks registry lookup, TOOL_CAP_SCHEDULABLE, enabled state, and the
 * TOOL_CAP_REQUIRES_VALUE-vs-empty-value rule.  Used at create time (LLM
 * scheduler tool, dispatcher) AND at fire time (briefing_thread_func) so a
 * tool that was disabled between schedule and fire fails gracefully.
 *
 * Also runs the tool's optional per-action schedulability gate
 * (validate_schedulable_action) so a tool that is schedulable for some actions
 * but not others (messaging: read_* yes, send no) rejects the unsafe action at
 * create time as well as fire time.
 *
 * @param tool_name Tool name to validate (must be NUL-terminated)
 * @param tool_action Action being scheduled (NULL = unspecified); fed to the
 *                    tool's per-action gate when one is registered
 * @param tool_value Optional value (NULL or "" treated as absent)
 * @param err_buf Output buffer for error message (untouched on success)
 * @param err_buf_size Size of err_buf
 * @return SUCCESS or FAILURE
 */
int tool_registry_validate_schedulable(const char *tool_name,
                                       const char *tool_action,
                                       const char *tool_value,
                                       char *err_buf,
                                       size_t err_buf_size);

/**
 * @brief Resolve device name from meta-tool device map
 *
 * For meta-tools, maps parameter values to actual device names.
 *
 * @param metadata The meta-tool metadata
 * @param key The parameter value to look up
 * @return The actual device name, or NULL if not found
 */
const char *tool_registry_resolve_device(const tool_metadata_t *metadata, const char *key);

/**
 * @brief Get the effective parameter definition for a tool
 *
 * Returns the parameter with any dynamic enum overrides applied.
 * This should be used for schema generation to ensure discovery
 * updates are reflected.
 *
 * @param tool_name Tool name
 * @param param_index Parameter index (0-based)
 * @return Pointer to effective param, or NULL if not found
 */
const treg_param_t *tool_registry_get_effective_param(const char *tool_name, int param_index);

/**
 * @brief Get the JSON args key that carries a tool's action value
 *
 * Returns the `.name` of the tool's first TOOL_MAPS_TO_ACTION parameter — i.e.
 * the key under which the action value appears in the tool-call args JSON.  This
 * is the param's declared name, which is usually "action" but NOT always
 * (e.g. switch_llm uses "target", an audio param uses "type").  Callers that
 * need the action value from args JSON must use this rather than assuming
 * "action".
 *
 * @param tool_name Tool name or alias
 * @return Action param name (static string owned by metadata), or NULL if the
 *         tool has no action parameter
 */
const char *tool_registry_get_action_param_name(const char *tool_name);

/**
 * @brief Check whether a tool's action is declared non-deterministic (repeatable)
 *
 * The duplicate-tool-call guard uses this to exempt actions whose identical-args
 * repeat is a feature, not an infinite loop (e.g. calculator "random").  Looks up
 * the tool (by name or alias) and tests @p action against its
 * repeatable_actions[] declaration.
 *
 * @param tool_name Tool name or alias
 * @param action    Action value to test (may be NULL)
 * @return true if the tool declares @p action repeatable, false otherwise
 */
bool tool_registry_action_is_repeatable(const char *tool_name, const char *action);

/* =============================================================================
 * Config Integration
 * ============================================================================= */

/**
 * @brief Parse config sections for all registered tools
 *
 * Opens the config file and parses tool-specific sections.
 * Called after tools are registered but before they're initialized.
 *
 * @param config_path Path to dawn.toml config file
 * @return 0 on success, non-zero on error
 */
int tool_registry_parse_configs(const char *config_path);

/**
 * @brief Write tool-owned config sections to an open TOML file
 *
 * Iterates all registered tools that have config_writer and config_section,
 * writing their sections at the current file position. Called by config_write_toml()
 * to preserve tool config when rewriting the main config file.
 *
 * @param fp Open file pointer (FILE*, passed as void* to avoid stdio.h in header)
 */
void tool_registry_write_configs(void *fp);

/**
 * @brief Get a secret value by name
 *
 * Tools use this to access secrets they declared in secret_requirements.
 * Returns NULL if secret not found or tool didn't declare it.
 *
 * @param tool_name Name of requesting tool (for validation)
 * @param secret_name Secret key name
 * @return Secret value string, or NULL
 */
const char *tool_registry_get_secret(const char *tool_name, const char *secret_name);

/**
 * @brief Get a config string by path
 *
 * Allows tools to access global config values.
 * Path format: "section.key" (e.g., "localization.location")
 *
 * @param path Config path
 * @return Config value string, or NULL
 */
const char *tool_registry_get_config_string(const char *path);

/* =============================================================================
 * Iteration Functions
 * ============================================================================= */

/**
 * @brief Callback type for registry iteration
 */
typedef void (*tool_foreach_callback_t)(const tool_metadata_t *metadata, void *user_data);

/**
 * @brief Iterate over all registered tools
 *
 * @param callback Function to call for each tool
 * @param user_data Opaque pointer passed to callback
 */
void tool_registry_foreach(tool_foreach_callback_t callback, void *user_data);

/**
 * @brief Get count of registered tools
 *
 * @return Number of tools in registry
 */
int tool_registry_count(void);

/**
 * @brief Get tool metadata by index
 *
 * Allows iteration through all registered tools without needing
 * to know their names in advance.
 *
 * @param index Index from 0 to tool_registry_count()-1
 * @return Tool metadata pointer, or NULL if index out of range
 */
const tool_metadata_t *tool_registry_get_by_index(int index);

/**
 * @brief Get count of enabled tools
 *
 * @return Number of enabled tools
 */
int tool_registry_enabled_count(void);

/* =============================================================================
 * Capability Queries
 * ============================================================================= */

/**
 * @brief Check if a tool has a specific capability
 *
 * @param name Tool name
 * @param cap Capability flag to check
 * @return true if tool has capability, false otherwise
 */
bool tool_registry_has_capability(const char *name, tool_capability_t cap);

/**
 * @brief Iterate over tools with specific capability
 *
 * @param cap Capability flag to filter by
 * @param callback Function to call for each matching tool
 * @param user_data Opaque pointer passed to callback
 */
void tool_registry_foreach_with_capability(tool_capability_t cap,
                                           tool_foreach_callback_t callback,
                                           void *user_data);

/* =============================================================================
 * LLM Schema Generation
 * ============================================================================= */


/* =============================================================================
 * Dynamic Parameter Updates
 * ============================================================================= */

/**
 * @brief Update enum values for a tool parameter dynamically
 *
 * This allows runtime modification of enum parameters, typically used for
 * MQTT-based discovery where external devices advertise their capabilities.
 *
 * The function makes a deep copy of the enum values into mutable storage
 * managed by the registry. The tool's original metadata is not modified;
 * instead, the registry maintains override storage for dynamic enums.
 *
 * Thread-safe: Uses registry mutex for synchronization.
 *
 * @param tool_name Name of the tool to update
 * @param param_name Name of the parameter with enum type
 * @param values Array of enum value strings (will be copied)
 * @param count Number of values in array
 * @return 0 on success, non-zero on error:
 *         1 = tool not found
 *         2 = parameter not found
 *         3 = parameter is not enum type
 *         4 = count exceeds TOOL_PARAM_ENUM_MAX
 */
int tool_registry_update_param_enum(const char *tool_name,
                                    const char *param_name,
                                    const char **values,
                                    int count);

/**
 * @brief Invalidate cached tool schemas
 *
 * Call after updating tool parameters to force regeneration of LLM schemas.
 * This ensures the LLM sees the updated enum values on the next request.
 *
 * Thread-safe: Uses registry mutex for synchronization.
 */
void tool_registry_invalidate_cache(void);

/**
 * @brief Check if schema cache is valid
 *
 * @return true if cache is valid, false if invalidated
 */
bool tool_registry_is_cache_valid(void);

/* =============================================================================
 * Direct Command Variation Statistics
 * ============================================================================= */

/**
 * @brief Count total direct command variations across all tools
 *
 * Calculates the total number of unique voice command patterns that can
 * be recognized for direct command execution. This counts:
 * - All patterns for each device type (boolean, analog, getter, etc.)
 * - Multiplied by (1 + alias_count) for each tool
 *
 * For example, a boolean tool with 2 aliases has:
 * - 14 patterns (8 enable + 6 disable) × 3 names (primary + 2 aliases) = 42 variations
 *
 * @return Total count of direct command variations
 */
int tool_registry_count_variations(void);

/**
 * @brief Count variations for a single tool
 *
 * @param name Tool name
 * @return Number of variations, or 0 if tool not found
 */
int tool_registry_count_tool_variations(const char *name);

/* =============================================================================
 * Custom Parameter Extraction Helpers
 *
 * TOOL_MAPS_TO_CUSTOM parameters are encoded by llm_tools.c as:
 *   "base_value::field_name::field_value[::field_name::field_value...]"
 *
 * These inline helpers decode the encoding. Co-located here so the
 * encode/decode contract lives in one place.
 * ============================================================================= */

#include <stdio.h>
#include <string.h>

#include "core/session_manager.h"

/**
 * @brief Extract a custom parameter value from an encoded value string
 *
 * @param value Full value string (may contain custom params)
 * @param field_name Name of field to extract
 * @param out_value Buffer for extracted value
 * @param out_len Size of out_value buffer
 * @return true if found, false otherwise
 */
static inline bool tool_param_extract_custom(const char *value,
                                             const char *field_name,
                                             char *out_value,
                                             size_t out_len) {
   if (!value || !field_name || !out_value || out_len == 0)
      return false;

   char pattern[64];
   snprintf(pattern, sizeof(pattern), "::%s::", field_name);

   const char *pos = strstr(value, pattern);
   if (!pos)
      return false;

   const char *val_start = pos + strlen(pattern);
   const char *val_end = strstr(val_start, "::");
   size_t val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

   if (val_len >= out_len)
      val_len = out_len - 1;

   memcpy(out_value, val_start, val_len);
   out_value[val_len] = '\0';
   return true;
}

/**
 * @brief Extract a terminal custom parameter, reading to end-of-string
 *
 * Like tool_param_extract_custom() but, once it finds "::field_name::", it
 * copies everything to the end of the string rather than stopping at the next
 * "::". This is required for TOOL_PARAM_TYPE_ARRAY values: the serialized JSON
 * array (or an element) may contain a literal "::", which the plain extractor
 * would truncate. Valid ONLY for the LAST-declared param (the terminal slot) —
 * see the ARRAY contract note near tool_param_type_t.
 *
 * @param value Full value string (may contain custom params)
 * @param field_name Name of the terminal field to extract
 * @param out_value Buffer for extracted value
 * @param out_len Size of out_value buffer
 * @return true if found, false otherwise
 */
static inline bool tool_param_extract_custom_tail(const char *value,
                                                  const char *field_name,
                                                  char *out_value,
                                                  size_t out_len) {
   if (!value || !field_name || !out_value || out_len == 0)
      return false;

   char pattern[64];
   snprintf(pattern, sizeof(pattern), "::%s::", field_name);

   const char *pos = strstr(value, pattern);
   if (!pos)
      return false;

   const char *val_start = pos + strlen(pattern);
   size_t val_len = strlen(val_start);

   if (val_len >= out_len)
      val_len = out_len - 1;

   memcpy(out_value, val_start, val_len);
   out_value[val_len] = '\0';
   return true;
}

/**
 * @brief Extract the base value (before any custom params) from an encoded string
 *
 * @param value Full value string
 * @param out_base Buffer for base value
 * @param out_len Size of out_base buffer
 */
static inline void tool_param_extract_base(const char *value, char *out_base, size_t out_len) {
   if (!value || !out_base)
      return;

   const char *delim = strstr(value, "::");
   size_t base_len = delim ? (size_t)(delim - value) : strlen(value);

   if (base_len >= out_len)
      base_len = out_len - 1;

   memcpy(out_base, value, base_len);
   out_base[base_len] = '\0';
}

static inline int tool_get_current_user_id(void) {
   session_t *session = session_get_command_context();
   if (session && session->metrics.user_id > 0)
      return session->metrics.user_id;
   return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* TOOL_REGISTRY_H */
