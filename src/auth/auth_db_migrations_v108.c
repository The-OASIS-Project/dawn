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
 * Schema migration v108: a repeating scheduled event's time of day
 * (original_time, HH:MM) moves from the daemon's clock to its owner's.  The
 * scheduler now repeats events in their owner's zone; converting each
 * still-live row at the moment it fires keeps it firing when it does today.
 * Runs under the auth_db lock, so the owner's zone is read here with SQL
 * (never user_tz(), which takes that lock).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/tz.h"
#include "logging.h"

#define V108_DAY 86400

static const char *const V108_NAMES[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

/* @p hhmm's clock (seconds past midnight), or -1 when it isn't HH:MM. */
static long v108_clock(const char *hhmm) {
   int hour = -1, minute = -1;
   if (!hhmm || sscanf(hhmm, "%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 ||
       minute > 59)
      return -1;
   return (long)hour * 3600 + (long)minute * 60;
}

/* Convert every row, or none, and once: converting again would move each
 * alarm by the zones' difference a second time.  A rerun happens when this
 * succeeds and another migration on the same start fails (the schema version
 * then isn't raised), so a mark written with the rows says it's done. */
static int v108_convert(sqlite3 *db);

int auth_db_migrations_v108(sqlite3 *db) {
   if (sqlite3_exec(db, "SAVEPOINT v108", NULL, NULL, NULL) != SQLITE_OK)
      return AUTH_DB_FAILURE;
   int rc = AUTH_DB_FAILURE;
   if (sqlite3_exec(db,
                    "CREATE TABLE IF NOT EXISTS migration_marks (name TEXT PRIMARY KEY);"
                    "INSERT OR IGNORE INTO migration_marks (name) VALUES ('v108')",
                    NULL, NULL, NULL) == SQLITE_OK) {
      /* No row added: the mark was there, done before.  Any other error
       * fails the migration, so it runs again next start. */
      rc = sqlite3_changes(db) > 0 ? v108_convert(db) : AUTH_DB_SUCCESS;
   }
   if (rc != AUTH_DB_SUCCESS)
      sqlite3_exec(db, "ROLLBACK TO v108", NULL, NULL, NULL);
   if (sqlite3_exec(db, "RELEASE v108", NULL, NULL, NULL) != SQLITE_OK)
      return AUTH_DB_FAILURE;
   return rc;
}

/* The weekdays (bit 0 = Sunday) @p recurrence fires on, or 0 for a rule that
 * doesn't name days (daily, weekly).  Reads "custom" days as the scheduler
 * does: three-letter names, any case, separated by commas or spaces. */
static int v108_days(const char *recurrence, const char *days) {
   if (!recurrence)
      return 0;
   if (strcmp(recurrence, "weekdays") == 0)
      return 0x3e;
   if (strcmp(recurrence, "weekends") == 0)
      return 0x41;
   if (strcmp(recurrence, "custom") != 0 || !days)
      return 0;
   int mask = 0;
   for (const char *p = days; *p;) {
      while (*p == ' ' || *p == ',')
         p++;
      for (int d = 0; d < 7; d++) {
         if (strncasecmp(p, V108_NAMES[d], 3) == 0 && (p[3] == '\0' || p[3] == ',' || p[3] == ' '))
            mask |= 1 << d;
      }
      while (*p && *p != ',')
         p++;
   }
   return mask;
}

static int v108_convert(sqlite3 *db) {
   sqlite3_stmt *rows = NULL, *update = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT e.id, e.fire_at, e.original_time, s.timezone, "
                          "(e.snooze_count > 0 OR e.status = 'snoozed'), "
                          "e.recurrence, e.recurrence_days "
                          "FROM scheduled_events e LEFT JOIN user_settings s ON s.user_id = "
                          "e.user_id WHERE e.recurrence != 'once' AND e.status IN "
                          "('pending', 'snoozed', 'ringing')",
                          -1, &rows, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db,
                          "UPDATE scheduled_events SET original_time = COALESCE(?, original_time), "
                          "recurrence = COALESCE(?, recurrence), "
                          "recurrence_days = COALESCE(?, recurrence_days) WHERE id = ?",
                          -1, &update, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v108 prepare failed: %s", sqlite3_errmsg(db));
      sqlite3_finalize(rows);
      return AUTH_DB_FAILURE;
   }
   const tz_t *daemon = tz_daemon();
   int rc = SQLITE_DONE;
   while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
      const tz_t *owner = tz_lookup((const char *)sqlite3_column_text(rows, 3));
      if (!owner || owner == daemon)
         continue; /* the same clock */
      /* The occurrence it stands for: the daemon's clock at original_time on
       * fire_at's date (the day before when a snooze carried it past
       * midnight).  With no original_time the scheduler uses fire_at's time,
       * which needs no converting, but its named days still may. */
      const time_t fire_at = (time_t)sqlite3_column_int64(rows, 1);
      const time_t fire_wall = tz_wall(daemon, fire_at);
      const long stored = v108_clock((const char *)sqlite3_column_text(rows, 2));
      const long clock = stored >= 0 ? stored : (long)(fire_wall % V108_DAY);
      time_t day = fire_wall - fire_wall % V108_DAY;
      if (sqlite3_column_int(rows, 4) > 0 && tz_from_wall(daemon, day + clock) > fire_at)
         day -= V108_DAY;
      const time_t owner_wall = tz_wall(owner, tz_from_wall(daemon, day + clock));
      const long owner_clock = (long)(owner_wall % V108_DAY);
      char hhmm[8];
      snprintf(hhmm, sizeof(hhmm), "%02ld:%02ld", owner_clock / 3600, owner_clock / 60 % 60);

      /* On the owner's clock the occurrence may fall the day before or
       * after: a rule naming weekdays moves with it, as named days. */
      const int shift = (int)((owner_wall - owner_clock - day) / V108_DAY);
      const int mask = v108_days((const char *)sqlite3_column_text(rows, 5),
                                 (const char *)sqlite3_column_text(rows, 6));
      char days[32] = "";
      size_t len = 0;
      if (shift != 0 && mask != 0) {
         for (int d = 0; d < 7; d++) {
            if (mask & (1 << ((d - shift + 7) % 7)) && len < sizeof(days))
               len += (size_t)snprintf(days + len, sizeof(days) - len, "%s%s", len ? "," : "",
                                       V108_NAMES[d]);
         }
      }
      if (stored < 0 && !days[0])
         continue;
      sqlite3_reset(update);
      if (stored >= 0)
         sqlite3_bind_text(update, 1, hhmm, -1, SQLITE_TRANSIENT);
      else
         sqlite3_bind_null(update, 1);
      if (days[0]) {
         sqlite3_bind_text(update, 2, "custom", -1, SQLITE_STATIC);
         sqlite3_bind_text(update, 3, days, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(update, 2);
         sqlite3_bind_null(update, 3);
      }
      sqlite3_bind_int64(update, 4, sqlite3_column_int64(rows, 0));
      if (sqlite3_step(update) != SQLITE_DONE) {
         rc = SQLITE_ERROR;
         break;
      }
   }
   sqlite3_finalize(rows);
   sqlite3_finalize(update);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: v108 migration failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
