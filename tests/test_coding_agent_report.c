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
 * The report-back on a finished coding job: what is posted where, and what the
 * no-tools take is asked.  The job system, the take, the post and the framing
 * are stubs.
 */

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "tools/coding_agent_report.h"
#include "unity.h"

#define ASK_MAX 4096
#define POST_MAX 1024

static int64_t g_parent = 7;
static bool g_injection;
static const char *g_take = "It fixed it and the tests pass.";
static atomic_int g_posts;
static atomic_int g_takes;
static char g_post[POST_MAX];
static char g_ask[ASK_MAX];
static session_t g_session;
/* What the stubs saw on the report thread, checked in the test body (a Unity
 * assert there would longjmp into the main thread). */
static atomic_bool g_counted_cap;
static atomic_bool g_streamed_at_end;
static atomic_bool g_no_persona;
static atomic_llong g_posted_to;

int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   memset(out, 0, sizeof(*out));
   out->id = conv_id;
   out->user_id = user_id;
   out->parent_id = g_parent;
   snprintf(out->title, sizeof(out->title), "fix the oracle");
   return AUTH_DB_SUCCESS;
}
int conv_db_job_get_goal(int64_t conv_id, int user_id, char **out) {
   (void)conv_id, (void)user_id;
   *out = strdup("Fix the calendar fuzzer oracle.");
   return AUTH_DB_SUCCESS;
}
int job_manager_begin_ex(int user_id,
                         int64_t conv_id,
                         job_provider_class_t provider,
                         bool count_against_user_cap,
                         session_t **out) {
   (void)user_id, (void)conv_id, (void)provider;
   if (count_against_user_cap) {
      atomic_store(&g_counted_cap, true);
   }
   atomic_store(&g_session.stream_conversation_id, conv_id);
   *out = &g_session;
   return JOB_MGR_OK;
}
void job_manager_end(session_t *s) {
   if (atomic_load(&s->stream_conversation_id) != 0) {
      atomic_store(&g_streamed_at_end, true);
   }
}
job_provider_class_t job_provider_from_default(void) {
   return JOB_PROVIDER_CLOUD;
}
void job_report_persona(char *out, size_t out_len) {
   snprintf(out, out_len, "You are Friday.");
}
char *job_report_take(struct session *s, const char *system_prompt, const char *ask, int user_id) {
   (void)s, (void)user_id;
   if (strstr(system_prompt, "You are Friday.") == NULL) {
      atomic_store(&g_no_persona, true);
   }
   snprintf(g_ask, sizeof(g_ask), "%s", ask);
   atomic_fetch_add(&g_takes, 1);
   return g_take ? strdup(g_take) : NULL;
}
int job_report_post(int64_t parent_conv, int user_id, const char *text) {
   (void)user_id;
   atomic_store(&g_posted_to, parent_conv);
   snprintf(g_post, sizeof(g_post), "%s", text);
   atomic_fetch_add(&g_posts, 1);
   return SUCCESS;
}
bool memory_filter_check_injection_commands(const char *text) {
   (void)text;
   return g_injection;
}
char *llm_context_neutralize(const char *text) {
   return strdup(text);
}
char *llm_context_neutralize_owned(char *text) {
   return text;
}
char *llm_third_party_frame(const char *name, const char *tag, const char *body) {
   (void)tag;
   char *out = malloc(strlen(body) + 64);
   sprintf(out, "--- %s ---\n%s--- END %s ---", name, body, name);
   return out;
}
const char *coding_gateway_jstr(struct json_object *o, const char *key) {
   struct json_object *v = NULL;
   if (o == NULL || !json_object_object_get_ex(o, key, &v) || v == NULL ||
       json_object_is_type(v, json_type_null)) {
      return "";
   }
   return json_object_get_string(v);
}

/* Report @p kind with @p data_json and wait for its post (or for nothing). */
static void report(const char *kind, const char *data_json, int expect_posts) {
   struct json_object *data = data_json ? json_tokener_parse(data_json) : NULL;
   const int before = atomic_load(&g_posts);
   coding_report_job_end(42, 5, kind, data);
   json_object_put(data);
   for (int i = 0; i < 200 && atomic_load(&g_posts) < before + expect_posts; i++) {
      usleep(10 * 1000);
   }
   usleep(20 * 1000); /* nothing more comes */
   TEST_ASSERT_EQUAL_INT(before + expect_posts, atomic_load(&g_posts));
   TEST_ASSERT_FALSE(atomic_load(&g_counted_cap));
   TEST_ASSERT_FALSE(atomic_load(&g_streamed_at_end));
   TEST_ASSERT_FALSE(atomic_load(&g_no_persona));
   if (expect_posts > 0) {
      TEST_ASSERT_EQUAL_INT64(g_parent, atomic_load(&g_posted_to));
   }
}

static const char *k_done = "{\"summary\":\"Did it.\",\"branch\":\"agent/x\","
                            "\"commits\":[\"ab12 fix x\",\"cd34 add test\"],"
                            "\"uncommitted\":0,\"cost_usd\":1.07,"
                            "\"checks\":[{\"command\":\"make tests-ci\",\"ok\":true},"
                            "{\"command\":\"make | tail\",\"ok\":null},"
                            "{\"command\":\"ctest -L ci\",\"ok\":false}]}";

void setUp(void) {
   g_parent = 7;
   g_injection = false;
   g_take = "It fixed it and the tests pass.";
   g_post[0] = g_ask[0] = '\0';
   atomic_store(&g_takes, 0);
   atomic_store(&g_posted_to, 0);
}

void tearDown(void) {
}

/* A completed task: the take, led by DAWN's line; the take is asked about the
 * agent's words in a frame and only the facts the gateway sent. */
static void test_completed_posts_the_take(void) {
   report("completed", k_done, 1);
   TEST_ASSERT_NOT_NULL(strstr(g_post, "Coding job #42 (fix the oracle) finished."));
   TEST_ASSERT_NOT_NULL(strstr(g_post, "the tests pass"));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "Fix the calendar fuzzer oracle."));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "--- AGENT CONTENT ---"));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "ab12 fix x\ncd34 add test"));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "Nothing was left uncommitted."));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "It cost $1.07."));
   /* What the gateway saw, in DAWN's words; the commands themselves framed. */
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "Of the last 3 build or test commands the gateway saw it "
                                      "run (each by its last run), 1 succeeded and 1 failed; 1 "
                                      "had no exit status"));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "- failed: ctest -L ci"));
   TEST_ASSERT_NOT_NULL(strstr(g_ask, "- ran: make | tail"));
}

/* The task record (after a restart) has no counts: none are claimed. */
static void test_only_facts_that_came(void) {
   report("completed", "{\"summary\":\"Did it.\",\"branch\":\"agent/x\"}", 1);
   TEST_ASSERT_NULL(strstr(g_ask, "uncommitted"));
   TEST_ASSERT_NULL(strstr(g_ask, "cost"));
   TEST_ASSERT_NULL(strstr(g_ask, "build or test")); /* no checks: no claim either way */
   report("completed", "{\"summary\":\"Did it.\",\"branch\":\"agent/x\",\"checks\":[]}", 1);
   TEST_ASSERT_NULL(strstr(g_ask, "build or test"));
}

/* No take (failed, empty, cut off): DAWN's fixed line. */
static void test_no_take_posts_the_fixed_line(void) {
   g_take = NULL;
   report("completed", k_done, 1);
   TEST_ASSERT_NOT_NULL(strstr(g_post, "is done. Ask me for what it did"));
}

/* A report the injection filter flags never reaches a take. */
static void test_flagged_report_gets_the_fixed_line(void) {
   g_injection = true;
   report("completed", k_done, 1);
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_takes));
   TEST_ASSERT_NOT_NULL(strstr(g_post, "is done."));
}

/* Other ends: their fixed lines, no take. */
static void test_other_ends(void) {
   report("failed", k_done, 1);
   TEST_ASSERT_NOT_NULL(strstr(g_post, "stopped with an error"));
   report("cancelled", NULL, 1);
   TEST_ASSERT_NOT_NULL(strstr(g_post, "stopped at the gateway"));
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_takes));
}

/* A job with no conversation to report into: nothing posted. */
static void test_no_parent(void) {
   g_parent = 0;
   report("completed", k_done, 0);
}

/* After stop, nothing new is reported. */
static void test_stopped(void) {
   coding_report_stop();
   report("completed", k_done, 0);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_completed_posts_the_take);
   RUN_TEST(test_only_facts_that_came);
   RUN_TEST(test_no_take_posts_the_fixed_line);
   RUN_TEST(test_flagged_report_gets_the_fixed_line);
   RUN_TEST(test_other_ends);
   RUN_TEST(test_no_parent);
   RUN_TEST(test_stopped);
   return UNITY_END();
}
