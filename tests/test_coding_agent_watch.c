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
 * The coding-agent watcher: gateway events onto coding jobs (checked against
 * the job row, idempotent), the start-up check of open jobs, and the saved
 * cursor.  The gateway and the job store are stubs.
 */

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_manager.h"
#include "dawn_error.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_watch.h"
#include "unity.h"

dawn_config_t g_config;

/* --- the job store: one row, job 42 of user 5 ------------------------------ */

static job_record_t g_row;
static bool g_row_exists;
static char g_end[JOB_STATUS_MAX];
static char g_end_why[64];
static int g_events;
static char g_last_payload[512];
static int g_notices;

int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   if (!g_row_exists || conv_id != g_row.id) {
      return AUTH_DB_NOT_FOUND;
   }
   if (user_id != g_row.user_id) {
      return AUTH_DB_FORBIDDEN;
   }
   *out = g_row;
   return AUTH_DB_SUCCESS;
}
int job_manager_set_terminal_if_active(int64_t conv_id,
                                       int user_id,
                                       const char *status,
                                       const char *error_or_null,
                                       time_t finished_at,
                                       int64_t final_message_id) {
   (void)conv_id, (void)user_id, (void)finished_at, (void)final_message_id;
   if (strcmp(g_row.job_status, "queued") != 0 && strcmp(g_row.job_status, "running") != 0) {
      return AUTH_DB_NOT_FOUND;
   }
   snprintf(g_row.job_status, sizeof(g_row.job_status), "%s", status);
   snprintf(g_end, sizeof(g_end), "%s", status);
   snprintf(g_end_why, sizeof(g_end_why), "%s", error_or_null ? error_or_null : "");
   return AUTH_DB_SUCCESS;
}
static char g_last_kind[32];
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id, (void)user_id;
   snprintf(g_last_kind, sizeof(g_last_kind), "%s", kind);
   g_events++;
   snprintf(g_last_payload, sizeof(g_last_payload), "%s", payload_owned ? payload_owned : "");
   free(payload_owned);
}
static char g_notice[512];
static char g_posted[512];
static int64_t g_posted_to;
bool job_notify_user(int user_id, const char *text, int64_t conv_id, int running) {
   (void)user_id, (void)conv_id, (void)running;
   snprintf(g_notice, sizeof(g_notice), "%s", text);
   g_notices++;
   return true;
}
int job_report_post(int64_t parent_conv, int user_id, const char *text) {
   (void)user_id;
   g_posted_to = parent_conv;
   snprintf(g_posted, sizeof(g_posted), "%s", text);
   return SUCCESS;
}
char *llm_context_neutralize(const char *text) {
   return strdup(text);
}
bool coding_gateway_request_line(struct json_object *req, char *out, size_t out_len) {
   struct json_object *payload = json_object_object_get(req, "payload");
   struct json_object *v = json_object_object_get(payload, "value");
   if (v == NULL) {
      return false;
   }
   snprintf(out, out_len, "reach the host %s", json_object_get_string(v));
   return true;
}
int conv_db_job_scan_active_kind(const char *kind, job_record_t *out, int max, int *count_out) {
   TEST_ASSERT_EQUAL_STRING(JOB_KIND_CODING, kind);
   const bool open = g_row_exists && (strcmp(g_row.job_status, "queued") == 0 ||
                                      strcmp(g_row.job_status, "running") == 0);
   if (open && max > 0) {
      out[0] = g_row;
   }
   *count_out = open ? 1 : 0;
   return AUTH_DB_SUCCESS;
}
int conv_db_job_set_running(int64_t conv_id, time_t at) {
   (void)conv_id, (void)at;
   snprintf(g_row.job_status, sizeof(g_row.job_status), "running");
   return AUTH_DB_SUCCESS;
}
void job_update_emit(int64_t conv_id, int user_id) {
   (void)conv_id, (void)user_id;
}
int mcp_bridge_interrupt(const char *alias) {
   (void)alias;
   return SUCCESS;
}

/* --- the report-back (its own unit) --------------------------------------- */

static int g_reports;
static char g_report_kind[16];
void coding_report_job_end(int64_t job,
                           int user_id,
                           const char *gw_kind,
                           struct json_object *data) {
   (void)job, (void)user_id, (void)data;
   g_reports++;
   snprintf(g_report_kind, sizeof(g_report_kind), "%s", gw_kind);
}

/* --- the gateway ------------------------------------------------------------ */

static atomic_int g_cancels;
static atomic_int g_polls;
static bool g_has_task = true;  /* task_list finds the task */
static bool g_pending = true;   /* ...with a request waiting */
static bool g_events_on = true; /* task_events has the completion */

const char *coding_gateway_alias(void) {
   return "agents";
}
const char *coding_gateway_jstr(struct json_object *o, const char *key) {
   struct json_object *v = NULL;
   if (o == NULL || !json_object_object_get_ex(o, key, &v) || v == NULL ||
       json_object_is_type(v, json_type_null)) {
      return "";
   }
   return json_object_get_string(v);
}
void coding_gateway_ref(int64_t conv_id, char *out, size_t out_len) {
   snprintf(out, out_len, CODING_AGENT_CLIENT_REF_FMT, (long long)conv_id);
}
int64_t coding_gateway_ref_job(const char *client_ref) {
   long long id = 0;
   char tail;
   return client_ref && sscanf(client_ref, CODING_AGENT_CLIENT_REF_FMT "%c", &id, &tail) == 1 ? id
                                                                                              : 0;
}
void coding_gateway_cancel(int user_id, int64_t conv_id) {
   (void)user_id, (void)conv_id;
   atomic_fetch_add(&g_cancels, 1);
}
int coding_gateway_call(int user_id,
                        const char *tool,
                        struct json_object *args,
                        struct json_object **out,
                        char **err) {
   (void)user_id;
   json_object_put(args);
   *err = NULL;
   if (strcmp(tool, "task_list") == 0) {
      *out = json_tokener_parse(!g_has_task ? "{\"task\":null,\"pending\":[]}"
                                : g_pending ? "{\"task\":{\"status\":\"input_required\"},"
                                              "\"pending\":[{\"id\":1,\"kind\":\"domain\","
                                              "\"payload\":{\"value\":\"pypi.org\"}}]}"
                                            : "{\"task\":{\"status\":\"working\"},"
                                              "\"pending\":[]}");
      return CODING_GW_OK;
   }
   /* task_events: one completion, then nothing new. */
   usleep(10 * 1000);
   if (atomic_fetch_add(&g_polls, 1) == 0 && g_events_on) {
      *out = json_tokener_parse("{\"events\":[{\"seq\":5,\"task_id\":\"t1\",\"client_ref\":"
                                "\"job:42\",\"dawn_user_id\":5,\"kind\":\"completed\","
                                "\"data\":{\"summary\":\"done\"}}],\"last_seq\":5}");
   } else {
      *out = json_tokener_parse("{\"events\":[],\"last_seq\":5}");
   }
   return CODING_GW_OK;
}

/* --- helpers ---------------------------------------------------------------- */

static struct json_object *event(const char *kind, const char *ref, int user) {
   struct json_object *e = json_object_new_object();
   json_object_object_add(e, "task_id", json_object_new_string("t1"));
   json_object_object_add(e, "client_ref", json_object_new_string(ref));
   json_object_object_add(e, "dawn_user_id", json_object_new_int(user));
   json_object_object_add(e, "kind", json_object_new_string(kind));
   struct json_object *data = json_object_new_object();
   json_object_object_add(data, "text", json_object_new_string("working on it"));
   json_object_object_add(data, "secret_field", json_object_new_string("dropped"));
   json_object_object_add(e, "data", data);
   return e;
}

static void apply(const char *kind, const char *ref, int user) {
   struct json_object *e = event(kind, ref, user);
   coding_watch_apply(e);
   json_object_put(e);
}

static void row(const char *status, const char *kind) {
   memset(&g_row, 0, sizeof(g_row));
   g_row.id = 42;
   g_row.user_id = 5;
   snprintf(g_row.job_status, sizeof(g_row.job_status), "%s", status);
   snprintf(g_row.job_kind, sizeof(g_row.job_kind), "%s", kind);
   snprintf(g_row.title, sizeof(g_row.title), "fix the oracle");
   g_row.parent_id = 7;
   g_row_exists = true;
}

void setUp(void) {
   row("running", JOB_KIND_CODING);
   g_end[0] = '\0';
   g_events = 0;
   g_notices = 0;
   g_reports = 0;
   g_pending = true;
   atomic_store(&g_cancels, 0);
}

void tearDown(void) {
}

static void test_steps_go_into_the_job(void) {
   apply("progress", "job:42", 5);
   TEST_ASSERT_EQUAL_INT(1, g_events);
   TEST_ASSERT_EQUAL_STRING(CONV_EVENT_AGENT_STEP, g_last_kind);
   TEST_ASSERT_NOT_NULL(strstr(g_last_payload, "\"gw\":\"progress\""));
   TEST_ASSERT_NOT_NULL(strstr(g_last_payload, "working on it"));
   TEST_ASSERT_NULL(strstr(g_last_payload, "secret_field"));
   TEST_ASSERT_EQUAL_STRING("", g_end);
}

static void test_ends(void) {
   apply("completed", "job:42", 5);
   TEST_ASSERT_EQUAL_STRING("done", g_end);
   TEST_ASSERT_EQUAL_INT(1, g_reports); /* reported once, */
   TEST_ASSERT_EQUAL_STRING("completed", g_report_kind);
   apply("completed", "job:42", 5); /* not again on a replay */
   TEST_ASSERT_EQUAL_INT(1, g_reports);
   setUp();
   apply("failed", "job:42", 5);
   TEST_ASSERT_EQUAL_STRING("failed", g_end);
   TEST_ASSERT_EQUAL_STRING("failed", g_report_kind);
   setUp();
   apply("cancelled", "job:42", 5); /* not asked for here */
   TEST_ASSERT_EQUAL_STRING("failed", g_end);
   TEST_ASSERT_EQUAL_STRING("cancelled", g_report_kind);
   TEST_ASSERT_EQUAL_STRING("stopped at the gateway", g_end_why);
   setUp();
   apply("input_required", "job:42", 5);
   TEST_ASSERT_EQUAL_INT(1, g_notices);
   /* What it asks, in DAWN's words, and the same line in its conversation. */
   TEST_ASSERT_EQUAL_STRING("Coding job #42 (fix the oracle) is waiting: it wants to reach the "
                            "host pypi.org.",
                            g_notice);
   TEST_ASSERT_EQUAL_STRING(g_notice, g_posted);
   TEST_ASSERT_EQUAL_INT64(7, g_posted_to);
   TEST_ASSERT_EQUAL_STRING(CONV_EVENT_AGENT_REQUEST, g_last_kind);
   TEST_ASSERT_EQUAL_STRING("", g_end);
   g_pending = false; /* replayed after the answer: no notice */
   apply("input_required", "job:42", 5);
   TEST_ASSERT_EQUAL_INT(1, g_notices);
   apply("started", "job:42", 5); /* state only */
   apply("whatever_new", "job:42", 5);
   TEST_ASSERT_EQUAL_STRING(CONV_EVENT_AGENT_REQUEST, g_last_kind);
}

/* An ended job takes nothing more, and its task, still reporting, is stopped
 * (once): DAWN ended it, by a cancel or otherwise. */
static void test_ended_job_takes_nothing_and_its_task_is_stopped(void) {
   row("failed", JOB_KIND_CODING);
   apply("completed", "job:42", 5);
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_cancels)); /* an end needs nothing */
   row("cancelled", JOB_KIND_CODING);
   apply("progress", "job:42", 5);
   apply("tool", "job:42", 5);
   TEST_ASSERT_EQUAL_INT(0, g_events);
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_cancels));
}

/* A deleted job's task is stopped; its end needs nothing. */
static void test_deleted_job(void) {
   g_row_exists = false;
   apply("completed", "job:43", 5);
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_cancels));
   apply("progress", "job:43", 5);
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_cancels));
}

/* Another user's job, a job of another kind, a task DAWN didn't start, and a
 * system event are all left alone. */
static void test_not_ours(void) {
   apply("completed", "job:42", 6);
   apply("completed", "other-ref", 5);
   row("running", "");
   apply("completed", "job:42", 5);
   struct json_object *sys = json_tokener_parse(
       "{\"task_id\":\"\",\"kind\":\"update\",\"data\":{\"result\":\"updated\"}}");
   coding_watch_apply(sys);
   json_object_put(sys);
   TEST_ASSERT_EQUAL_STRING("", g_end);
   TEST_ASSERT_EQUAL_INT(0, g_events);
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_cancels));
}

/* The agent's text is kept whole-character and capped; lists are joined. */
static void test_agent_payload(void) {
   char big[5000];
   memset(big, 'x', sizeof(big));
   memcpy(big + 4094, "\xe2\x80\x94", 3); /* a dash across the cap */
   big[sizeof(big) - 1] = '\0';
   struct json_object *data = json_object_new_object();
   json_object_object_add(data, "summary", json_object_new_string(big));
   struct json_object *commits = json_object_new_array();
   json_object_array_add(commits, json_object_new_string("abc fix"));
   json_object_array_add(commits, json_object_new_string("def test"));
   json_object_object_add(data, "commits", commits);
   static const char *const fields[] = { "summary", "commits", "request_ids", "input", NULL };
   static const char *const scrubbed[] = { "input", NULL };
   struct json_object *ids = json_object_new_array();
   json_object_array_add(ids, json_object_new_int(3));
   json_object_object_add(data, "request_ids", ids);
   json_object_object_add(data, "input",
                          json_object_new_string("curl -H 'Authorization: Bearer abc123' "
                                                 "KEY=sk-live-1234 x"));
   json_object_object_add(data, "repo", json_object_new_string("not listed"));
   char *p = event_payload_agent("completed", 7, 0, data, fields, scrubbed);
   json_object_put(data);
   struct json_object *o = json_tokener_parse(p);
   TEST_ASSERT_NOT_NULL(o);
   const char *summary = json_object_get_string(json_object_object_get(o, "summary"));
   TEST_ASSERT_EQUAL_INT(4094, (int)strlen(summary));
   TEST_ASSERT_EQUAL_STRING("abc fix\ndef test",
                            json_object_get_string(json_object_object_get(o, "commits")));
   TEST_ASSERT_TRUE(json_object_is_type(json_object_object_get(o, "request_ids"), json_type_array));
   const char *input = json_object_get_string(json_object_object_get(o, "input"));
   TEST_ASSERT_NULL(strstr(input, "abc123"));
   TEST_ASSERT_NULL(strstr(input, "sk-live"));
   TEST_ASSERT_NOT_NULL(strstr(input, "curl"));
   TEST_ASSERT_NULL(json_object_object_get(o, "repo"));
   json_object_put(o);
   free(p);

   /* Paths and URLs are kept; a JWT and a long mixed key are not. */
   data = json_object_new_object();
   json_object_object_add(data, "input",
                          json_object_new_string(
                              "Read /home/dawn-agent/work/abcd1234/src/tools/coding_agent_watch.c "
                              "https://github.com/The-OASIS-Project/dawn/pull/123/files "
                              "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.sig "
                              "TOKEN=a1b2c3d4e5f6a7b8c9d0a1b2c3d4e5f6a7b8c9d0x"));
   p = event_payload_agent("request", 8, 0, data, fields, scrubbed);
   json_object_put(data);
   o = json_tokener_parse(p);
   input = json_object_get_string(json_object_object_get(o, "input"));
   TEST_ASSERT_NOT_NULL(strstr(input, "src/tools/coding_agent_watch.c"));
   TEST_ASSERT_NOT_NULL(strstr(input, "github.com/The-OASIS-Project/dawn/pull/123"));
   TEST_ASSERT_NULL(strstr(input, "eyJhbGci"));
   TEST_ASSERT_NULL(strstr(input, "a1b2c3d4e5f6"));

   /* A list cut at the cap ends on a whole character. */
   json_object_put(o);
   free(p);
   data = json_object_new_object();
   struct json_object *list = json_object_new_array();
   char dashes[700];
   for (size_t k = 0; k + 3 < sizeof(dashes); k += 3) {
      memcpy(dashes + k, "\xe2\x80\x94", 3);
      dashes[k + 3] = '\0';
   }
   for (int k = 0; k < 8; k++) {
      json_object_array_add(list, json_object_new_string(dashes));
   }
   json_object_object_add(data, "commits", list);
   p = event_payload_agent("completed", 9, 0, data, fields, scrubbed);
   json_object_put(data);
   o = json_tokener_parse(p);
   const char *joined = json_object_get_string(json_object_object_get(o, "commits"));
   const size_t clen = strlen(joined);
   TEST_ASSERT_TRUE(clen <= EVENT_AGENT_TEXT_CAP);
   TEST_ASSERT_EQUAL_HEX8(0x94, (unsigned char)joined[clen - 1]);
   TEST_ASSERT_EQUAL_INT(9, json_object_get_int(json_object_object_get(o, "gw_seq")));
   json_object_put(o);
   free(p);
}

/* Start-up: the missed events come first (the completion ends the job, with
 * its result kept), the cursor is saved, mode 600; nothing is left to check. */
static void test_thread_catches_up_then_saves_the_cursor(void) {
   char dir[] = "/tmp/dawn-cw-XXXXXX";
   TEST_ASSERT_NOT_NULL(mkdtemp(dir));
   snprintf(g_config.paths.data_dir_resolved, sizeof(g_config.paths.data_dir_resolved), "%s", dir);
   g_row.created_at = time(NULL) - 60;
   g_has_task = false; /* would fail the job, were it still open after the catch-up */
   atomic_store(&g_polls, 0);
   TEST_ASSERT_EQUAL_INT(SUCCESS, coding_watch_start());
   for (int i = 0; i < 200 && atomic_load(&g_polls) < 3; i++) {
      usleep(10 * 1000);
   }
   coding_watch_stop();
   TEST_ASSERT_EQUAL_STRING("done", g_end);
   TEST_ASSERT_EQUAL_STRING(CONV_EVENT_AGENT_END, g_last_kind);
   TEST_ASSERT_NOT_NULL(strstr(g_last_payload, "\"summary\":\"done\""));

   char path[512];
   snprintf(path, sizeof(path), "%s/coding_agent_agents.cursor", dir);
   FILE *f = fopen(path, "r");
   TEST_ASSERT_NOT_NULL(f);
   long long seq = 0;
   TEST_ASSERT_EQUAL_INT(1, fscanf(f, "%lld", &seq));
   fclose(f);
   TEST_ASSERT_EQUAL_INT(5, (int)seq);
   struct stat st;
   TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
   TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777);
   unlink(path);

   /* A job still open after the catch-up with no task at the gateway fails,
    * and its possible start is stopped. */
   row("running", JOB_KIND_CODING);
   g_row.created_at = time(NULL) - 60;
   g_end[0] = '\0';
   g_events_on = false;
   atomic_store(&g_polls, 0);
   TEST_ASSERT_EQUAL_INT(SUCCESS, coding_watch_start());
   for (int i = 0; i < 200 && atomic_load(&g_polls) < 3; i++) {
      usleep(10 * 1000);
   }
   coding_watch_stop();
   TEST_ASSERT_EQUAL_STRING("failed", g_end);
   TEST_ASSERT_EQUAL_STRING("the gateway has no record of it", g_end_why);
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_cancels));
   unlink(path);
   rmdir(dir);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_steps_go_into_the_job);
   RUN_TEST(test_ends);
   RUN_TEST(test_ended_job_takes_nothing_and_its_task_is_stopped);
   RUN_TEST(test_deleted_job);
   RUN_TEST(test_not_ours);
   RUN_TEST(test_agent_payload);
   RUN_TEST(test_thread_catches_up_then_saves_the_cursor);
   return UNITY_END();
}
