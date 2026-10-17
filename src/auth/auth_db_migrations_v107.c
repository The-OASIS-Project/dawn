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
 * Schema migration v107: a user's time zone setting, unset or a real zone.
 * 'UTC' was the default written for everyone who never chose a zone, so it
 * becomes unset (''): the server's zone.  Two names the settings list offered
 * aren't zones and become the ones they meant.  Calendars are fetched again:
 * times a daylight-saving change skips or repeats are now read as RFC 5545
 * says (a skipped time moves forward, a repeated one is the earlier).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v107(sqlite3 *db) {
   char *errmsg = NULL;
   if (sqlite3_exec(db,
                    "UPDATE user_settings SET timezone = '' "
                    "WHERE timezone IS NULL OR timezone = 'UTC';"
                    "UPDATE user_settings SET timezone = 'Pacific/Honolulu' "
                    "WHERE timezone = 'America/Honolulu';"
                    "UPDATE user_settings SET timezone = 'Asia/Kolkata' "
                    "WHERE timezone = 'Asia/Mumbai'",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v107 time zone settings failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return auth_db_migrations_calendar_refetch(db);
}
