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
 * JSON from outside DAWN, made safe to read.  json-c reports a member whose
 * value is `null` as present with a NULL value, and most readers then pass that
 * NULL on (json_object_get_string(NULL) is NULL, json_object_new_string(NULL)
 * crashes).  Dropping null members once, where a request is parsed, makes them
 * read as absent everywhere after.  Header-only: depends on json-c alone.
 */

#ifndef JSON_NULLS_H
#define JSON_NULLS_H

#include <json-c/json.h>
#include <json-c/linkhash.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Remove every member whose value is null from @p obj, through nested
 *        objects and objects inside arrays.
 *
 * Null array elements stay (removing one would shift the indexes after it).
 * Safe on NULL and on any value that isn't an object or array.  The recursion
 * is as deep as the value's nesting, which json-c's parser bounds (32 levels by
 * default).
 *
 * @param obj The value to clean, changed in place
 * @return How many members were removed
 */
static inline int json_drop_nulls(struct json_object *obj) {
   int dropped = 0;
   if (json_object_is_type(obj, json_type_array)) {
      const size_t n = json_object_array_length(obj);
      for (size_t i = 0; i < n; i++) {
         dropped += json_drop_nulls(json_object_array_get_idx(obj, i));
      }
   } else if (json_object_is_type(obj, json_type_object)) {
      struct lh_table *members = json_object_get_object(obj);
      struct lh_entry *e = NULL;
      struct lh_entry *next = NULL;
      /* lh_foreach_safe keeps the next entry, so the current one can go. */
      lh_foreach_safe(members, e, next) {
         struct json_object *v = (struct json_object *)lh_entry_v(e);
         if (v) {
            dropped += json_drop_nulls(v);
         } else {
            lh_table_delete_entry(members, e);
            dropped++;
         }
      }
   }
   return dropped;
}

#ifdef __cplusplus
}
#endif

#endif /* JSON_NULLS_H */
