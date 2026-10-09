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
 * Unit tests for src/tools/email_db.c — multi-account IMAP/SMTP storage CRUD.
 *
 * Tests use raw bytes in encrypted_password (skipping crypto_store init);
 * the encrypt/decrypt wrappers themselves are thin shims over crypto_store
 * which is covered by test_crypto_store.c.
 */

#include <string.h>

#include "auth/auth_db.h"
#include "tools/email_db.h"
#include "unity.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static email_account_t make_account(int user_id, const char *name, const char *username) {
   email_account_t a;
   memset(&a, 0, sizeof(a));
   a.user_id = user_id;
   strncpy(a.name, name, sizeof(a.name) - 1);
   strncpy(a.imap_server, "imap.example.com", sizeof(a.imap_server) - 1);
   a.imap_port = 993;
   a.imap_ssl = true;
   strncpy(a.smtp_server, "smtp.example.com", sizeof(a.smtp_server) - 1);
   a.smtp_port = 465;
   a.smtp_ssl = true;
   strncpy(a.username, username, sizeof(a.username) - 1);
   strncpy(a.display_name, "Test User", sizeof(a.display_name) - 1);
   strncpy(a.auth_type, "app_password", sizeof(a.auth_type) - 1);
   /* Place dummy bytes in encrypted_password (skip crypto_store init) */
   memset(a.encrypted_password, 0xAB, 32);
   a.encrypted_password_len = 32;
   a.enabled = true;
   a.max_recent = 10;
   a.max_body_chars = 4000;
   return a;
}

/* ── setUp / tearDown ────────────────────────────────────────────────────── */

void setUp(void) {
   auth_db_init(":memory:");
   auth_db_create_user("testuser1", "hash1", true);
   auth_db_create_user("testuser2", "hash2", false);
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ── Account Create / Get ────────────────────────────────────────────────── */

static void test_account_create(void) {
   email_account_t a = make_account(1, "Work", "work@example.com");
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_create(&a, &id));
   TEST_ASSERT_TRUE(id > 0);
}

static void test_account_create_null_id_out(void) {
   email_account_t a = make_account(1, "NoIdOut", "noidout@example.com");
   /* id_out NULL is allowed */
   TEST_ASSERT_EQUAL_INT(0, email_db_account_create(&a, NULL));
}

static void test_account_get(void) {
   email_account_t a = make_account(1, "Personal", "me@example.com");
   int64_t id = 0;
   email_db_account_create(&a, &id);

   email_account_t out;
   memset(&out, 0, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, email_db_account_get(id, &out));
   TEST_ASSERT_EQUAL_STRING("Personal", out.name);
   TEST_ASSERT_EQUAL_STRING("me@example.com", out.username);
   TEST_ASSERT_EQUAL_STRING("imap.example.com", out.imap_server);
   TEST_ASSERT_EQUAL_INT(993, out.imap_port);
   TEST_ASSERT_TRUE(out.imap_ssl);
   TEST_ASSERT_EQUAL_INT(465, out.smtp_port);
   TEST_ASSERT_EQUAL_INT(1, out.user_id);
   TEST_ASSERT_TRUE(out.enabled);
}

static void test_account_get_not_found(void) {
   email_account_t out;
   memset(&out, 0, sizeof(out));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, email_db_account_get(99999, &out));
}

static void test_account_preserves_encrypted_password(void) {
   email_account_t a = make_account(1, "Crypted", "crypt@example.com");
   /* Distinctive byte pattern in encrypted_password */
   for (int i = 0; i < 64; i++)
      a.encrypted_password[i] = (uint8_t)(i + 1);
   a.encrypted_password_len = 64;
   int64_t id = 0;
   email_db_account_create(&a, &id);

   email_account_t out;
   memset(&out, 0, sizeof(out));
   email_db_account_get(id, &out);
   TEST_ASSERT_EQUAL_INT(64, out.encrypted_password_len);
   for (int i = 0; i < 64; i++) {
      TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), out.encrypted_password[i]);
   }
}

/* ── Account List ────────────────────────────────────────────────────────── */

static void test_account_list(void) {
   email_account_t a1 = make_account(1, "A1", "a1@example.com");
   email_account_t a2 = make_account(1, "A2", "a2@example.com");
   email_db_account_create(&a1, NULL);
   email_db_account_create(&a2, NULL);

   email_account_t out[8];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_list(1, out, 8, &count));
   TEST_ASSERT_EQUAL_INT(2, count);
}

static void test_account_list_empty(void) {
   email_account_t out[4];
   int count = -1;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_list(1, out, 4, &count));
   TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_account_list_user_isolation(void) {
   email_account_t a1 = make_account(1, "User1Mail", "u1@example.com");
   email_account_t a2 = make_account(2, "User2Mail", "u2@example.com");
   email_db_account_create(&a1, NULL);
   email_db_account_create(&a2, NULL);

   email_account_t out[4];
   int count = 0;
   email_db_account_list(1, out, 4, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("User1Mail", out[0].name);
}

static void test_account_list_max_count(void) {
   for (int i = 0; i < 5; i++) {
      char name[32];
      char user[64];
      snprintf(name, sizeof(name), "Acct%d", i);
      snprintf(user, sizeof(user), "u%d@example.com", i);
      email_account_t a = make_account(1, name, user);
      email_db_account_create(&a, NULL);
   }

   email_account_t out[3];
   int count = 0;
   email_db_account_list(1, out, 3, &count);
   /* Honors max_count cap */
   TEST_ASSERT_EQUAL_INT(3, count);
}

/* ── Account Update ──────────────────────────────────────────────────────── */

static void test_account_update(void) {
   email_account_t a = make_account(1, "OrigName", "orig@example.com");
   int64_t id = 0;
   email_db_account_create(&a, &id);

   /* Modify and update */
   email_account_t mod;
   email_db_account_get(id, &mod);
   strncpy(mod.name, "Updated", sizeof(mod.name));
   mod.imap_port = 143;
   mod.max_recent = 25;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_update(&mod));

   email_account_t out;
   email_db_account_get(id, &out);
   TEST_ASSERT_EQUAL_STRING("Updated", out.name);
   TEST_ASSERT_EQUAL_INT(143, out.imap_port);
   TEST_ASSERT_EQUAL_INT(25, out.max_recent);
}

/* digest_depth sits AFTER created_at in every statement; assert both so a
 * column-index shift in row_to_account can't pass silently. */
static void test_digest_depth_default_update_and_clamp(void) {
   email_account_t a = make_account(1, "Depth", "depth@example.com");
   int64_t id = 0;
   time_t before = time(NULL);
   TEST_ASSERT_EQUAL_INT(0, email_db_account_create(&a, &id));

   email_account_t got;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_get(id, &got));
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_DEPTH_DEFAULT, got.digest_depth);
   TEST_ASSERT_TRUE(got.created_at >= before && got.created_at <= time(NULL) + 1);
   TEST_ASSERT_EQUAL_INT(4000, got.max_body_chars);

   got.digest_depth = 120;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_update(&got));
   email_account_t out;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_get(id, &out));
   TEST_ASSERT_EQUAL_INT(120, out.digest_depth);
   TEST_ASSERT_EQUAL_INT64((int64_t)got.created_at, (int64_t)out.created_at);

   out.digest_depth = EMAIL_DIGEST_DEPTH_MAX + 500;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_update(&out));
   TEST_ASSERT_EQUAL_INT(0, email_db_account_get(id, &got));
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_DEPTH_MAX, got.digest_depth);
}

/* ── Account Delete ──────────────────────────────────────────────────────── */

static void test_account_delete(void) {
   email_account_t a = make_account(1, "ToDelete", "del@example.com");
   int64_t id = 0;
   email_db_account_create(&a, &id);

   TEST_ASSERT_EQUAL_INT(0, email_db_account_delete(id));

   email_account_t out;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, email_db_account_get(id, &out));
}

/* ── Account Flags ───────────────────────────────────────────────────────── */

static void test_account_set_read_only(void) {
   email_account_t a = make_account(1, "RO", "ro@example.com");
   int64_t id = 0;
   email_db_account_create(&a, &id);

   TEST_ASSERT_EQUAL_INT(0, email_db_account_set_read_only(id, true));

   email_account_t out;
   email_db_account_get(id, &out);
   TEST_ASSERT_TRUE(out.read_only);

   email_db_account_set_read_only(id, false);
   email_db_account_get(id, &out);
   TEST_ASSERT_FALSE(out.read_only);
}

static void test_account_set_enabled(void) {
   email_account_t a = make_account(1, "Disabled", "dis@example.com");
   int64_t id = 0;
   email_db_account_create(&a, &id);

   TEST_ASSERT_EQUAL_INT(0, email_db_account_set_enabled(id, false));

   email_account_t out;
   email_db_account_get(id, &out);
   TEST_ASSERT_FALSE(out.enabled);

   email_db_account_set_enabled(id, true);
   email_db_account_get(id, &out);
   TEST_ASSERT_TRUE(out.enabled);
}

/* ── OAuth account variant ───────────────────────────────────────────────── */

static void test_account_oauth_type(void) {
   email_account_t a = make_account(1, "Gmail", "user@gmail.com");
   strncpy(a.auth_type, "oauth", sizeof(a.auth_type));
   strncpy(a.oauth_account_key, "google_user_at_gmail_com", sizeof(a.oauth_account_key));
   /* OAuth accounts don't store an encrypted password */
   memset(a.encrypted_password, 0, sizeof(a.encrypted_password));
   a.encrypted_password_len = 0;

   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(0, email_db_account_create(&a, &id));

   email_account_t out;
   email_db_account_get(id, &out);
   TEST_ASSERT_EQUAL_STRING("oauth", out.auth_type);
   TEST_ASSERT_EQUAL_STRING("google_user_at_gmail_com", out.oauth_account_key);
   TEST_ASSERT_EQUAL_INT(0, out.encrypted_password_len);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* Create / get */
   RUN_TEST(test_account_create);
   RUN_TEST(test_account_create_null_id_out);
   RUN_TEST(test_account_get);
   RUN_TEST(test_account_get_not_found);
   RUN_TEST(test_account_preserves_encrypted_password);

   /* List */
   RUN_TEST(test_account_list);
   RUN_TEST(test_account_list_empty);
   RUN_TEST(test_account_list_user_isolation);
   RUN_TEST(test_account_list_max_count);

   /* Update / delete */
   RUN_TEST(test_account_update);
   RUN_TEST(test_account_delete);

   /* Flags */
   RUN_TEST(test_account_set_read_only);
   RUN_TEST(test_account_set_enabled);

   /* OAuth */
   RUN_TEST(test_account_oauth_type);

   RUN_TEST(test_digest_depth_default_update_and_clamp);
   return UNITY_END();
}
