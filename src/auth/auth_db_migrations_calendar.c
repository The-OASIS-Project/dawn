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
 * Calendar refetch, a migration step with no schema change: forget each
 * calendar's stored ctag, so the next sync fetches every calendar once and
 * stores its events again.  Used when the way events are cached changes:
 *   v105: recurring events repeat in the event's own time zone (cached ones
 *         were expanded in UTC and sit an hour off after a daylight-saving
 *         change);
 *   v106: a series' cancelled dates (EXDATE) and moved instances are applied,
 *         each instance carries its key, and every event gets its server path.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_calendar_refetch(sqlite3 *db) {
   char *err = NULL;
   if (sqlite3_exec(db, "UPDATE calendar_calendars SET ctag = ''", NULL, NULL, &err) != SQLITE_OK) {
      OLOG_ERROR("auth_db: calendar refetch failed: %s", err ? err : "unknown");
      sqlite3_free(err);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
