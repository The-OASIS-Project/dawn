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
 * coding_agent tool: a task starts only on the user's yes to the preview, in
 * order (job row, task_start, the running claim), and a cancel ends the row
 * here and asks the gateway to stop.  The gateway, the job store and the
 * session are stubs.
 */

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_tool.h"
#include "tools/tool_registry.h"
#include "unity.h"

dawn_config_t g_config;

/* --- the gateway --------------------------------------------------------- */

#define MAX_CALLS 16
static char g_calls[MAX_CALLS][64]; /* tool names called, in order */
static char g_args[MAX_CALLS][512];
static atomic_int g_ncalls; /* the cancel thread calls too */
static bool g_start_fails;
static int g_start_lost; /* task_start answers lost before one arrives */
static int g_ntasks;     /* tasks task_list returns */
static bool g_reachable = true;

int mcp_bridge_ensure_connected(const char *alias) {
   (void)alias;
   return g_reachable ? SUCCESS : FAILURE;
}
bool mcp_bridge_is_agent_gateway(const char *alias) {
   (void)alias;
   return g_reachable;
}
int mcp_bridge_interrupt(const char *alias) {
   (void)alias;
   return SUCCESS;
}
int mcp_bridge_call_tool(const char *alias,
                         const char *tool,
                         const char *args_json,
                         long timeout_ms,
                         char **result_out) {
   (void)alias;
   (void)timeout_ms;
   const int i = atomic_load(&g_ncalls);
   if (i < MAX_CALLS) {
      snprintf(g_calls[i], sizeof(g_calls[0]), "%s", tool);
      snprintf(g_args[i], sizeof(g_args[0]), "%s", args_json ? args_json : "");
      atomic_store(&g_ncalls, i + 1);
   }
   if (strcmp(tool, "agents_list") == 0) {
      *result_out = strdup("{\"repos\":[{\"name\":\"dawn\",\"base\":\"main\"}]}");
   } else if (strcmp(tool, "task_start") == 0) {
      if (g_start_fails) {
         *result_out = strdup("too many tasks running");
         return FAILURE;
      }
      if (g_start_lost > 0) {
         g_start_lost--;
         return FAILURE; /* no text: no answer */
      }
      *result_out = strdup("{\"id\":\"t1\",\"branch\":\"agent/t1-fix\",\"status\":\"working\"}");
   } else if (strcmp(tool, "task_list") == 0 && args_json && strstr(args_json, "client_ref")) {
      *result_out = strdup("{\"task\":{\"status\":\"working\",\"client_ref\":\"job:42\","
                           "\"repo\":\"dawn\"},\"pending\":[]}");
   } else if (strcmp(tool, "task_list") == 0 && g_ntasks > 0) {
      /* Each summary ends in a multi-byte character past any excerpt's cut. */
      struct json_object *tasks = json_object_new_array();
      char summary[400];
      memset(summary, 'x', sizeof(summary));
      snprintf(summary + 158, sizeof(summary) - 158, "\xe2\x80\x94 done");
      for (int t = 0; t < g_ntasks; t++) {
         struct json_object *o = json_object_new_object();
         json_object_object_add(o, "client_ref", json_object_new_string("job:9"));
         json_object_object_add(o, "status", json_object_new_string("completed"));
         json_object_object_add(o, "task", json_object_new_string(summary));
         json_object_object_add(o, "summary", json_object_new_string(summary));
         json_object_array_add(tasks, o);
      }
      struct json_object *ans = json_object_new_object();
      json_object_object_add(ans, "tasks", tasks);
      *result_out = strdup(json_object_to_json_string(ans));
      json_object_put(ans);
   } else {
      *result_out = strdup("{}");
   }
   return SUCCESS;
}
static bool g_granted = true;
int auth_db_mcp_check_access(int64_t user_id, const char *alias, bool *allowed_out) {
   (void)alias;
   *allowed_out = g_granted && user_id == 5;
   return AUTH_DB_SUCCESS;
}

/* --- the job store -------------------------------------------------------- */

static char g_status[JOB_STATUS_MAX];
static char g_kind[JOB_KIND_MAX];
static bool g_fired;
static bool g_claim_lost;

int conv_db_create_job_ex(int user_id,
                          const char *title,
                          int64_t parent_id,
                          const char *spawn_mode,
                          const char *on_complete,
                          const char *deliver_to,
                          int spawn_depth,
                          const char *goal,
                          const char *origin,
                          int64_t *conv_id_out) {
   (void)user_id, (void)title, (void)parent_id, (void)spawn_mode, (void)on_complete;
   (void)deliver_to, (void)spawn_depth, (void)goal, (void)origin;
   snprintf(g_status, sizeof(g_status), "queued");
   g_kind[0] = '\0';
   g_fired = false;
   *conv_id_out = 42;
   return AUTH_DB_SUCCESS;
}
int conv_db_job_set_kind(int64_t conv_id, const char *kind) {
   (void)conv_id;
   snprintf(g_kind, sizeof(g_kind), "%s", kind);
   return AUTH_DB_SUCCESS;
}
int conv_db_job_set_running(int64_t conv_id, time_t at) {
   (void)conv_id, (void)at;
   if (g_claim_lost || strcmp(g_status, "queued") != 0) {
      return AUTH_DB_NOT_FOUND;
   }
   snprintf(g_status, sizeof(g_status), "running");
   return AUTH_DB_SUCCESS;
}
int conv_db_job_mark_fired(int64_t conv_id) {
   (void)conv_id;
   g_fired = true;
   return AUTH_DB_SUCCESS;
}
int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   if (conv_id != 42 || user_id != 5 || g_status[0] == '\0') {
      return AUTH_DB_NOT_FOUND;
   }
   memset(out, 0, sizeof(*out));
   out->id = 42;
   out->user_id = 5;
   snprintf(out->job_status, sizeof(out->job_status), "%s", g_status);
   snprintf(out->job_kind, sizeof(out->job_kind), "%s", g_kind);
   return AUTH_DB_SUCCESS;
}
void conv_generate_title(const char *content, char *title_out, size_t max_len) {
   snprintf(title_out, max_len, "%.20s", content);
}
int job_manager_set_terminal_if_active(int64_t conv_id,
                                       int user_id,
                                       const char *status,
                                       const char *error_or_null,
                                       time_t finished_at,
                                       int64_t final_message_id) {
   (void)conv_id, (void)user_id, (void)error_or_null, (void)finished_at, (void)final_message_id;
   if (strcmp(g_status, "queued") != 0 && strcmp(g_status, "running") != 0) {
      return AUTH_DB_NOT_FOUND;
   }
   snprintf(g_status, sizeof(g_status), "%s", status);
   return AUTH_DB_SUCCESS;
}
void job_update_emit(int64_t conv_id, int user_id) {
   (void)conv_id, (void)user_id;
}
static job_kind_cancel_fn g_hook;
int job_manager_register_kind_cancel(const char *kind, job_kind_cancel_fn fn) {
   TEST_ASSERT_EQUAL_STRING(JOB_KIND_CODING, kind);
   g_hook = fn;
   return SUCCESS;
}
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id, (void)user_id, (void)kind;
   free(payload_owned);
}
char *event_payload_spawn(int64_t child_conv_id, const char *title) {
   (void)child_conv_id, (void)title;
   return strdup("{}");
}
const char *job_spawn_origin_string(void) {
   return "webui";
}

/* The job's stored events: seven steps, the last a word from the agent. */
int conv_db_event_list(int64_t conv_id,
                       int user_id,
                       int64_t after_seq,
                       int max,
                       conv_event_t *out,
                       int *count_out) {
   (void)conv_id, (void)user_id;
   int n = 0;
   for (int64_t seq = after_seq + 1; seq <= 7 && n < max; seq++, n++) {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].seq = seq;
      out[n].created_at = time(NULL) - 120;
      snprintf(out[n].kind, sizeof(out[n].kind), "%s", CONV_EVENT_AGENT_STEP);
      char payload[128];
      if (seq == 7) { /* the gateway's own time: ten minutes ago */
         snprintf(payload, sizeof(payload),
                  "{\"gw\":\"progress\",\"gw_ts\":%lld,\"text\":\"tests pass\"}",
                  (long long)(time(NULL) - 600));
      } else {
         snprintf(payload, sizeof(payload),
                  "{\"gw\":\"tool\",\"tool\":\"Read\",\"brief\":\"file%lld.c\"}", (long long)seq);
      }
      out[n].payload = strdup(payload);
   }
   *count_out = n;
   return AUTH_DB_SUCCESS;
}
void conv_db_event_rows_free(conv_event_t *rows, int n) {
   for (int i = 0; i < n; i++) {
      free(rows[i].payload);
      rows[i].payload = NULL;
   }
}

/* --- the session and the turn -------------------------------------------- */

static session_t *g_session;
static uint64_t g_turn = 1;

session_t *session_get_command_context(void) {
   return g_session;
}
int session_effective_user_id(session_t *session) {
   (void)session;
   return 5;
}
int session_default_voice_user_id(void) {
   return 1;
}
bool scheduled_context_get(int *user_id_out) {
   (void)user_id_out;
   return false;
}
bool turn_origin_capture(turn_origin_t *out) {
   memset(out, 0, sizeof(*out));
   out->session_id = 7;
   out->turn_token = g_turn;
   out->turn_number = (int)g_turn;
   return true;
}

/* --- the watcher (its own test) ------------------------------------------- */

void coding_report_stop(void) {
}

int coding_watch_start(void) {
   return SUCCESS;
}
void coding_watch_stop(void) {
}

/* --- the registry ---------------------------------------------------------- */

static tool_metadata_t g_meta;
int tool_registry_register(const tool_metadata_t *metadata) {
   g_meta = *metadata;
   return 0;
}
struct json_object *tool_parse_details(const char *value, bool no_required_fields) {
   if (value == NULL || value[0] == '\0') {
      return json_object_new_object();
   }
   struct json_object *o = json_tokener_parse(value);
   if (o != NULL && json_object_is_type(o, json_type_object)) {
      return o;
   }
   json_object_put(o);
   return no_required_fields ? json_object_new_object() : NULL;
}

/* --- helpers ---------------------------------------------------------------- */

static char *call(const char *action, const char *args) {
   int respond = 0;
   char *v = args ? strdup(args) : NULL;
   char *out = g_meta.callback(action, v, &respond);
   free(v);
   return out;
}

static bool is_error(const char *s) {
   return s && strncmp(s, TOOL_RESULT_ERROR_MARK, strlen(TOOL_RESULT_ERROR_MARK)) == 0;
}

/* The pending id a delegate preview names. */
static unsigned preview_id(const char *preview) {
   static const char key[] = "{\"id\": ";
   const char *p = strstr(preview, key);
   return p ? (unsigned)strtoul(p + strlen(key), NULL, 10) : 0;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.mcp.enabled = true;
   g_config.mcp.server_count = 1;
   g_config.mcp.servers[0].enabled = true;
   snprintf(g_config.mcp.servers[0].alias, sizeof(g_config.mcp.servers[0].alias), "agents");
   snprintf(g_config.mcp.servers[0].role, sizeof(g_config.mcp.servers[0].role), "agent_gateway");
   atomic_store(&g_ncalls, 0);
   g_start_fails = false;
   g_start_lost = 0;
   g_ntasks = 0;
   g_claim_lost = false;
   g_reachable = true;
   g_granted = true;
   g_status[0] = '\0';
   g_turn++;
}

void tearDown(void) {
}

/* Delegate stages and starts nothing; the yes in the next turn starts the job:
 * row, kind, then task_start named after it, then running. */
static void test_delegate_then_confirm_starts_in_order(void) {
   char *preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"fix the oracle\"}");
   TEST_ASSERT_FALSE(is_error(preview));
   TEST_ASSERT_NOT_NULL(strstr(preview, "fix the oracle"));
   const unsigned id = preview_id(preview);
   free(preview);
   TEST_ASSERT_NOT_EQUAL(0, id);
   for (int i = 0; i < atomic_load(&g_ncalls); i++) {
      TEST_ASSERT_NOT_EQUAL(0, strcmp(g_calls[i], "task_start"));
   }

   char out[96];
   int valid = 0;
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", id);
   g_turn++; /* the user's reply */
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         g_meta.describe_call("confirm_delegate", args, out, sizeof(out), &valid));
   TEST_ASSERT_NOT_NULL(strstr(out, "fix the oracle"));

   char *done = call("confirm_delegate", args);
   TEST_ASSERT_FALSE(is_error(done));
   TEST_ASSERT_NOT_NULL(strstr(done, "#42"));
   TEST_ASSERT_NOT_NULL(strstr(done, "agent/t1-fix"));
   free(done);
   TEST_ASSERT_EQUAL_STRING(JOB_KIND_CODING, g_kind);
   TEST_ASSERT_EQUAL_STRING("running", g_status);
   const int last = atomic_load(&g_ncalls) - 1;
   TEST_ASSERT_EQUAL_STRING("task_start", g_calls[last]);
   struct json_object *sent = json_tokener_parse(g_args[last]);
   struct json_object *v = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(sent, "client_ref", &v));
   TEST_ASSERT_EQUAL_STRING("job:42", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(sent, "dawn_user_id", &v));
   TEST_ASSERT_EQUAL_INT(5, json_object_get_int(v));
   json_object_put(sent);
}

/* A question first: the yes two turns later still starts it; a turn that
 * read the agent's report can't confirm. */
static void test_confirm_in_a_later_turn(void) {
   char *preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", preview_id(preview));
   free(preview);
   g_turn++; /* the user asks something */
   g_turn++; /* then reads the agent's report and says yes in that turn */
   char *list = call("list", NULL);
   free(list);
   char *r = call("confirm_delegate", args);
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   g_turn++; /* yes, in a turn of its own */
   r = call("confirm_delegate", args);
   TEST_ASSERT_FALSE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_STRING("running", g_status);
}

/* A confirm in the turn that proposed it, or for another id, starts nothing. */
static void test_confirm_needs_a_later_turn_and_the_id(void) {
   char *preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   const unsigned id = preview_id(preview);
   free(preview);
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", id);
   char *same = call("confirm_delegate", args);
   TEST_ASSERT_TRUE(is_error(same));
   free(same);
   g_turn++;
   snprintf(args, sizeof(args), "{\"id\":%u}", id + 1);
   char *other = call("confirm_delegate", args);
   TEST_ASSERT_TRUE(is_error(other));
   free(other);
   TEST_ASSERT_EQUAL_STRING("", g_status); /* no job row */
   snprintf(args, sizeof(args), "{\"id\":%u}", id);
   char *right = call("confirm_delegate", args);
   TEST_ASSERT_FALSE(is_error(right));
   free(right);
}

static void test_unknown_repo_is_refused_before_staging(void) {
   char *r = call("delegate", "{\"repo\":\"other\",\"task\":\"t\"}");
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
}

/* A refused start fails the job, quietly (the user hears it in the reply). */
static void test_refused_start_fails_the_job(void) {
   char *preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", preview_id(preview));
   free(preview);
   g_turn++;
   g_start_fails = true;
   char *r = call("confirm_delegate", args);
   TEST_ASSERT_TRUE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "too many tasks running"));
   free(r);
   TEST_ASSERT_EQUAL_STRING("failed", g_status);
   TEST_ASSERT_TRUE(g_fired);
   TEST_ASSERT_EQUAL_STRING("task_cancel", g_calls[atomic_load(&g_ncalls) - 1]);
}

/* A start whose answer is lost is asked again under the same name, which
 * returns the task it started; lost twice, it is failed and stopped. */
static void test_lost_start_answer_is_retried_then_stopped(void) {
   char *preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", preview_id(preview));
   free(preview);
   g_turn++;
   g_start_lost = 1;
   char *r = call("confirm_delegate", args);
   TEST_ASSERT_FALSE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_STRING("running", g_status);
   int starts = 0;
   for (int i = 0; i < atomic_load(&g_ncalls); i++) {
      if (strcmp(g_calls[i], "task_start") == 0) {
         starts++;
         TEST_ASSERT_NOT_NULL(strstr(g_args[i], "\"job:42\""));
      }
   }
   TEST_ASSERT_EQUAL_INT(2, starts);

   preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   snprintf(args, sizeof(args), "{\"id\":%u}", preview_id(preview));
   free(preview);
   g_turn++;
   g_start_lost = 2;
   r = call("confirm_delegate", args);
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_STRING("failed", g_status);
   TEST_ASSERT_EQUAL_STRING("task_cancel", g_calls[atomic_load(&g_ncalls) - 1]);
}

/* A turn that read the agent's report can't propose a task. */
static void test_no_delegate_in_a_turn_that_read_the_agent(void) {
   char *list = call("list", NULL);
   free(list);
   char *r = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   g_turn++; /* the user's own next turn */
   r = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   TEST_ASSERT_FALSE(is_error(r));
   free(r);
}

/* A reply-code approval shows the whole task or refuses. */
static void test_long_task_cannot_be_approved_by_code(void) {
   char task[1200];
   memset(task, 'a', sizeof(task) - 1);
   task[sizeof(task) - 1] = '\0';
   char json[1300];
   snprintf(json, sizeof(json), "{\"repo\":\"dawn\",\"task\":\"%s\"}", task);
   char *preview = call("delegate", json);
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", preview_id(preview));
   free(preview);
   g_turn++;
   char out[960];
   int valid = 0;
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         g_meta.describe_call("confirm_delegate", args, out, sizeof(out), &valid));
   TEST_ASSERT_NOT_NULL(strstr(out, "too long"));
}

/* Many tasks: a bounded list that says what it left out, in valid UTF-8. */
static void test_list_is_bounded_and_whole_characters(void) {
   g_ntasks = 60;
   char *r = call("list", NULL);
   TEST_ASSERT_NOT_NULL(r);
   TEST_ASSERT_FALSE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "older not shown"));
   /* No byte of a cut three-byte character: each 0xE2 has its two followers. */
   for (const char *c = strchr(r, '\xe2'); c; c = strchr(c + 1, '\xe2')) {
      TEST_ASSERT_TRUE((unsigned char)c[1] == 0x80 && (unsigned char)c[2] == 0x94);
   }
   free(r);
}

/* Cancelled while it started: the task it started is stopped. */
static void test_lost_claim_cancels_the_started_task(void) {
   char *preview = call("delegate", "{\"repo\":\"dawn\",\"task\":\"t\"}");
   char args[48];
   snprintf(args, sizeof(args), "{\"id\":%u}", preview_id(preview));
   free(preview);
   g_turn++;
   g_claim_lost = true;
   char *r = call("confirm_delegate", args);
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_STRING("failed", g_status);
   TEST_ASSERT_TRUE(g_fired);
   TEST_ASSERT_EQUAL_STRING("task_cancel", g_calls[atomic_load(&g_ncalls) - 1]);
}

/* The cancel hook ends the row here (no notice) and asks the gateway to stop. */
static void test_cancel_hook_ends_the_row_and_asks_the_gateway(void) {
   TEST_ASSERT_NOT_NULL(g_hook);
   snprintf(g_status, sizeof(g_status), "running");
   g_hook(42, 5);
   TEST_ASSERT_EQUAL_STRING("cancelled", g_status);
   TEST_ASSERT_TRUE(g_fired);
   for (int i = 0; i < 100 && atomic_load(&g_ncalls) == 0; i++) {
      usleep(10 * 1000);
   }
   g_meta.stop(); /* waits for the cancel thread to finish */
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_ncalls));
   TEST_ASSERT_EQUAL_STRING("task_cancel", g_calls[0]);
   TEST_ASSERT_NOT_NULL(strstr(g_args[0], "\"job:42\""));
}

/* status says when the agent last acted, its last steps, and that a running
 * run's spend isn't counted yet. */
static void test_status_shows_recent_steps(void) {
   snprintf(g_status, sizeof(g_status), "running");
   snprintf(g_kind, sizeof(g_kind), "%s", JOB_KIND_CODING);
   char *r = call("status", "{\"job\":42}");
   TEST_ASSERT_FALSE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "Last activity: 2 min ago."));
   TEST_ASSERT_NULL(strstr(r, "file2.c")); /* only the last five */
   TEST_ASSERT_NOT_NULL(strstr(r, "Read: file3.c"));
   TEST_ASSERT_NOT_NULL(strstr(r, "10 min ago, said: tests pass"));
   TEST_ASSERT_NOT_NULL(strstr(r, "isn't in the total yet"));
   free(r);
}

static void test_list_shows_repos(void) {
   char *r = call("list", NULL);
   TEST_ASSERT_FALSE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "dawn (base main)"));
   free(r);
}

/* Without the user's grant for the gateway, nothing is called. */
static void test_needs_the_gateway_grant(void) {
   g_granted = false;
   char *r = call("list", NULL);
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_ncalls));
}

int main(void) {
   g_session = calloc(1, sizeof(session_t));
   g_session->type = SESSION_TYPE_WEBUI;
   setUp(); /* a declared gateway, so init registers the cancel hook */
   TEST_ASSERT_EQUAL_INT(0, coding_agent_tool_register());
   TEST_ASSERT_EQUAL_INT(0, g_meta.init());
   UNITY_BEGIN();
   RUN_TEST(test_delegate_then_confirm_starts_in_order);
   RUN_TEST(test_confirm_needs_a_later_turn_and_the_id);
   RUN_TEST(test_confirm_in_a_later_turn);
   RUN_TEST(test_unknown_repo_is_refused_before_staging);
   RUN_TEST(test_refused_start_fails_the_job);
   RUN_TEST(test_lost_start_answer_is_retried_then_stopped);
   RUN_TEST(test_no_delegate_in_a_turn_that_read_the_agent);
   RUN_TEST(test_long_task_cannot_be_approved_by_code);
   RUN_TEST(test_list_is_bounded_and_whole_characters);
   RUN_TEST(test_lost_claim_cancels_the_started_task);
   RUN_TEST(test_list_shows_repos);
   RUN_TEST(test_status_shows_recent_steps);
   RUN_TEST(test_needs_the_gateway_grant);
   RUN_TEST(test_cancel_hook_ends_the_row_and_asks_the_gateway);
   free(g_session);
   return UNITY_END();
}
