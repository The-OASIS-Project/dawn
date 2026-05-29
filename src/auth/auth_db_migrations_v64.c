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
 * Schema migration v64: mcp_user_access (coding-harness MCP bridge per-user
 * allowlist). Split out of auth_db_schema.c (eff-17) to avoid growing that
 * already-oversized file. Idempotent: safe to run on fresh installs and
 * upgrades. NOT gated on DAWN_ENABLE_MCP_BRIDGE_TOOL — schema versioning is
 * global and must advance uniformly across build configurations.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v64(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   const char *sql =
       "CREATE TABLE IF NOT EXISTS mcp_user_access ("
       "  user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
       "  server_alias TEXT    NOT NULL,"
       "  enabled      INTEGER NOT NULL DEFAULT 1,"
       "  updated_at   INTEGER NOT NULL,"
       "  PRIMARY KEY (user_id, server_alias)"
       ");"
       "CREATE INDEX IF NOT EXISTS idx_mcp_user_access_user ON mcp_user_access(user_id);";

   char *errmsg = NULL;
   int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: v64 mcp_user_access creation failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
