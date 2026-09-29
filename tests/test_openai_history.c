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
 * Unit tests for preparing conversation history for the OpenAI-compatible
 * chat-completions path (OpenAI, Gemini, OpenRouter, local): DAWN's own message
 * keys never reach the wire, and a Claude turn's reasoning is left out.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>

#include "llm/llm_openai_internal.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

/* A Claude request's carrier: its endpoint and key tag (llm_request_carrier). */
#define TEST_CLAUDE_CARRIER "api.anthropic.com#0123456789abcdef"

/* ---- stubs ---- */
int is_vision_enabled_for_current_llm(void) {
   return 1;
}
struct json_object *llm_history_strip_vision_content(struct json_object *history) {
   return json_object_get(history);
}

void setUp(void) {
}
void tearDown(void) {
}

static json_object *msg(const char *role, const char *content) {
   json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

static void test_internal_keys_never_go_on_the_wire(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "Hi"));
   json_object *answer = msg("assistant", "Hello.");
   json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC,
                                 "claude-opus-5-5",
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":\"x\","
                                                    "\"signature\":\"S\"}"));
   llm_turn_blocks_add_text(blocks, "Hello.");
   json_object_object_add(answer, LLM_TURN_BLOCKS_KEY, blocks);
   json_object_object_add(answer, "_internal_probe", json_object_new_object());
   json_object_array_add(history, answer);
   json_object_array_add(history, msg("user", "Bye"));

   json_object *prepared = llm_openai_prepare_chat_history(history, "api.example.com#00000000",
                                                           "m");
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(prepared));
   json_object *out = json_object_array_get_idx(prepared, 1);
   TEST_ASSERT_FALSE(json_object_object_get_ex(out, LLM_TURN_BLOCKS_KEY, NULL));
   TEST_ASSERT_FALSE(json_object_object_get_ex(out, "_internal_probe", NULL));
   TEST_ASSERT_EQUAL_STRING("Hello.",
                            json_object_get_string(json_object_object_get(out, "content")));
   /* The history itself is untouched (the keys still describe the turn). */
   TEST_ASSERT_TRUE(json_object_object_get_ex(answer, LLM_TURN_BLOCKS_KEY, NULL));
   /* A message with nothing internal is shared, not copied. */
   TEST_ASSERT_TRUE(json_object_array_get_idx(prepared, 0) ==
                    json_object_array_get_idx(history, 0));
   json_object_put(prepared);
   json_object_put(history);
}

static void test_claude_tool_turn_leaves_its_thinking_out(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "Weather?"));
   json_object *turn = json_tokener_parse(
       "{\"role\":\"assistant\",\"content\":["
       "{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"S\"},"
       "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"weather\",\"input\":{\"c\":1}}]}");
   json_object_array_add(history, turn);
   json_object_array_add(history, json_tokener_parse("{\"role\":\"user\",\"content\":[{\"type\":"
                                                     "\"tool_result\",\"tool_use_id\":\"toolu_1\","
                                                     "\"content\":\"72F\"}]}"));
   json_object *prepared = llm_openai_prepare_chat_history(history, "api.example.com#00000000",
                                                           "m");
   const char *wire = json_object_to_json_string_ext(prepared, JSON_C_TO_STRING_PLAIN);
   TEST_ASSERT_NULL_MESSAGE(strstr(wire, "signature"), wire);
   TEST_ASSERT_NULL_MESSAGE(strstr(wire, "\"thinking\""), wire);
   TEST_ASSERT_NOT_NULL(strstr(wire, "tool_calls"));
   json_object_put(prepared);
   json_object_put(history);
}

/* A Gemini-signed tool turn: the signature goes back on its call to the same
 * endpoint and model, and to no other. */
static void test_signed_call_goes_back_to_its_endpoint_only(void) {
   const char *gemini = "generativelanguage.googleapis.com#22222222";
   for (int pass = 0; pass < 2; pass++) {
      json_object *history = json_object_new_array();
      json_object_array_add(history, msg("user", "Weather?"));
      json_object *turn = msg("assistant", "");
      json_object *blocks = llm_turn_blocks_new();
      llm_turn_blocks_add_signed_tool_call(blocks, "c1", "weather", "{}", gemini, "g", "GEMSIG");
      json_object_object_add(turn, LLM_TURN_BLOCKS_KEY, blocks);
      json_object_array_add(history, turn);
      json_object *result = msg("tool", "72F");
      json_object_object_add(result, "tool_call_id", json_object_new_string("c1"));
      json_object_array_add(history, result);

      json_object *prepared = llm_openai_prepare_chat_history(
          history, pass == 0 ? gemini : "openrouter.ai#33333333", "g");
      const char *wire = json_object_to_json_string_ext(prepared, JSON_C_TO_STRING_PLAIN);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(wire, "\"c1\""), wire);
      if (pass == 0) {
         TEST_ASSERT_NOT_NULL_MESSAGE(strstr(wire, "GEMSIG"), wire);
      } else {
         TEST_ASSERT_NULL_MESSAGE(strstr(wire, "GEMSIG"), wire);
         TEST_ASSERT_NULL_MESSAGE(strstr(wire, "extra_content"), wire);
      }
      json_object_put(prepared);
      json_object_put(history);
   }
}

/* A Claude tool turn with its blocks, then the result in a Claude user message,
 * sent to a chat-completions model: the call and the result stay a pair. */
static void test_claude_turn_with_blocks_keeps_its_result(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "Weather?"));
   json_object *content = json_tokener_parse(
       "[{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"S\"},"
       "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"weather\",\"input\":{}}]");
   json_object *turn = json_object_new_object();
   json_object_object_add(turn, "role", json_object_new_string("assistant"));
   json_object_object_add(turn, LLM_TURN_BLOCKS_KEY,
                          llm_turn_blocks_from_claude(content, TEST_CLAUDE_CARRIER,
                                                      "claude-opus-5-5"));
   json_object_object_add(turn, "content", content);
   json_object_array_add(history, turn);
   json_object_array_add(history, json_tokener_parse("{\"role\":\"user\",\"content\":[{\"type\":"
                                                     "\"tool_result\",\"tool_use_id\":\"toolu_1\","
                                                     "\"content\":\"72F\"}]}"));
   json_object *prepared = llm_openai_prepare_chat_history(history, "api.example.com#00000000",
                                                           "m");
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(prepared));
   json_object *call = json_object_array_get_idx(prepared, 1);
   json_object *result = json_object_array_get_idx(prepared, 2);
   TEST_ASSERT_TRUE(json_object_object_get_ex(call, "tool_calls", NULL));
   TEST_ASSERT_EQUAL_STRING("tool", json_object_get_string(json_object_object_get(result, "role")));
   TEST_ASSERT_EQUAL_STRING("toolu_1",
                            json_object_get_string(json_object_object_get(result, "tool_call_id")));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(prepared), "\"hm\""));
   json_object_put(prepared);
   json_object_put(history);
}

/* A turn with one of its two results lost keeps its reasoning with the call
 * that remains. */
static void test_partial_turn_keeps_its_reasoning(void) {
   const char *C = "openrouter.ai#44444444";
   json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "Two cities?"));
   json_object *turn = msg("assistant", "");
   json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, C, LLM_FORMAT_OPENROUTER, "m",
                                 json_tokener_parse("{\"type\":\"reasoning.encrypted\","
                                                    "\"data\":\"KEEPME\"}"));
   llm_turn_blocks_add_tool_call(blocks, "c1", "weather", "{}");
   llm_turn_blocks_add_tool_call(blocks, "c2", "weather", "{}");
   json_object_object_add(turn, LLM_TURN_BLOCKS_KEY, blocks);
   json_object_array_add(history, turn);
   json_object *result = msg("tool", "sunny");
   json_object_object_add(result, "tool_call_id", json_object_new_string("c1"));
   json_object_array_add(history, result); /* c2's result was lost */

   json_object *prepared = llm_openai_prepare_chat_history(history, C, "m");
   const char *wire = json_object_to_json_string_ext(prepared, JSON_C_TO_STRING_PLAIN);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(wire, "KEEPME"), wire);
   TEST_ASSERT_NULL_MESSAGE(strstr(wire, "\"c2\""), wire);
   json_object_put(prepared);
   json_object_put(history);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_internal_keys_never_go_on_the_wire);
   RUN_TEST(test_claude_tool_turn_leaves_its_thinking_out);
   RUN_TEST(test_signed_call_goes_back_to_its_endpoint_only);
   RUN_TEST(test_claude_turn_with_blocks_keeps_its_result);
   RUN_TEST(test_partial_turn_keeps_its_reasoning);
   return UNITY_END();
}
