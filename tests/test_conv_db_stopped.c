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
 * Unit tests for message_stopped (v117): a reply the user stopped is marked on
 * its row and read back by every display read and the replay read, only an
 * ordinary assistant reply can be marked, the mark goes with its message, and
 * the migration is idempotent.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "auth/auth_db_messages.h"
#include "test_tmp.h"
#include "unity.h"

static char TEST_DB[TEST_TMP_PATH_MAX];
static int s_user = 0;

void setUp(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   s_user = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(TEST_DB);
}

/* ---- helpers ---- */

typedef struct {
   int count;
   int64_t ids[8];
   bool stopped[8];
} flags_t;

static int collect_display(const conversation_message_t *msg, void *ctx) {
   flags_t *out = ctx;
   if (out->count < 8) {
      out->ids[out->count] = msg->id;
      out->stopped[out->count++] = msg->stopped;
   }
   return 0;
}

static int collect_llm(const conversation_llm_row_t *row, void *ctx) {
   flags_t *out = ctx;
   if (out->count < 8) {
      out->ids[out->count] = row->id;
      out->stopped[out->count++] = row->stopped;
   }
   return 0;
}

static int64_t add(int64_t conv, const conv_message_row_t *row) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, row, &id));
   TEST_ASSERT_TRUE(id > 0);
   return id;
}

static int64_t count_marks(void) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM message_stopped", -1,
                                            &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   const int64_t n = sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   return n;
}

/* A conversation: a question, a stopped reply, a question, a finished reply. */
static int64_t seed(int64_t *stopped_id) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "c", &conv));
   add(conv, &(conv_message_row_t){ .role = "user", .content = "Q1" });
   *stopped_id = add(conv, &(conv_message_row_t){ .role = "assistant",
                                                  .content = "Half an ans",
                                                  .stopped = true });
   add(conv, &(conv_message_row_t){ .role = "user", .content = "Q2" });
   add(conv, &(conv_message_row_t){ .role = "assistant", .content = "A whole answer." });
   return conv;
}

static void assert_only_second_stopped(const flags_t *f) {
   TEST_ASSERT_EQUAL_INT(4, f->count);
   TEST_ASSERT_FALSE(f->stopped[0]);
   TEST_ASSERT_TRUE(f->stopped[1]);
   TEST_ASSERT_FALSE(f->stopped[2]);
   TEST_ASSERT_FALSE(f->stopped[3]);
}

/* ---- tests ---- */

static void test_the_mark_reaches_every_read(void) {
   int64_t stopped = 0;
   const int64_t conv = seed(&stopped);

   flags_t f = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_messages(conv, s_user, collect_display, &f));
   assert_only_second_stopped(&f);

   memset(&f, 0, sizeof(f));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_after(conv, s_user, 0, collect_display, &f));
   assert_only_second_stopped(&f);

   memset(&f, 0, sizeof(f));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_messages_admin(conv, collect_display, &f));
   assert_only_second_stopped(&f);

   memset(&f, 0, sizeof(f));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, s_user, 0, collect_llm, &f));
   assert_only_second_stopped(&f);

   memset(&f, 0, sizeof(f));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_by_range(conv, s_user, 1, INT64_MAX, 0, true,
                                                       collect_display, &f));
   assert_only_second_stopped(&f);
   TEST_ASSERT_EQUAL_INT64(stopped, f.ids[1]);
}

/* Only an ordinary assistant reply with text and no tool calls is a stopped
 * reply; anything else saves unmarked. */
static void test_only_an_assistant_reply_is_marked(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "c", &conv));
   add(conv, &(conv_message_row_t){ .role = "user", .content = "Q", .stopped = true });
   add(conv, &(conv_message_row_t){ .role = "assistant",
                                    .content = "Checking.",
                                    .tool_calls = "[{\"id\":\"t1\",\"type\":\"function\","
                                                  "\"function\":{\"name\":\"x\","
                                                  "\"arguments\":\"{}\"}}]",
                                    .stopped = true });
   add(conv, &(conv_message_row_t){ .role = "tool",
                                    .content = "r",
                                    .tool_call_id = "t1",
                                    .stopped = true });
   add(conv, &(conv_message_row_t){ .role = "assistant", .content = "", .stopped = true });
   TEST_ASSERT_EQUAL_INT64(0, count_marks());
}

static void test_the_mark_goes_with_its_conversation(void) {
   int64_t stopped = 0;
   const int64_t conv = seed(&stopped);
   TEST_ASSERT_EQUAL_INT64(1, count_marks());
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(conv, s_user));
   TEST_ASSERT_EQUAL_INT64(0, count_marks());
}

static void test_a_batch_marks_its_stopped_reply(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "c", &conv));
   const conv_message_row_t rows[] = {
      { .role = "user", .content = "Q" },
      { .role = "assistant", .content = "Cut", .stopped = true },
   };
   int64_t ids[2] = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_rows(conv, s_user, rows, 2, ids));
   flags_t f = { 0 };
   conv_db_get_messages(conv, s_user, collect_display, &f);
   TEST_ASSERT_EQUAL_INT(2, f.count);
   TEST_ASSERT_FALSE(f.stopped[0]);
   TEST_ASSERT_TRUE(f.stopped[1]);
}

/* The side table exists on a new database, the step can run again, and the
 * schema is stamped at the current version. */
static void test_v117_creates_the_table_once(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v117(s_db.db));
   TEST_ASSERT_EQUAL_INT64(0, count_marks());
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK, sqlite3_prepare_v2(s_db.db, "SELECT version FROM schema_version", -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SCHEMA_VERSION, sqlite3_column_int(st, 0));
   sqlite3_finalize(st);
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_conv_stopped.db");
   UNITY_BEGIN();
   RUN_TEST(test_the_mark_reaches_every_read);
   RUN_TEST(test_only_an_assistant_reply_is_marked);
   RUN_TEST(test_the_mark_goes_with_its_conversation);
   RUN_TEST(test_a_batch_marks_its_stopped_reply);
   RUN_TEST(test_v117_creates_the_table_once);
   return UNITY_END();
}
