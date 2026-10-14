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
 * Device Types - Compile-time definitions for direct command pattern matching
 *
 * Replaces the JSON "types" section with C structs for:
 * - Compile-time checking
 * - Natural exclusion when tools are disabled
 * - No runtime JSON parsing overhead
 */

#ifndef DEVICE_TYPES_H
#define DEVICE_TYPES_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/tool_registry.h"

/* ========== Constants ========== */

#define DEVICE_TYPE_MAX_PATTERNS 16
#define DEVICE_TYPE_MAX_ACTIONS 8
#define DEVICE_TYPE_MAX_VALUE 256

/* ========== Data Structures ========== */

/**
 * @brief Action pattern for direct command matching
 *
 * Patterns use placeholders:
 *   %device_name% - replaced with device name or aliases
 *   %value%       - captures a value parameter
 */
typedef struct {
   const char *name;                               /* Action name: "enable", "disable", etc. */
   const char *patterns[DEVICE_TYPE_MAX_PATTERNS]; /* Pattern strings */
   int pattern_count;
} device_action_def_t;

/**
 * @brief Device type definition with action patterns
 *
 * Maps to tool_device_type_t enum values.
 */
typedef struct {
   const char *name; /* Type name: "boolean", "analog", etc. */
   device_action_def_t actions[DEVICE_TYPE_MAX_ACTIONS];
   int action_count;
} device_type_def_t;

/* ========== Predefined Device Types ========== */

extern const device_type_def_t DEVICE_TYPE_BOOLEAN;
extern const device_type_def_t DEVICE_TYPE_ANALOG;
extern const device_type_def_t DEVICE_TYPE_GETTER;
extern const device_type_def_t DEVICE_TYPE_MUSIC;
extern const device_type_def_t DEVICE_TYPE_TRIGGER;
extern const device_type_def_t DEVICE_TYPE_PASSPHRASE;

/* ========== API Functions ========== */

/**
 * @brief Get device type definition by enum value
 *
 * @param type The device type enum from tool_device_type_t
 * @return Pointer to device type definition, or NULL if invalid
 */
const device_type_def_t *device_type_get_def(tool_device_type_t type);

/**
 * @brief Match user input against device type patterns
 *
 * Tries to match the input against all action patterns for the given device type,
 * substituting the device name and aliases for %device_name% placeholders.
 *
 * @param type       Device type definition to match against
 * @param input      User's spoken/typed input (will be matched case-insensitively)
 * @param device_name Primary device name to substitute for %device_name%
 * @param aliases    Array of device aliases (NULL-terminated), or NULL if none
 * @param out_action Output: matched action name (e.g., "enable", "set")
 * @param out_value  Output: captured value if pattern has %value% (caller provides buffer)
 * @param value_size Size of out_value buffer
 * @return true if matched, false otherwise
 */
bool device_type_match_pattern(const device_type_def_t *type,
                               const char *input,
                               const char *device_name,
                               const char **aliases,
                               const char **out_action,
                               char *out_value,
                               size_t value_size);

/**
 * @brief Get device type name string
 *
 * @param type The device type enum
 * @return Type name string (e.g., "boolean", "analog"), or "unknown"
 */
const char *device_type_get_name(tool_device_type_t type);

/**
 * @brief Whether a device type's spoken value may be a secret (a passphrase),
 *        kept out of the match log
 */
bool device_type_value_is_secret(tool_device_type_t type);

#endif /* DEVICE_TYPES_H */
