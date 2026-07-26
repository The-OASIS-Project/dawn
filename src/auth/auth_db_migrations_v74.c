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
 * Schema migration v74: durable job goal.
 *
 * Adds `job_goal` to `conversations` — the instruction a background job was
 * created with, written at CREATE time rather than emerging as a side effect of
 * a successful dispatch.
 *
 * The bug it fixes: until now the goal existed ONLY as the conversation's first
 * `messages` row, which core_text_input_dispatch writes once the worker is
 * running.  A job that failed BEFORE dispatch — the "job capacity reached"
 * class, or any pre-dispatch failure — therefore left an empty conversation
 * whose goal was nowhere recoverable (`title` is a generated summary, not the
 * instruction).  Resuming one re-engaged the model with a "continue where you
 * left off" directive and no task, which produced a confident non-answer, marked
 * the job `done`, and — because `done` is not resumable — destroyed the retry.
 * Observed live on conv 970.
 *
 * BACKFILL: existing rows get their goal from their own first user message where
 * one exists.  Rows that never dispatched have no such message and stay NULL;
 * those are unresumable by construction and the resume path refuses them
 * explicitly rather than fabricating a result.
 *
 * v72/v73 discipline is reused: ALTER ADD COLUMN is not idempotent, so the
 * column is probed first, and the schema_version bump hard-gates on this
 * returning SUCCESS.
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
static bool v74_column_exists(sqlite3 *db, const char *table, const char *col) {
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

static int v74_exec_or_fail(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v74 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v74(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   if (v74_column_exists(db, "conversations", "job_goal")) {
      return AUTH_DB_SUCCESS; /* already migrated — backfill ran with the ALTER */
   }

   if (v74_exec_or_fail(db, "ALTER TABLE conversations ADD COLUMN job_goal TEXT DEFAULT NULL",
                        "job_goal column") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   /* Backfill from each job's own first user message — for a job that ran, that
    * row IS the goal (core_text_input_dispatch persists the dispatched text).
    *
    * The LIKE exclusion is not hypothetical: a job resumed BEFORE this migration
    * had no goal to send, so it was dispatched the continuation directive, and
    * that directive became its first (and only) user message.  Taking it as the
    * goal would store a job whose recorded instruction is "continue your
    * previous attempt" — self-referential nonsense that a panel would display
    * and a future resume would re-send.  Verified against the live DB: exactly
    * one row (a job resumed while this bug was live) matches.  Leaving those
    * NULL is correct — they genuinely have no recoverable goal, and the resume
    * path refuses them explicitly. */
   if (v74_exec_or_fail(db,
                        "UPDATE conversations SET job_goal = ("
                        "  SELECT m.content FROM messages m"
                        "  WHERE m.conversation_id = conversations.id AND m.role = 'user'"
                        "    AND m.content NOT LIKE 'Your previous attempt at this task did not%'"
                        "  ORDER BY m.id ASC LIMIT 1"
                        ") WHERE job_status IS NOT NULL AND job_goal IS NULL",
                        "job_goal backfill") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
