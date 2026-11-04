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
 * Schema migration v115: message_unprompted.replayed.  A post DAWN makes from
 * now on is told to the conversation's next turn in its context and left out
 * of the replay (conv_db_posted_after); one already stored was never told
 * that way, so it keeps replaying as it did (replayed = 1, set only in the run
 * that adds the column, in one transaction with it).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v115(sqlite3 *db) {
   if (auth_db_column_exists(db, "message_unprompted", "replayed")) {
      return AUTH_DB_SUCCESS;
   }
   char *errmsg = NULL;
   if (sqlite3_exec(db,
                    "BEGIN IMMEDIATE;"
                    "ALTER TABLE message_unprompted ADD COLUMN replayed INTEGER NOT NULL "
                    "DEFAULT 0;"
                    "UPDATE message_unprompted SET replayed = 1;"
                    "COMMIT;",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v115 message_unprompted.replayed failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
