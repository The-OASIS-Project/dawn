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
 * Schema migration v104: memory_facts.superseded_kind (a merged duplicate or a
 * replaced value: forgetting a fact takes its duplicates and brings back what
 * it replaced), and an index on memory_relations.closed_by_fact_id, which every
 * fact delete needs (the foreign key's ON DELETE SET NULL looks it up).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* Run @p sql, logging @p what on failure. */
static int v104_exec(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v104 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v104(sqlite3 *db) {
   /* Rows superseded before this count as merged, as forgetting treated them. */
   if (!auth_db_column_exists(db, "memory_facts", "superseded_kind") &&
       v104_exec(db,
                 "ALTER TABLE memory_facts ADD COLUMN superseded_kind INTEGER NOT NULL DEFAULT 0",
                 "superseded_kind") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   return v104_exec(db,
                    "CREATE INDEX IF NOT EXISTS idx_memory_relations_closed_by ON "
                    "memory_relations(closed_by_fact_id) WHERE closed_by_fact_id IS NOT NULL",
                    "closed_by index");
}
