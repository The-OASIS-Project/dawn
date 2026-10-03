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
 * Schema v101 on a table shaped like before it: every link counts as verified
 * (an SMS link already unlinked doesn't, and a rerun proves nothing new),
 * a Telegram private chat is owned by its id, a group stays unowned, two
 * users on one group keep only the most recently used row enabled, and the
 * owner index then allows one enabled, verified row per (chat, owner).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "unity.h"

static int s_user_a;
static int s_user_b;

static int make_user(const char *name) {
   auth_db_create_user(name, "$argon2id$v=19$m=65536,t=3,p=4$c2FsdA$aGFzaA", false);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user(name, &u));
   return u.id;
}

static void exec_ok(const char *sql) {
   char *err = NULL;
   int rc = sqlite3_exec(s_db.db, sql, NULL, NULL, &err);
   TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, rc, err ? err : sql);
   sqlite3_free(err);
}

/* A row as written before v101: no owner, no verification state. */
static void add_old_row(int user, const char *provider, const char *address, long last_used) {
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO messaging_channels (user_id, provider, provider_address, address_json, "
            "display_name, created_at, last_used_at) VALUES (%d, '%s', '%s', '{}', '%s_%s', 1000, "
            "%ld)",
            user, provider, address, provider, address, last_used);
   exec_ok(sql);
}

static long long query_int(const char *sql) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   long long v = -1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      v = sqlite3_column_type(st, 0) == SQLITE_NULL ? -2 : sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   return v;
}

static void query_text(const char *sql, char *out, size_t n) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   out[0] = '\0';
   if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
      snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
   } else {
      snprintf(out, n, "(null)");
   }
   sqlite3_finalize(st);
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   s_user_a = make_user("user_a");
   s_user_b = make_user("user_b");
   /* Back to the pre-v101 shape: no owner index, none of its columns. */
   exec_ok("DROP INDEX IF EXISTS idx_messaging_channels_owner");
   static const char *const cols[] = { "owner_sender",       "verified_at",     "verify_code_hash",
                                       "verify_expires_at",  "verify_attempts", "verify_sends",
                                       "verify_window_start" };
   for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
      char sql[96];
      snprintf(sql, sizeof(sql), "ALTER TABLE messaging_channels DROP COLUMN %s", cols[i]);
      exec_ok(sql);
   }
   add_old_row(s_user_a, "telegram", "42", 5);
   add_old_row(s_user_a, "telegram", "-55", 10);
   add_old_row(s_user_b, "telegram", "-55", 20);
   add_old_row(s_user_a, "sms", "+15551230000", 0);
   add_old_row(s_user_a, "discord", "999", 0);
   add_old_row(s_user_b, "sms", "+15559990000", 0);
   exec_ok("UPDATE messaging_channels SET is_enabled = 0 WHERE provider_address = '+15559990000'");
}

void tearDown(void) {
   auth_db_shutdown();
}

static void test_backfill(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v101(s_db.db));

   /* Every row is proven except the SMS link already unlinked. */
   TEST_ASSERT_EQUAL_INT64(1, query_int("SELECT COUNT(*) FROM messaging_channels "
                                        "WHERE verified_at IS NULL"));
   TEST_ASSERT_EQUAL_INT64(-2, query_int("SELECT verified_at FROM messaging_channels "
                                         "WHERE provider_address='+15559990000'"));
   TEST_ASSERT_EQUAL_INT64(1000, query_int("SELECT verified_at FROM messaging_channels "
                                           "WHERE provider_address='+15551230000'"));

   char owner[64];
   query_text("SELECT owner_sender FROM messaging_channels WHERE provider_address='42'", owner,
              sizeof(owner));
   TEST_ASSERT_EQUAL_STRING("42", owner);
   TEST_ASSERT_EQUAL_INT64(0, query_int("SELECT COUNT(*) FROM messaging_channels WHERE "
                                        "provider_address IN ('-55','999','+15551230000') AND "
                                        "owner_sender IS NOT NULL"));
}

/* Two users on one group: the more recently used row stays enabled. */
static void test_duplicates_disabled(void) {
   char sql[160];
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v101(s_db.db));
   snprintf(sql, sizeof(sql),
            "SELECT is_enabled FROM messaging_channels WHERE provider_address='-55' AND "
            "user_id=%d",
            s_user_a);
   TEST_ASSERT_EQUAL_INT64(0, query_int(sql));
   snprintf(sql, sizeof(sql),
            "SELECT is_enabled FROM messaging_channels WHERE provider_address='-55' AND "
            "user_id=%d",
            s_user_b);
   TEST_ASSERT_EQUAL_INT64(1, query_int(sql));
}

/* One enabled, verified row per (chat, owner); a pending SMS row doesn't
 * count, and re-running the migration changes nothing. */
static void test_owner_index_and_rerun(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v101(s_db.db));

   char sql[512];
   snprintf(
       sql, sizeof(sql),
       "INSERT INTO messaging_channels (user_id, provider, provider_address, address_json, "
       "created_at, owner_sender, verified_at) VALUES (%d, 'telegram', '42', '{}', 1, '42', 1)",
       s_user_b);
   TEST_ASSERT_NOT_EQUAL(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));

   snprintf(sql, sizeof(sql),
            "INSERT INTO messaging_channels (user_id, provider, provider_address, address_json, "
            "created_at, verify_code_hash, verify_expires_at) VALUES (%d, 'sms', "
            "'+15551230000', '{}', 1, 'abc', 99)",
            s_user_b);
   exec_ok(sql);

   /* A rerun (as while a later step keeps failing) proves nothing new: the
    * pending SMS row and the unlinked one stay unproven. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v101(s_db.db));
   TEST_ASSERT_EQUAL_INT64(2, query_int("SELECT COUNT(*) FROM messaging_channels "
                                        "WHERE verified_at IS NULL"));
   TEST_ASSERT_EQUAL_INT64(1, query_int("SELECT COUNT(*) FROM messaging_channels "
                                        "WHERE provider_address='-55' AND is_enabled=1"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_backfill);
   RUN_TEST(test_duplicates_disabled);
   RUN_TEST(test_owner_index_and_rerun);
   return UNITY_END();
}
