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
 * Schema migration v95: memory_facts.superseded_at, when a fact was merged
 * into another.  Superseded facts are pruned a retention window after THAT,
 * not after they were created: pruning by creation time deleted a merged fact
 * older than the window at the next prune, so a merge meant to be
 * recoverable was permanent.  Facts already superseded are stamped with the
 * migration time, which gives them the full window from now.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v95(sqlite3 *db) {
   if (!auth_db_column_exists(db, "memory_facts", "superseded_at")) {
      char *errmsg = NULL;
      if (sqlite3_exec(db, "ALTER TABLE memory_facts ADD COLUMN superseded_at INTEGER DEFAULT NULL",
                       NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v95 ALTER (memory_facts.superseded_at) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }
   char *errmsg = NULL;
   if (sqlite3_exec(
           db,
           "UPDATE memory_facts SET superseded_at = CAST(strftime('%s', 'now') AS INTEGER) "
           "WHERE superseded_by IS NOT NULL AND superseded_at IS NULL",
           NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v95 stamping superseded facts failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   const int stamped = sqlite3_changes(db);
   if (stamped > 0) {
      OLOG_INFO("auth_db: v95 gave %d merged fact(s) a fresh retention window", stamped);
   }
   return AUTH_DB_SUCCESS;
}
