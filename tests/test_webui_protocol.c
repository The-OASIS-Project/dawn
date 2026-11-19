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
 * The WebUI protocol's version and feature flags (webui_protocol.c).
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "webui/webui_protocol.h"

/* The mail panel's check: here, whatever the test says. */
static bool s_email_on;
bool webui_email_client_enabled(void) {
   return s_email_on;
}

/* Memory's check: likewise. */
static bool s_memory_on;
bool webui_memory_batches_enabled(void) {
   return s_memory_on;
}

void setUp(void) {
}

void tearDown(void) {
}

/* The advertised members make a valid object with the version and the flags. */
static void test_the_members_are_the_version_and_the_flags(void) {
   char members[WEBUI_PROTOCOL_JSON_MAX];
   TEST_ASSERT_TRUE(webui_protocol_json_members(members, sizeof(members)) > 0);
   char object[WEBUI_PROTOCOL_JSON_MAX + 2];
   snprintf(object, sizeof(object), "{%s}", members);
   struct json_object *root = json_tokener_parse(object);
   TEST_ASSERT_NOT_NULL(root);
   struct json_object *protocol = NULL;
   struct json_object *features = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "protocol", &protocol));
   TEST_ASSERT_EQUAL_INT(WEBUI_PROTOCOL_VERSION, json_object_get_int(protocol));
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "features", &features));
   TEST_ASSERT_TRUE(json_object_is_type(features, json_type_array));
   bool logout = false;
   bool app_logins = false;
   bool image_turns = false;
   bool image_only = false;
   bool turn_refs = false;
   bool visual_guard = false;
   bool attachments = false;
   bool login_unavailable = false;
   bool turn_wait = false;
   bool stopped_saved = false;
   for (size_t i = 0; i < json_object_array_length(features); i++) {
      const char *f = json_object_get_string(json_object_array_get_idx(features, i));
      logout = logout || strcmp(f, "logout_closes_sockets") == 0;
      app_logins = app_logins || strcmp(f, "app_logins") == 0;
      image_turns = image_turns || strcmp(f, "image_turns_by_id") == 0;
      image_only = image_only || strcmp(f, "image_only_turns") == 0;
      turn_refs = turn_refs || strcmp(f, "turn_refs") == 0;
      visual_guard = visual_guard || strcmp(f, "visual_prompt_guard") == 0;
      attachments = attachments || strcmp(f, "document_attachments") == 0;
      login_unavailable = login_unavailable || strcmp(f, "login_unavailable") == 0;
      turn_wait = turn_wait || strcmp(f, "turn_wait") == 0;
      stopped_saved = stopped_saved || strcmp(f, "stopped_replies_saved") == 0;
   }
   TEST_ASSERT_TRUE(logout);
   TEST_ASSERT_TRUE(app_logins);
   TEST_ASSERT_TRUE(image_turns);
   TEST_ASSERT_TRUE(image_only);
   TEST_ASSERT_TRUE(turn_refs);
   TEST_ASSERT_TRUE(visual_guard);
   TEST_ASSERT_TRUE(attachments);
   TEST_ASSERT_TRUE(login_unavailable);
   TEST_ASSERT_TRUE(turn_wait);
   TEST_ASSERT_TRUE(stopped_saved);
   json_object_put(root);
}

/* email_client is advertised only while the mail panel can answer. */
static void test_email_client_follows_its_check(void) {
   char members[WEBUI_PROTOCOL_JSON_MAX];
   s_email_on = false;
   TEST_ASSERT_TRUE(webui_protocol_json_members(members, sizeof(members)) > 0);
   TEST_ASSERT_NULL(strstr(members, "\"email_client\""));
   s_email_on = true;
   TEST_ASSERT_TRUE(webui_protocol_json_members(members, sizeof(members)) > 0);
   TEST_ASSERT_NOT_NULL(strstr(members, "\"email_client\""));
   char object[WEBUI_PROTOCOL_JSON_MAX + 2];
   snprintf(object, sizeof(object), "{%s}", members);
   struct json_object *root = json_tokener_parse(object);
   TEST_ASSERT_NOT_NULL(root);
   json_object_put(root);
   s_email_on = false;
}

/* memory_batches is advertised only while memory is enabled; with every
 * flag on, the members still fit (and parse). */
static void test_memory_batches_follows_its_check(void) {
   char members[WEBUI_PROTOCOL_JSON_MAX];
   s_memory_on = false;
   TEST_ASSERT_TRUE(webui_protocol_json_members(members, sizeof(members)) > 0);
   TEST_ASSERT_NULL(strstr(members, "\"memory_batches\""));
   s_memory_on = true;
   s_email_on = true;
   TEST_ASSERT_TRUE(webui_protocol_json_members(members, sizeof(members)) > 0);
   TEST_ASSERT_NOT_NULL(strstr(members, "\"memory_batches\""));
   char object[WEBUI_PROTOCOL_JSON_MAX + 2];
   snprintf(object, sizeof(object), "{%s}", members);
   struct json_object *root = json_tokener_parse(object);
   TEST_ASSERT_NOT_NULL(root);
   json_object_put(root);
   s_memory_on = false;
   s_email_on = false;
}

/* Every flag on still fits the buffer the callers give. */
static void test_every_flag_fits(void) {
   TEST_ASSERT_TRUE(webui_protocol_json_max_len() < WEBUI_PROTOCOL_JSON_MAX);
   char members[WEBUI_PROTOCOL_JSON_MAX];
   s_memory_on = true;
   s_email_on = true;
   const size_t n = webui_protocol_json_members(members, sizeof(members));
   TEST_ASSERT_TRUE(n > 0 && n <= webui_protocol_json_max_len());
   s_memory_on = false;
   s_email_on = false;
}

static void test_too_small_a_buffer_gives_nothing(void) {
   char tiny[8] = "x";
   TEST_ASSERT_EQUAL_size_t(0, webui_protocol_json_members(tiny, sizeof(tiny)));
   TEST_ASSERT_EQUAL_STRING("", tiny);
}

/* A client is noted once; a payload without one (or with a malformed one)
 * notes nothing. */
static void test_a_client_is_noted_once(void) {
   bool noted = false;
   struct json_object *none = json_tokener_parse("{\"tts_enabled\":true}");
   webui_protocol_note_client(none, &noted);
   TEST_ASSERT_FALSE(noted);
   struct json_object *bad = json_tokener_parse("{\"client\":\"aurora\"}");
   webui_protocol_note_client(bad, &noted);
   TEST_ASSERT_FALSE(noted);
   struct json_object *ok = json_tokener_parse(
       "{\"client\":{\"name\":\"aurora\\u0007\",\"version\":\"1.2.3\",\"protocol\":1}}");
   webui_protocol_note_client(ok, &noted);
   TEST_ASSERT_TRUE(noted);
   json_object_put(none);
   json_object_put(bad);
   json_object_put(ok);
}

/* req is echoed only when it is plain: a string, short, no controls, valid
 * UTF-8, no embedded NUL. */
static void test_req_is_taken_only_when_safe_to_echo(void) {
   char out[WEBUI_REQ_MAX + 1];
   const char *bad[] = {
      "{}",
      "{\"req\":7}",
      "{\"req\":\"a\\u0001b\"}",
      "{\"req\":\"a\\u0000b\"}",
      "{\"req\":\"x\\u007f\"}",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      struct json_object *p = json_tokener_parse(bad[i]);
      TEST_ASSERT_NOT_NULL_MESSAGE(p, bad[i]);
      TEST_ASSERT_FALSE_MESSAGE(webui_protocol_payload_req(p, WEBUI_REQ_MAX, out, sizeof(out)),
                                bad[i]);
      json_object_put(p);
   }
   TEST_ASSERT_FALSE(webui_protocol_payload_req(NULL, WEBUI_REQ_MAX, out, sizeof(out)));

   /* invalid UTF-8 (a lone continuation byte) */
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "req", json_object_new_string("ab\x80"));
   TEST_ASSERT_FALSE(webui_protocol_payload_req(p, WEBUI_REQ_MAX, out, sizeof(out)));
   json_object_put(p);

   char longer[WEBUI_REQ_MAX + 2];
   memset(longer, 'r', sizeof(longer) - 1);
   longer[sizeof(longer) - 1] = '\0';
   p = json_object_new_object();
   json_object_object_add(p, "req", json_object_new_string(longer));
   TEST_ASSERT_FALSE(webui_protocol_payload_req(p, WEBUI_REQ_MAX, out, sizeof(out)));
   longer[WEBUI_REQ_MAX] = '\0'; /* exactly the max */
   json_object_object_add(p, "req", json_object_new_string(longer));
   TEST_ASSERT_TRUE(webui_protocol_payload_req(p, WEBUI_REQ_MAX, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING(longer, out);
   json_object_put(p);

   p = json_tokener_parse("{\"req\":\"r-\\u00e9-1\"}");
   TEST_ASSERT_TRUE(webui_protocol_payload_req(p, WEBUI_REQ_MAX, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("r-\xc3\xa9-1", out);
   json_object_put(p);
}

static const char *field(struct json_object *o, const char *key) {
   struct json_object *f;
   return json_object_object_get_ex(o, key, &f) ? json_object_get_string(f) : NULL;
}

/* While a request is handled, frames to its connection carry its req (and its
 * type on an error); frames to another connection, frames that already carry a
 * req, and frames sent after it ended don't. */
static void test_a_request_is_named_on_its_own_frames_only(void) {
   int conn_a, conn_b;
   struct json_object *req_payload = json_tokener_parse("{\"req\":\"q-1\"}");
   struct json_object *p = json_object_new_object();

   /* outside a request: nothing */
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn_a, p, true));
   TEST_ASSERT_EQUAL_INT(0, json_object_object_length(p));

   webui_protocol_request_begin(&conn_a, "get_config", req_payload);

   /* a reply: req only, and the caller is told it was added */
   TEST_ASSERT_TRUE(webui_protocol_request_stamp(&conn_a, p, false));
   TEST_ASSERT_EQUAL_STRING("q-1", field(p, "req"));
   TEST_ASSERT_NULL(field(p, "request_type"));

   /* an error: both; a req already there is kept and not reported as added */
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn_a, p, true));
   TEST_ASSERT_EQUAL_STRING("get_config", field(p, "request_type"));
   json_object_object_add(p, "req", json_object_new_string("own"));
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn_a, p, true));
   TEST_ASSERT_EQUAL_STRING("own", field(p, "req"));
   json_object_put(p);

   /* another connection, no connection, no payload object */
   p = json_object_new_object();
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn_b, p, true));
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(NULL, p, true));
   TEST_ASSERT_EQUAL_INT(0, json_object_object_length(p));
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn_a, NULL, true));

   webui_protocol_request_end();
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn_a, p, true));
   TEST_ASSERT_EQUAL_INT(0, json_object_object_length(p));
   json_object_put(p);
   json_object_put(req_payload);
}

/* A request with no req still names its type on an error; an unsafe type or
 * req is left out; the next request doesn't inherit the last one's. */
static void test_a_request_echoes_only_what_is_safe(void) {
   int conn;
   struct json_object *with_req = json_tokener_parse("{\"req\":\"q-2\"}");
   struct json_object *bad_req = json_tokener_parse("{\"req\":\"a\\nb\"}");

   /* a request with a req, ended, leaves nothing for the next */
   webui_protocol_request_begin(&conn, "load_conversation", with_req);
   webui_protocol_request_end();
   webui_protocol_request_begin(&conn, "set_session_llm", NULL);
   struct json_object *p = json_object_new_object();
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn, p, true));
   TEST_ASSERT_EQUAL_STRING("set_session_llm", field(p, "request_type"));
   TEST_ASSERT_NULL(field(p, "req"));
   json_object_put(p);
   webui_protocol_request_end();

   webui_protocol_request_begin(&conn, "bad\xff", bad_req);
   p = json_object_new_object();
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn, p, true));
   TEST_ASSERT_EQUAL_INT(0, json_object_object_length(p));
   json_object_put(p);
   webui_protocol_request_end();

   /* an empty type and req are valid, and echoed as they came */
   struct json_object *empty_req = json_tokener_parse("{\"req\":\"\"}");
   webui_protocol_request_begin(&conn, "", empty_req);
   p = json_object_new_object();
   TEST_ASSERT_TRUE(webui_protocol_request_stamp(&conn, p, true));
   TEST_ASSERT_EQUAL_STRING("", field(p, "request_type"));
   TEST_ASSERT_EQUAL_STRING("", field(p, "req"));
   json_object_put(p);
   webui_protocol_request_end();
   json_object_put(empty_req);

   /* a type or req one byte over the limit */
   char longer[WEBUI_REQ_MAX + 2];
   memset(longer, 't', sizeof(longer) - 1);
   longer[sizeof(longer) - 1] = '\0';
   struct json_object *long_req = json_object_new_object();
   json_object_object_add(long_req, "req", json_object_new_string(longer));
   webui_protocol_request_begin(&conn, longer, long_req);
   p = json_object_new_object();
   TEST_ASSERT_FALSE(webui_protocol_request_stamp(&conn, p, true));
   TEST_ASSERT_EQUAL_INT(0, json_object_object_length(p));
   json_object_put(p);
   webui_protocol_request_end();
   json_object_put(long_req);

   json_object_put(with_req);
   json_object_put(bad_req);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_the_members_are_the_version_and_the_flags);
   RUN_TEST(test_email_client_follows_its_check);
   RUN_TEST(test_memory_batches_follows_its_check);
   RUN_TEST(test_every_flag_fits);
   RUN_TEST(test_too_small_a_buffer_gives_nothing);
   RUN_TEST(test_a_client_is_noted_once);
   RUN_TEST(test_req_is_taken_only_when_safe_to_echo);
   RUN_TEST(test_a_request_is_named_on_its_own_frames_only);
   RUN_TEST(test_a_request_echoes_only_what_is_safe);
   return UNITY_END();
}
