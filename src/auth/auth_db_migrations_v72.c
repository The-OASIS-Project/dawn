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
 * Schema migration v72: background-jobs foundation.
 *
 * A background job IS a parented conversation (no separate job subsystem). This
 * migration adds the job lifecycle columns to `conversations` and creates the
 * durable step-log table `conversation_events` used by the observe/replay
 * contract.  See docs/BACKGROUND_JOBS_DESIGN.md.
 *
 * ALTER TABLE ADD COLUMN is NOT idempotent, and the migration ladder hard-gates
 * the schema_version bump on this returning AUTH_DB_SUCCESS, so each column is
 * probed via PRAGMA table_info first and skipped when already present.  Every
 * DEFAULT is a literal constant to keep SQLite on the fast ALTER path (no table
 * rewrite).  The parent_id index is created HERE (not in the base SCHEMA_SQL),
 * because the base schema runs before migrations on every boot — indexing a
 * migration-added column there would fail "no such column" on an existing DB
 * (same rule as idx_conversations_pinned in v69).
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

/* True if @col exists on @table. @table is a fixed literal (not user input);
 * PRAGMA cannot be parameterized, so it is interpolated directly. */
static bool conv_column_exists(sqlite3 *db, const char *table, const char *col) {
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

/* ALTER TABLE conversations ADD COLUMN <ddl>, skipping if @col already exists.
 * @ddl is the full column definition (e.g. "job_status TEXT DEFAULT NULL"). */
static int add_conversations_column(sqlite3 *db, const char *col, const char *ddl) {
   if (conv_column_exists(db, "conversations", col)) {
      return AUTH_DB_SUCCESS;
   }
   char sql[256];
   snprintf(sql, sizeof(sql), "ALTER TABLE conversations ADD COLUMN %s", ddl);
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v72 ALTER (%s) failed: %s", col, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

static int exec_or_fail(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v72 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v72(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   /* Job lifecycle columns on `conversations`.  parent_id NULL = root/user-
    * initiated; job_status NULL = not a job.  parent_id carries the self-FK with
    * ON DELETE SET NULL (a job tree is single-user; running children are reaped
    * before a parent delete — see the design doc — so SET NULL only affects
    * already-finished children). */
   static const struct {
      const char *col;
      const char *ddl;
   } columns[] = {
      { "parent_id",
        "parent_id INTEGER DEFAULT NULL REFERENCES conversations(id) ON DELETE SET NULL" },
      { "spawn_mode", "spawn_mode TEXT DEFAULT NULL" },
      { "on_complete", "on_complete TEXT DEFAULT NULL" },
      { "on_complete_fired", "on_complete_fired INTEGER NOT NULL DEFAULT 0" },
      { "job_status", "job_status TEXT DEFAULT NULL" },
      { "job_error", "job_error TEXT DEFAULT NULL" },
      { "spawn_depth", "spawn_depth INTEGER NOT NULL DEFAULT 0" },
      { "reinvoke_count", "reinvoke_count INTEGER NOT NULL DEFAULT 0" },
      { "started_at", "started_at INTEGER NOT NULL DEFAULT 0" },
      { "finished_at", "finished_at INTEGER NOT NULL DEFAULT 0" },
      { "workspace_ref", "workspace_ref TEXT DEFAULT NULL" },
   };
   for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
      if (add_conversations_column(db, columns[i].col, columns[i].ddl) != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
   }

   /* Resumability / cascade lookup: parent's children by job_status. */
   if (exec_or_fail(db,
                    "CREATE INDEX IF NOT EXISTS idx_conversations_parent "
                    "ON conversations(parent_id, job_status)",
                    "idx_conversations_parent") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   /* Durable step-granular event log for the observe/replay contract.  Live
    * tokens are NOT persisted here (in-memory replay ring only); final assistant
    * text stays in `messages`.  New table -> IF NOT EXISTS is idempotent whether
    * or not the base SCHEMA_SQL already created it this boot. */
   if (exec_or_fail(
           db,
           "CREATE TABLE IF NOT EXISTS conversation_events ("
           "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
           "   conversation_id INTEGER NOT NULL,"
           "   seq INTEGER NOT NULL,"
           "   kind TEXT NOT NULL,"
           "   payload TEXT,"
           "   created_at INTEGER NOT NULL,"
           "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
           ")",
           "conversation_events create") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   /* UNIQUE enforces the per-conversation monotonic seq invariant at the DB
    * layer (seq is assigned MAX(seq)+1 under the auth_db mutex). */
   if (exec_or_fail(db,
                    "CREATE UNIQUE INDEX IF NOT EXISTS idx_conv_events "
                    "ON conversation_events(conversation_id, seq ASC)",
                    "idx_conv_events") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
