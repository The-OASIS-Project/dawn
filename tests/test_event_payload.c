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
 * Unit tests for the conversation-event payload redactor (src/core/event_payload.c).
 * Focus: redaction is CONTENT-based — a real credential in an arg is scrubbed by
 * key name or value shape, while benign tool state (device readings, entity ids)
 * stays readable, including for tools that carry TOOL_CAP_SECRETS in config.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/event_payload.h"
#include "unity.h"

/* event_payload.c reads g_config.jobs.event_chunk_cap via payload_cap(). */
dawn_config_t g_config;

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.jobs.event_chunk_cap = 16384;
}

void tearDown(void) {
}

/* Fetch a nested string field from a payload: root -> @container -> @field. */
static void assert_field_equals(const char *payload,
                                const char *container,
                                const char *field,
                                const char *expected) {
   struct json_object *root = json_tokener_parse(payload);
   TEST_ASSERT_NOT_NULL(root);
   struct json_object *c = NULL, *f = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, container, &c));
   TEST_ASSERT_TRUE(json_object_object_get_ex(c, field, &f));
   TEST_ASSERT_EQUAL_STRING(expected, json_object_get_string(f));
   json_object_put(root);
}

/* A tool_result body is stored readable — no whole-result redaction.  This is
 * the home_assistant / calendar case: their secret is a config credential, not
 * echoed in the response, so device/event state must survive. */
static void test_result_body_is_readable(void) {
   char *p = event_payload_tool_result("home_assistant", "living room light is on, 72F");
   TEST_ASSERT_NOT_NULL(p);
   struct json_object *root = json_tokener_parse(p);
   struct json_object *tool = NULL, *result = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "tool", &tool));
   TEST_ASSERT_EQUAL_STRING("home_assistant", json_object_get_string(tool));
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "result", &result));
   TEST_ASSERT_EQUAL_STRING("living room light is on, 72F", json_object_get_string(result));
   json_object_put(root);
   free(p);
}

/* A sensitive KEY NAME still redacts its value; a benign sibling stays readable. */
static void test_sensitive_key_is_redacted(void) {
   char *p = event_payload_tool_call("some_tool",
                                     "{\"api_key\":\"whatever\",\"entity\":\"light.kitchen\"}");
   TEST_ASSERT_NOT_NULL(p);
   assert_field_equals(p, "args", "api_key", EVENT_REDACTED_MARKER);
   assert_field_equals(p, "args", "entity", "light.kitchen");
   free(p);
}

/* A secret-SHAPED value in an innocuously named field still redacts. */
static void test_secret_shaped_value_is_redacted(void) {
   char *p = event_payload_tool_call("some_tool", "{\"note\":\"sk-abcdef0123456789\"}");
   TEST_ASSERT_NOT_NULL(p);
   assert_field_equals(p, "args", "note", EVENT_REDACTED_MARKER);
   free(p);
}

/* home_assistant args are benign (entity ids, actions) and must NOT be
 * whole-redacted just because the tool declares TOOL_CAP_SECRETS in config. */
static void test_home_assistant_args_are_readable(void) {
   char *p = event_payload_tool_call("home_assistant",
                                     "{\"action\":\"turn_on\",\"entity_id\":\"light.kitchen\"}");
   TEST_ASSERT_NOT_NULL(p);
   assert_field_equals(p, "args", "action", "turn_on");
   assert_field_equals(p, "args", "entity_id", "light.kitchen");
   free(p);
}

/* A secret nested in an ARRAY under a benign key still redacts — the array walker
 * mirrors the object walker (no blind spot for e.g. an auth header list). */
static void test_array_nested_secret_is_redacted(void) {
   char *p = event_payload_tool_call(
       "fetch",
       "{\"headers\":[{\"name\":\"Authorization\",\"value\":\"Bearer sk-abcdef0123456789\"}]}");
   TEST_ASSERT_NOT_NULL(p);
   struct json_object *root = json_tokener_parse(p);
   TEST_ASSERT_NOT_NULL(root);
   struct json_object *args = NULL, *headers = NULL, *h0 = NULL, *val = NULL, *nm = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "args", &args));
   TEST_ASSERT_TRUE(json_object_object_get_ex(args, "headers", &headers));
   TEST_ASSERT_TRUE(json_object_is_type(headers, json_type_array));
   h0 = json_object_array_get_idx(headers, 0);
   TEST_ASSERT_NOT_NULL(h0);
   TEST_ASSERT_TRUE(json_object_object_get_ex(h0, "value", &val));
   TEST_ASSERT_EQUAL_STRING(EVENT_REDACTED_MARKER, json_object_get_string(val));
   TEST_ASSERT_TRUE(json_object_object_get_ex(h0, "name", &nm)); /* benign sibling survives */
   TEST_ASSERT_EQUAL_STRING("Authorization", json_object_get_string(nm));
   json_object_put(root);
   free(p);
}

/* A non-UTF-8 byte in tool output must not survive into the WS text frame (it would
 * be an invalid frame that persists and re-wedges every replay). */
static void test_non_utf8_result_is_sanitized(void) {
   /* Split literal so \xff doesn't greedily absorb the following 'b' as a hex digit. */
   char *p = event_payload_tool_result("search", "clean\xff"
                                                 "bytes");
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_NULL(strchr(p, (char)0xFF)); /* invalid byte gone */
   struct json_object *root = json_tokener_parse(p);
   TEST_ASSERT_NOT_NULL(root); /* still valid, parseable JSON */
   struct json_object *result = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "result", &result));
   TEST_ASSERT_EQUAL_STRING("clean?bytes", json_object_get_string(result));
   json_object_put(root);
   free(p);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_result_body_is_readable);
   RUN_TEST(test_sensitive_key_is_redacted);
   RUN_TEST(test_secret_shaped_value_is_redacted);
   RUN_TEST(test_home_assistant_args_are_readable);
   RUN_TEST(test_array_nested_secret_is_redacted);
   RUN_TEST(test_non_utf8_result_is_sanitized);
   return UNITY_END();
}
