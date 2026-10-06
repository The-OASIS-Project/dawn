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
 * Unit tests for parsing Anthropic's input_transformations (thinking binding
 * controls) into a drop report.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "llm/llm_claude_binding.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static llm_claude_drops_t parse_message(const char *json) {
   llm_claude_drops_t d;
   memset(&d, 0, sizeof(d));
   json_object *msg = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL(msg);
   llm_claude_drops_from_message(msg, &d);
   json_object_put(msg);
   return d;
}

static void test_absent_list_is_unreported(void) {
   llm_claude_drops_t d = parse_message("{\"id\":\"msg_1\"}");
   TEST_ASSERT_FALSE(d.reported);
   TEST_ASSERT_EQUAL_INT(0, d.prefix_drops + d.model_drops + d.other_drops);
}

static void test_empty_list_is_reported_clean(void) {
   llm_claude_drops_t d = parse_message("{\"input_transformations\":[]}");
   TEST_ASSERT_TRUE(d.reported);
   TEST_ASSERT_EQUAL_INT(0, d.prefix_drops + d.model_drops + d.other_drops);
   TEST_ASSERT_EQUAL_STRING("", d.first_path);
}

static void test_reasons_are_classified(void) {
   llm_claude_drops_t d = parse_message(
       "{\"input_transformations\":["
       "{\"type\":\"thinking_dropped\",\"path\":\"messages.3.content.0\","
       "\"reason\":\"prefix_binding_mismatch\"},"
       "{\"type\":\"thinking_dropped\",\"path\":\"messages.5.content.0\","
       "\"reason\":\"prefix_binding_mismatch\"},"
       "{\"type\":\"thinking_dropped\",\"path\":\"messages.1.content.0\","
       "\"reason\":\"model_binding_mismatch\"},"
       "{\"type\":\"thinking_dropped\",\"reason\":\"some_future_reason\"},"
       "{\"type\":\"some_future_type\",\"reason\":\"prefix_binding_mismatch\"}]}");
   TEST_ASSERT_TRUE(d.reported);
   TEST_ASSERT_EQUAL_INT(2, d.prefix_drops);
   TEST_ASSERT_EQUAL_INT(1, d.model_drops);
   TEST_ASSERT_EQUAL_INT(2, d.other_drops);
   TEST_ASSERT_EQUAL_STRING("messages.3.content.0", d.first_path);
}

static void test_non_array_is_unreported(void) {
   llm_claude_drops_t d = parse_message("{\"input_transformations\":null}");
   TEST_ASSERT_FALSE(d.reported);
   d = parse_message("{\"input_transformations\":{\"type\":\"thinking_dropped\"}}");
   TEST_ASSERT_FALSE(d.reported);
}

static void test_final_delta_replaces_start(void) {
   /* message_start reported a drop; the final message_delta (after a
    * mid-stream fallback) carries the authoritative list, and a delta
    * without one leaves the earlier report alone. */
   llm_claude_drops_t d;
   memset(&d, 0, sizeof(d));
   json_object *start = json_tokener_parse(
       "{\"input_transformations\":[{\"type\":\"thinking_dropped\",\"path\":\"messages.1\","
       "\"reason\":\"prefix_binding_mismatch\"}]}");
   json_object *plain_delta = json_tokener_parse("{\"delta\":{\"stop_reason\":\"end_turn\"}}");
   json_object *final_delta = json_tokener_parse("{\"input_transformations\":[]}");
   llm_claude_drops_from_message(start, &d);
   TEST_ASSERT_EQUAL_INT(1, d.prefix_drops);
   llm_claude_drops_from_message(plain_delta, &d);
   TEST_ASSERT_EQUAL_INT(1, d.prefix_drops);
   llm_claude_drops_from_message(final_delta, &d);
   TEST_ASSERT_TRUE(d.reported);
   TEST_ASSERT_EQUAL_INT(0, d.prefix_drops);
   json_object_put(start);
   json_object_put(plain_delta);
   json_object_put(final_delta);
}

static void test_long_path_is_bounded(void) {
   char json[512];
   char path[200];
   memset(path, 'x', sizeof(path) - 1);
   path[sizeof(path) - 1] = '\0';
   snprintf(json, sizeof(json),
            "{\"input_transformations\":[{\"type\":\"thinking_dropped\",\"path\":\"%s\","
            "\"reason\":\"prefix_binding_mismatch\"}]}",
            path);
   llm_claude_drops_t d = parse_message(json);
   TEST_ASSERT_EQUAL_INT(1, d.prefix_drops);
   TEST_ASSERT_EQUAL_size_t(sizeof(d.first_path) - 1, strlen(d.first_path));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_absent_list_is_unreported);
   RUN_TEST(test_empty_list_is_reported_clean);
   RUN_TEST(test_reasons_are_classified);
   RUN_TEST(test_non_array_is_unreported);
   RUN_TEST(test_final_delta_replaces_start);
   RUN_TEST(test_long_path_is_bounded);
   return UNITY_END();
}
