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
 * Unit tests for memory_summarize_missing_count.  The worker thread itself
 * makes real LLM calls and is exercised in the live integration test
 * (`dawn-admin memory summarize-missing --user <u>`); these tests cover the
 * eligibility query that drives the dry-run preview and the worker loop's
 * pagination input — exactly the surface a regression in the SQL would
 * silently mis-handle (e.g. skipping the message_count gate, leaking
 * private conversations into the backlog).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "memory/memory_summarize_missing.h"
#include "unity.h"

/* DDL: minimal schema mirroring production for the columns the query uses. */
/* clang-format off */
static const char *DDL =
   "CREATE TABLE IF NOT EXISTS users ("
   "  id INTEGER PRIMARY KEY,"
   "  username TEXT UNIQUE NOT NULL"
   ");"
   "INSERT INTO users (id, username) VALUES (1, 'alice'), (2, 'bob');"

   "CREATE TABLE IF NOT EXISTS conversations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  message_count INTEGER NOT NULL DEFAULT 0,"
   "  is_private INTEGER NOT NULL DEFAULT 0,"
   "  job_status TEXT DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"

   "CREATE TABLE IF NOT EXISTS memory_summaries ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");";
/* clang-format on */

static void seed_conv(int user_id, int message_count, bool is_private) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(
       s_db.db, "INSERT INTO conversations (user_id, message_count, is_private) VALUES (?, ?, ?)",
       -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, message_count);
   sqlite3_bind_int(stmt, 3, is_private ? 1 : 0);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static int64_t last_conv_id(void) {
   return sqlite3_last_insert_rowid(s_db.db);
}

static void seed_summary_for(int user_id, int64_t conv_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(
       s_db.db, "INSERT INTO memory_summaries (user_id, source_conversation_id) VALUES (?, ?)", -1,
       &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, conv_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static void open_db(void) {
   if (sqlite3_open(":memory:", &s_db.db) != SQLITE_OK) {
      fprintf(stderr, "open failed\n");
      exit(1);
   }
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   char *err = NULL;
   if (sqlite3_exec(s_db.db, DDL, NULL, NULL, &err) != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", err ? err : "?");
      sqlite3_free(err);
      exit(1);
   }
   s_db.initialized = true;
}

static void close_db(void) {
   s_db.initialized = false;
   sqlite3_close(s_db.db);
   s_db.db = NULL;
}

void setUp(void) {
   open_db();
}
void tearDown(void) {
   close_db();
}

/* ============================================================================
 * Tests
 * ============================================================================ */

void test_count_zero_when_no_conversations(void) {
   int count = -1;
   int rc = memory_summarize_missing_count(1, &count);
   TEST_ASSERT_EQUAL(SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, count);
}

void test_count_returns_unsummarized_eligible_only(void) {
   /* conv A: 5 msgs, public, no summary    -> eligible */
   seed_conv(1, 5, false);
   /* conv B: 3 msgs, public, has summary  -> not eligible (summarized) */
   seed_conv(1, 3, false);
   int64_t conv_b = last_conv_id();
   seed_summary_for(1, conv_b);
   /* conv C: 4 msgs, private              -> not eligible (private) */
   seed_conv(1, 4, true);
   /* conv D: 1 msg, public                -> not eligible (too short) */
   seed_conv(1, 1, false);
   /* conv E: 6 msgs, public, no summary    -> eligible */
   seed_conv(1, 6, false);

   int count = -1;
   int rc = memory_summarize_missing_count(1, &count);
   TEST_ASSERT_EQUAL(SUCCESS, rc);
   TEST_ASSERT_EQUAL(2, count);
}

void test_count_is_per_user_scoped(void) {
   /* alice: 2 eligible, bob: 1 eligible */
   seed_conv(1, 3, false);
   seed_conv(1, 3, false);
   seed_conv(2, 4, false);

   int a_count = 0, b_count = 0;
   TEST_ASSERT_EQUAL(SUCCESS, memory_summarize_missing_count(1, &a_count));
   TEST_ASSERT_EQUAL(SUCCESS, memory_summarize_missing_count(2, &b_count));
   TEST_ASSERT_EQUAL(2, a_count);
   TEST_ASSERT_EQUAL(1, b_count);
}

/* A background job's transcript is the model's own work, never summarized. */
void test_count_excludes_job_conversations(void) {
   seed_conv(1, 6, false);
   seed_conv(1, 6, false);
   sqlite3_exec(s_db.db, "UPDATE conversations SET job_status = 'done' WHERE id = 1", NULL, NULL,
                NULL);

   int count = 0;
   TEST_ASSERT_EQUAL(SUCCESS, memory_summarize_missing_count(1, &count));
   TEST_ASSERT_EQUAL(1, count);
}

void test_count_excludes_2plus_msg_with_summary_kept(void) {
   /* Two convs both with >= 2 msgs, only one summarized. */
   seed_conv(1, 2, false); /* conv_a */
   int64_t conv_a = last_conv_id();
   seed_conv(1, 2, false); /* conv_b */
   seed_summary_for(1, conv_a);

   int count = 0;
   TEST_ASSERT_EQUAL(SUCCESS, memory_summarize_missing_count(1, &count));
   TEST_ASSERT_EQUAL(1, count);
}

void test_count_rejects_invalid_inputs(void) {
   int count = 99;
   TEST_ASSERT_EQUAL(FAILURE, memory_summarize_missing_count(0, &count));
   TEST_ASSERT_EQUAL(FAILURE, memory_summarize_missing_count(-1, &count));
   TEST_ASSERT_EQUAL(FAILURE, memory_summarize_missing_count(1, NULL));
}

void test_start_rejects_invalid_user_id(void) {
   /* Worker must refuse user_id <= 0 without flipping the running flag. */
   TEST_ASSERT_FALSE(memory_summarize_missing_is_running());
   TEST_ASSERT_EQUAL(FAILURE, memory_summarize_missing_start(0, 0));
   TEST_ASSERT_FALSE(memory_summarize_missing_is_running());
   TEST_ASSERT_EQUAL(FAILURE, memory_summarize_missing_start(-5, 10));
   TEST_ASSERT_FALSE(memory_summarize_missing_is_running());
}

void test_stop_is_safe_when_not_running(void) {
   /* Idempotent no-op when no worker active. */
   TEST_ASSERT_FALSE(memory_summarize_missing_is_running());
   memory_summarize_missing_stop();
   memory_summarize_missing_stop();
   TEST_ASSERT_FALSE(memory_summarize_missing_is_running());
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_count_zero_when_no_conversations);
   RUN_TEST(test_count_returns_unsummarized_eligible_only);
   RUN_TEST(test_count_is_per_user_scoped);
   RUN_TEST(test_count_excludes_job_conversations);
   RUN_TEST(test_count_excludes_2plus_msg_with_summary_kept);
   RUN_TEST(test_count_rejects_invalid_inputs);
   RUN_TEST(test_start_rejects_invalid_user_id);
   RUN_TEST(test_stop_is_safe_when_not_running);
   return UNITY_END();
}
