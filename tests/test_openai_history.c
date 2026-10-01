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

/* A turn's context in front of its question as one string, a direction as an
 * operator's note after it; no marks on the wire; an image question keeps both. */
static void test_request_context_for_chat(void) {
   json_object *history = json_tokener_parse(
       "[{\"role\":\"system\",\"content\":\"P\"},"
       "{\"role\":\"user\",\"content\":["
       "{\"type\":\"text\",\"text\":\"MEM\",\"_kind\":\"memory\"},"
       "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"},"
       "{\"type\":\"text\",\"text\":\"Hi\"}]},"
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\"}]");
   json_object *prepared = llm_openai_prepare_chat_history(history, "api.example.com#00000000",
                                                           "m");
   TEST_ASSERT_EQUAL_STRING("[{\"role\":\"system\",\"content\":\"P\"},{\"role\":\"user\","
                            "\"content\":\"MEM\\n\\nCTX\\n\\nHi\\n\\n[Operator note] D\"}]",
                            json_object_to_json_string_ext(prepared, JSON_C_TO_STRING_PLAIN));
   json_object_put(prepared);
   json_object_put(history);

   /* An image question (its images parts of its own message, as the dispatch
    * builds it): its context in front, the image after its text, the note last. */
   history = json_tokener_parse(
       "[{\"role\":\"system\",\"content\":\"P\"},"
       "{\"role\":\"user\",\"content\":["
       "{\"type\":\"text\",\"text\":\"MEM\",\"_kind\":\"memory\"},"
       "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"},"
       "{\"type\":\"text\",\"text\":\"Hi\"},"
       "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,iVBO\"}}]},"
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\"}]");
   prepared = llm_openai_prepare_chat_history(history, "api.example.com#00000000", "m");
   TEST_ASSERT_EQUAL_STRING(
       "[{\"role\":\"system\",\"content\":\"P\"},{\"role\":\"user\",\"content\":["
       "{\"type\":\"text\",\"text\":\"MEM\"},{\"type\":\"text\",\"text\":\"CTX\"},"
       "{\"type\":\"text\",\"text\":\"Hi\"},{\"type\":\"image_url\",\"image_url\":{\"url\":"
       "\"data:image\\/png;base64,iVBO\"}},{\"type\":\"text\",\"text\":\"[Operator note] D\"}]}]",
       json_object_to_json_string_ext(prepared, JSON_C_TO_STRING_PLAIN));
   json_object_put(prepared);
   json_object_put(history);
}

/* A direction after a Claude message of tool results is a note of its own
 * (text in a results message doesn't survive becoming tool messages). */
static void test_note_after_claude_results(void) {
   json_object *history = json_tokener_parse(
       "[{\"role\":\"user\",\"content\":\"Weather?\"},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\","
       "\"name\":\"weather\",\"input\":{}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":\"72F\"}]},"
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"instruction\"}]");
   json_object *prepared = llm_openai_prepare_chat_history(history, "api.example.com#00000000",
                                                           "m");
   const char *wire = json_object_to_json_string_ext(prepared, JSON_C_TO_STRING_PLAIN);
   TEST_ASSERT_NOT_NULL(strstr(wire, "\"role\":\"tool\""));
   TEST_ASSERT_NOT_NULL(strstr(wire, "{\"role\":\"user\",\"content\":\"[Operator note] D\"}"));
   json_object_put(prepared);
   json_object_put(history);
}

/* A history whose prefix records the tag, then Q, a directive, A. */
static json_object *history_with_directive(void) {
   json_object *history = json_object_new_array();
   json_object *prefix = msg("system", "P");
   json_object_object_add(prefix, "_kind", json_object_new_string("prefix"));
   json_object_object_add(prefix, "_in_force", json_tokener_parse("{\"tag\":\"dawn-feed\"}"));
   json_object_array_add(history, prefix);
   json_object_array_add(history, msg("user", "Q"));
   json_object *directive = msg("system", "Room=Kitchen.");
   json_object_object_add(directive, "_kind", json_object_new_string("directive"));
   json_object_array_add(history, directive);
   json_object_array_add(history, msg("assistant", "A"));
   return history;
}

/* OpenAI's own endpoint takes a directive as a system message where it sits. */
static void test_openai_gets_directions_as_system_messages(void) {
   json_object *history = history_with_directive();
   json_object *prepared = llm_openai_prepare_chat_history(history,
                                                           "api.openai.com/abcdef#00000000",
                                                           "gpt-5.6");
   TEST_ASSERT_NOT_NULL(prepared);
   TEST_ASSERT_EQUAL_INT(4, (int)json_object_array_length(prepared));
   json_object *d = json_object_array_get_idx(prepared, 2);
   TEST_ASSERT_EQUAL_STRING("system", json_object_get_string(json_object_object_get(d, "role")));
   TEST_ASSERT_EQUAL_STRING("Room=Kitchen.",
                            json_object_get_string(json_object_object_get(d, "content")));
   json_object_put(prepared);
   json_object_put(history);
}

/* Any other server gets it as a note carrying the conversation's tag. */
static void test_other_servers_get_a_tagged_note(void) {
   json_object *history = history_with_directive();
   json_object *prepared = llm_openai_prepare_chat_history(history, "openrouter.ai/x#00000000",
                                                           "some/model");
   TEST_ASSERT_NOT_NULL(prepared);
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(prepared));
   const char *q = json_object_get_string(
       json_object_object_get(json_object_array_get_idx(prepared, 1), "content"));
   TEST_ASSERT_NOT_NULL(strstr(q, "[Operator note dawn-feed] Room=Kitchen."));
   json_object_put(prepared);
   json_object_put(history);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_openai_gets_directions_as_system_messages);
   RUN_TEST(test_other_servers_get_a_tagged_note);
   RUN_TEST(test_note_after_claude_results);
   RUN_TEST(test_request_context_for_chat);
   RUN_TEST(test_internal_keys_never_go_on_the_wire);
   RUN_TEST(test_claude_tool_turn_leaves_its_thinking_out);
   RUN_TEST(test_signed_call_goes_back_to_its_endpoint_only);
   RUN_TEST(test_claude_turn_with_blocks_keeps_its_result);
   RUN_TEST(test_partial_turn_keeps_its_reasoning);
   return UNITY_END();
}
