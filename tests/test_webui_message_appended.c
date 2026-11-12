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
 * The message_appended frame: its fields, which session a row names, and the
 * origin connection's client_ref copy.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "webui/webui_message_frame.h"

#ifdef DAWN_ENABLE_EMAIL_TOOL
#include "tools/email_service.h"
#include "webui/webui_email_client.h"

/* ---- stubs: reading a frame's email_refs, which these tests never do ---- */
bool webui_email_client_enabled(void) {
   return false;
}
int email_service_find_account_by_id(int u, int64_t a, bool e, email_account_t *out) {
   (void)u, (void)a, (void)e, (void)out;
   return EMAIL_RC_UNKNOWN_ACCOUNT;
}
int email_service_find_account_by_name(int u, const char *n, email_account_t *out) {
   (void)u, (void)n, (void)out;
   return EMAIL_RC_UNKNOWN_ACCOUNT;
}
bool session_attach_name_ok(const char *name, size_t max) {
   (void)name, (void)max;
   return false;
}
bool email_service_account_uses_lease(const email_account_t *acct) {
   (void)acct;
   return false;
}
bool email_service_message_id_ok(const char *id, bool is_imap) {
   (void)id, (void)is_imap;
   return false;
}
#endif

#define REF "{\"account_id\":3,\"subject\":\"Lunch\"}"

void setUp(void) {
}

void tearDown(void) {
}

/* The frame's payload (the frame keeps ownership). */
static json_object *payload_of(json_object *frame) {
   json_object *p = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(frame, "payload", &p));
   TEST_ASSERT_TRUE(json_object_is_type(p, json_type_object));
   return p;
}

static bool has(json_object *p, const char *key) {
   return json_object_object_get_ex(p, key, NULL);
}

static int64_t int_field(json_object *p, const char *key) {
   json_object *v = NULL;
   TEST_ASSERT_TRUE_MESSAGE(json_object_object_get_ex(p, key, &v), key);
   TEST_ASSERT_TRUE_MESSAGE(json_object_is_type(v, json_type_int), key);
   return json_object_get_int64(v);
}

static const char *str_field(json_object *p, const char *key) {
   json_object *v = NULL;
   TEST_ASSERT_TRUE_MESSAGE(json_object_object_get_ex(p, key, &v), key);
   TEST_ASSERT_TRUE_MESSAGE(json_object_is_type(v, json_type_string), key);
   return json_object_get_string(v);
}

/* The type and the always-present fields, with their values and types. */
static void test_fields(void) {
   json_object *f = webui_message_appended_frame(1006, 21547, "assistant", "hello", NULL, 4, NULL,
                                                 NULL);
   TEST_ASSERT_EQUAL_STRING("message_appended", str_field(f, "type"));
   json_object *p = payload_of(f);
   TEST_ASSERT_EQUAL_INT64(1006, int_field(p, "conversation_id"));
   TEST_ASSERT_EQUAL_INT64(21547, int_field(p, "message_id"));
   TEST_ASSERT_EQUAL_STRING("assistant", str_field(p, "role"));
   TEST_ASSERT_EQUAL_STRING("hello", str_field(p, "text"));
   TEST_ASSERT_EQUAL_INT64(4, int_field(p, "stream_id"));
   TEST_ASSERT_EQUAL_INT(5, json_object_object_length(p));
   json_object_put(f);
}

/* No role is sent as "assistant"; a given role as given; stream_id 0 is sent;
 * an unsigned stream_id above INT32_MAX stays positive. */
static void test_role_and_stream(void) {
   json_object *f = webui_message_appended_frame(1, 2, NULL, "x", NULL, 0, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("assistant", str_field(payload_of(f), "role"));
   TEST_ASSERT_EQUAL_INT64(0, int_field(payload_of(f), "stream_id"));
   json_object_put(f);
   f = webui_message_appended_frame(1, 2, "user", "x", NULL, 0xFFFFFFFFu, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("user", str_field(payload_of(f), "role"));
   TEST_ASSERT_EQUAL_INT64(4294967295LL, int_field(payload_of(f), "stream_id"));
   json_object_put(f);
}

/* session_id: present and equal with an origin (0 included), absent without. */
static void test_session_id(void) {
   session_t s = { .session_id = 7, .type = SESSION_TYPE_WEBUI };
   json_object *f = webui_message_appended_frame(1, 2, "assistant", "x", NULL, 1, &s, NULL);
   TEST_ASSERT_EQUAL_INT64(7, int_field(payload_of(f), "session_id"));
   json_object_put(f);

   session_t local = { .session_id = 0, .type = SESSION_TYPE_LOCAL };
   f = webui_message_appended_frame(1, 2, "assistant", "x", NULL, 1, &local, NULL);
   TEST_ASSERT_EQUAL_INT64(0, int_field(payload_of(f), "session_id"));
   json_object_put(f);

   f = webui_message_appended_frame(1, 2, "assistant", "x", NULL, 1, NULL, NULL);
   TEST_ASSERT_FALSE(has(payload_of(f), "session_id"));
   json_object_put(f);
}

/* A persisted row names its session unless that session is a job's. */
static void test_origin_rule(void) {
   session_t web = { .session_id = 7, .type = SESSION_TYPE_WEBUI };
   session_t job = { .session_id = 9, .type = SESSION_TYPE_JOB };
   TEST_ASSERT_EQUAL_PTR(&web, webui_message_origin(&web));
   TEST_ASSERT_NULL(webui_message_origin(&job));
   TEST_ASSERT_NULL(webui_message_origin(NULL));

   json_object *f = webui_message_appended_frame(1, 2, "assistant", "x", NULL, 1,
                                                 webui_message_origin(&job), NULL);
   TEST_ASSERT_FALSE(has(payload_of(f), "session_id"));
   json_object_put(f);
}

/* reasoning: only when non-empty. */
static void test_reasoning(void) {
   json_object *f = webui_message_appended_frame(1, 2, "assistant", "x", "why", 1, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("why", str_field(payload_of(f), "reasoning"));
   json_object_put(f);
   f = webui_message_appended_frame(1, 2, "assistant", "x", "", 1, NULL, NULL);
   TEST_ASSERT_FALSE(has(payload_of(f), "reasoning"));
   json_object_put(f);
   f = webui_message_appended_frame(1, 2, "assistant", "x", NULL, 1, NULL, NULL);
   TEST_ASSERT_FALSE(has(payload_of(f), "reasoning"));
   json_object_put(f);
}

/* email_ref: an object, on a user row holding a valid one; never on another
 * role (a NULL role included, though it is sent as "assistant"). */
static void test_email_ref(void) {
   json_object *f = webui_message_appended_frame(1, 2, "user", "x", NULL, 0, NULL, REF);
   json_object *ref = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(payload_of(f), "email_ref", &ref));
   TEST_ASSERT_TRUE(json_object_is_type(ref, json_type_object));
   TEST_ASSERT_EQUAL_STRING("Lunch", str_field(ref, "subject"));
   json_object_put(f);

   const char *roles[] = { "assistant", NULL };
   for (size_t i = 0; i < 2; i++) {
      f = webui_message_appended_frame(1, 2, roles[i], "x", NULL, 0, NULL, REF);
      TEST_ASSERT_FALSE(has(payload_of(f), "email_ref"));
      json_object_put(f);
   }
   f = webui_message_appended_frame(1, 2, "user", "x", NULL, 0, NULL, "not json");
   TEST_ASSERT_FALSE(has(payload_of(f), "email_ref"));
   json_object_put(f);
   f = webui_message_appended_frame(1, 2, "user", "x", NULL, 0, NULL, "");
   TEST_ASSERT_FALSE(has(payload_of(f), "email_ref"));
   json_object_put(f);
}

/* The origin's copy carries client_ref; the frame every other connection
 * gets doesn't, before or after. */
static void test_client_ref(void) {
   session_t s = { .session_id = 7, .type = SESSION_TYPE_WEBUI };
   json_object *f = webui_message_appended_frame(1, 2, "user", "x", NULL, 0, &s, NULL);

   char *copy = webui_message_appended_origin_json(f, &s, "turn-42");
   TEST_ASSERT_NOT_NULL(copy);
   json_object *parsed = json_tokener_parse(copy);
   TEST_ASSERT_NOT_NULL(parsed);
   json_object *p = payload_of(parsed);
   TEST_ASSERT_EQUAL_STRING("turn-42", str_field(p, "client_ref"));
   TEST_ASSERT_EQUAL_INT64(7, int_field(p, "session_id"));
   TEST_ASSERT_EQUAL_STRING("x", str_field(p, "text"));
   json_object_put(parsed);
   free(copy);
   TEST_ASSERT_FALSE(has(payload_of(f), "client_ref"));

   TEST_ASSERT_NULL(webui_message_appended_origin_json(f, NULL, "turn-42"));
   TEST_ASSERT_NULL(webui_message_appended_origin_json(f, &s, NULL));
   TEST_ASSERT_NULL(webui_message_appended_origin_json(f, &s, ""));
   TEST_ASSERT_FALSE(has(payload_of(f), "client_ref"));
   json_object_put(f);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_fields);
   RUN_TEST(test_role_and_stream);
   RUN_TEST(test_session_id);
   RUN_TEST(test_origin_rule);
   RUN_TEST(test_reasoning);
   RUN_TEST(test_email_ref);
   RUN_TEST(test_client_ref);
   return UNITY_END();
}
