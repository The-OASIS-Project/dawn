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
 * Unit tests for the v75 deep-research foundation migration: the `job_kind`
 * discriminator column, the four `research_*` tables + indexes, and the
 * `idx_conv_jobs_user` list partial index, applied on top of v72; plus
 * idempotency, the NULL default, FK cascade off the parent conversation, and
 * that the list index is chosen for the job/research list scan.
 */

#include <sqlite3.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#define MIG_SUCCESS 0
int auth_db_migrations_v72(sqlite3 *db);
int auth_db_migrations_v75(sqlite3 *db);

static const char *TEST_DB = "/tmp/dawn_test_mig_v75.db";
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

/* Minimal pre-v72 conversations table + one root row; v72 adds the job columns,
 * so v75 runs on a realistic post-v72 shape (it needs job_status to exist for
 * the partial-index predicate). */
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

/* ── v75 adds job_kind + the four research tables + indexes ─────────────────── */

static void test_v75_adds_column_tables_indexes(void) {
   seed_and_v72();
   TEST_ASSERT_FALSE(col_exists("job_kind"));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   TEST_ASSERT_TRUE(col_exists("job_kind"));
   TEST_ASSERT_TRUE(obj_exists("research_runs"));
   TEST_ASSERT_TRUE(obj_exists("research_questions"));
   TEST_ASSERT_TRUE(obj_exists("research_claims"));
   TEST_ASSERT_TRUE(obj_exists("research_report_revisions"));
   TEST_ASSERT_TRUE(obj_exists("idx_research_runs_user"));
   TEST_ASSERT_TRUE(obj_exists("idx_research_questions_run"));
   TEST_ASSERT_TRUE(obj_exists("idx_research_claims_run"));
   TEST_ASSERT_TRUE(obj_exists("idx_research_revisions_run"));
   TEST_ASSERT_TRUE(obj_exists("idx_conv_jobs_user"));
}

/* ── Re-running v75 is a success no-op ─────────────────────────────────────── */

static void test_v75_idempotent(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   TEST_ASSERT_TRUE(col_exists("job_kind"));
   TEST_ASSERT_TRUE(obj_exists("research_runs"));
}

/* ── job_kind is nullable / defaults to NULL on existing rows ───────────────── */

static void test_v75_job_kind_default_null(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT job_kind FROM conversations WHERE id=1", -1, &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(st, 0));
   sqlite3_finalize(st);
}

/* ── FK cascade: deleting the parent conversation collapses the whole run ───── */

static void test_v75_fk_cascade_collapses_run(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   const char *seed =
       "INSERT INTO research_runs(id,conversation_id,user_id,brief,status,created_at)"
       "  VALUES(1,1,1,'q','planning',100);"
       "INSERT INTO research_questions(id,run_id,question,created_at) VALUES(1,1,'sub',100);"
       "INSERT INTO research_claims(id,run_id,claim,round,created_at) VALUES(1,1,'c',0,100);"
       "INSERT INTO research_report_revisions(id,run_id,round,markdown,created_at)"
       "  VALUES(1,1,0,'md',100);";
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, seed, NULL, NULL, NULL));
   /* Deleting conversation 1 should cascade transitively through runs. */
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, "DELETE FROM conversations WHERE id=1", NULL,
                                                 NULL, NULL));
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db,
                      "SELECT (SELECT COUNT(*) FROM research_runs)"
                      "     + (SELECT COUNT(*) FROM research_questions)"
                      "     + (SELECT COUNT(*) FROM research_claims)"
                      "     + (SELECT COUNT(*) FROM research_report_revisions)",
                      -1, &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(0, sqlite3_column_int(st, 0));
   sqlite3_finalize(st);
}

/* ── research_runs.conversation_id is UNIQUE (a run is 1:1 with its conv) ────── */

static void test_v75_run_conversation_unique(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   const char *first =
       "INSERT INTO research_runs(id,conversation_id,user_id,brief,status,created_at)"
       "  VALUES(1,1,1,'q','planning',100)";
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, first, NULL, NULL, NULL));
   /* A second run pointing at the same conversation must be rejected. */
   const char *dup = "INSERT INTO research_runs(id,conversation_id,user_id,brief,status,created_at)"
                     "  VALUES(2,1,1,'q2','planning',101)";
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, sqlite3_exec(db, dup, NULL, NULL, NULL));
}

/* ── The list partial index is chosen for the job/research list scan ────────── */

static void test_v75_index_covers_list_scan(void) {
   seed_and_v72();
   TEST_ASSERT_EQUAL_INT(MIG_SUCCESS, auth_db_migrations_v75(db));
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(db,
                               "EXPLAIN QUERY PLAN SELECT id FROM conversations "
                               "WHERE user_id=1 AND job_status IS NOT NULL "
                               "ORDER BY created_at DESC, id DESC",
                               -1, &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
   int used_index = 0;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const char *detail = (const char *)sqlite3_column_text(st, 3);
      if (detail && strstr(detail, "idx_conv_jobs_user")) {
         used_index = 1;
      }
   }
   sqlite3_finalize(st);
   TEST_ASSERT_TRUE_MESSAGE(used_index, "list scan should use idx_conv_jobs_user");
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_v75_adds_column_tables_indexes);
   RUN_TEST(test_v75_idempotent);
   RUN_TEST(test_v75_job_kind_default_null);
   RUN_TEST(test_v75_fk_cascade_collapses_run);
   RUN_TEST(test_v75_run_conversation_unique);
   RUN_TEST(test_v75_index_covers_list_scan);
   return UNITY_END();
}
