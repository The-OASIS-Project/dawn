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
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_turn_blocks.h"
#include "memory/memory_history_loader.h"
#include "unity.h"

/* A Claude request's carrier: its endpoint and key tag (llm_request_carrier). */
#define TEST_CLAUDE_CARRIER "api.anthropic.com#0123456789abcdef"

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

/* @p q opens with the compaction's summary part, then its own words. */
static void assert_summary_leads(struct json_object *q, const char *summary, const char *own) {
   struct json_object *parts = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(q, "content", &parts));
   TEST_ASSERT_TRUE(json_object_is_type(parts, json_type_array));
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(parts));
   struct json_object *first = json_object_array_get_idx(parts, 0);
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_SUMMARY, llm_history_kind_of(first));
   struct json_object *t = NULL;
   json_object_object_get_ex(first, "text", &t);
   TEST_ASSERT_NOT_NULL(strstr(json_object_get_string(t), "--- CONVERSATION SUMMARY"));
   TEST_ASSERT_NOT_NULL(strstr(json_object_get_string(t), summary));
   TEST_ASSERT_EQUAL_STRING(own, llm_history_question_text(q));
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
   /* The answer, then the next question with the summary in front of it:
    * results 2 and 3, whose call is in the summary, are gone. */
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(h));
   TEST_ASSERT_EQUAL_STRING("assistant", role_at(h, 0));
   TEST_ASSERT_EQUAL_STRING("user", role_at(h, 1));
   assert_summary_leads(json_object_array_get_idx(h, 1), "They added numbers.", "thanks");
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
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(h)); /* two (summary first), second */
   TEST_ASSERT_EQUAL_STRING("user", role_at(h, 0));
   assert_summary_leads(json_object_array_get_idx(h, 0), "One.", "two");
   json_object_put(h);
}

/* A row as stored: blocks with a thinking signature and one tool call. */
static int64_t add_with_blocks(int64_t conv, const char *call_id, const char *calls_json) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC, "m",
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":"
                                                    "\"plan\",\"signature\":\"SIG\"}"));
   llm_turn_blocks_add_text(blocks, "Checking.");
   llm_turn_blocks_add_tool_call(blocks, call_id, "weather", "{}");
   char *stored = llm_turn_blocks_to_stored(blocks);
   json_object_put(blocks);
   TEST_ASSERT_NOT_NULL(stored);
   const conv_message_row_t row = { .role = "assistant",
                                    .content = "Checking.",
                                    .tool_calls = calls_json,
                                    .llm_blocks = stored };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user_id, &row, &id));
   free(stored);
   return id;
}

static struct json_object *message_with_id(struct json_object *h, int64_t id) {
   for (size_t i = 0; i < json_object_array_length(h); i++) {
      struct json_object *m = json_object_array_get_idx(h, i), *v = NULL;
      if (json_object_object_get_ex(m, "id", &v) && json_object_get_int64(v) == id)
         return m;
   }
   return NULL;
}

/* A replay load brings each turn's blocks back; nothing else does. */
static void test_blocks_reload_for_replay_only(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user_id, "chat", &conv));
   add(conv, "user", "weather?", NULL, NULL);
   const char *calls = "[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
                       "{\"name\":\"weather\",\"arguments\":\"{}\"}}]";
   const int64_t turn = add_with_blocks(conv, "c1", calls);
   add(conv, "tool", "sunny", NULL, "c1");
   /* Blocks naming another call than their row's: not replayed. */
   const int64_t stale = add_with_blocks(conv, "c9", calls);

   struct json_object *h = memory_history_load_for_llm(conv, s_user_id, NULL);
   TEST_ASSERT_NOT_NULL(h);
   struct json_object *blocks = NULL;
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(message_with_id(h, turn), LLM_TURN_BLOCKS_KEY, &blocks));
   struct json_object *claude = llm_turn_blocks_render_claude(blocks, TEST_CLAUDE_CARRIER);
   const char *wire = json_object_to_json_string_ext(claude, JSON_C_TO_STRING_PLAIN);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(wire, "\"signature\":\"SIG\""), wire);
   TEST_ASSERT_NOT_NULL(strstr(wire, "\"id\":\"c1\""));
   json_object_put(claude);
   TEST_ASSERT_FALSE(
       json_object_object_get_ex(message_with_id(h, stale), LLM_TURN_BLOCKS_KEY, NULL));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(h), "_blocks_raw"));
   json_object_put(h);

   /* Extraction and summaries load the same rows without them. */
   h = memory_history_load_from_db(conv, s_user_id, NULL);
   TEST_ASSERT_NOT_NULL(h);
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(h), "SIG"));
   json_object_put(h);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_point_inside_a_tool_exchange);
   RUN_TEST(test_point_at_a_turn);
   RUN_TEST(test_blocks_reload_for_replay_only);
   return UNITY_END();
}
