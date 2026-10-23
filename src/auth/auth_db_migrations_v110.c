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
 * Schema migration v110: what a user forgot stays forgotten.  Its conversations'
 * messages still say it, so a later extraction of them could learn it again.
 *   memory_forgotten: one fact the user forgot, with the last message id at the
 *     time (forget_msg_id).  An extraction of a batch that began at or before
 *     it is checked against it; what was said after it is the user saying it
 *     again.  created_at: a voice session saved after the forget moves
 *     forget_msg_id past its rows (memory_forgotten_saved).
 *   conversations.extract_floor_msg_id: where a forget-everything or a
 *     conversation forget left the conversation; extracting it again from the
 *     start (the reextract wipe) begins after it.  No content.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v110(sqlite3 *db) {
   char *errmsg = NULL;
   if (sqlite3_exec(db,
                    "CREATE TABLE IF NOT EXISTS memory_forgotten ("
                    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                    "  fact_text TEXT NOT NULL,"
                    "  forget_msg_id INTEGER NOT NULL,"
                    "  created_at INTEGER NOT NULL);"
                    "CREATE INDEX IF NOT EXISTS idx_memory_forgotten_user "
                    "  ON memory_forgotten(user_id, forget_msg_id);",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v110 memory_forgotten failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   /* In the base schema too (a new database): there, the column is there. */
   if (sqlite3_exec(db,
                    "ALTER TABLE conversations ADD COLUMN extract_floor_msg_id INTEGER NOT NULL "
                    "DEFAULT 0",
                    NULL, NULL, &errmsg) != SQLITE_OK &&
       !(errmsg && strstr(errmsg, "duplicate column"))) {
      OLOG_ERROR("auth_db: v110 extract_floor_msg_id failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   sqlite3_free(errmsg);
   return AUTH_DB_SUCCESS;
}
