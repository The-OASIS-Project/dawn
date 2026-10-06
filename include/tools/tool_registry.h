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
#include <stdint.h>
#include <string.h>

/* Forward declaration for TOML table (avoid including toml.h everywhere) */
typedef struct toml_table_t toml_table_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TOOL_MAX_REGISTERED 64  /* Max tools in registry */
#define TOOL_NAME_MAX 64        /* Max length of tool name */
#define TOOL_DESCRIBE_DEFAULT 2 /* describe_call: this action takes the default description */
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
#define TOOL_PARAM_ENUM_MAX 24        /* Max enum values per parameter */
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
 * ARRAY param delivery:
 *   The LLM emits a native JSON array; the schema advertises
 *   {"type":"array","items":{"type":"string"}}. At encode time
 *   (llm_tools.c) json-c serializes the array to its compact JSON string,
 *   which rides the TOOL_MAPS_TO_CUSTOM "::field::value" packing like any
 *   other value (escaped, so its "::" can't break it); the callback reads it
 *   with tool_param_extract_custom().
 */

/** A tool's param count, from its params array: never written by hand, so a
 *  param added to the array can't be left out of the count. */
#define TOOL_PARAM_COUNT(params)                                                       \
   ((int)(sizeof(params) / sizeof((params)[0]) +                                       \
          0 * sizeof(struct {                                                          \
             int unused;                                                               \
             _Static_assert(!__builtin_types_compatible_p(__typeof__(params),          \
                                                          __typeof__(&(params)[0])),   \
                            "TOOL_PARAM_COUNT needs the params array, not a pointer"); \
          })))

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
 * Kinds of Action
 *
 * What a call does, by its action: who may make it depends on this (a text
 * from an unverified sender may read; a background job may read and fetch; an
 * action needs the user).  Deny by default: an action a tool doesn't list is
 * TOOL_KIND_ACT, the zero value, so a tool that declares nothing acts.
 * ============================================================================= */

typedef enum {
   TOOL_KIND_ACT = 0, /**< changes, sends or starts something (the default) */
   /** no effect the user or anyone outside would notice (known, benign
    *  writes: a mailbox's \Seen flag, an SMS marked read, recall
    *  statistics) */
   TOOL_KIND_READ,
   /** an outward read: the request (a query, a URL) reaches a host the caller
    *  picks, so it can carry data out */
   TOOL_KIND_FETCH,
   /** state scoped to the session or the run, nothing in the home or outside
    *  (a research ledger, the active code project) */
   TOOL_KIND_STATE,
   /** an effect someone in the home hears or sees (music, volume, speech) */
   TOOL_KIND_DEVICE,
   /** stages a pending item that does nothing until its confirm runs (an
    *  email draft, a call preview, a delete awaiting its confirm) */
   TOOL_KIND_PREPARE,
} tool_action_kind_t;

/**
 * @brief One action's kind.  A PREPARE entry names its confirm: the action
 *        that carries out what it staged, itself listed in the same table.
 */
typedef struct {
   const char *action;
   tool_action_kind_t kind;
   const char *confirm; /**< TOOL_KIND_PREPARE only: the action that confirms it (listed ACT) */
} tool_action_kind_entry_t;

/** Entries in an action_kinds table: .action_kind_count = TOOL_KIND_COUNT(t) */
#define TOOL_KIND_COUNT(table) ((int)(sizeof(table) / sizeof((table)[0])))

/** The kind's name, for logs and messages ("read", "fetch", ...). */
const char *tool_action_kind_name(tool_action_kind_t kind);

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

struct json_object; /* forward decl — avoids forcing <json-c/json.h> on consumers */

/**
 * @brief Parse a tool's `details` argument (the VALUE param) into a JSON object.
 *
 * DAWN tools carry their per-action arguments as a single JSON-encoded string
 * (the `details`/VALUE param).  A reasoning model frequently fills that param
 * with a prose *rationale* ("List configured email accounts so the briefing can
 * inspect all inboxes.") instead of a JSON object — harmless for an action that
 * reads no fields, but a hard failure everywhere it is parsed as JSON-or-die.
 * This helper is the single seam for that contract.
 *
 * CALLER OBLIGATION: pass @p no_required_fields = true only when running the
 * action with every `details` field absent is an acceptable outcome.  Two
 * sanctioned tiers qualify, and the choice is a per-call PRODUCT decision (a
 * `strcmp` on the action, living at the call site — keep it in sync):
 *   1. Actions that take NO arguments at all, so an empty object loses nothing
 *      (e.g. email 'accounts', calendar 'calendars', job 'list').
 *   2. Actions whose fields are ALL OPTIONAL and whose defaults are a sensible
 *      "no filter" result, so a prose value degrading to defaults is desired,
 *      not a dropped request (e.g. email 'recent'/'search'/'folders'/'digest',
 *      which default to recent mail across the appropriate accounts).
 * Do NOT pass true for an action that reads a REQUIRED field (e.g. email 'read'
 * needs message_id) or where dropping an optional filter would be a silent wrong
 * result the user never sees (e.g. scheduler 'list' with its `type` filter treats
 * that as a bug to surface, so it passes false) — there, a non-object value must
 * hard-error rather than run with the field missing.
 *
 * @param value             The raw VALUE string.  NULL or empty always yields a
 *                          fresh empty object, regardless of @p no_required_fields.
 * @param no_required_fields true if the action reads no required field from
 *                          `details`: a value that is not a JSON OBJECT degrades
 *                          to an empty object so the call still succeeds.  false
 *                          otherwise: a non-object value returns NULL so the
 *                          caller can raise "invalid JSON in details parameter"
 *                          rather than silently dropping the caller's request.
 * @return An owned json_object the caller must json_object_put(), or NULL only
 *         when @p value is non-empty, is NOT a valid JSON object (unparseable, OR
 *         parseable but an array/string/number/bool/null), AND @p
 *         no_required_fields is false.  Never returns NULL when @p
 *         no_required_fields is true.
 */
struct json_object *tool_parse_details(const char *value, bool no_required_fields);

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
   /**< Show this tool's result whole up to this many characters (0: no
    * ask; its share of the tool loop's batch budget decides).  A result
    * within it is served first in the batch's split, whole while it fits the
    * batch budget, which it never exceeds; a larger one is split like any
    * other.  An MCP tool's comes from _meta["anthropic/maxResultSizeChars"]. */
   size_t max_result_chars;
   /**< Never store this tool's result (over its share it is still shown as a
    * view, with no handle): result_read's own answers, so a read can't hand
    * out a handle to itself. */
   bool result_no_store;
   /**< Always shown whole, never viewed: render_visual's markup, which the
    * WebUI renders from the full text.  Keyed on the tool, never on content,
    * so untrusted text can't claim the exemption. */
   bool result_whole;
   /**< When true, a scheduled-briefing step running this tool has its result
    * persisted into the briefing conversation as a synthetic tool-call/result
    * pair (rendered as a tool entry, reloaded into LLM context) alongside the
    * summary, so later turns can act on it — e.g. resolve an email digest's
    * [E-NN]/[ID] rows. Most tools leave this false: weather/search output is
    * noise once summarized.
    * CONTRACT for adopters: the result must be COMPACT, actionable reference
    * data (it is stored verbatim and rejoins context on every follow-up turn),
    * and the synthetic call's arguments are serialized as {action, arguments} —
    * a good fit for command-callback tools; a native-structured-params tool
    * would show the model args that don't match its own schema. */
   bool persist_scheduled_output;
   bool mqtt_only;      /**< Only available via MQTT */
   bool sync_wait;      /**< Wait for MQTT response */
   bool default_local;  /**< Available to local sessions */
   bool default_remote; /**< Available to remote sessions */

   /** Optional runtime availability check (NULL = always available) */
   bool (*is_available)(void);

   /** Optional per-action schedulability gate.  TOOL_CAP_SCHEDULABLE is a
    *  tool-level grant; a tool that is schedulable for some actions but not
    *  others (e.g. messaging: read_* yes, send no) implements this to reject
    *  the unsafe actions at BOTH create time (scheduler tool) and fire time.
    *  NULL = every action of a schedulable tool may be scheduled.
    *  @return SUCCESS if `action` may be scheduled, FAILURE otherwise (writes err_buf). */
   int (*validate_schedulable_action)(const char *action, char *err_buf, size_t err_buf_size);

   /** Optional check of a call's resolved arguments before it runs (NULL =
    *  none): for a parameter whose valid values change at runtime and so are
    *  kept out of the schema (a conversation freezes the schema; the live set
    *  reaches the model in its standing directions).
    *  @return SUCCESS to run it, FAILURE to refuse it (writes err_buf, which
    *          the model is told). */
   int (*validate_call)(const char *device,
                        const char *action,
                        const char *value,
                        char *err_buf,
                        size_t err_buf_size);

   /* Kinds of action (see tool_action_kind_t).  action_kinds lists the
    * actions of the tool's ENUM action parameter (checked at registration),
    * each with its kind; any other action, and every call of a tool without
    * an action parameter, is default_kind (TOOL_KIND_ACT unless set). */
   const tool_action_kind_entry_t *action_kinds;
   int action_kind_count;
   tool_action_kind_t default_kind;
   /** A call of this tool can't be approved by reply code (a plan: its
    *  steps would each need their own): a call that would need one is refused. */
   bool no_reply_code;
   /** Optional: what a call does, in DAWN's words, for the text that asks the
    *  user for its reply code (NULL = the tool, its action and its declared
    *  parameters).  Names what the call acts on as it will resolve it (the
    *  item, call, recipient or number), with addresses and accounts in full;
    *  what it writes or sends, its start at least.  Gets the effective action
    *  and the packed value; runs on the turn's thread (its user and session).
    *  Sets *valid_for_sec when what it confirms expires sooner than a code
    *  would (the code then expires with it).
    *
    *  The description is part of what the code approves: it is made again
    *  when the approved call runs, and a call whose description changed is
    *  refused.  So it must depend only on what the call will act on, and what
    *  a confirm carries out must not change under the id it names.
    *
    *  @return SUCCESS; FAILURE when it can't say, or what it says doesn't
    *          fit @p out (the call is refused; a reason written to @p out is
    *          given to the model); or
    *          TOOL_DESCRIBE_DEFAULT for an action it leaves to the default
    *          description. */
   int (*describe_call)(const char *action,
                        const char *value,
                        char *out,
                        size_t out_len,
                        int *valid_for_sec);
   /** Optional: a call's kind when it depends on more than its action, or
    *  on configuration (NULL = the table's).  Gets the resolved device, the
    *  effective action, the packed value (NULL when empty) and the kind the
    *  table gives; returns the call's kind. */
   tool_action_kind_t (*classify_call)(const char *device,
                                       const char *action,
                                       const char *value,
                                       tool_action_kind_t listed);

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

/**
 * @brief The action a tool runs when a call names none: by its device type
 *        (boolean "toggle", analog "set", trigger "trigger", music "play",
 *        otherwise "get").  What command_execute and an MQTT publish use.
 */
const char *tool_default_action(const tool_metadata_t *meta);

/**
 * @brief The action a native tool call runs, as the tool receives it: the
 *        call's own when it names one; else "get" for a callback tool (the
 *        native path's rule), the device default (tool_default_action) for
 *        an MQTT tool, and none ("") for the viewing sync path and a tool
 *        with no callback (command_execute defaults it by the tool it
 *        resolves the device to).
 *        What is classified must be what runs: the gate and the dispatch
 *        both take it from here.
 *
 * @param meta   The tool
 * @param action The action the call named ("" or NULL when none)
 * @return A static or borrowed string (never NULL)
 */
const char *tool_effective_action(const tool_metadata_t *meta, const char *action);

/**
 * @brief A call's kind of action: the action's entry in the tool's table, else
 *        its default_kind, passed through its classify_call when it has one;
 *        a confirm a PREPARE names is always ACT
 *
 * @param meta   The tool (not an alias lookup: the metadata that runs)
 * @param device The resolved device (a meta-tool's target; may be NULL)
 * @param action The effective action (tool_effective_action; may be NULL)
 * @param value  The packed value, as the callback receives it (NULL when empty)
 * @return The kind; TOOL_KIND_ACT when @p meta is NULL
 */
tool_action_kind_t tool_action_kind(const tool_metadata_t *meta,
                                    const char *device,
                                    const char *action,
                                    const char *value);

/**
 * @brief The tool's own spelling of @p action: the value of its ENUM action
 *        parameter it matches, ignoring case (a tool without one takes any
 *        action, as given)
 *
 * @param out     Receives the spelling (NUL-terminated; may be cut to fit)
 * @param out_len Size of @p out
 * @return false when the tool has an ENUM action parameter and @p action is
 *         none of its values
 */
bool tool_action_canonical(const tool_metadata_t *meta,
                           const char *action,
                           char *out,
                           size_t out_len);

/**
 * @brief The tool's actions, comma-separated, for a message ("" for a tool
 *        without an ENUM action parameter)
 */
void tool_action_list(const tool_metadata_t *meta, char *out, size_t out_len);

/**
 * @brief Check a tool's action_kinds table: every action is one of its ENUM
 *        action parameter's values, listed once; a tool without such a
 *        parameter lists none; a PREPARE entry names a confirm the table
 *        lists as ACT, and no other entry names one.
 *        Registration refuses a tool that fails (scripts/
 *        check_tool_action_kinds.sh catches it at build time).
 *
 * @param meta    The tool
 * @param why     Receives the reason on failure (may be NULL)
 * @param why_len Size of @p why
 * @return SUCCESS, or FAILURE with @p why set
 */
int tool_action_kinds_validate(const tool_metadata_t *meta, char *why, size_t why_len);

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

/* tool_registry_update_param_enum() return codes (0 = success; disjoint values,
 * so each error condition — including the two former uses of "4" — is distinct). */
#define TREG_ENUM_RC_OK 0
#define TREG_ENUM_RC_FAILURE 1         /* bad args, registry uninitialized, or tool not found */
#define TREG_ENUM_RC_PARAM_NOT_FOUND 2 /* param name not present on the tool */
#define TREG_ENUM_RC_NOT_ENUM 3        /* param exists but is not enum-typed */
#define TREG_ENUM_RC_TOO_MANY 4        /* count exceeds TOOL_PARAM_ENUM_MAX */
#define TREG_ENUM_RC_SLOTS_EXHAUSTED 5 /* no free enum-override slot */

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
 * @return TREG_ENUM_RC_OK on success, or one of the TREG_ENUM_RC_* error codes:
 *         TREG_ENUM_RC_FAILURE        = bad args, registry uninitialized, or tool not found
 *         TREG_ENUM_RC_PARAM_NOT_FOUND = parameter not found on the tool
 *         TREG_ENUM_RC_NOT_ENUM       = parameter exists but is not enum-typed
 *         TREG_ENUM_RC_TOO_MANY       = count exceeds TOOL_PARAM_ENUM_MAX
 *         TREG_ENUM_RC_SLOTS_EXHAUSTED = no free enum-override slot
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

/**
 * @brief A number that rises whenever a tool is registered, the registry is
 *        (re)initialized, a parameter's values or a tool's config change, or
 *        the cache is invalidated: anything that can change a tool's schema
 *        or the set of tools.  For callers caching what they derive from it.
 */
uint64_t tool_registry_generation(void);

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
 * A value can hold any text, "::" included: the encoder escapes each colon a
 * separator could be mistaken for (one next to another colon, or at either end
 * of the value) as TOOL_VALUE_ESC 'c', and a TOOL_VALUE_ESC byte itself as
 * TOOL_VALUE_ESC 'u', so every "::" in the packed string is a separator.  The
 * helpers below decode as they copy.  A string packed by anything that doesn't
 * escape (a direct command, MQTT) decodes unchanged.
 *
 * Only a tool that declares CUSTOM params gets an escaped value; a callback of
 * one reads it through these helpers (or tool_value_decode_copy), never raw.
 * Direct commands, MQTT and scheduled steps pack without escaping: their text
 * is written with its separators in it, so a "::" inside a value there still
 * ends it (and could set a field).  No authorization check may trust a packed
 * field unless the call is known to come from the LLM tool loop.
 * Co-located here so the encode/decode contract lives in one place.
 * ============================================================================= */

#include <stdio.h>
#include <string.h>

#include "core/scheduled_context.h"
#include "core/session_manager.h"

/** The escape byte of a packed tool value (ASCII unit separator). */
#define TOOL_VALUE_ESC '\x1F'

/**
 * @brief Escape @p in (@p len bytes) as a packed tool value into @p out.
 * @return The escaped length (excluding the NUL); when it is >= @p out_len the
 *         output was cut short (still NUL-terminated when @p out_len > 0).
 */
static inline size_t tool_value_escape(const char *in, size_t len, char *out, size_t out_len) {
   size_t n = 0; /* the escaped length */
   size_t w = 0; /* bytes written: what fits, never half an escape */
   bool fits = out_len > 0;
   for (size_t i = 0; i < len; i++) {
      const char c = in[i];
      const bool colon_at_risk = c == ':' &&
                                 (i == 0 || i + 1 == len || in[i - 1] == ':' || in[i + 1] == ':');
      char pair = 0;
      if (colon_at_risk) {
         pair = 'c';
      } else if (c == TOOL_VALUE_ESC) {
         pair = 'u';
      }
      const size_t need = pair ? 2 : 1;
      if (fits && w + need < out_len) {
         if (pair) {
            out[w++] = TOOL_VALUE_ESC;
            out[w++] = pair;
         } else {
            out[w++] = c;
         }
      } else {
         fits = false;
      }
      n += need;
   }
   if (out_len > 0) {
      out[w] = '\0';
   }
   return n;
}

/**
 * @brief Copy @p len bytes of a packed value from @p src into @p out, decoded.
 *        Always NUL-terminates (when @p out_len > 0); truncates to fit.
 */
static inline void tool_value_decode_copy(const char *src, size_t len, char *out, size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   size_t n = 0;
   for (size_t i = 0; i < len && n + 1 < out_len; i++) {
      char c = src[i];
      if (c == TOOL_VALUE_ESC && i + 1 < len && (src[i + 1] == 'c' || src[i + 1] == 'u')) {
         c = src[i + 1] == 'c' ? ':' : TOOL_VALUE_ESC;
         i++;
      }
      out[n++] = c;
   }
   out[n] = '\0';
}

/**
 * @brief Extract a custom parameter value from an encoded value string
 *
 * Walks the name/value pairs after the base, so a value can't be mistaken for
 * a field name.
 *
 * @param value Full value string (may contain custom params)
 * @param field_name Name of field to extract
 * @param out_value Buffer for the decoded value
 * @param out_len Size of out_value buffer
 * @return true if found, false otherwise
 */
static inline bool tool_param_extract_custom(const char *value,
                                             const char *field_name,
                                             char *out_value,
                                             size_t out_len) {
   if (!value || !field_name || !out_value || out_len == 0)
      return false;

   const size_t name_len = strlen(field_name);
   const char *sep = strstr(value, "::"); /* the end of the base */
   while (sep) {
      const char *name = sep + 2;
      const char *name_end = strstr(name, "::");
      if (!name_end)
         return false;
      const char *val = name_end + 2;
      const char *val_end = strstr(val, "::");
      if ((size_t)(name_end - name) == name_len && strncmp(name, field_name, name_len) == 0) {
         tool_value_decode_copy(val, val_end ? (size_t)(val_end - val) : strlen(val), out_value,
                                out_len);
         return true;
      }
      sep = val_end;
   }
   return false;
}

/**
 * @brief Extract the base value (before any custom params) from an encoded string
 *
 * @param value Full value string
 * @param out_base Buffer for the decoded base value
 * @param out_len Size of out_base buffer
 */
static inline void tool_param_extract_base(const char *value, char *out_base, size_t out_len) {
   if (!value || !out_base || out_len == 0)
      return;

   const char *delim = strstr(value, "::");
   tool_value_decode_copy(value, delim ? (size_t)(delim - value) : strlen(value), out_base,
                          out_len);
}

/**
 * @brief The user a tool call acts for; 0 for a guest
 *
 * The calling session's user (session_effective_user_id(): the local mic is
 * the default voice user's, an unmapped satellite is a guest's).  With no
 * session user, a scheduled briefing step acts for the briefing's owner
 * (scheduled_context_set; the scheduler thread has no command context).  A
 * caller with no session at all (MQTT, the device itself) acts for the default
 * voice user.  A tool reading or changing personal data refuses a guest (0)
 * with TOOL_GUEST_REFUSAL.
 */
static inline int tool_get_current_user_id(void) {
   session_t *session = session_get_command_context();
   const int user_id = session ? session_effective_user_id(session) : 0;
   if (user_id > 0)
      return user_id;
   int sched_user = 0;
   if (scheduled_context_get(&sched_user) && sched_user > 0)
      return sched_user;
   return session ? 0 : session_default_voice_user_id();
}

/** Whether the calling turn was spoken (session_turn_spoken); false with no
 *  session. */
static inline bool tool_turn_spoken(void) {
   return session_turn_spoken(session_get_command_context());
}

/** The calling user's own words this turn and the turn before
 *  (session_recent_questions_dup; caller frees): "" in a session with none
 *  (an image-only turn: nothing counts as said), NULL with no session (no
 *  turn to check against). */
static inline char *tool_user_words_dup(void) {
   session_t *session = session_get_command_context();
   if (!session) {
      return NULL;
   }
   char *words = session_recent_questions_dup(session);
   return words ? words : strdup("");
}

/** The result for a guest (tool_get_current_user_id() == 0) asking for personal data. */
#define TOOL_GUEST_REFUSAL                                                                   \
   TOOL_RESULT_ERROR_MARK "This device isn't assigned to a user, so personal data (memory, " \
                          "calendar, email, documents, reminders) isn't available here. An " \
                          "admin can assign it to a user on the satellite management page."


#ifdef __cplusplus
}
#endif

#endif /* TOOL_REGISTRY_H */
