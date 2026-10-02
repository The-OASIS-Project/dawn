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
 * The client_ref a text turn may carry, and the per-thread turn ref that
 * names the turn in its errors and echo.
 */

#include <pthread.h>
#include <string.h>

#include "unity.h"
#include "webui/webui_turn_ref.h"

void setUp(void) {
   webui_turn_ref_set(NULL);
}

void tearDown(void) {
   webui_turn_ref_set(NULL);
}

static void test_valid_refs(void) {
   TEST_ASSERT_TRUE(webui_client_ref_valid("1"));
   TEST_ASSERT_TRUE(webui_client_ref_valid("turn-42 a~z"));
   char max[WEBUI_CLIENT_REF_MAX + 1];
   memset(max, 'a', WEBUI_CLIENT_REF_MAX);
   max[WEBUI_CLIENT_REF_MAX] = '\0';
   TEST_ASSERT_TRUE(webui_client_ref_valid(max));
}

static void test_invalid_refs(void) {
   TEST_ASSERT_FALSE(webui_client_ref_valid(NULL));
   TEST_ASSERT_FALSE(webui_client_ref_valid(""));
   TEST_ASSERT_FALSE(webui_client_ref_valid("a\nb"));
   TEST_ASSERT_FALSE(webui_client_ref_valid("caf\xc3\xa9"));
   TEST_ASSERT_FALSE(webui_client_ref_valid("tab\there"));
   char over[WEBUI_CLIENT_REF_MAX + 2];
   memset(over, 'a', WEBUI_CLIENT_REF_MAX + 1);
   over[WEBUI_CLIENT_REF_MAX + 1] = '\0';
   TEST_ASSERT_FALSE(webui_client_ref_valid(over));
}

static void test_set_get_clear(void) {
   TEST_ASSERT_NULL(webui_turn_ref_get());
   webui_turn_ref_set("7");
   TEST_ASSERT_EQUAL_STRING("7", webui_turn_ref_get());
   webui_turn_ref_set("");
   TEST_ASSERT_NULL(webui_turn_ref_get());
   webui_turn_ref_set("8");
   webui_turn_ref_set(NULL);
   TEST_ASSERT_NULL(webui_turn_ref_get());
}

static void *other_thread(void *arg) {
   (void)arg;
   /* Another thread holds no ref of this one's, and its own stays its own. */
   const bool none = webui_turn_ref_get() == NULL;
   webui_turn_ref_set("other");
   return none ? (void *)1 : NULL;
}

static void test_ref_is_per_thread(void) {
   webui_turn_ref_set("mine");
   pthread_t t;
   void *ok = NULL;
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, other_thread, NULL));
   TEST_ASSERT_EQUAL_INT(0, pthread_join(t, &ok));
   TEST_ASSERT_NOT_NULL(ok);
   TEST_ASSERT_EQUAL_STRING("mine", webui_turn_ref_get());
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_valid_refs);
   RUN_TEST(test_invalid_refs);
   RUN_TEST(test_set_get_clear);
   RUN_TEST(test_ref_is_per_thread);
   return UNITY_END();
}
