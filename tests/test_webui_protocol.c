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
#include <string.h>

#include "unity.h"
#include "webui/webui_protocol.h"

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
   for (size_t i = 0; i < json_object_array_length(features); i++) {
      const char *f = json_object_get_string(json_object_array_get_idx(features, i));
      logout = logout || strcmp(f, "logout_closes_sockets") == 0;
      app_logins = app_logins || strcmp(f, "app_logins") == 0;
      image_turns = image_turns || strcmp(f, "image_turns_by_id") == 0;
      image_only = image_only || strcmp(f, "image_only_turns") == 0;
   }
   TEST_ASSERT_TRUE(logout);
   TEST_ASSERT_TRUE(app_logins);
   TEST_ASSERT_TRUE(image_turns);
   TEST_ASSERT_TRUE(image_only);
   json_object_put(root);
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

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_the_members_are_the_version_and_the_flags);
   RUN_TEST(test_too_small_a_buffer_gives_nothing);
   RUN_TEST(test_a_client_is_noted_once);
   return UNITY_END();
}
