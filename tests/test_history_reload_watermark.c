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
 * A conversation whose compaction point was recorded inside a tool exchange
 * reloads without results whose call is in the summary.
 */

#include <stdbool.h>
#include <string.h>

#include "auth/auth_db.h"
#include "memory/memory_history_loader.h"
#include "unity.h"

static int s_user_id;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   s_user_id = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

static int64_t add(int64_t conv,
                   const char *role,
                   const char *content,
                   const char *calls,
                   const char *call_id) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_add_message_with_tools_ex(conv, s_user_id, role, content, calls,
                                                           call_id, NULL, false, &id));
   return id;
}

static const char *role_at(struct json_object *h, int i) {
   struct json_object *r = NULL;
   return json_object_object_get_ex(json_object_array_get_idx(h, i), "role", &r)
              ? json_object_get_string(r)
              : NULL;
}

/* An older build recorded the point at the first of three parallel results. */
static void test_point_inside_a_tool_exchange(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user_id, "chat", &conv));
   add(conv, "user", "add these up", NULL, NULL);
   add(conv, "assistant", "",
       "[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"calculator\","
       "\"arguments\":\"{}\"}},{\"id\":\"c2\",\"type\":\"function\",\"function\":"
       "{\"name\":\"calculator\",\"arguments\":\"{}\"}},{\"id\":\"c3\",\"type\":\"function\","
       "\"function\":{\"name\":\"calculator\",\"arguments\":\"{}\"}}]",
       NULL);
   const int64_t first_result = add(conv, "tool", "1", NULL, "c1");
   add(conv, "tool", "2", NULL, "c2");
   add(conv, "tool", "3", NULL, "c3");
   add(conv, "assistant", "The total is 6.", NULL, NULL);
   add(conv, "user", "thanks", NULL, NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv, s_user_id, "They added numbers.",
                                                          first_result));

   struct json_object *h = memory_history_load_from_db(conv, s_user_id, NULL);
   TEST_ASSERT_NOT_NULL(h);
   /* The summary, then the answer and the next question: results 2 and 3,
    * whose call is in the summary, are gone. */
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(h));
   TEST_ASSERT_EQUAL_STRING("assistant", role_at(h, 0));
   TEST_ASSERT_EQUAL_STRING("assistant", role_at(h, 1));
   TEST_ASSERT_EQUAL_STRING("user", role_at(h, 2));
   json_object_put(h);
}

/* A point at a turn boundary loads exactly what follows it. */
static void test_point_at_a_turn(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user_id, "chat", &conv));
   add(conv, "user", "one", NULL, NULL);
   const int64_t answer = add(conv, "assistant", "first", NULL, NULL);
   add(conv, "user", "two", NULL, NULL);
   add(conv, "assistant", "second", NULL, NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv, s_user_id, "One.", answer));
   struct json_object *h = memory_history_load_from_db(conv, s_user_id, NULL);
   TEST_ASSERT_NOT_NULL(h);
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(h)); /* summary, two, second */
   TEST_ASSERT_EQUAL_STRING("user", role_at(h, 1));
   json_object_put(h);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_point_inside_a_tool_exchange);
   RUN_TEST(test_point_at_a_turn);
   return UNITY_END();
}
