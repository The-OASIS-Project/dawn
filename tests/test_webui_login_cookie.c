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
 * One login cookie per app (webui_login_cookie.c).
 */

#include <string.h>

#include "unity.h"
#include "webui/webui_login_cookie.h"

void setUp(void) {
}

void tearDown(void) {
}

static const char *name_for(const char *app) {
   static char out[WEBUI_LOGIN_COOKIE_NAME_MAX];
   return webui_login_cookie_name(app, out, sizeof(out)) ? out : NULL;
}

static void test_each_app_has_its_own_cookie(void) {
   TEST_ASSERT_EQUAL_STRING("__Host-dawn_session", name_for(NULL));
   TEST_ASSERT_EQUAL_STRING("__Host-dawn_session", name_for(""));
   TEST_ASSERT_EQUAL_STRING("__Host-dawn_session", name_for("webui"));
   TEST_ASSERT_EQUAL_STRING("__Host-dawn_session_aurora", name_for("aurora"));
   TEST_ASSERT_EQUAL_STRING("__Host-dawn_session_hud_2", name_for("hud_2"));
   TEST_ASSERT_EQUAL_STRING("__Host-dawn_session_abcdefghijklmnop", name_for("abcdefghijklmnop"));
}

/* No other name (a different header, attributes, another cookie) can be made. */
static void test_invalid_app_names_have_no_cookie(void) {
   TEST_ASSERT_NULL(name_for("Aurora"));
   TEST_ASSERT_NULL(name_for("a-b"));
   TEST_ASSERT_NULL(name_for("a;b"));
   TEST_ASSERT_NULL(name_for("a=b"));
   TEST_ASSERT_NULL(name_for("abcdefghijklmnopq")); /* 17 */
}

static const char *value_of(const char *header, const char *name) {
   static char out[80];
   return webui_cookie_value(header, name, out, sizeof(out)) ? out : NULL;
}

/* The exact name, wherever it sits; a cookie whose name only starts or ends
 * with it is someone else's. */
static void test_a_cookie_is_found_by_its_exact_name(void) {
   const char *h = "theme=dark; dawn_session_aurora=bbb; xdawn_session=zzz; dawn_session=aaa";
   TEST_ASSERT_EQUAL_STRING("aaa", value_of(h, "dawn_session"));
   TEST_ASSERT_EQUAL_STRING("bbb", value_of(h, "dawn_session_aurora"));
   TEST_ASSERT_NULL(value_of("dawn_session_aurora=bbb", "dawn_session"));
   TEST_ASSERT_NULL(value_of("xdawn_session=zzz", "dawn_session"));
   TEST_ASSERT_EQUAL_STRING("aaa", value_of("dawn_session=aaa", "dawn_session"));
}

static void test_empty_or_oversized_values_are_absent(void) {
   TEST_ASSERT_NULL(value_of("dawn_session=; other=1", "dawn_session"));
   TEST_ASSERT_NULL(value_of("", "dawn_session"));
   char tiny[4];
   TEST_ASSERT_FALSE(webui_cookie_value("dawn_session=abcdef", "dawn_session", tiny, sizeof(tiny)));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_each_app_has_its_own_cookie);
   RUN_TEST(test_invalid_app_names_have_no_cookie);
   RUN_TEST(test_a_cookie_is_found_by_its_exact_name);
   RUN_TEST(test_empty_or_oversized_values_are_absent);
   return UNITY_END();
}
