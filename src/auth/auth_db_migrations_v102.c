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
 * Schema migration v102: messages.email_ref, on a question row the email the
 * user attached to it as the panel names it (a JSON object), so a reload can
 * show what was attached.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v102(sqlite3 *db) {
   if (auth_db_column_exists(db, "messages", "email_ref")) {
      return AUTH_DB_SUCCESS;
   }
   char *errmsg = NULL;
   if (sqlite3_exec(db, "ALTER TABLE messages ADD COLUMN email_ref TEXT DEFAULT NULL", NULL, NULL,
                    &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v102 email_ref failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
