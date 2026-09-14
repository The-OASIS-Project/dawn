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
 * Unit tests for the note-extraction guard.  Covers both provider history
 * shapes (OpenAI tool_calls / Claude tool_use), filed-body redaction in the
 * originating user message + the tool-call arguments, document_read result-echo
 * elision, whitespace-normalized matching, the short-body floor, the
 * config/no-op pass-throughs, and (the headline assertion) that an unrelated
 * fact in the same window survives redaction.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "memory/memory_note_guard.h"
#include "unity.h"

/* log_message comes from dawn_common; g_config is ours to drive the flag. */
dawn_config_t g_config;

/* A distinctive reference body, comfortably above the 16-char scan floor.  No
 * real names (placeholders per project rule). */
#define BIO                                                                                   \
   "Jonathan Smith is a veteran systems engineer and the founder of Dawn Robotics, with two " \
   "decades of embedded experience across robotics and voice."
#define DENVER "Also, I am moving to Denver next month."

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.note_extraction_guard = true;
}

void tearDown(void) {
}

/* ---- builders ---------------------------------------------------------- */

static struct json_object *msg_user_text(const char *text) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("user"));
   json_object_object_add(m, "content", json_object_new_string(text));
   return m;
}

/* OpenAI assistant message invoking one tool, arguments as a JSON string. */
static struct json_object *msg_openai_tool_call(const char *id,
                                                const char *name,
                                                const char *args_json) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(m, "content", json_object_new_string(""));
   struct json_object *tcs = json_object_new_array();
   struct json_object *tc = json_object_new_object();
   json_object_object_add(tc, "id", json_object_new_string(id));
   json_object_object_add(tc, "type", json_object_new_string("function"));
   struct json_object *func = json_object_new_object();
   json_object_object_add(func, "name", json_object_new_string(name));
   json_object_object_add(func, "arguments", json_object_new_string(args_json));
   json_object_object_add(tc, "function", func);
   json_object_array_add(tcs, tc);
   json_object_object_add(m, "tool_calls", tcs);
   return m;
}

/* OpenAI tool result message. */
static struct json_object *msg_openai_tool_result(const char *call_id, const char *content) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("tool"));
   json_object_object_add(m, "tool_call_id", json_object_new_string(call_id));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

/* Claude assistant message with one tool_use block, input as an object. */
static struct json_object *msg_claude_tool_use(const char *id,
                                               const char *name,
                                               struct json_object *input) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   struct json_object *content = json_object_new_array();
   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("tool_use"));
   json_object_object_add(block, "id", json_object_new_string(id));
   json_object_object_add(block, "name", json_object_new_string(name));
   json_object_object_add(block, "input", input);
   json_object_array_add(content, block);
   json_object_object_add(m, "content", content);
   return m;
}

/* Claude tool result (role:user, content array of tool_result blocks). */
static struct json_object *msg_claude_tool_result(const char *use_id, const char *content_str) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("user"));
   struct json_object *content = json_object_new_array();
   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("tool_result"));
   json_object_object_add(block, "tool_use_id", json_object_new_string(use_id));
   json_object_object_add(block, "content", json_object_new_string(content_str));
   json_object_array_add(content, block);
   json_object_object_add(m, "content", content);
   return m;
}

static struct json_object *save_note_args_obj(const char *label, const char *text) {
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "action", json_object_new_string("save_note"));
   json_object_object_add(o, "label", json_object_new_string(label));
   json_object_object_add(o, "text", json_object_new_string(text));
   return o;
}

static char *save_note_args_json(const char *label, const char *text) {
   struct json_object *o = save_note_args_obj(label, text);
   char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
   json_object_put(o);
   return s;
}

/* edit args: `change` is a JSON object carried as a STRING (the declared shape). */
static char *edit_args_json(const char *label, const char *find, const char *replace) {
   struct json_object *change = json_object_new_object();
   json_object_object_add(change, "find", json_object_new_string(find));
   json_object_object_add(change, "replace", json_object_new_string(replace));
   char *change_str = strdup(json_object_to_json_string_ext(change, JSON_C_TO_STRING_PLAIN));
   json_object_put(change);

   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "action", json_object_new_string("edit"));
   json_object_object_add(o, "label", json_object_new_string(label));
   json_object_object_add(o, "change", json_object_new_string(change_str));
   free(change_str);
   char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
   json_object_put(o);
   return s;
}

static __attribute__((unused)) char *append_args_json(const char *label, const char *text) {
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "action", json_object_new_string("append"));
   json_object_object_add(o, "label", json_object_new_string(label));
   json_object_object_add(o, "text", json_object_new_string(text));
   char *s = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
   json_object_put(o);
   return s;
}

/* Redact one message and return its serialized JSON (caller frees). */
static char *redact_to_string(const memory_note_guard_t *g, struct json_object *msg) {
   struct json_object *out = memory_note_guard_redact(g, msg);
   char *s = strdup(json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));
   json_object_put(out);
   return s;
}

/* ---- tests ------------------------------------------------------------- */

static void test_openai_save_note_redacts_body_and_args(void) {
   char *args = save_note_args_json("Public Bio", BIO);
   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text("Please file this as my Public Bio: " BIO);
   struct json_object *a1 = msg_openai_tool_call("call_1", "document_manage", args);
   struct json_object *t1 = msg_openai_tool_result("call_1", "Saved note 'Public Bio'.");
   struct json_object *u2 = msg_user_text(DENVER);
   json_object_array_add(history, json_object_get(u1));
   json_object_array_add(history, json_object_get(a1));
   json_object_array_add(history, json_object_get(t1));
   json_object_array_add(history, json_object_get(u2));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);
   TEST_ASSERT_TRUE(memory_note_guard_active(g));

   char *ru1 = redact_to_string(g, u1);
   char *ra1 = redact_to_string(g, a1);
   char *ru2 = redact_to_string(g, u2);

   /* The bio body is gone from the user message; the file intent survives. */
   TEST_ASSERT_NULL(strstr(ru1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "filed to notes store"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "Public Bio"));
   /* Tool-call arguments text redacted. */
   TEST_ASSERT_NULL(strstr(ra1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ra1, "filed to notes store"));
   /* The unrelated fact in the same window is untouched. */
   TEST_ASSERT_NOT_NULL(strstr(ru2, "Denver"));

   free(ru1);
   free(ra1);
   free(ru2);
   free(args);
   json_object_put(u1);
   json_object_put(a1);
   json_object_put(t1);
   json_object_put(u2);
   json_object_put(history);
   memory_note_guard_free(g);
}

static void test_claude_save_note_redacts_body_and_input(void) {
   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text("Save this as my Public Bio: " BIO);
   struct json_object *a1 = msg_claude_tool_use("toolu_1", "document_manage",
                                                save_note_args_obj("Public Bio", BIO));
   struct json_object *u2 = msg_user_text(DENVER);
   json_object_array_add(history, json_object_get(u1));
   json_object_array_add(history, json_object_get(a1));
   json_object_array_add(history, json_object_get(u2));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);

   char *ru1 = redact_to_string(g, u1);
   char *ra1 = redact_to_string(g, a1);
   char *ru2 = redact_to_string(g, u2);

   TEST_ASSERT_NULL(strstr(ru1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "filed to notes store"));
   TEST_ASSERT_NULL(strstr(ra1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ra1, "filed to notes store"));
   TEST_ASSERT_NOT_NULL(strstr(ru2, "Denver"));

   free(ru1);
   free(ra1);
   free(ru2);
   json_object_put(u1);
   json_object_put(a1);
   json_object_put(u2);
   json_object_put(history);
   memory_note_guard_free(g);
}

static void test_openai_document_read_result_elided(void) {
   struct json_object *history = json_object_new_array();
   struct json_object *a1 = msg_openai_tool_call("call_9", "document_read",
                                                 "{\"name\":\"Public Bio\"}");
   struct json_object *t1 = msg_openai_tool_result("call_9", BIO);
   json_object_array_add(history, json_object_get(a1));
   json_object_array_add(history, json_object_get(t1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);

   char *rt1 = redact_to_string(g, t1);
   TEST_ASSERT_NULL(strstr(rt1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(rt1, "content elided"));

   free(rt1);
   json_object_put(a1);
   json_object_put(t1);
   json_object_put(history);
   memory_note_guard_free(g);
}

static void test_claude_document_read_result_elided(void) {
   struct json_object *history = json_object_new_array();
   struct json_object *input = json_object_new_object();
   json_object_object_add(input, "name", json_object_new_string("Public Bio"));
   struct json_object *a1 = msg_claude_tool_use("toolu_9", "document_search", input);
   struct json_object *u1 = msg_claude_tool_result("toolu_9", "Result: " BIO);
   json_object_array_add(history, json_object_get(a1));
   json_object_array_add(history, json_object_get(u1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);

   char *ru1 = redact_to_string(g, u1);
   TEST_ASSERT_NULL(strstr(ru1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "content elided"));

   free(ru1);
   json_object_put(a1);
   json_object_put(u1);
   json_object_put(history);
   memory_note_guard_free(g);
}

/* The filed body appears in the user turn with different whitespace (newlines,
 * tabs, doubled spaces) than the canonical filed text — still redacted. */
static void test_whitespace_normalized_match(void) {
   char *args = save_note_args_json("Public Bio", BIO);
   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text(
       "File this:\n\tJonathan   Smith is a veteran   systems engineer and the "
       "founder\nof Dawn Robotics,  with two decades of embedded experience across "
       "robotics\tand voice.");
   struct json_object *a1 = msg_openai_tool_call("call_1", "document_manage", args);
   json_object_array_add(history, json_object_get(u1));
   json_object_array_add(history, json_object_get(a1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);

   char *ru1 = redact_to_string(g, u1);
   TEST_ASSERT_NULL(strstr(ru1, "veteran"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "filed to notes store"));

   free(ru1);
   free(args);
   json_object_put(u1);
   json_object_put(a1);
   json_object_put(history);
   memory_note_guard_free(g);
}

/* A short filed body is below the scan floor, so it is NOT hunted inside other
 * messages (the save-call's own argument is still redacted structurally). */
static void test_short_body_not_scanned_in_user_message(void) {
   char *args = save_note_args_json("Tag", "go");
   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text("Remember to go to the store later today.");
   struct json_object *a1 = msg_openai_tool_call("call_1", "document_manage", args);
   json_object_array_add(history, json_object_get(u1));
   json_object_array_add(history, json_object_get(a1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g); /* still active: the read floor doesn't gate collection */

   char *ru1 = redact_to_string(g, u1);
   /* "go" stays in the user message — short bodies are not hunted. */
   TEST_ASSERT_NOT_NULL(strstr(ru1, "go to the store"));
   TEST_ASSERT_NULL(strstr(ru1, "filed to notes store"));

   free(ru1);
   free(args);
   json_object_put(u1);
   json_object_put(a1);
   json_object_put(history);
   memory_note_guard_free(g);
}

static void test_inert_without_document_tools(void) {
   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text("What is the capital of France?");
   json_object_array_add(history, json_object_get(u1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NULL(g); /* nothing filed → inert (NULL) */

   /* NULL guard is a pass-through. */
   char *ru1 = redact_to_string(NULL, u1);
   TEST_ASSERT_NOT_NULL(strstr(ru1, "capital of France"));

   free(ru1);
   json_object_put(u1);
   json_object_put(history);
}

static void test_disabled_by_config(void) {
   g_config.memory.note_extraction_guard = false;
   char *args = save_note_args_json("Public Bio", BIO);
   struct json_object *history = json_object_new_array();
   struct json_object *a1 = msg_openai_tool_call("call_1", "document_manage", args);
   json_object_array_add(history, json_object_get(a1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NULL(g); /* disabled → no guard */

   free(args);
   json_object_put(a1);
   json_object_put(history);
}

/* B1a: an 'edit' carries the new note content in change.replace — that content
 * must be redacted from history too, or it gets re-mined into memory. */
static void test_openai_edit_redacts_replace_body(void) {
   char *args = edit_args_json("Public Bio", "old summary", BIO);
   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text("Update my Public Bio to read: " BIO);
   struct json_object *a1 = msg_openai_tool_call("call_1", "document_manage", args);
   json_object_array_add(history, json_object_get(u1));
   json_object_array_add(history, json_object_get(a1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);
   TEST_ASSERT_TRUE(memory_note_guard_active(g));

   char *ru1 = redact_to_string(g, u1);
   char *ra1 = redact_to_string(g, a1);
   TEST_ASSERT_NULL(strstr(ru1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "filed to notes store"));
   TEST_ASSERT_NULL(strstr(ra1, "veteran systems engineer"));

   free(ru1);
   free(ra1);
   free(args);
   json_object_put(u1);
   json_object_put(a1);
   json_object_put(history);
   memory_note_guard_free(g);
}

/* B1a: an 'append' carries new note content in `text` (Claude shape here). */
static void test_claude_append_redacts_text_body(void) {
   struct json_object *input = json_object_new_object();
   json_object_object_add(input, "action", json_object_new_string("append"));
   json_object_object_add(input, "label", json_object_new_string("Public Bio"));
   json_object_object_add(input, "text", json_object_new_string(BIO));

   struct json_object *history = json_object_new_array();
   struct json_object *u1 = msg_user_text("Add this to my Public Bio: " BIO);
   struct json_object *a1 = msg_claude_tool_use("toolu_1", "document_manage", input);
   json_object_array_add(history, json_object_get(u1));
   json_object_array_add(history, json_object_get(a1));

   memory_note_guard_t *g = memory_note_guard_create(history);
   TEST_ASSERT_NOT_NULL(g);
   TEST_ASSERT_TRUE(memory_note_guard_active(g));

   char *ru1 = redact_to_string(g, u1);
   char *ra1 = redact_to_string(g, a1);
   TEST_ASSERT_NULL(strstr(ru1, "veteran systems engineer"));
   TEST_ASSERT_NOT_NULL(strstr(ru1, "filed to notes store"));
   TEST_ASSERT_NULL(strstr(ra1, "veteran systems engineer"));

   free(ru1);
   free(ra1);
   json_object_put(u1);
   json_object_put(a1);
   json_object_put(history);
   memory_note_guard_free(g);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_openai_save_note_redacts_body_and_args);
   RUN_TEST(test_claude_save_note_redacts_body_and_input);
   RUN_TEST(test_openai_document_read_result_elided);
   RUN_TEST(test_claude_document_read_result_elided);
   RUN_TEST(test_whitespace_normalized_match);
   RUN_TEST(test_short_body_not_scanned_in_user_message);
   RUN_TEST(test_inert_without_document_tools);
   RUN_TEST(test_disabled_by_config);
   RUN_TEST(test_openai_edit_redacts_replace_body);
   RUN_TEST(test_claude_append_redacts_text_body);
   return UNITY_END();
}
