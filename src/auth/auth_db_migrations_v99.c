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
 * Schema migration v99: memory_citation_audit.referenced_ids.  A turn sends
 * only the retrieved items its conversation doesn't already show, and names
 * the relevant ones it does show on a "[still relevant: ...]" line; the audit
 * keeps those named items apart from the sent ones (injected_ids), so the
 * cite rate of each can be measured.  Rows from before have no value (NULL).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v99(sqlite3 *db) {
   if (auth_db_column_exists(db, "memory_citation_audit", "referenced_ids")) {
      return AUTH_DB_SUCCESS;
   }
   char *errmsg = NULL;
   if (sqlite3_exec(db, "ALTER TABLE memory_citation_audit ADD COLUMN referenced_ids TEXT", NULL,
                    NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v99 ALTER (memory_citation_audit.referenced_ids) failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
