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
 * Merging a background compaction result into the history a later turn runs on.
 */

#include "llm/llm_context_merge.h"

#include <string.h>

static bool message_is_system(struct json_object *msg) {
   struct json_object *role = NULL;
   return msg && json_object_object_get_ex(msg, "role", &role) &&
          strcmp(json_object_get_string(role), "system") == 0;
}

bool llm_context_merge_compacted(struct json_object *history,
                                 struct json_object *compacted,
                                 struct json_object *snapshot_last) {
   if (!history || !compacted || !snapshot_last) {
      return false;
   }
   const int len = (int)json_object_array_length(history);
   int boundary = -1;
   for (int i = len - 1; i >= 0; i--) {
      if (json_object_array_get_idx(history, i) == snapshot_last) {
         boundary = i;
         break;
      }
   }
   if (boundary < 0) {
      return false;
   }
   int n_leading = 0;
   while (n_leading < len && message_is_system(json_object_array_get_idx(history, n_leading))) {
      n_leading++;
   }
   if (boundary < n_leading) {
      return false; /* the summary would stand in for this turn's own prompt */
   }
   struct json_object *merged = json_object_new_array();
   if (!merged) {
      return false;
   }
   for (int i = 0; i < n_leading; i++) {
      json_object_array_add(merged, json_object_get(json_object_array_get_idx(history, i)));
   }
   /* The compacted body without its own leading prompt; a system message later
    * in it (the summary, one from a history an older build saved) stays. */
   const int n_compacted = (int)json_object_array_length(compacted);
   int skip = 0;
   while (skip < n_compacted && message_is_system(json_object_array_get_idx(compacted, skip))) {
      skip++;
   }
   for (int i = skip; i < n_compacted; i++) {
      json_object_array_add(merged, json_object_get(json_object_array_get_idx(compacted, i)));
   }
   for (int i = boundary + 1; i < len; i++) {
      json_object_array_add(merged, json_object_get(json_object_array_get_idx(history, i)));
   }
   /* In place: the array is the one the session and the turn hold. */
   json_object_array_del_idx(history, 0, (size_t)len);
   const int n = (int)json_object_array_length(merged);
   for (int i = 0; i < n; i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(merged, i)));
   }
   json_object_put(merged);
   return true;
}
