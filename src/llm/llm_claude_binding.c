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
 * the project author(s).
 *
 * Anthropic thinking binding controls: parse what the API dropped.
 */

#include "llm/llm_claude_binding.h"

#include <string.h>

#include "utils/string_utils.h"

void llm_claude_drops_parse(struct json_object *transformations, llm_claude_drops_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   if (!transformations || !json_object_is_type(transformations, json_type_array)) {
      return;
   }
   out->reported = true;
   const size_t n = json_object_array_length(transformations);
   for (size_t i = 0; i < n; i++) {
      struct json_object *entry = json_object_array_get_idx(transformations, i);
      struct json_object *v = NULL;
      const char *type = NULL;
      const char *reason = NULL;
      if (json_object_object_get_ex(entry, "type", &v)) {
         type = json_object_get_string(v);
      }
      if (json_object_object_get_ex(entry, "reason", &v)) {
         reason = json_object_get_string(v);
      }
      const bool dropped = type && strcmp(type, "thinking_dropped") == 0;
      if (dropped && reason && strcmp(reason, "prefix_binding_mismatch") == 0) {
         if (out->prefix_drops++ == 0 && json_object_object_get_ex(entry, "path", &v)) {
            safe_strscpy(out->first_path, json_object_get_string(v));
         }
      } else if (dropped && reason && strcmp(reason, "model_binding_mismatch") == 0) {
         out->model_drops++;
      } else {
         out->other_drops++;
      }
   }
}

void llm_claude_drops_from_message(struct json_object *message, llm_claude_drops_t *out) {
   struct json_object *list = NULL;
   if (message && json_object_object_get_ex(message, "input_transformations", &list)) {
      llm_claude_drops_parse(list, out);
   }
}
