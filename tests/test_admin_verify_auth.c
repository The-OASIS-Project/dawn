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
 * Unit tests for verify_admin_auth(), the admin-password check in front of
 * dawn-admin's privileged commands: only an admin's right password passes.
 * Runs on an in-memory auth database with real Argon2id hashes; the rest of
 * the admin socket's dependencies are stubbed in test_admin_verify_auth_stub.c.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <sodium.h>
#include <stdint.h>
#include <string.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "dawn_error.h"
#include "unity.h"

#define ADMIN_NAME "operator"
#define ADMIN_PASS "correct horse battery"
#define USER_NAME "visitor"
#define USER_PASS "visitor password"

/* Builds the prefix verify_admin_auth() reads (two length bytes, the name, the
 * password) followed by a few bytes of operation payload. */
static uint16_t build_prefix(char *buf, size_t cap, const char *name, const char *pass) {
   const size_t nlen = strlen(name);
   const size_t plen = strlen(pass);
   const char tail[] = "op";
   TEST_ASSERT_TRUE(2 + nlen + plen + sizeof(tail) <= cap);
   buf[0] = (char)nlen;
   buf[1] = (char)plen;
   memcpy(buf + 2, name, nlen);
   memcpy(buf + 2 + nlen, pass, plen);
   memcpy(buf + 2 + nlen + plen, tail, sizeof(tail));
   return (uint16_t)(2 + nlen + plen + sizeof(tail));
}

static void create_user(const char *name, const char *pass, bool is_admin) {
   char hash[AUTH_HASH_LEN];
   TEST_ASSERT_EQUAL_INT(AUTH_CRYPTO_SUCCESS, auth_hash_password(pass, hash));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user(name, hash, is_admin));
}

static int verify(const char *name, const char *pass, size_t *auth_size) {
   char buf[512];
   const uint16_t len = build_prefix(buf, sizeof(buf), name, pass);
   return verify_admin_auth(buf, len, auth_size);
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_CRYPTO_SUCCESS, auth_crypto_init());
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   create_user(ADMIN_NAME, ADMIN_PASS, true);
   create_user(USER_NAME, USER_PASS, false);
}

void tearDown(void) {
   auth_db_shutdown();
   auth_crypto_shutdown();
}

static void test_admin_with_right_password_passes(void) {
   size_t auth_size = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, verify(ADMIN_NAME, ADMIN_PASS, &auth_size));
   TEST_ASSERT_EQUAL_size_t(2 + strlen(ADMIN_NAME) + strlen(ADMIN_PASS), auth_size);
}

static void test_admin_with_wrong_password_is_refused(void) {
   size_t auth_size = 0;
   TEST_ASSERT_NOT_EQUAL_INT(SUCCESS, verify(ADMIN_NAME, "wrong password!", &auth_size));
}

static void test_non_admin_with_right_password_is_refused(void) {
   size_t auth_size = 0;
   TEST_ASSERT_NOT_EQUAL_INT(SUCCESS, verify(USER_NAME, USER_PASS, &auth_size));
}

static void test_unknown_user_is_refused(void) {
   size_t auth_size = 0;
   TEST_ASSERT_NOT_EQUAL_INT(SUCCESS, verify("nobody", ADMIN_PASS, &auth_size));
}

static void test_password_below_minimum_length_is_refused(void) {
   size_t auth_size = 0;
   TEST_ASSERT_NOT_EQUAL_INT(SUCCESS, verify(ADMIN_NAME, "", &auth_size));
}

/* An unknown user or a non-admin is checked against the dummy hash so the
 * reply takes as long as a real check.  That holds only if libsodium accepts
 * the string and does the full work: a malformed one is refused at once. */
static void test_dummy_hash_is_well_formed(void) {
   /* libsodium declares its argument as a full crypto_pwhash_STRBYTES buffer. */
   char hash[crypto_pwhash_STRBYTES] = { 0 };
   TEST_ASSERT_TRUE(sizeof(AUTH_DUMMY_PASSWORD_HASH) <= sizeof(hash));
   memcpy(hash, AUTH_DUMMY_PASSWORD_HASH, sizeof(AUTH_DUMMY_PASSWORD_HASH));
   TEST_ASSERT_EQUAL_INT(0, crypto_pwhash_str_needs_rehash(hash, AUTH_OPSLIMIT, AUTH_MEMLIMIT));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_admin_with_right_password_passes);
   RUN_TEST(test_admin_with_wrong_password_is_refused);
   RUN_TEST(test_non_admin_with_right_password_is_refused);
   RUN_TEST(test_unknown_user_is_refused);
   RUN_TEST(test_password_below_minimum_length_is_refused);
   RUN_TEST(test_dummy_hash_is_well_formed);
   return UNITY_END();
}
