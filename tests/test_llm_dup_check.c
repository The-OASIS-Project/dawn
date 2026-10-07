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
 * Tests for llm_tools_is_duplicate_call() turn-scoping (both provider shapes).
 * The duplicate-tool-call guard must catch a tool repeating itself WITHIN the
 * current turn (a runaway loop) but NOT block a repeat from an earlier user turn
 * (the user asked again, or the data changed between turns — e.g. the user says
 * "reread that doc" after editing it).  The turn boundary is the last real user
 * message; Claude tool-result messages are role "user" too and must not count.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>

#include "llm/llm_tools.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ---- builders ---------------------------------------------------------- */

static struct json_object *user_text(const char *text) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("user"));
   json_object_object_add(m, "content", json_object_new_string(text));
   return m;
}

/* Claude assistant message carrying one tool_use block; reuses @p input. */
static struct json_object *claude_tool_use(const char *name, struct json_object *input) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   struct json_object *content = json_object_new_array();
   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("tool_use"));
   json_object_object_add(block, "id", json_object_new_string("t1"));
   json_object_object_add(block, "name", json_object_new_string(name));
   json_object_object_add(block, "input", input);
   json_object_array_add(content, block);
   json_object_object_add(m, "content", content);
   return m;
}

/* Claude tool-result message (role "user" — NOT a turn boundary). */
static struct json_object *claude_tool_result(const char *content_str) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("user"));
   struct json_object *content = json_object_new_array();
   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("tool_result"));
   json_object_object_add(block, "tool_use_id", json_object_new_string("t1"));
   json_object_object_add(block, "content", json_object_new_string(content_str));
   json_object_array_add(content, block);
   json_object_object_add(m, "content", content);
   return m;
}

static struct json_object *claude_text(const char *text) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   struct json_object *content = json_object_new_array();
   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("text"));
   json_object_object_add(block, "text", json_object_new_string(text));
   json_object_array_add(content, block);
   json_object_object_add(m, "content", content);
   return m;
}

static struct json_object *openai_tool_call(const char *name, const char *args_str) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(m, "content", json_object_new_string(""));
   struct json_object *tcs = json_object_new_array();
   struct json_object *tc = json_object_new_object();
   json_object_object_add(tc, "id", json_object_new_string("c1"));
   json_object_object_add(tc, "type", json_object_new_string("function"));
   struct json_object *func = json_object_new_object();
   json_object_object_add(func, "name", json_object_new_string(name));
   json_object_object_add(func, "arguments", json_object_new_string(args_str));
   json_object_object_add(tc, "function", func);
   json_object_array_add(tcs, tc);
   json_object_object_add(m, "tool_calls", tcs);
   return m;
}

static struct json_object *openai_tool_result(const char *content) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("tool"));
   json_object_object_add(m, "tool_call_id", json_object_new_string("c1"));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

/* Build the stored Claude {"document":"Open Sauce 2026"} input, and the arguments as
 * the model sends them on the next call: its own spacing, not json-c's. */
static struct json_object *doc_input(char **args_out) {
   struct json_object *input = json_object_new_object();
   json_object_object_add(input, "document", json_object_new_string("Open Sauce 2026"));
   *args_out = strdup("{\"document\":\"Open Sauce 2026\"}");
   return input;
}

/* ---- tests ------------------------------------------------------------- */

/* The same tool call from a PRIOR turn must not be flagged — the user can ask
 * to repeat it (the headline "reread that doc" regression). */
static void test_claude_repeat_across_turns_allowed(void) {
   char *args = NULL;
   struct json_object *input = doc_input(&args);
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, user_text("read the doc"));
   json_object_array_add(history, claude_tool_use("document_read", input)); /* prior turn */
   json_object_array_add(history, claude_tool_result("...doc text..."));
   json_object_array_add(history, claude_text("here it is"));
   json_object_array_add(history, user_text("reread it")); /* current turn */

   TEST_ASSERT_FALSE(
       llm_tools_is_duplicate_call(history, "document_read", args, LLM_HISTORY_CLAUDE));

   free(args);
   json_object_put(history);
}

/* The same tool call repeated WITHIN the current turn is a loop — still caught. */
static void test_claude_repeat_within_turn_blocked(void) {
   char *args = NULL;
   struct json_object *input = doc_input(&args);
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, user_text("read the doc")); /* current turn start */
   json_object_array_add(history, claude_tool_use("document_read", input));
   json_object_array_add(history, claude_tool_result("...doc text..."));

   TEST_ASSERT_TRUE(
       llm_tools_is_duplicate_call(history, "document_read", args, LLM_HISTORY_CLAUDE));

   free(args);
   json_object_put(history);
}

/* A call with no arguments repeats one whose stored input is an empty object. */
static void test_claude_repeat_no_args_blocked(void) {
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, user_text("what time is it")); /* current turn start */
   json_object_array_add(history, claude_tool_use("datetime", json_object_new_object()));
   json_object_array_add(history, claude_tool_result("...time..."));

   TEST_ASSERT_TRUE(llm_tools_is_duplicate_call(history, "datetime", "", LLM_HISTORY_CLAUDE));
   TEST_ASSERT_TRUE(llm_tools_is_duplicate_call(history, "datetime", NULL, LLM_HISTORY_CLAUDE));

   json_object_put(history);
}

/* Different arguments are a different call. */
static void test_claude_different_args_allowed(void) {
   char *args = NULL;
   struct json_object *input = doc_input(&args);
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, user_text("read the doc")); /* current turn start */
   json_object_array_add(history, claude_tool_use("document_read", input));
   json_object_array_add(history, claude_tool_result("...doc text..."));

   TEST_ASSERT_FALSE(llm_tools_is_duplicate_call(
       history, "document_read", "{\"document\": \"Other Doc\"}", LLM_HISTORY_CLAUDE));

   free(args);
   json_object_put(history);
}

static void test_openai_repeat_across_turns_allowed(void) {
   const char *args = "{\"document\":\"Open Sauce 2026\"}";
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, user_text("read the doc"));
   json_object_array_add(history, openai_tool_call("document_read", args)); /* prior turn */
   json_object_array_add(history, openai_tool_result("...doc text..."));
   json_object_array_add(history, user_text("reread it")); /* current turn */

   TEST_ASSERT_FALSE(
       llm_tools_is_duplicate_call(history, "document_read", args, LLM_HISTORY_OPENAI));

   json_object_put(history);
}

static void test_openai_repeat_within_turn_blocked(void) {
   const char *args = "{\"document\":\"Open Sauce 2026\"}";
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, user_text("read the doc")); /* current turn start */
   json_object_array_add(history, openai_tool_call("document_read", args));
   json_object_array_add(history, openai_tool_result("...doc text..."));

   TEST_ASSERT_TRUE(
       llm_tools_is_duplicate_call(history, "document_read", args, LLM_HISTORY_OPENAI));

   json_object_put(history);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_claude_repeat_across_turns_allowed);
   RUN_TEST(test_claude_repeat_within_turn_blocked);
   RUN_TEST(test_claude_repeat_no_args_blocked);
   RUN_TEST(test_claude_different_args_allowed);
   RUN_TEST(test_openai_repeat_across_turns_allowed);
   RUN_TEST(test_openai_repeat_within_turn_blocked);
   return UNITY_END();
}
