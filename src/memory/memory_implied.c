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
 * The entities every turn implies: the user's own and the assistant's.
 */

#include "memory/memory_implied.h"

#include "config/dawn_config.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"

static void add_class(int user_id, int64_t entity_id, memory_implied_entities_t *out) {
   int n = 0;
   if (entity_id <= 0 ||
       memory_db_entity_class_members(user_id, entity_id, out->ids + out->count,
                                      MEMORY_ENTITY_CLASS_MAX, &n) != MEMORY_DB_SUCCESS)
      return;
   out->count += n;
}

void memory_implied_entities_resolve(int user_id, memory_implied_entities_t *out) {
   out->count = 0;
   int64_t self = 0;
   if (memory_db_entity_get_user_self_id(user_id, &self) == MEMORY_DB_SUCCESS)
      add_class(user_id, self, out);
   if (g_config.general.ai_name[0]) {
      char canonical[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(g_config.general.ai_name, canonical, sizeof(canonical));
      int64_t assistant = 0;
      if (memory_db_entity_root_by_name(user_id, canonical, &assistant) == MEMORY_DB_SUCCESS)
         add_class(user_id, assistant, out);
   }
}

bool memory_implied_entities_has(const memory_implied_entities_t *implied, int64_t entity_id) {
   for (int i = 0; i < implied->count; i++) {
      if (implied->ids[i] == entity_id)
         return true;
   }
   return false;
}
