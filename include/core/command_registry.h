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
 * Unified Command Registry
 *
 * This module provides a single source of truth for all command definitions,
 * parsed from commands_config_nuevo.json at startup. It replaces the need for
 * separate command definitions in llm_tools.c and text_to_command_nuevo.c.
 */

#ifndef COMMAND_REGISTRY_H
#define COMMAND_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define CMD_MAX_DEVICES 64
#define CMD_MAX_PARAMS 8
#define CMD_MAX_ENUM_VALUES 16
#define CMD_MAX_DEVICE_MAP 8
#define CMD_NAME_LEN 64
#define CMD_DESC_LEN 512
#define CMD_TOPIC_LEN 32

/* =============================================================================
 * Parameter Types and Mapping
 * ============================================================================= */

/**
 * @brief Parameter data types for command/tool definitions
 */
typedef enum {
   CMD_PARAM_STRING,  /**< String parameter */
   CMD_PARAM_INTEGER, /**< Integer parameter */
   CMD_PARAM_NUMBER,  /**< Floating-point parameter */
   CMD_PARAM_BOOLEAN, /**< Boolean parameter */
   CMD_PARAM_ENUM     /**< Enumeration (string with allowed values) */
} cmd_param_type_t;

/**
 * @brief How a parameter maps to the device/action/value model
 */
typedef enum {
   CMD_MAPS_TO_VALUE,  /**< Parameter becomes "value" field */
   CMD_MAPS_TO_ACTION, /**< Parameter becomes "action" field */
   CMD_MAPS_TO_DEVICE, /**< Parameter becomes "device" field (overrides default) */
   CMD_MAPS_TO_CUSTOM  /**< Custom field name (specified by field_name) */
} cmd_param_mapping_t;

/* =============================================================================
 * Command Parameter Definition
 * ============================================================================= */

/**
 * @brief Parameter definition for a command
 */
typedef struct {
   char name[CMD_NAME_LEN];                   /**< Parameter name */
   char description[256];                     /**< Parameter description for LLM */
   cmd_param_type_t type;                     /**< Parameter type */
   bool required;                             /**< Is this parameter required? */
   cmd_param_mapping_t maps_to;               /**< How to map to device/action/value */
   char field_name[CMD_NAME_LEN];             /**< Custom field name for MAPS_TO_CUSTOM */
   char enum_values[CMD_MAX_ENUM_VALUES][64]; /**< Allowed values for ENUM type */
   int enum_count;                            /**< Number of enum values */
} cmd_param_t;

/**
 * @brief Device map entry for meta-tools
 *
 * Maps a parameter value to an actual device name for meta-tools
 * like hud_control that dispatch to multiple underlying devices.
 */
typedef struct {
   char key[CMD_NAME_LEN];    /**< Parameter value (e.g., "armor_display") */
   char device[CMD_NAME_LEN]; /**< Actual device name to execute */
} cmd_device_map_t;

/* =============================================================================
 * Command Definition
 * ============================================================================= */

/**
 * @brief Complete command definition
 *
 * Represents a command that can be executed via voice, <command> tags, or
 * native tool calling. Contains all metadata needed for execution and
 * tool generation.
 */
typedef struct {
   char name[CMD_NAME_LEN];                /**< Command/device name (e.g., "weather") */
   char description[CMD_DESC_LEN];         /**< Description for LLM tool generation */
   char device_string[CMD_NAME_LEN];       /**< Device name for callback lookup */
   char topic[CMD_TOPIC_LEN];              /**< MQTT topic for hardware commands */
   bool has_callback;                      /**< true if deviceCallbackArray has entry */
   bool mqtt_only;                         /**< true if no callback, MQTT-only device */
   bool sync_wait;                         /**< Wait for MQTT response (e.g., viewing) */
   bool skip_followup;                     /**< Skip LLM follow-up after execution */
   bool enabled;                           /**< Runtime enable/disable */
   bool is_meta_tool;                      /**< true if this is a meta-tool (aggregates devices) */
   cmd_param_t parameters[CMD_MAX_PARAMS]; /**< Parameter definitions */
   int param_count;                        /**< Number of parameters */
   cmd_device_map_t device_map[CMD_MAX_DEVICE_MAP]; /**< Device mapping for meta-tools */
   int device_map_count;                            /**< Number of device map entries */
} cmd_definition_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the command registry from commands_config_nuevo.json
 *
 * Parses the JSON configuration file and builds the internal registry.
 * Must be called before any other registry functions.
 *
 * @return 0 on success, non-zero on error
 */
int command_registry_init(void);

/**
 * @brief Shutdown and free command registry resources
 */
void command_registry_shutdown(void);

/* =============================================================================
 * Lookup Functions
 * ============================================================================= */

/**
 * @brief Look up a command definition by name
 *
 * Searches the registry for a command matching the given name.
 * Also checks device aliases.
 *
 * @param name Command/device name to look up
 * @return Pointer to command definition, or NULL if not found
 */
const cmd_definition_t *command_registry_lookup(const char *name);

/**
 * @brief Validate that a device exists and get its topic
 *
 * Security function to validate commands before execution.
 * Replaces validate_device_in_config() from llm_command_parser.c.
 *
 * @param device Device name to validate
 * @param topic_out Output buffer for topic (can be NULL)
 * @param topic_size Size of topic_out buffer
 * @return true if device exists and is valid, false otherwise
 */
bool command_registry_validate(const char *device, char *topic_out, size_t topic_size);

/**
 * @brief Get count of registered commands
 *
 * @return Number of commands in the registry
 */
int command_registry_count(void);

/**
 * @brief Get count of enabled commands
 *
 * @return Number of enabled commands in the registry
 */
int command_registry_enabled_count(void);

/**
 * @brief Resolve device name from meta-tool device map
 *
 * For meta-tools like hud_control, maps parameter values to actual device names.
 * For example, hud_control with element="armor_display" resolves to device "armor_display".
 *
 * @param cmd The meta-tool command definition
 * @param key The parameter value to look up (e.g., "armor_display")
 * @return The actual device name, or NULL if not found
 */
const char *command_registry_resolve_device(const cmd_definition_t *cmd, const char *key);

/* =============================================================================
 * Iteration Functions
 * ============================================================================= */

/**
 * @brief Callback type for registry iteration
 */
typedef void (*cmd_foreach_callback_t)(const cmd_definition_t *cmd, void *user_data);

/**
 * @brief Iterate over all commands in the registry
 *
 * Calls the provided callback for each command in the registry.
 *
 * @param callback Function to call for each command
 * @param user_data Opaque pointer passed to callback
 */
void command_registry_foreach(cmd_foreach_callback_t callback, void *user_data);

/**
 * @brief Iterate over enabled commands only
 *
 * Calls the provided callback for each enabled command.
 * Useful for generating tool definitions.
 *
 * @param callback Function to call for each enabled command
 * @param user_data Opaque pointer passed to callback
 */
void command_registry_foreach_enabled(cmd_foreach_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_REGISTRY_H */
