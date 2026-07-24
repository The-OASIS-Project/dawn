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
 * Schema migration v73: background-jobs Phase 1 (lifecycle + monitor).
 *
 * Builds on the v72 foundation (job columns + conversation_events).  Adds:
 *   1. `deliver_to` on `conversations` — the completion-notification target for
 *      a job (empty/NULL = local chime+banner+voice; a messaging channel
 *      display_name = deliver the "job done" alert to that channel, mirroring
 *      the scheduler `deliver_to` semantics).  v72's §3 column set referenced a
 *      per-job deliver target but had no column for it; storing it here keeps
 *      the target restart-safe (the completion monitor fires after a reboot).
 *   2. A partial index over (job_status, on_complete_fired) for the once-per-
 *      second completion monitor's pending-follow-up scan, so an idle system
 *      never full-scans `conversations`.
 *
 * v72's discipline is reused: ALTER ADD COLUMN is not idempotent, so the column
 * is probed first; the schema_version bump hard-gates on this returning SUCCESS.
 * The index is created HERE (not in base SCHEMA_SQL) because the base schema
 * runs before migrations on every boot — indexing a migration-added column
 * there would fail "no such column" on an existing DB (same rule as v69/v72).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* True if @col exists on @table.  @table is a fixed literal (not user input);
 * PRAGMA cannot be parameterized, so it is interpolated directly. */
static bool v73_column_exists(sqlite3 *db, const char *table, const char *col) {
   char sql[128];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool found = false;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *name = sqlite3_column_text(st, 1); /* col 1 = column name */
      if (name != NULL && strcmp((const char *)name, col) == 0) {
         found = true;
         break;
      }
   }
   sqlite3_finalize(st);
   return found;
}

static int v73_exec_or_fail(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v73 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v73(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   /* Job completion-notification target (channel display_name or NULL/local). */
   if (!v73_column_exists(db, "conversations", "deliver_to")) {
      if (v73_exec_or_fail(db, "ALTER TABLE conversations ADD COLUMN deliver_to TEXT DEFAULT NULL",
                           "deliver_to column") != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
   }

   /* Completion-monitor scan: terminal job rows awaiting their follow-up.  The
    * WHERE clause keeps this off every non-job conversation row. */
   if (v73_exec_or_fail(db,
                        "CREATE INDEX IF NOT EXISTS idx_conv_job_followup "
                        "ON conversations(job_status, on_complete_fired) "
                        "WHERE job_status IS NOT NULL",
                        "idx_conv_job_followup") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
