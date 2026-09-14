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
 * Text-to-command processing using tool_registry pattern matching.
 * The legacy JSON-based command configuration has been removed;
 * all patterns are now defined in compile-time tool metadata.
 */

#include "text_to_command_nuevo.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "core/device_types.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

void normalize_for_matching(const char *input, char *output, size_t size) {
   if (input == NULL || output == NULL || size == 0) {
      if (output && size > 0) {
         output[0] = '\0';
      }
      return;
   }

   const char *src = input;
   size_t dst_idx = 0;

   /* Skip leading punctuation and whitespace */
   while (*src && (ispunct((unsigned char)*src) || isspace((unsigned char)*src))) {
      src++;
   }

   /* Copy and lowercase */
   while (*src && dst_idx < size - 1) {
      output[dst_idx++] = tolower((unsigned char)*src);
      src++;
   }
   output[dst_idx] = '\0';

   /* Strip trailing punctuation and whitespace */
   while (dst_idx > 0 && (ispunct((unsigned char)output[dst_idx - 1]) ||
                          isspace((unsigned char)output[dst_idx - 1]))) {
      output[--dst_idx] = '\0';
   }
}

/**
 * @brief Try to match input against tool_registry tools using device_types patterns
 *
 * Iterates through all registered tools and tries to match the input against
 * their device type patterns. This allows direct command matching using
 * compile-time defined patterns instead of JSON.
 *
 * @param input       Normalized input text (lowercase, trimmed)
 * @param out_command Buffer to receive the command JSON on match
 * @param command_size Size of out_command buffer
 * @param out_topic   Buffer to receive the MQTT topic on match (can be NULL)
 * @param topic_size  Size of out_topic buffer
 * @return 1 if matched, 0 if no match
 */
int try_tool_registry_match(const char *input,
                            char *out_command,
                            size_t command_size,
                            char *out_topic,
                            size_t topic_size) {
   if (!input || !out_command || command_size == 0) {
      return 0;
   }

   int tool_count = tool_registry_count();
   if (tool_count == 0) {
      return 0;
   }

   /* Iterate through all registered tools to find pattern match.
    *
    * NOTE: This O(tools × patterns) iteration is intentional and necessary.
    * Voice command matching must check each tool's device name/aliases against
    * the spoken input. With ~30 tools and ~5 patterns each, this performs ~150
    * string comparisons per voice command - completing in <1ms on Jetson.
    * Voice commands arrive at ~1/second max, so this is not a hot path concern.
    * No early-exit optimization is possible without knowing which tool will match.
    */
   for (int i = 0; i < tool_count; i++) {
      const tool_metadata_t *tool = tool_registry_get_by_index(i);
      if (!tool) {
         continue;
      }

      /* Get device type definition for this tool */
      const device_type_def_t *type_def = device_type_get_def(tool->device_type);
      if (!type_def) {
         continue;
      }

      /* Build aliases array (NULL-terminated) */
      const char *aliases[TOOL_ALIAS_MAX + 1];
      for (int j = 0; j < tool->alias_count && j < TOOL_ALIAS_MAX; j++) {
         aliases[j] = tool->aliases[j];
      }
      aliases[tool->alias_count < TOOL_ALIAS_MAX ? tool->alias_count : TOOL_ALIAS_MAX] = NULL;

      /* Try to match input against this tool's device type patterns */
      const char *action = NULL;
      char value[DEVICE_TYPE_MAX_VALUE] = { 0 };

      if (device_type_match_pattern(type_def, input, tool->device_string, aliases, &action, value,
                                    sizeof(value))) {
         /* Match found! Build command JSON using json-c for proper escaping */
         struct json_object *cmd_json = json_object_new_object();
         json_object_object_add(cmd_json, "device", json_object_new_string(tool->device_string));
         json_object_object_add(cmd_json, "action", json_object_new_string(action));
         if (value[0] != '\0') {
            json_object_object_add(cmd_json, "value", json_object_new_string(value));
         }

         const char *json_str = json_object_to_json_string(cmd_json);
         safe_strncpy(out_command, json_str, command_size);
         json_object_put(cmd_json);

         if (out_topic && topic_size > 0) {
            safe_strncpy(out_topic, tool->topic, topic_size);
         }

         OLOG_INFO("TREG MATCH: \"%s\" → tool=%s, action=%s, value=%s", input, tool->name, action,
                   value[0] ? value : "(none)");

         return 1;
      }
   }

   return 0;
}
