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
 * Schema migration v66: add branch / kind / graph_name to code_projects
 * (coding-harness branch tracking, link-local repos, persisted cbm graph slug).
 *
 * ALTER TABLE ADD COLUMN is NOT idempotent (it errors on a duplicate column),
 * and the migration ladder hard-gates the schema_version bump on this function
 * returning AUTH_DB_SUCCESS. A non-idempotent ALTER would therefore wedge the
 * version on any re-run (e.g. after a partial failure). We probe
 * PRAGMA table_info first and skip columns that already exist, so v66 is safe to
 * re-run and returns SUCCESS whenever all three columns are present.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* True if @col exists on @table. @table is a fixed literal (not user input);
 * PRAGMA cannot be parameterized, so it is interpolated directly. */
static bool cp_column_exists(sqlite3 *db, const char *table, const char *col) {
   char sql[128];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool found = false;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *name = sqlite3_column_text(st, 1); /* col 1 = column name */
      if (name != NULL && strcmp((const char *)name, col) == 0) {
         found = true;
         break;
      }
   }
   sqlite3_finalize(st);
   return found;
}

/* Add a column only if missing. @coldef is the full "<name> <type> [...]" clause. */
static int cp_add_column(sqlite3 *db, const char *col, const char *coldef) {
   if (cp_column_exists(db, "code_projects", col)) {
      return AUTH_DB_SUCCESS;
   }
   char sql[256];
   snprintf(sql, sizeof(sql), "ALTER TABLE code_projects ADD COLUMN %s", coldef);
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v66 ALTER (%s) failed: %s", col, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v66(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }
   /* branch: tracked git branch (NULL/'' = remote HEAD / whatever is checked out).
    * kind: 'clone' (DAWN-managed clone) | 'local' (linked existing checkout) —
    *       validated in C (code_project_db_create), not a CHECK constraint, to
    *       avoid a table rebuild; all writes must go through db_create.
    * graph_name: cbm's path-derived graph slug, persisted so delete/rebuild can
    *       target the right on-disk .db without a live cbm round-trip. */
   if (cp_add_column(db, "branch", "branch TEXT") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   if (cp_add_column(db, "kind", "kind TEXT NOT NULL DEFAULT 'clone'") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   if (cp_add_column(db, "graph_name", "graph_name TEXT") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
