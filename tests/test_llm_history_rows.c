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
 * Unit tests for the rows a history message saves as: the canonical rows
 * every reader expects (pinned by fixture), the turn's blocks alongside, and
 * no vendor data in anything a client reads.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_history_rows.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

/* A Claude request's carrier: its endpoint and key tag (llm_request_carrier). */
#define TEST_CLAUDE_CARRIER "api.anthropic.com#0123456789abcdef"

void setUp(void) {
}
void tearDown(void) {
}

#define FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

/* The rows without their stored blocks, as text. */
static char *display_rows(struct json_object *rows) {
   struct json_object *copy = NULL;
   json_object_deep_copy(rows, &copy, NULL);
   for (size_t i = 0; i < json_object_array_length(copy); i++) {
      json_object_object_del(json_object_array_get_idx(copy, i), LLM_HISTORY_ROW_STORED_KEY);
   }
   char *out = strdup(json_object_to_json_string_ext(copy, FLAGS));
   json_object_put(copy);
   return out;
}

static const char *stored_of(struct json_object *rows, size_t i) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(json_object_array_get_idx(rows, i), LLM_HISTORY_ROW_STORED_KEY,
                                    &v)
              ? json_object_get_string(v)
              : NULL;
}

/* A Claude turn calling two tools, and the user message answering both. */
static void test_claude_turn_with_two_results(void) {
   struct json_object *content = json_tokener_parse(
       "[{\"type\":\"thinking\",\"thinking\":\"plan\",\"signature\":\"SIG\"},"
       "{\"type\":\"text\",\"text\":\"Checking both.\"},"
       "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"weather\",\"input\":{\"city\":\"Paris\"}},"
       "{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"time\",\"input\":{}}]");
   struct json_object *turn = json_object_new_object();
   json_object_object_add(turn, "role", json_object_new_string("assistant"));
   json_object_object_add(turn, LLM_TURN_BLOCKS_KEY,
                          llm_turn_blocks_from_claude(content, TEST_CLAUDE_CARRIER, "m"));
   json_object_object_add(turn, "content", content);
   struct json_object *results = json_tokener_parse(
       "{\"role\":\"user\",\"content\":["
       "{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"sunny\"},"
       "{\"type\":\"tool_result\",\"tool_use_id\":\"t2\",\"content\":"
       "[{\"type\":\"text\",\"text\":\"noon\"}]}]}");

   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(1, llm_history_rows_append(turn, rows));
   TEST_ASSERT_EQUAL_INT(2, llm_history_rows_append(results, rows));

   char *shown = display_rows(rows);
   TEST_ASSERT_EQUAL_STRING(
       "[{\"role\":\"assistant\",\"content\":\"Checking both.\",\"tool_calls\":["
       "{\"id\":\"t1\",\"type\":\"function\",\"function\":{\"name\":\"weather\","
       "\"arguments\":\"{\\\"city\\\":\\\"Paris\\\"}\"}},"
       "{\"id\":\"t2\",\"type\":\"function\",\"function\":{\"name\":\"time\","
       "\"arguments\":\"{}\"}}]},"
       "{\"role\":\"tool\",\"content\":\"sunny\",\"tool_call_id\":\"t1\"},"
       "{\"role\":\"tool\",\"content\":\"noon\",\"tool_call_id\":\"t2\"}]",
       shown);
   free(shown);

   /* The turn's blocks ride along, signature and all, and reload as they were. */
   const char *stored = stored_of(rows, 0);
   TEST_ASSERT_NOT_NULL(stored);
   TEST_ASSERT_NOT_NULL(strstr(stored, "\"signature\":\"SIG\""));
   struct json_object *back = llm_turn_blocks_from_stored(stored, strlen(stored), 1);
   TEST_ASSERT_NOT_NULL(back);
   struct json_object *blocks = NULL;
   json_object_object_get_ex(turn, LLM_TURN_BLOCKS_KEY, &blocks);
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string_ext(blocks, FLAGS),
                            json_object_to_json_string_ext(back, FLAGS));
   json_object_put(back);
   TEST_ASSERT_NULL(stored_of(rows, 1));
   TEST_ASSERT_NULL(stored_of(rows, 2));

   json_object_put(rows);
   json_object_put(turn);
   json_object_put(results);
}

/* An OpenAI turn with reasoning of its own, and its result. */
static void test_openai_turn(void) {
   struct json_object *turn = json_tokener_parse(
       "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":"
       "\"function\",\"function\":{\"name\":\"search\",\"arguments\":\"{\\\"q\\\":\\\"x\\\"}\"}}]"
       "}");
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, "api.example.com#01", LLM_FORMAT_OPENAI, "m",
                                 json_tokener_parse("{\"type\":\"reasoning\",\"id\":\"r1\","
                                                    "\"encrypted_content\":\"ENC\"}"));
   llm_turn_blocks_add_tool_call(blocks, "c1", "search", "{\"q\":\"x\"}");
   json_object_object_add(turn, LLM_TURN_BLOCKS_KEY, blocks);
   struct json_object *result = json_tokener_parse(
       "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"found\"}");

   struct json_object *rows = json_object_new_array();
   llm_history_rows_append(turn, rows);
   llm_history_rows_append(result, rows);
   char *shown = display_rows(rows);
   TEST_ASSERT_EQUAL_STRING(
       "[{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"type\":"
       "\"function\",\"function\":{\"name\":\"search\",\"arguments\":\"{\\\"q\\\":\\\"x\\\"}\"}}]},"
       "{\"role\":\"tool\",\"content\":\"found\",\"tool_call_id\":\"c1\"}]",
       shown);
   TEST_ASSERT_NULL(strstr(shown, "ENC"));
   free(shown);
   TEST_ASSERT_NOT_NULL(strstr(stored_of(rows, 0), "ENC"));
   json_object_put(rows);
   json_object_put(turn);
   json_object_put(result);
}

/* Long text is saved whole, and a plain turn saves no blocks. */
static void test_long_text_and_plain_turns(void) {
   char *text = malloc(20001);
   memset(text, 'x', 20000);
   text[20000] = '\0';
   struct json_object *turn = json_object_new_object();
   json_object_object_add(turn, "role", json_object_new_string("assistant"));
   json_object_object_add(turn, "content", json_object_new_string(text));
   struct json_object *rows = json_object_new_array();
   llm_history_rows_append(turn, rows);
   struct json_object *content = NULL;
   json_object_object_get_ex(json_object_array_get_idx(rows, 0), "content", &content);
   TEST_ASSERT_EQUAL_size_t(20000, strlen(json_object_get_string(content)));
   TEST_ASSERT_NULL(stored_of(rows, 0));
   free(text);
   json_object_put(rows);
   json_object_put(turn);
}

/* Blocks naming calls their message doesn't carry aren't saved with it. */
static void test_blocks_must_match_the_calls(void) {
   struct json_object *turn = json_tokener_parse("{\"role\":\"assistant\",\"content\":\"hi\"}");
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_text(blocks, "hi");
   json_object_object_add(turn, LLM_TURN_BLOCKS_KEY, blocks);
   struct json_object *rows = json_object_new_array();
   llm_history_rows_append(turn, rows);
   TEST_ASSERT_NOT_NULL(stored_of(rows, 0)); /* no calls on either side */
   json_object_put(rows);
   json_object_put(turn);
}

/* A message with image parts saves its text, and a marker for each image. */
static void test_image_parts_become_markers(void) {
   struct json_object *msg = json_tokener_parse(
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"What is this?\"},"
       "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"}}]}");
   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(1, llm_history_rows_append(msg, rows));
   char *shown = display_rows(rows);
   TEST_ASSERT_EQUAL_STRING("[{\"role\":\"user\",\"content\":\"What is this?\\n\\n[image]\"}]",
                            shown);
   free(shown);
   json_object_put(rows);
   json_object_put(msg);
}

/* An image part carrying a short "text" of its own still gets room for its
 * marker. */
static void test_image_part_with_text_key(void) {
   struct json_object *msg = json_tokener_parse(
       "{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"text\":\"\"},"
       "{\"type\":\"image\",\"text\":\"ab\"}]}");
   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(1, llm_history_rows_append(msg, rows));
   char *shown = display_rows(rows);
   TEST_ASSERT_EQUAL_STRING("[{\"role\":\"user\",\"content\":\"[image]\\n\\n[image]\"}]", shown);
   free(shown);
   json_object_put(rows);
   json_object_put(msg);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_claude_turn_with_two_results);
   RUN_TEST(test_openai_turn);
   RUN_TEST(test_long_text_and_plain_turns);
   RUN_TEST(test_blocks_must_match_the_calls);
   RUN_TEST(test_image_parts_become_markers);
   RUN_TEST(test_image_part_with_text_key);
   return UNITY_END();
}
