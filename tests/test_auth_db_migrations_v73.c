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
 * Unit tests for the v73 background-jobs Phase-1 migration: the `deliver_to`
 * column + the completion-monitor partial index, applied on top of v72, plus
 * idempotency of a re-run.
 */

#include <sqlite3.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#define MIG_SUCCESS 0
int auth_db_migrations_v72(sqlite3 *db);
int auth_db_migrations_v73(sqlite3 *db);

static const char *TEST_DB = "/tmp/dawn_test_mig_v73.db";
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

/* Minimal pre-v72 conversations table + one root row; v72 then adds the job
 * columns, so v73 runs on a realistic post-v72 shape. */
static void seed_and_v72(void) {
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
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v72(db));
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

/* ── v73 adds deliver_to + the partial follow-up index ─────────────────────── */

static void test_v73_adds_column_and_index(void) {
   seed_and_v72();
   TEST_ASSERT_FALSE(col_exists("deliver_to"));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v73(db));
   TEST_ASSERT_TRUE(col_exists("deliver_to"));
   TEST_ASSERT_TRUE(obj_exists("idx_conv_job_followup"));
}

/* ── Re-running v73 is a success no-op ─────────────────────────────────────── */

static void test_v73_idempotent(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v73(db));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v73(db));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v73(db));
   TEST_ASSERT_TRUE(col_exists("deliver_to"));
}

/* ── deliver_to is nullable / defaults to NULL on existing rows ────────────── */

static void test_v73_deliver_to_default_null(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v73(db));
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT deliver_to FROM conversations WHERE id=1", -1, &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(st, 0));
   sqlite3_finalize(st);
}

/* ── The partial index is usable for the monitor's follow-up scan ──────────── */

static void test_v73_index_covers_followup_scan(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v73(db));
   /* EXPLAIN QUERY PLAN should mention the partial index for the drain query. */
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(db,
                               "EXPLAIN QUERY PLAN SELECT id FROM conversations "
                               "WHERE job_status IN ('done','failed','interrupted') "
                               "AND on_complete_fired=0",
                               -1, &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
   int used_index = 0;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const char *detail = (const char *)sqlite3_column_text(st, 3);
      if (detail && strstr(detail, "idx_conv_job_followup")) {
         used_index = 1;
      }
   }
   sqlite3_finalize(st);
   TEST_ASSERT_TRUE_MESSAGE(used_index, "monitor scan should use idx_conv_job_followup");
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_v73_adds_column_and_index);
   RUN_TEST(test_v73_idempotent);
   RUN_TEST(test_v73_deliver_to_default_null);
   RUN_TEST(test_v73_index_covers_followup_scan);
   return UNITY_END();
}
