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
 * auth.db's storage (real auth_db): checkpoints on the storage thread, the WAL
 * truncated after a burst, incremental auto-vacuum and its draining.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "auth/auth_db_storage.h"
#include "test_tmp.h"
#include "unity.h"

static char TEST_DB[TEST_TMP_PATH_MAX];
static char TEST_DB_WAL[TEST_TMP_PATH_MAX + 4];
static char TEST_DB_SHM[TEST_TMP_PATH_MAX + 4];

static void remove_db(void) {
   unlink(TEST_DB);
   unlink(TEST_DB_WAL);
   unlink(TEST_DB_SHM);
}

void setUp(void) {
   remove_db();
}

void tearDown(void) {
   auth_db_shutdown();
   remove_db();
}

static int64_t main_pragma(const char *sql) {
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   int64_t v = -1;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) == SQLITE_OK &&
       sqlite3_step(st) == SQLITE_ROW) {
      v = sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return v;
}

static void main_exec(const char *sql) {
   pthread_mutex_lock(&s_db.mutex);
   TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL), sql);
   pthread_mutex_unlock(&s_db.mutex);
}

static int64_t file_size(const char *path) {
   struct stat sb;
   return stat(path, &sb) == 0 ? (int64_t)sb.st_size : -1;
}

/* A new database starts in incremental auto-vacuum, with the connection's
 * settings. */
static void test_a_new_database(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   TEST_ASSERT_EQUAL_INT64(2, main_pragma("PRAGMA auto_vacuum"));
   TEST_ASSERT_EQUAL_INT64(2, main_pragma("PRAGMA secure_delete")); /* FAST */
   TEST_ASSERT_EQUAL_INT64(AUTH_DB_WAL_SIZE_LIMIT, main_pragma("PRAGMA journal_size_limit"));
   TEST_ASSERT_EQUAL_INT64(2, main_pragma("PRAGMA synchronous")); /* FULL, unchanged */
}

/* An existing file without auto-vacuum is converted once at startup. */
static void test_an_existing_file_is_converted(void) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db,
                                                 "CREATE TABLE old(x); INSERT INTO old "
                                                 "VALUES (zeroblob(100000));",
                                                 NULL, NULL, NULL));
   sqlite3_close(db);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   TEST_ASSERT_EQUAL_INT64(2, main_pragma("PRAGMA auto_vacuum"));
   TEST_ASSERT_EQUAL_INT64(1, main_pragma("SELECT COUNT(*) FROM old")); /* kept */
}

/* A burst past the threshold is checkpointed by the storage thread (not in
 * the commit, and not a silent no-op), and the WAL is cut back to its limit
 * at the next write. */
static void test_the_storage_thread_checkpoints(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   const uint64_t before = auth_db_storage_checkpoints();
   main_exec("CREATE TABLE burst(b)");
   main_exec("INSERT INTO burst VALUES (zeroblob(24 * 1024 * 1024))");
   for (int i = 0; i < 100 && auth_db_storage_checkpoints() == before; i++) {
      usleep(50 * 1000);
   }
   TEST_ASSERT_TRUE_MESSAGE(auth_db_storage_checkpoints() > before, "no checkpoint ran");
   /* Everything was written back: a checkpoint from here has nothing left. */
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   sqlite3_exec(db, "SELECT 1 FROM sqlite_master LIMIT 1", NULL, NULL, NULL);
   int log = -1;
   int done = -1;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                                              &log, &done));
   TEST_ASSERT_EQUAL_INT(log, done);
   sqlite3_close(db);
   /* The next write restarts the WAL and cuts it to the limit. */
   main_exec("INSERT INTO burst VALUES (1)");
   TEST_ASSERT_TRUE(file_size(TEST_DB_WAL) <= AUTH_DB_WAL_SIZE_LIMIT);
   /* An admin TRUNCATE checkpoint still works beside the thread. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_checkpoint());
}

/* Freed pages are drained in chunks, and the file shrinks. */
static void test_free_pages_are_drained(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   main_exec("CREATE TABLE big(b)");
   main_exec("INSERT INTO big VALUES (zeroblob(8 * 1024 * 1024))");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_checkpoint());
   const int64_t full = file_size(TEST_DB);
   main_exec("DELETE FROM big");
   TEST_ASSERT_TRUE(main_pragma("PRAGMA freelist_count") > AUTH_DB_VACUUM_FLOOR_PAGES);
   int freed = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_storage_vacuum_pass(10000, &freed));
   TEST_ASSERT_TRUE(freed > 0);
   TEST_ASSERT_TRUE(main_pragma("PRAGMA freelist_count") <= AUTH_DB_VACUUM_FLOOR_PAGES);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_checkpoint());
   TEST_ASSERT_TRUE_MESSAGE(file_size(TEST_DB) < full - 4 * 1024 * 1024, "the file didn't shrink");
}

/* Other callers get the mutex between vacuum chunks (a pass must not lock
 * them out for its whole budget). */
static _Atomic int s_probing;
static _Atomic int64_t s_worst_wait_ms;

static int64_t mono_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *prober(void *arg) {
   (void)arg;
   while (atomic_load(&s_probing)) {
      const int64_t t0 = mono_ms();
      auth_user_t u;
      (void)auth_db_get_user("nobody", &u);
      const int64_t waited = mono_ms() - t0;
      if (waited > atomic_load(&s_worst_wait_ms)) {
         atomic_store(&s_worst_wait_ms, waited);
      }
      usleep(2000);
   }
   return NULL;
}

static void test_a_vacuum_pass_shares_the_mutex(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   main_exec("CREATE TABLE big(b)");
   main_exec("INSERT INTO big VALUES (zeroblob(64 * 1024 * 1024))");
   main_exec("DELETE FROM big");
   atomic_store(&s_probing, 1);
   atomic_store(&s_worst_wait_ms, 0);
   pthread_t t;
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, prober, NULL));
   int freed = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_storage_vacuum_pass(1500, &freed));
   atomic_store(&s_probing, 0);
   pthread_join(t, NULL);
   TEST_ASSERT_TRUE(freed > 1000);
   char msg[64];
   snprintf(msg, sizeof(msg), "a caller waited %lld ms", (long long)atomic_load(&s_worst_wait_ms));
   TEST_ASSERT_TRUE_MESSAGE(atomic_load(&s_worst_wait_ms) < 150, msg);
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_auth_db_storage.db");
   snprintf(TEST_DB_WAL, sizeof(TEST_DB_WAL), "%s-wal", TEST_DB);
   snprintf(TEST_DB_SHM, sizeof(TEST_DB_SHM), "%s-shm", TEST_DB);
   UNITY_BEGIN();
   RUN_TEST(test_a_new_database);
   RUN_TEST(test_an_existing_file_is_converted);
   RUN_TEST(test_the_storage_thread_checkpoints);
   RUN_TEST(test_free_pages_are_drained);
   RUN_TEST(test_a_vacuum_pass_shares_the_mutex);
   return UNITY_END();
}
