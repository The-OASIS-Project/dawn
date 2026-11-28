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
 * Unit tests for the WebUI's change_password request: a non-admin changes
 * their own password only with their current one (a wrong one, null or a
 * number is refused and the stored hash stays), never someone else's, and an
 * admin changes another user's password without one.  Runs on an in-memory
 * auth database with real Argon2id hashes; the reply is captured by a stub.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/music_queue_db.h"
#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "core/conv_images.h"
#include "core/session_prefix.h"
#include "unity.h"
#include "webui/webui_handlers.h"
#include "webui/webui_internal.h"

#define ADMIN_NAME "operator"
#define ADMIN_PASS "admin password"
#define USER_NAME "visitor"
#define USER_PASS "visitor password"
#define OTHER_NAME "neighbor"
#define OTHER_PASS "neighbor password"
#define NEW_PASS "a brand new password"

/* ---- stubs: the connection's replies and the other handlers' calls ---- */

static json_object *s_sent; /* the last frame sent */

void send_json_response(ws_connection_t *conn, json_object *response) {
   (void)conn;
   json_object_put(s_sent);
   s_sent = json_object_get(response);
}

bool conn_require_auth(ws_connection_t *conn) {
   return conn->auth_user_id > 0;
}

bool conn_require_admin(ws_connection_t *conn) {
   (void)conn;
   return false;
}

/* Called by the user-delete handler in the same file; never reached here. */
bool conv_images_purge_user(int user_id) {
   (void)user_id;
   return true;
}

int music_queue_db_delete_user(int user_id) {
   (void)user_id;
   return 0;
}

void session_withdraw_forgotten_async(int user_id, bool memory_bodies) {
   (void)user_id;
   (void)memory_bodies;
}

/* ---- fixtures ---- */

static ws_connection_t *s_conn;

static void create_user(const char *name, const char *pass, bool is_admin) {
   char hash[AUTH_HASH_LEN];
   TEST_ASSERT_EQUAL_INT(AUTH_CRYPTO_SUCCESS, auth_hash_password(pass, hash));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user(name, hash, is_admin));
}

/* Signs the connection in as @p name with a real session, as a login does. */
static void sign_in(const char *name) {
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user(name, &user));
   TEST_ASSERT_EQUAL_INT(AUTH_CRYPTO_SUCCESS, auth_generate_token(s_conn->auth_session_token));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         auth_db_create_session(user.id, s_conn->auth_session_token, NULL, NULL));
   s_conn->auth_user_id = user.id;
   snprintf(s_conn->username, sizeof(s_conn->username), "%s", name);
}

static bool password_is(const char *name, const char *pass) {
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user(name, &user));
   return auth_verify_password(user.password_hash, pass);
}

/* Sends @p json as a change_password request; returns the reply's payload. */
static json_object *change(const char *json) {
   json_object *payload = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL(payload);
   handle_change_password(s_conn, payload);
   json_object_put(payload);
   TEST_ASSERT_NOT_NULL(s_sent);
   json_object *type = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(s_sent, "type", &type));
   TEST_ASSERT_EQUAL_STRING("change_password_response", json_object_get_string(type));
   json_object *reply = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(s_sent, "payload", &reply));
   return reply;
}

static bool succeeded(json_object *reply) {
   json_object *ok = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(reply, "success", &ok));
   return json_object_get_boolean(ok);
}

static const char *error_of(json_object *reply) {
   json_object *err = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(reply, "error", &err));
   return json_object_get_string(err);
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_CRYPTO_SUCCESS, auth_crypto_init());
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   create_user(ADMIN_NAME, ADMIN_PASS, true);
   create_user(USER_NAME, USER_PASS, false);
   create_user(OTHER_NAME, OTHER_PASS, false);
   s_conn = calloc(1, sizeof(*s_conn));
   TEST_ASSERT_NOT_NULL(s_conn);
   s_sent = NULL;
}

void tearDown(void) {
   json_object_put(s_sent);
   s_sent = NULL;
   free(s_conn);
   s_conn = NULL;
   auth_db_shutdown();
   auth_crypto_shutdown();
}

/* ---- a non-admin's own password ---- */

static void test_wrong_current_password_is_refused(void) {
   sign_in(USER_NAME);
   json_object *reply = change("{\"username\": \"" USER_NAME "\", \"new_password\": \"" NEW_PASS
                               "\", \"current_password\": \"not my password\"}");
   TEST_ASSERT_FALSE(succeeded(reply));
   TEST_ASSERT_TRUE(password_is(USER_NAME, USER_PASS));
   TEST_ASSERT_FALSE(password_is(USER_NAME, NEW_PASS));
}

static void test_null_current_password_is_refused(void) {
   sign_in(USER_NAME);
   json_object *reply = change("{\"username\": \"" USER_NAME "\", \"new_password\": \"" NEW_PASS
                               "\", \"current_password\": null}");
   TEST_ASSERT_FALSE(succeeded(reply));
   /* Refused as not given, before any hash is run, not as a wrong password. */
   TEST_ASSERT_EQUAL_STRING("Current password required", error_of(reply));
   TEST_ASSERT_TRUE(password_is(USER_NAME, USER_PASS));
}

static void test_numeric_current_password_is_refused(void) {
   sign_in(USER_NAME);
   json_object *reply = change("{\"username\": \"" USER_NAME "\", \"new_password\": \"" NEW_PASS
                               "\", \"current_password\": 12345678}");
   TEST_ASSERT_FALSE(succeeded(reply));
   TEST_ASSERT_EQUAL_STRING("Current password required", error_of(reply));
   TEST_ASSERT_TRUE(password_is(USER_NAME, USER_PASS));
}

static void test_right_current_password_changes_it(void) {
   sign_in(USER_NAME);
   json_object *reply = change("{\"username\": \"" USER_NAME "\", \"new_password\": \"" NEW_PASS
                               "\", \"current_password\": \"" USER_PASS "\"}");
   TEST_ASSERT_TRUE(succeeded(reply));
   TEST_ASSERT_TRUE(password_is(USER_NAME, NEW_PASS));
   TEST_ASSERT_FALSE(password_is(USER_NAME, USER_PASS));
}

static void test_non_admin_cannot_change_another_users_password(void) {
   sign_in(USER_NAME);
   json_object *reply = change("{\"username\": \"" OTHER_NAME "\", \"new_password\": \"" NEW_PASS
                               "\", \"current_password\": \"" USER_PASS "\"}");
   TEST_ASSERT_FALSE(succeeded(reply));
   TEST_ASSERT_TRUE(password_is(OTHER_NAME, OTHER_PASS));
}

/* ---- an admin ---- */

static void test_admin_changes_another_users_password_without_current(void) {
   sign_in(ADMIN_NAME);
   json_object *reply = change("{\"username\": \"" OTHER_NAME "\", \"new_password\": \"" NEW_PASS
                               "\"}");
   TEST_ASSERT_TRUE(succeeded(reply));
   TEST_ASSERT_TRUE(password_is(OTHER_NAME, NEW_PASS));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_wrong_current_password_is_refused);
   RUN_TEST(test_null_current_password_is_refused);
   RUN_TEST(test_numeric_current_password_is_refused);
   RUN_TEST(test_right_current_password_changes_it);
   RUN_TEST(test_non_admin_cannot_change_another_users_password);
   RUN_TEST(test_admin_changes_another_users_password_without_current);
   return UNITY_END();
}
