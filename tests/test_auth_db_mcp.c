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
 * Unit tests for the v55 mcp_user_access migration + grant/revoke/check API.
 * Uses an in-memory SQLite database via auth_db_init(":memory:").
 */

#include <stdbool.h>

#include "auth/auth_db.h"
#include "auth/auth_db_mcp.h"
#include "unity.h"

static int64_t g_uid;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   /* mcp_user_access.user_id has an FK to users(id) (foreign_keys=ON), so a
    * real user must exist before granting. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("tester", "hash", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("tester", &u));
   g_uid = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

/* No grant yet -> not allowed; granting -> allowed. */
static void test_grant_enables_access(void) {
   bool allowed = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_check_access(g_uid, "cbm", &allowed));
   TEST_ASSERT_FALSE(allowed);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(g_uid, "cbm"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_check_access(g_uid, "cbm", &allowed));
   TEST_ASSERT_TRUE(allowed);
}

/* Revoke disables an existing grant. */
static void test_revoke_disables_access(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(g_uid, "cbm"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_revoke(g_uid, "cbm"));

   bool allowed = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_check_access(g_uid, "cbm", &allowed));
   TEST_ASSERT_FALSE(allowed);
}

/* Grant is idempotent and re-enables after a revoke. */
static void test_regrant_after_revoke(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(g_uid, "cbm"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_revoke(g_uid, "cbm"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(g_uid, "cbm"));

   bool allowed = false;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_check_access(g_uid, "cbm", &allowed));
   TEST_ASSERT_TRUE(allowed);
}

/* Access is scoped per (user, server_alias). */
static void test_access_is_per_server(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(g_uid, "cbm"));

   bool allowed = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_check_access(g_uid, "other", &allowed));
   TEST_ASSERT_FALSE(allowed);
}

/* Revoking a never-granted server is a successful no-op. */
static void test_revoke_nonexistent_is_noop(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_revoke(g_uid, "ghost"));
   bool allowed = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_check_access(g_uid, "ghost", &allowed));
   TEST_ASSERT_FALSE(allowed);
}

/* The admin-flag lookup used by the bridge dangerous-tool gate. */
static void test_user_is_admin(void) {
   bool is_admin = false;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_user_is_admin(g_uid, &is_admin));
   TEST_ASSERT_TRUE(is_admin); /* setUp created an admin */

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("plebe", "h", false));
   auth_user_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("plebe", &p));
   is_admin = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_user_is_admin(p.id, &is_admin));
   TEST_ASSERT_FALSE(is_admin);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_grant_enables_access);
   RUN_TEST(test_revoke_disables_access);
   RUN_TEST(test_regrant_after_revoke);
   RUN_TEST(test_access_is_per_server);
   RUN_TEST(test_revoke_nonexistent_is_noop);
   RUN_TEST(test_user_is_admin);
   return UNITY_END();
}
