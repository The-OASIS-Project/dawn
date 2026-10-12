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
 * user stated as ended ("worked there 2018-2020"); and its index, which every
 * fact delete needs (the foreign key's ON DELETE SET NULL looks it up).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* Run @p sql, logging @p what on failure. */
static int v103_exec(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v103 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v103(sqlite3 *db) {
   if (!auth_db_column_exists(db, "memory_relations", "closed_by_fact_id") &&
       v103_exec(db,
                 "ALTER TABLE memory_relations ADD COLUMN closed_by_fact_id INTEGER DEFAULT NULL "
                 "REFERENCES memory_facts(id) ON DELETE SET NULL",
                 "closed_by_fact_id") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   /* Deleting a fact sets this column NULL wherever it names the fact: without
    * an index every fact delete scans all of the relations. */
   return v103_exec(db,
                    "CREATE INDEX IF NOT EXISTS idx_memory_relations_closed_by ON "
                    "memory_relations(closed_by_fact_id) WHERE closed_by_fact_id IS NOT NULL",
                    "closed_by index");
}
