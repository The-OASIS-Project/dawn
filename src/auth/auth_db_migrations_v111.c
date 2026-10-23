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
 * Schema migration v111: scheduled_events.zone, the zone each event's fire_at
 * was placed in.  Every live row was placed on its owner's clock (v108), so
 * it gets the owner's zone: theirs when set, else the daemon's.  Runs under
 * the auth_db lock, so the owner's zone is read here with SQL (never
 * user_tz(), which takes that lock).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/tz.h"
#include "logging.h"

int auth_db_migrations_v111(sqlite3 *db) {
   /* A fresh database has the column already (the base schema). */
   char *errmsg = NULL;
   if (sqlite3_exec(db, "ALTER TABLE scheduled_events ADD COLUMN zone TEXT", NULL, NULL, &errmsg) !=
           SQLITE_OK &&
       !(errmsg && strstr(errmsg, "duplicate column"))) {
      OLOG_ERROR("auth_db: v111 migration (scheduled zones) failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   sqlite3_free(errmsg);

   /* The name a new row would get (tz_name(user_tz())): the setting read as
    * user_tz reads it, so an old spelling or a bad name records the zone in
    * force.  tz_lookup takes no auth_db lock. */
   sqlite3_stmt *rows = NULL, *update = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT e.id, s.timezone FROM scheduled_events e LEFT JOIN "
                          "user_settings s ON s.user_id = e.user_id WHERE e.zone IS NULL AND "
                          "e.status IN ('pending', 'snoozed', 'ringing')",
                          -1, &rows, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db, "UPDATE scheduled_events SET zone = ? WHERE id = ?", -1, &update,
                          NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v111 prepare failed: %s", sqlite3_errmsg(db));
      sqlite3_finalize(rows);
      return AUTH_DB_FAILURE;
   }
   /* One commit for the backfill, all or nothing (it reruns: zone IS NULL). */
   int rc = sqlite3_exec(db, "SAVEPOINT v111", NULL, NULL, NULL) == SQLITE_OK ? SQLITE_ROW
                                                                              : SQLITE_ERROR;
   while (rc == SQLITE_ROW && (rc = sqlite3_step(rows)) == SQLITE_ROW) {
      const tz_t *zone = tz_lookup((const char *)sqlite3_column_text(rows, 1));
      sqlite3_reset(update);
      sqlite3_bind_text(update, 1, tz_name(zone ? zone : tz_daemon()), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(update, 2, sqlite3_column_int64(rows, 0));
      if (sqlite3_step(update) != SQLITE_DONE) {
         rc = SQLITE_ERROR;
         break;
      }
   }
   sqlite3_finalize(rows);
   sqlite3_finalize(update);
   if (rc != SQLITE_DONE)
      sqlite3_exec(db, "ROLLBACK TO v111", NULL, NULL, NULL);
   if (sqlite3_exec(db, "RELEASE v111", NULL, NULL, NULL) != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); /* never leave it open */
      rc = SQLITE_ERROR;
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: v111 migration failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
