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
 * Unit tests for llm_context_merge.c: a background compaction result merged into
 * the history the next turn runs on, which is a rebuilt array holding the same
 * message objects under this turn's system prompt.
 */

#include <json-c/json.h>
#include <string.h>

#include "llm/llm_context_merge.h"
#include "unity.h"

static struct json_object *msg(const char *role, const char *content) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

static const char *content_at(struct json_object *arr, int i) {
   struct json_object *c = NULL;
   json_object_object_get_ex(json_object_array_get_idx(arr, i), "content", &c);
   return json_object_get_string(c);
}

void setUp(void) {
}
void tearDown(void) {
}

/* Turn N's history: [sys-old, u1, a1, u2, a2]; the summary covers all of it.
 * Turn N+1 rebuilt the array: [sys-new, vol-new, u1, a1, u2, a2, u3]. */
void test_merge_into_rebuilt_history_keeps_this_turns_prompt(void) {
   struct json_object *u1 = msg("user", "u1"), *a1 = msg("assistant", "a1");
   struct json_object *u2 = msg("user", "u2"), *a2 = msg("assistant", "a2");
   struct json_object *snapshot_last = json_object_get(a2);

   struct json_object *rebuilt = json_object_new_array();
   json_object_array_add(rebuilt, msg("system", "sys-new"));
   json_object_array_add(rebuilt, msg("system", "vol-new"));
   json_object_array_add(rebuilt, u1);
   json_object_array_add(rebuilt, a1);
   json_object_array_add(rebuilt, u2);
   json_object_array_add(rebuilt, a2);
   json_object_array_add(rebuilt, msg("user", "u3"));

   struct json_object *compacted = json_object_new_array();
   json_object_array_add(compacted, msg("system", "sys-old"));
   json_object_array_add(compacted, msg("assistant", "[summary]"));
   json_object_array_add(compacted, msg("user", "u2"));
   json_object_array_add(compacted, msg("assistant", "a2"));

   TEST_ASSERT_TRUE(llm_context_merge_compacted(rebuilt, compacted, snapshot_last));
   TEST_ASSERT_EQUAL_INT(6, (int)json_object_array_length(rebuilt));
   TEST_ASSERT_EQUAL_STRING("sys-new", content_at(rebuilt, 0));
   TEST_ASSERT_EQUAL_STRING("vol-new", content_at(rebuilt, 1));
   TEST_ASSERT_EQUAL_STRING("[summary]", content_at(rebuilt, 2));
   TEST_ASSERT_EQUAL_STRING("u2", content_at(rebuilt, 3));
   TEST_ASSERT_EQUAL_STRING("a2", content_at(rebuilt, 4));
   TEST_ASSERT_EQUAL_STRING("u3", content_at(rebuilt, 5));

   json_object_put(snapshot_last);
   json_object_put(compacted);
   json_object_put(rebuilt);
}

/* The history was replaced (another conversation loaded): the result is stale. */
void test_merge_refused_when_snapshot_is_gone(void) {
   struct json_object *gone = msg("assistant", "old");
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("system", "sys"));
   json_object_array_add(history, msg("user", "other conversation"));
   struct json_object *compacted = json_object_new_array();
   json_object_array_add(compacted, msg("assistant", "[summary]"));

   TEST_ASSERT_FALSE(llm_context_merge_compacted(history, compacted, gone));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(history));
   TEST_ASSERT_EQUAL_STRING("other conversation", content_at(history, 1));

   json_object_put(gone);
   json_object_put(compacted);
   json_object_put(history);
}

/* Nothing arrived since the snapshot: the history is exactly prompt + summary. */
void test_merge_with_no_new_messages(void) {
   struct json_object *last = msg("assistant", "a1");
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("system", "sys"));
   json_object_array_add(history, msg("user", "u1"));
   json_object_array_add(history, json_object_get(last));
   struct json_object *compacted = json_object_new_array();
   json_object_array_add(compacted, msg("assistant", "[summary]"));

   TEST_ASSERT_TRUE(llm_context_merge_compacted(history, compacted, last));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(history));
   TEST_ASSERT_EQUAL_STRING("sys", content_at(history, 0));
   TEST_ASSERT_EQUAL_STRING("[summary]", content_at(history, 1));

   json_object_put(last);
   json_object_put(compacted);
   json_object_put(history);
}

/* A device notice in the kept tail survives; only the summary's own prompt goes. */
void test_merge_keeps_notices_inside_the_body(void) {
   struct json_object *last = msg("assistant", "a1");
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("system", "sys"));
   json_object_array_add(history, msg("user", "u1"));
   json_object_array_add(history, json_object_get(last));
   struct json_object *compacted = json_object_new_array();
   json_object_array_add(compacted, msg("system", "old prompt"));
   json_object_array_add(compacted, msg("assistant", "[summary]"));
   json_object_array_add(compacted, msg("system", "incoming call"));

   TEST_ASSERT_TRUE(llm_context_merge_compacted(history, compacted, last));
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(history));
   TEST_ASSERT_EQUAL_STRING("sys", content_at(history, 0));
   TEST_ASSERT_EQUAL_STRING("[summary]", content_at(history, 1));
   TEST_ASSERT_EQUAL_STRING("incoming call", content_at(history, 2));

   json_object_put(last);
   json_object_put(compacted);
   json_object_put(history);
}

/* A boundary inside the leading prompt would replace this turn's prompt. */
void test_merge_refuses_boundary_in_prompt(void) {
   struct json_object *sys = msg("system", "sys");
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, json_object_get(sys));
   json_object_array_add(history, msg("system", "vol"));
   struct json_object *compacted = json_object_new_array();
   json_object_array_add(compacted, msg("assistant", "[summary]"));

   TEST_ASSERT_FALSE(llm_context_merge_compacted(history, compacted, sys));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(history));

   json_object_put(sys);
   json_object_put(compacted);
   json_object_put(history);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_merge_into_rebuilt_history_keeps_this_turns_prompt);
   RUN_TEST(test_merge_refused_when_snapshot_is_gone);
   RUN_TEST(test_merge_with_no_new_messages);
   RUN_TEST(test_merge_keeps_notices_inside_the_body);
   RUN_TEST(test_merge_refuses_boundary_in_prompt);
   return UNITY_END();
}
