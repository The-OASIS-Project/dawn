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
 * Schema migration v65: code_projects (coding-harness imported repositories).
 * Idempotent; NOT gated on a feature flag (schema versioning is global). See
 * auth_db_migrations_v64.c for the rationale.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v65(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   /* user_id ON DELETE CASCADE (locked Phase 0); imported_by nullable + SET NULL
    * (best-effort audit of who imported it). */
   const char *sql = "CREATE TABLE IF NOT EXISTS code_projects ("
                     "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "  name        TEXT    NOT NULL UNIQUE,"
                     "  source_url  TEXT    NOT NULL,"
                     "  local_path  TEXT    NOT NULL,"
                     "  user_id     INTEGER REFERENCES users(id) ON DELETE CASCADE,"
                     "  is_global   INTEGER NOT NULL DEFAULT 0,"
                     "  status      TEXT    NOT NULL DEFAULT 'pending',"
                     "  status_msg  TEXT,"
                     "  created_at  INTEGER NOT NULL,"
                     "  updated_at  INTEGER NOT NULL,"
                     "  indexed_at  INTEGER,"
                     "  imported_by INTEGER REFERENCES users(id) ON DELETE SET NULL"
                     ");"
                     "CREATE INDEX IF NOT EXISTS idx_code_projects_user ON code_projects(user_id);";

   char *errmsg = NULL;
   int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: v65 code_projects creation failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
