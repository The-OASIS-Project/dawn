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
 * A conversation's frozen prefix message (prefix_message.h).
 */

#include "core/prefix_message.h"

#include <json-c/json.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "llm/llm_history_kind.h"
#include "logging.h"

struct json_object *prefix_message_new(const char *text,
                                       const char *tools_json,
                                       const char *in_force_json) {
   struct json_object *msg = json_object_new_object();
   if (!msg) {
      return NULL;
   }
   json_object_object_add(msg, "role", json_object_new_string("system"));
   json_object_object_add(msg, "content", json_object_new_string(text ? text : ""));
   llm_history_set_kind(msg, MESSAGE_KIND_PREFIX);
   struct json_object *tools = tools_json ? json_tokener_parse(tools_json) : NULL;
   if (json_object_is_type(tools, json_type_array)) {
      json_object_object_add(msg, LLM_HISTORY_TOOLS_KEY, tools);
   } else {
      json_object_put(tools);
   }
   /* What is in force (prefix_in_force.h): without it, the next turn works it
    * out from the prefix's text. */
   struct json_object *in_force = in_force_json ? json_tokener_parse(in_force_json) : NULL;
   if (json_object_is_type(in_force, json_type_object)) {
      json_object_object_add(msg, LLM_HISTORY_IN_FORCE_KEY, in_force);
   } else {
      json_object_put(in_force);
   }
   return msg;
}

struct json_object *prefix_message_stored(int64_t conv_id, int user_id) {
   conv_prefix_t stored;
   const int rc = conv_db_prefix_get(conv_id, user_id, &stored);
   if (rc != AUTH_DB_SUCCESS || !stored.prefix) {
      if (rc == AUTH_DB_INVALID) {
         OLOG_WARNING("prefix: conv %lld's stored prompt is unusable; its next "
                      "turn freezes a new one",
                      (long long)conv_id);
      }
      conv_prefix_free(&stored);
      return NULL;
   }
   struct json_object *msg = prefix_message_new(stored.prefix, stored.tools, stored.in_force);
   conv_prefix_free(&stored);
   return msg;
}

bool prefix_message_install(struct json_object *history, struct json_object *prefix) {
   if (!json_object_is_type(history, json_type_array) || !prefix) {
      json_object_put(prefix);
      return false;
   }
   struct json_object *rest = json_object_new_array();
   if (!rest) {
      json_object_put(prefix);
      return false;
   }
   const size_t n = json_object_array_length(history);
   for (size_t i = 0; i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role = NULL;
      const bool plain_system = json_object_object_get_ex(msg, "role", &role) &&
                                strcmp(json_object_get_string(role), "system") == 0 &&
                                llm_history_kind_of(msg) == MESSAGE_KIND_NONE;
      if (!(i == 0 && plain_system)) {
         json_object_array_add(rest, json_object_get(msg));
      }
   }
   json_object_array_del_idx(history, 0, n);
   json_object_array_add(history, prefix);
   const size_t m = json_object_array_length(rest);
   for (size_t i = 0; i < m; i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(rest, i)));
   }
   json_object_put(rest);
   return true;
}
