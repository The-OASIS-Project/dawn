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
 * Unit tests for llm_openai_merge_leading_system_messages(): the fix for the
 * strict-Jinja HTTP 500.  DAWN's two-segment [system, system, user] prompt is
 * collapsed to one system message for every Chat Completions request, under a
 * copy-on-write contract: request.messages may ALIAS the session's canonical
 * conversation_history, which must never be mutated.
 */

#include <json-c/json.h>

#include "llm/llm_openai_cache.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Builders / accessors ───────────────────────────────────────────────── */

static json_object *make_msg(const char *role, const char *content) {
   json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

/* A two-segment history: [system(stable), system(volatile), user]. */
static json_object *make_history(void) {
   json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", "Your name is Friday."));
   json_object_array_add(h, make_msg("system", "--- TURN CONTEXT ---"));
   json_object_array_add(h, make_msg("user", "hi"));
   return h;
}

static json_object *msg_at(json_object *arr, int idx) {
   return json_object_array_get_idx(arr, idx);
}

static json_object *content_at(json_object *arr, int idx) {
   json_object *c = NULL;
   json_object_object_get_ex(msg_at(arr, idx), "content", &c);
   return c;
}

static json_object *root_messages(json_object *root) {
   json_object *m = NULL;
   json_object_object_get_ex(root, "messages", &m);
   return m;
}

/* Build a request whose "messages" ALIASES the given history (shared ref), the
 * way the real request builder does (root.messages = prepare_chat_history(...)
 * returns the history by reference in the common path). */
static json_object *make_root_aliasing(json_object *history) {
   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));
   return root;
}

/* ── merge_leading_system_messages: the strict-Jinja 500 fix ─────────────── */

/* The two-segment [system, system, user] collapses to [system(merged), user] in
 * the REQUEST, joined by a blank line, while the shared history stays untouched. */
static void test_merge_collapses_two_system_messages(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history);

   llm_openai_merge_leading_system_messages(root);

   json_object *rmsgs = root_messages(root);
   /* Request now has exactly [system, user]. */
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(rmsgs));
   json_object *role0 = NULL;
   json_object_object_get_ex(msg_at(rmsgs, 0), "role", &role0);
   TEST_ASSERT_EQUAL_STRING("system", json_object_get_string(role0));
   json_object *role1 = NULL;
   json_object_object_get_ex(msg_at(rmsgs, 1), "role", &role1);
   TEST_ASSERT_EQUAL_STRING("user", json_object_get_string(role1));
   /* Merged content = both bodies joined by "\n\n", still a plain string. */
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(rmsgs, 0)));
   TEST_ASSERT_EQUAL_STRING("Your name is Friday.\n\n--- TURN CONTEXT ---",
                            json_object_get_string(content_at(rmsgs, 0)));

   /* COW: the session history is byte-for-byte unchanged (still 3 messages). */
   TEST_ASSERT_TRUE(rmsgs != history);
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(history));
   TEST_ASSERT_EQUAL_STRING("Your name is Friday.", json_object_get_string(content_at(history, 0)));
   TEST_ASSERT_EQUAL_STRING("--- TURN CONTEXT ---", json_object_get_string(content_at(history, 1)));

   json_object_put(root);
   json_object_put(history);
}

/* A single leading system message is a no-op — the request keeps aliasing history. */
static void test_merge_single_system_is_noop(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, make_msg("system", "solo"));
   json_object_array_add(history, make_msg("user", "hi"));
   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));

   llm_openai_merge_leading_system_messages(root);

   TEST_ASSERT_TRUE(root_messages(root) == history);
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(history));

   json_object_put(root);
   json_object_put(history);
}

/* Only the CONTIGUOUS leading run merges; a system message after a user turn is
 * left in place (order preserved). */
static void test_merge_only_leading_run(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, make_msg("system", "a"));
   json_object_array_add(history, make_msg("system", "b"));
   json_object_array_add(history, make_msg("user", "hi"));
   json_object_array_add(history, make_msg("system", "mid-stream system"));
   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));

   llm_openai_merge_leading_system_messages(root);

   json_object *rmsgs = root_messages(root);
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(rmsgs));
   TEST_ASSERT_EQUAL_STRING("a\n\nb", json_object_get_string(content_at(rmsgs, 0)));
   json_object *role2 = NULL;
   json_object_object_get_ex(msg_at(rmsgs, 2), "role", &role2);
   TEST_ASSERT_EQUAL_STRING("system", json_object_get_string(role2));
   TEST_ASSERT_EQUAL_STRING("mid-stream system", json_object_get_string(content_at(rmsgs, 2)));

   json_object_put(root);
   json_object_put(history);
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_merge_collapses_two_system_messages);
   RUN_TEST(test_merge_single_system_is_noop);
   RUN_TEST(test_merge_only_leading_run);

   return UNITY_END();
}
