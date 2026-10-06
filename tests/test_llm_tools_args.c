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
 * Tool-call arguments that don't fit are flagged, on every path: the
 * streamed append (OpenAI, Responses and Claude deltas), the replace
 * (Gemini chunks, the Responses final arguments) and the non-streamed
 * parses.  A flagged call is refused, so a cut argument never reaches a tool.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_tools.h"
#include "unity.h"

static char s_buf[LLM_TOOLS_ARGS_LEN];
static tool_call_list_t s_calls;

void setUp(void) {
   memset(s_buf, 0, sizeof(s_buf));
   memset(&s_calls, 0, sizeof(s_calls));
}

void tearDown(void) {
}

/* A string of n 'x' characters (caller frees). */
static char *filler(size_t n) {
   char *s = malloc(n + 1);
   TEST_ASSERT_NOT_NULL(s);
   memset(s, 'x', n);
   s[n] = '\0';
   return s;
}

static void test_deltas_that_fit_are_joined(void) {
   size_t len = 0;
   bool cut = false;
   llm_tools_args_append(s_buf, &len, &cut, "{\"a\":", 5, false);
   llm_tools_args_append(s_buf, &len, &cut, "1}", 2, false);
   TEST_ASSERT_EQUAL_STRING("{\"a\":1}", s_buf);
   TEST_ASSERT_EQUAL_size_t(7, len);
   TEST_ASSERT_FALSE(cut);
}

static void test_a_delta_past_the_end_is_flagged(void) {
   size_t len = 0;
   bool cut = false;
   char *big = filler(LLM_TOOLS_ARGS_LEN - 10);
   llm_tools_args_append(s_buf, &len, &cut, big, strlen(big), false);
   TEST_ASSERT_FALSE(cut);
   llm_tools_args_append(s_buf, &len, &cut, "0123456789abc", 13, false);
   TEST_ASSERT_TRUE(cut);
   TEST_ASSERT_EQUAL_size_t(LLM_TOOLS_ARGS_LEN - 1, len);
   TEST_ASSERT_EQUAL_size_t(LLM_TOOLS_ARGS_LEN - 1, strlen(s_buf));
   /* Later deltas don't clear it. */
   llm_tools_args_append(s_buf, &len, &cut, "}", 1, false);
   TEST_ASSERT_TRUE(cut);
   free(big);
}

static void test_a_replace_that_fits_clears_the_flag(void) {
   size_t len = 0;
   bool cut = false;
   char *big = filler(LLM_TOOLS_ARGS_LEN);
   llm_tools_args_append(s_buf, &len, &cut, big, strlen(big), true);
   TEST_ASSERT_TRUE(cut);
   llm_tools_args_append(s_buf, &len, &cut, "{}", 2, true);
   TEST_ASSERT_FALSE(cut);
   TEST_ASSERT_EQUAL_STRING("{}", s_buf);
   free(big);
}

/* The whole-arguments replace a non-streamed reply (or a final event) gives. */
static void test_full_arguments_too_long_are_flagged(void) {
   size_t len = 0;
   bool cut = false;
   char *big = filler(LLM_TOOLS_ARGS_LEN - 1);
   llm_tools_args_append(s_buf, &len, &cut, big, strlen(big), true);
   TEST_ASSERT_FALSE(cut); /* exactly the room there is */
   free(big);
   big = filler(LLM_TOOLS_ARGS_LEN);
   llm_tools_args_append(s_buf, &len, &cut, big, strlen(big), true);
   TEST_ASSERT_TRUE(cut);
   free(big);
}

/* A cut never ends inside a UTF-8 character. */
static void test_a_cut_lands_on_a_whole_character(void) {
   size_t len = 0;
   bool cut = false;
   char *big = filler(LLM_TOOLS_ARGS_LEN - 3);
   llm_tools_args_append(s_buf, &len, &cut, big, strlen(big), false);
   /* Two bytes of room; the three-byte character after "é" doesn't fit. */
   const char tail[] = "\xC3\xA9\xE2\x82\xAC";
   llm_tools_args_append(s_buf, &len, &cut, tail, strlen(tail), false);
   TEST_ASSERT_TRUE(cut);
   TEST_ASSERT_EQUAL_size_t(LLM_TOOLS_ARGS_LEN - 1, len);
   TEST_ASSERT_EQUAL_HEX8(0xA9, (unsigned char)s_buf[len - 1]);
   free(big);

   /* Room for only part of a character: none of it is copied. */
   memset(s_buf, 0, sizeof(s_buf));
   len = 0;
   cut = false;
   big = filler(LLM_TOOLS_ARGS_LEN - 2);
   llm_tools_args_append(s_buf, &len, &cut, big, strlen(big), false);
   llm_tools_args_append(s_buf, &len, &cut, "\xE2\x82\xAC", 3, false);
   TEST_ASSERT_TRUE(cut);
   TEST_ASSERT_EQUAL_size_t(LLM_TOOLS_ARGS_LEN - 2, len);
   free(big);
}

/* A length already at the buffer's size only flags; it never writes. */
static void test_a_full_length_only_flags(void) {
   size_t len = LLM_TOOLS_ARGS_LEN;
   bool cut = false;
   llm_tools_args_append(s_buf, &len, &cut, "x", 1, false);
   TEST_ASSERT_TRUE(cut);
   TEST_ASSERT_EQUAL_size_t(LLM_TOOLS_ARGS_LEN, len);
   TEST_ASSERT_EQUAL_CHAR('\0', s_buf[0]);
}

static void test_openai_parse_flags_long_arguments(void) {
   char *big = filler(LLM_TOOLS_ARGS_LEN + 100);
   json_object *resp = json_object_new_object();
   json_object *choices = json_object_new_array();
   json_object *choice = json_object_new_object();
   json_object *msg = json_object_new_object();
   json_object *calls = json_object_new_array();
   json_object *call = json_object_new_object();
   json_object *fn = json_object_new_object();
   json_object_object_add(fn, "name", json_object_new_string("note"));
   json_object_object_add(fn, "arguments", json_object_new_string(big));
   json_object_object_add(call, "id", json_object_new_string("call_1"));
   json_object_object_add(call, "type", json_object_new_string("function"));
   json_object_object_add(call, "function", fn);
   json_object_array_add(calls, call);
   json_object_object_add(msg, "tool_calls", calls);
   json_object_object_add(choice, "message", msg);
   json_object_array_add(choices, choice);
   json_object_object_add(resp, "choices", choices);

   TEST_ASSERT_EQUAL_INT(0, llm_tools_parse_openai_response(resp, &s_calls));
   TEST_ASSERT_EQUAL_INT(1, s_calls.count);
   TEST_ASSERT_TRUE(s_calls.calls[0].args_truncated);
   json_object_put(resp);
   free(big);
}

static void test_claude_parse_flags_long_input(void) {
   char *big = filler(LLM_TOOLS_ARGS_LEN + 100);
   json_object *resp = json_object_new_object();
   json_object *content = json_object_new_array();
   json_object *block = json_object_new_object();
   json_object *input = json_object_new_object();
   json_object_object_add(input, "text", json_object_new_string(big));
   json_object_object_add(block, "type", json_object_new_string("tool_use"));
   json_object_object_add(block, "id", json_object_new_string("toolu_1"));
   json_object_object_add(block, "name", json_object_new_string("note"));
   json_object_object_add(block, "input", input);
   json_object_array_add(content, block);
   json_object_object_add(resp, "content", content);

   TEST_ASSERT_EQUAL_INT(0, llm_tools_parse_claude_response(resp, &s_calls));
   TEST_ASSERT_TRUE(s_calls.calls[0].args_truncated);
   json_object_put(resp);
   free(big);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_deltas_that_fit_are_joined);
   RUN_TEST(test_a_delta_past_the_end_is_flagged);
   RUN_TEST(test_a_replace_that_fits_clears_the_flag);
   RUN_TEST(test_full_arguments_too_long_are_flagged);
   RUN_TEST(test_a_cut_lands_on_a_whole_character);
   RUN_TEST(test_a_full_length_only_flags);
   RUN_TEST(test_openai_parse_flags_long_arguments);
   RUN_TEST(test_claude_parse_flags_long_input);
   return UNITY_END();
}
