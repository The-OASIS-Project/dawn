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
 * Schema migration v75: deep-research foundation.
 *
 * Adds:
 *   1. `job_kind` discriminator on `conversations`.  A research run IS a
 *      background-job conversation, so the plain resume path (job_worker_resume)
 *      would otherwise grab it and re-dispatch the ordinary worker against an
 *      orphaned research_runs row.  `job_kind='research'` lets resume refuse it.
 *      NULL = an ordinary job (the existing behavior).
 *   2. The four `research_*` tables — the entire research state lives in SQLite
 *      rows (never an in-memory graph), so a run is restart-durable for free and
 *      the report is a VIEW over `research_claims`.  FK cascade off the parent
 *      conversation collapses the whole run on delete (PRAGMA foreign_keys=ON is
 *      set per-connection in auth_db_core.c).
 *   3. `idx_conv_jobs_user` — the paginated job/research list partial index
 *      (user_id, created_at DESC, id DESC) WHERE job_status IS NOT NULL.  This
 *      rides WITH the CONV_MAX_PER_USER 1000->5000 bump (auth_db.h): both list
 *      readers (conv_db_job_list_by_user + the keyset page) filter exactly that
 *      predicate, and their cursor is already sargable, so the index is the only
 *      missing half.  A 5000-cap scan without it is the regression.
 *
 * v72/v74 discipline is reused: the ALTER is not idempotent, so `job_kind` is
 * probed first; tables and indexes use CREATE ... IF NOT EXISTS so they are safe
 * whether or not the base SCHEMA_SQL already created them this boot.  The
 * schema_version bump hard-gates on this returning SUCCESS.
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
static bool v75_column_exists(sqlite3 *db, const char *table, const char *col) {
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

static int v75_exec_or_fail(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v75 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v75(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   /* 1. job_kind discriminator (probe-first ALTER; NULL = ordinary job). */
   if (!v75_column_exists(db, "conversations", "job_kind")) {
      if (v75_exec_or_fail(db, "ALTER TABLE conversations ADD COLUMN job_kind TEXT DEFAULT NULL",
                           "job_kind column") != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
   }

   /* 2. Research tables.  All research state is SQLite rows (design invariant);
    *    FK -> conversations(id)/research_runs(id) ON DELETE CASCADE collapses the
    *    whole run on delete.  The DDL is shared verbatim with the base schema via
    *    AUTH_DB_RESEARCH_SCHEMA_SQL (auth_db_internal.h) so fresh installs and
    *    migrated DBs can never diverge; sqlite3_exec runs the multi-statement
    *    string in one call. */
   if (v75_exec_or_fail(db, AUTH_DB_RESEARCH_SCHEMA_SQL, "research schema") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   /* 3. Paginated job/research list partial index.  Both list readers filter
    *    (user_id, job_status IS NOT NULL) ORDER BY created_at DESC [, id DESC];
    *    the keyset cursor is already sargable, so this index is the half that
    *    was missing.  Rides with the CONV_MAX_PER_USER 5000 bump. */
   if (v75_exec_or_fail(db,
                        "CREATE INDEX IF NOT EXISTS idx_conv_jobs_user "
                        "ON conversations(user_id, created_at DESC, id DESC) "
                        "WHERE job_status IS NOT NULL",
                        "idx_conv_jobs_user") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
