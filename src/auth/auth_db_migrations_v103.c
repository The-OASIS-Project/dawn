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
 * Schema migration v103: memory_relations.closed_by_fact_id, the fact whose new
 * value closed the relation (memory_db_relation_supersede_ex), so restoring a
 * superseded fact reopens exactly the relations that fact closed, and none a
 * user stated as ended ("worked there 2018-2020").
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v103(sqlite3 *db) {
   if (auth_db_column_exists(db, "memory_relations", "closed_by_fact_id")) {
      return AUTH_DB_SUCCESS;
   }
   char *errmsg = NULL;
   if (sqlite3_exec(
           db,
           "ALTER TABLE memory_relations ADD COLUMN closed_by_fact_id INTEGER DEFAULT NULL "
           "REFERENCES memory_facts(id) ON DELETE SET NULL",
           NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v103 closed_by_fact_id failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
