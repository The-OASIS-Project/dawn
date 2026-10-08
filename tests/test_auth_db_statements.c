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
 * Unit tests for the auth database's cached statements: each statement field
 * is prepared once (none left NULL, none prepared twice and leaked) and
 * finalize leaves nothing behind.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <sqlite3.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "test_tmp.h"
#include "unity.h"

static char TEST_DB[TEST_TMP_PATH_MAX];

static sqlite3_stmt **stmt_field(size_t i) {
   return (sqlite3_stmt **)((char *)&s_db + offsetof(auth_db_state_t, AUTH_DB_STMT_FIRST) +
                            i * sizeof(sqlite3_stmt *));
}

static int live_statements(void) {
   int n = 0;
   for (sqlite3_stmt *st = sqlite3_next_stmt(s_db.db, NULL); st;
        st = sqlite3_next_stmt(s_db.db, st))
      n++;
   return n;
}

void setUp(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(TEST_DB);
}

/* Statements SQLite keeps open itself (FTS5 holds one per table): the
 * connection's count with the cached ones finalized. */
static int sqlite_own_statements(void) {
   auth_db_finalize_statements();
   const int n = live_statements();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_prepare_statements());
   return n;
}

/* Every statement field is prepared, each to its own statement, and those are
 * all the statements open: a field listed twice in a table would leave one
 * prepared statement behind that no field holds. */
static void test_every_field_prepared_once(void) {
   const int own = sqlite_own_statements();
   for (size_t i = 0; i < AUTH_DB_STMT_FIELDS; i++) {
      TEST_ASSERT_NOT_NULL_MESSAGE(*stmt_field(i), "a statement field was left unprepared");
      for (size_t j = 0; j < i; j++)
         TEST_ASSERT_TRUE_MESSAGE(*stmt_field(i) != *stmt_field(j), "two fields share a statement");
   }
   TEST_ASSERT_EQUAL_INT(own + (int)AUTH_DB_STMT_FIELDS, live_statements());
}

/* Finalize closes every cached statement and clears every field, again is a
 * no-op, and preparing again works (init's failure path relies on it). */
static void test_finalize_clears_everything(void) {
   const int own = sqlite_own_statements();
   auth_db_finalize_statements();
   TEST_ASSERT_EQUAL_INT(own, live_statements());
   for (size_t i = 0; i < AUTH_DB_STMT_FIELDS; i++)
      TEST_ASSERT_NULL(*stmt_field(i));
   auth_db_finalize_statements();
   TEST_ASSERT_EQUAL_INT(own, live_statements());
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_prepare_statements());
   TEST_ASSERT_EQUAL_INT(own + (int)AUTH_DB_STMT_FIELDS, live_statements());
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_auth_db_statements.db");
   UNITY_BEGIN();
   RUN_TEST(test_every_field_prepared_once);
   RUN_TEST(test_finalize_clears_everything);
   return UNITY_END();
}
