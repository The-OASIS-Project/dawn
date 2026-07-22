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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for the v72 background-jobs schema migration (conversations job
 * columns, conversation_events table, indexes, parent_id ON DELETE SET NULL,
 * and idempotency of a re-run).
 */

#include <sqlite3.h>
#include <stddef.h>
#include <unistd.h>

#include "unity.h"

/* Public return codes (auth/auth_db.h): SUCCESS = 0, FAILURE = 1. Declared
 * locally so the test links only the migration TU + sqlite3 + logging. */
#define MIG_SUCCESS 0
int auth_db_migrations_v72(sqlite3 *db);

static const char *TEST_DB = "/tmp/dawn_test_mig_v72.db";
static sqlite3 *db = NULL;

void setUp(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
}

void tearDown(void) {
   if (db != NULL) {
      sqlite3_close(db);
      db = NULL;
   }
   unlink(TEST_DB);
}

/* Minimal pre-v72 conversations table (with the continued_from self-FK the real
 * one carries) + one root row. */
static void seed_pre_v72(void) {
   const char *sql =
       "CREATE TABLE users(id INTEGER PRIMARY KEY);"
       "CREATE TABLE conversations("
       "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "  user_id INTEGER NOT NULL,"
       "  title TEXT NOT NULL DEFAULT 'x',"
       "  created_at INTEGER NOT NULL,"
       "  updated_at INTEGER NOT NULL,"
       "  continued_from INTEGER DEFAULT NULL,"
       "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
       "  FOREIGN KEY(continued_from) REFERENCES conversations(id) ON DELETE SET NULL);"
       "INSERT INTO users(id) VALUES(1);"
       "INSERT INTO conversations(user_id,title,created_at,updated_at) VALUES(1,'root',100,100);";
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
}

static int col_exists(const char *col) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info('conversations') WHERE name=?", -1, &st,
                      NULL);
   sqlite3_bind_text(st, 1, col, -1, SQLITE_STATIC);
   int found = (sqlite3_step(st) == SQLITE_ROW);
   sqlite3_finalize(st);
   return found;
}

static int obj_exists(const char *name) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name=?", -1, &st, NULL);
   sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
   int found = (sqlite3_step(st) == SQLITE_ROW);
   sqlite3_finalize(st);
   return found;
}

/* ── Migrating an existing (pre-v72) DB adds every column + object ──────────── */

static void test_migrate_from_pre_v72(void) {
   seed_pre_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));

   static const char *cols[] = { "parent_id",  "spawn_mode",  "on_complete",  "on_complete_fired",
                                 "job_status", "job_error",   "spawn_depth",  "reinvoke_count",
                                 "started_at", "finished_at", "workspace_ref" };
   for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
      TEST_ASSERT_TRUE_MESSAGE(col_exists(cols[i]), cols[i]);
   }
   TEST_ASSERT_TRUE(obj_exists("conversation_events"));
   TEST_ASSERT_TRUE(obj_exists("idx_conv_events"));
   TEST_ASSERT_TRUE(obj_exists("idx_conversations_parent"));
}

/* ── Re-running is a success no-op (column probe + IF NOT EXISTS) ───────────── */

static void test_idempotent_rerun(void) {
   seed_pre_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));
   TEST_ASSERT_TRUE(col_exists("job_status"));
}

/* ── parent_id carries ON DELETE SET NULL ──────────────────────────────────── */

static void test_parent_fk_set_null(void) {
   seed_pre_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));

   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "INSERT INTO conversations"
                                      "(user_id,title,created_at,updated_at,parent_id,job_status)"
                                      " VALUES(1,'child',101,101,1,'running')",
                                      NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, "DELETE FROM conversations WHERE id=1", NULL,
                                                 NULL, NULL));

   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT parent_id FROM conversations WHERE title='child'", -1, &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(st, 0)); /* parent_id was SET NULL */
   sqlite3_finalize(st);
}

/* ── Fresh-DB path: base schema already added the columns → migration no-ops ── */

static void test_migrate_when_columns_present(void) {
   seed_pre_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db)); /* adds them */
   /* All columns/objects now present; a second call must still succeed. */
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_migrate_from_pre_v72);
   RUN_TEST(test_idempotent_rerun);
   RUN_TEST(test_parent_fk_set_null);
   RUN_TEST(test_migrate_when_columns_present);
   return UNITY_END();
}
