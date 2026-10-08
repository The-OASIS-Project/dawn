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
 * Tests for LLM request capture: who is captured, what is redacted, and the
 * files it writes.
 */

#include <curl/curl.h>
#include <dirent.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_request_capture.h"
#include "unity.h"

/* Session stubs: a command-context session whose user the test sets. */
static int s_session_user;
static session_t *s_session;
session_t *session_get_command_context(void) {
   return s_session;
}
int session_effective_user_id(session_t *session) {
   (void)session;
   return s_session_user;
}

static char s_dir[64];
static struct curl_slist *s_headers;

static int count_files(void) {
   DIR *d = opendir(s_dir);
   int n = 0;
   for (struct dirent *e; d && (e = readdir(d));) {
      n += e->d_name[0] != '.';
   }
   if (d) {
      closedir(d);
   }
   return n;
}

static json_object *read_file(const char *name) {
   char path[128];
   snprintf(path, sizeof(path), "%s/%s", s_dir, name);
   return json_object_from_file(path);
}

void setUp(void) {
   snprintf(s_dir, sizeof(s_dir), "/tmp/dawn_capture_test_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_dir));
   s_session = (session_t *)&s_session_user; /* any non-NULL pointer */
   s_session_user = 7;
   s_headers = curl_slist_append(NULL, "Content-Type: application/json");
   s_headers = curl_slist_append(s_headers, "x-api-key: sk-ant-secret");
   s_headers = curl_slist_append(s_headers, "Authorization: Bearer sk-secret");
}

void tearDown(void) {
   llm_request_capture_disarm();
   curl_slist_free_all(s_headers);
   char cmd[96];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
   TEST_ASSERT_EQUAL_INT(0, system(cmd));
}

static void test_arm_checks_its_arguments(void) {
   char err[96];
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(0, s_dir, 2, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(7, "relative", 2, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         llm_request_capture_arm(7, "/nonexistent/x", 2, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(7, s_dir, 0, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(7, s_dir, 51, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(0, llm_request_capture_remaining());
}

static void test_only_the_armed_users_turns_are_captured(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_request_capture_arm(7, s_dir, 2, NULL, 0));
   s_session_user = 1; /* another user */
   llm_request_capture("claude", "https://x/v1/messages", s_headers, "{}");
   s_session = NULL; /* a background call */
   s_session_user = 7;
   llm_request_capture("claude", "https://x/v1/messages", s_headers, "{}");
   TEST_ASSERT_EQUAL_INT(0, count_files());
   TEST_ASSERT_EQUAL_INT(2, llm_request_capture_remaining());
}

static void test_capture_redacts_and_disarms_after_count(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_request_capture_arm(7, s_dir, 2, NULL, 0));
   llm_request_capture("claude", "https://x/v1/messages?key=abc&v=1", s_headers,
                       "{\"model\":\"m\",\"max_tokens\":16384}");
   llm_request_capture("openai-chat", "https://y/v1/chat/completions", s_headers, "not json");
   llm_request_capture("claude", "https://x/v1/messages", s_headers, "{}"); /* past the count */
   TEST_ASSERT_EQUAL_INT(2, count_files());
   TEST_ASSERT_EQUAL_INT(0, llm_request_capture_remaining());

   json_object *a = read_file("001-claude.json");
   TEST_ASSERT_NOT_NULL(a);
   const char *text = json_object_to_json_string(a);
   TEST_ASSERT_NULL(strstr(text, "sk-ant-secret"));
   TEST_ASSERT_NULL(strstr(text, "sk-secret"));
   TEST_ASSERT_NULL(strstr(text, "key=abc"));
   json_object *v;
   TEST_ASSERT_TRUE(json_object_object_get_ex(a, "url", &v));
   TEST_ASSERT_EQUAL_STRING("https://x/v1/messages?key=[REDACTED]&v=1", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(a, "body", &v));
   json_object *mt;
   TEST_ASSERT_TRUE(json_object_object_get_ex(v, "max_tokens", &mt)); /* parsed, not a string */
   TEST_ASSERT_EQUAL_INT(16384, json_object_get_int(mt));
   json_object_put(a);

   json_object *b = read_file("002-openai-chat.json");
   TEST_ASSERT_NOT_NULL(b);
   TEST_ASSERT_TRUE(json_object_object_get_ex(b, "body", &v));
   TEST_ASSERT_EQUAL_STRING("not json", json_object_get_string(v));
   json_object_put(b);

   struct stat st;
   char path[128];
   snprintf(path, sizeof(path), "%s/001-claude.json", s_dir);
   TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
   TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777);
}

/* A directory that isn't empty, is a symlink, or others can write is refused:
 * the directory checked at arming is the one written into, and no file is
 * planted or overwritten. */
static void test_arm_refuses_unsafe_directories(void) {
   char err[96];
   char path[128];
   snprintf(path, sizeof(path), "%s/planted.json", s_dir);
   FILE *f = fopen(path, "w");
   TEST_ASSERT_NOT_NULL(f);
   fclose(f);
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(7, s_dir, 1, err, sizeof(err)));
   TEST_ASSERT_EQUAL_STRING("directory must be empty", err);
   TEST_ASSERT_EQUAL_INT(0, unlink(path));

   char link[128];
   snprintf(link, sizeof(link), "%s.link", s_dir);
   TEST_ASSERT_EQUAL_INT(0, symlink(s_dir, link));
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(7, link, 1, err, sizeof(err)));
   unlink(link);

   TEST_ASSERT_EQUAL_INT(0, chmod(s_dir, 0777));
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_request_capture_arm(7, s_dir, 1, err, sizeof(err)));
   TEST_ASSERT_EQUAL_INT(0, chmod(s_dir, 0700));
   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_request_capture_arm(7, s_dir, 1, err, sizeof(err)));
}

static void test_url_credentials_are_redacted(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_request_capture_arm(7, s_dir, 1, NULL, 0));
   llm_request_capture("openai-chat", "https://user:secret@llm.lan/v1/chat?v=2&api_key=abc&x=1",
                       NULL, "{}");
   json_object *a = read_file("001-openai-chat.json");
   TEST_ASSERT_NOT_NULL(a);
   json_object *v;
   TEST_ASSERT_TRUE(json_object_object_get_ex(a, "url", &v));
   TEST_ASSERT_EQUAL_STRING("https://[REDACTED]@llm.lan/v1/chat?v=2&api_key=[REDACTED]&x=1",
                            json_object_get_string(v));
   json_object_put(a);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_arm_checks_its_arguments);
   RUN_TEST(test_only_the_armed_users_turns_are_captured);
   RUN_TEST(test_capture_redacts_and_disarms_after_count);
   RUN_TEST(test_arm_refuses_unsafe_directories);
   RUN_TEST(test_url_credentials_are_redacted);
   return UNITY_END();
}
