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
 * Schema migration v112: a user's persona in two halves, values and voice,
 * each with its own text and mode (added to the server's half, or replacing
 * it).  The older single text (persona_description / persona_mode) is kept
 * as it is: it still applies until the user clears it.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v112(sqlite3 *db) {
   static const struct {
      const char *name, *sql;
   } columns[] = {
      { "persona_values", "ALTER TABLE user_settings ADD COLUMN persona_values TEXT" },
      { "persona_values_mode",
        "ALTER TABLE user_settings ADD COLUMN persona_values_mode TEXT DEFAULT 'append'" },
      { "persona_voice", "ALTER TABLE user_settings ADD COLUMN persona_voice TEXT" },
      { "persona_voice_mode",
        "ALTER TABLE user_settings ADD COLUMN persona_voice_mode TEXT DEFAULT 'append'" },
   };
   /* A fresh database has them already (the base schema); a rerun skips
    * the ones an earlier attempt added. */
   for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
      if (auth_db_column_exists(db, "user_settings", columns[i].name)) {
         continue;
      }
      char *errmsg = NULL;
      if (sqlite3_exec(db, columns[i].sql, NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v112 %s failed: %s", columns[i].name, errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }
   return AUTH_DB_SUCCESS;
}
