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
 * coding_agent's GitHub reads: framed as web content and marking the turn
 * read, {job} sent as its client_ref, the blockers said in plain words, a
 * gateway refusal passed on.  The gateway, the job store and the session are
 * stubs; the tool's other parts are stubbed out.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "tools/coding_agent_internal.h"
#include "tools/coding_agent_tool.h"
#include "tools/tool_registry.h"
#include "unity.h"

dawn_config_t g_config;

/* --- the gateway ----------------------------------------------------------- */

static char g_tool[64];       /* the last tool called */
static char g_args[1024];     /* its arguments */
static const char *g_answer;  /* what it answers */
static const char *g_refusal; /* or the reason it refuses */

#define HEAD "ed4167b01d630000000000000000000000000000"
#define STATUS                                                                                  \
   "{\"pr\":{\"owner\":\"o\",\"repo\":\"dawn\",\"number\":115,\"url\":"                         \
   "\"https://github.com/o/dawn/pull/115\",\"title\":\"Fix it\",\"author\":\"kris\","           \
   "\"state\":\"open\",\"draft\":false,\"merged\":false,\"mergeable\":\"mergeable\","           \
   "\"merge_state\":\"behind\",\"behind\":true,\"base\":\"main\",\"head_ref\":\"agent/x\","     \
   "\"head_sha\":\"" HEAD "\",\"review_decision\":null},"                                       \
   "\"checks\":[{\"name\":\"format-check\",\"required\":true,\"status\":\"completed\","         \
   "\"conclusion\":\"failure\",\"failed\":true,\"job_id\":133129755659,\"app\":\"github-"       \
   "actions\",\"url\":null},"                                                                   \
   "{\"name\":\"build\",\"required\":true,\"status\":\"completed\",\"conclusion\":\"success\"," \
   "\"failed\":false,"                                                                          \
   "\"job_id\":1,\"app\":\"github-actions\",\"url\":null}],\"checks_total\":2,"                 \
   "\"reviews\":[{\"login\":\"copilot-pull-request-reviewer\",\"state\":\"commented\","         \
   "\"commit\":\"" HEAD "\"}],\"requested\":[],\"copilot\":\"reviewed_head\","                  \
   "\"threads\":{\"unresolved\":4,\"resolved\":1,\"total\":5},"                                 \
   "\"configured_repo\":\"dawn\",\"job\":\"job:42.1000\"}"
#define THREADS                                                                            \
   "{\"threads\":[{\"id\":\"PRRT_1\",\"comment_id\":2501,\"n\":1,\"path\":\"src/x.c\","    \
   "\"line\":42,\"outdated\":false,\"resolved\":false,\"comments_total\":9,\"comments\":[" \
   "{\"comment_id\":2501,\"author\":\"copilot-pull-request-reviewer\",\"bot\":true,"       \
   "\"body\":\"Use safe_strscpy\\nhere.\"},{\"comment_id\":2509,\"author\":\"kris\","      \
   "\"bot\":false,\"body\":\"Done.\"}]}],\"reviews\":[{\"author\":\"alice\",\"state\":"    \
   "\"changes_requested\",\"body\":\"Split it.\"}],\"comments\":[],\"as_login\":\"kris\"," \
   "\"head_sha\":\"" HEAD "\",\"total\":1,\"truncated\":false}"
#define LOG                                                                           \
   "{\"name\":\"format-check\",\"conclusion\":\"failure\",\"url\":"                   \
   "\"https://github.com/o/dawn/actions/runs/9/job/133129755659\",\"excerpt\":"       \
   "\"src/x.c:4: error: bad indent\\n##[error]Process completed with exit code 1.\"," \
   "\"excerpt_bytes\":70,\"log_bytes_read\":20645,\"truncated\":false}"

int mcp_bridge_ensure_connected(const char *alias) {
   (void)alias;
   return SUCCESS;
}
bool mcp_bridge_is_agent_gateway(const char *alias) {
   (void)alias;
   return true;
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
   (void)alias, (void)timeout_ms;
   snprintf(g_tool, sizeof(g_tool), "%s", tool);
   snprintf(g_args, sizeof(g_args), "%s", args_json ? args_json : "");
   if (g_refusal != NULL) {
      *result_out = strdup(g_refusal);
      return FAILURE;
   }
   *result_out = strdup(g_answer ? g_answer : "{}");
   return SUCCESS;
}
int auth_db_mcp_check_access(int64_t user_id, const char *alias, bool *allowed_out) {
   (void)alias;
   *allowed_out = user_id == 5;
   return AUTH_DB_SUCCESS;
}

/* --- the job store: job 42 is the user's coding job, created at 1000 -------- */

int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   if (conv_id != 42 || user_id != 5) {
      return AUTH_DB_NOT_FOUND;
   }
   memset(out, 0, sizeof(*out));
   out->id = 42;
   out->user_id = 5;
   snprintf(out->job_status, sizeof(out->job_status), "done");
   snprintf(out->job_kind, sizeof(out->job_kind), "%s", JOB_KIND_CODING);
   return AUTH_DB_SUCCESS;
}
int conv_db_get_created_at(int64_t conv_id, int64_t *created_at_out) {
   *created_at_out = conv_id == 42 ? 1000 : 0;
   return *created_at_out ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}
int conv_db_job_mark_fired(int64_t conv_id) {
   (void)conv_id;
   return AUTH_DB_SUCCESS;
}
int job_manager_set_terminal_if_active(int64_t conv_id,
                                       int user_id,
                                       const char *status,
                                       const char *error_or_null,
                                       time_t finished_at,
                                       int64_t final_message_id) {
   (void)conv_id, (void)user_id, (void)status, (void)error_or_null, (void)finished_at;
   (void)final_message_id;
   return AUTH_DB_SUCCESS;
}
int job_manager_register_kind_cancel(const char *kind, job_kind_cancel_fn fn) {
   (void)kind, (void)fn;
   return SUCCESS;
}
int conv_db_event_list(int64_t conv_id,
                       int user_id,
                       int64_t after_seq,
                       int max,
                       conv_event_t *out,
                       int *count_out) {
   (void)conv_id, (void)user_id, (void)after_seq, (void)max, (void)out;
   *count_out = 0;
   return AUTH_DB_SUCCESS;
}
void conv_db_event_rows_free(conv_event_t *rows, int n) {
   (void)rows, (void)n;
}
int conv_db_job_holds_agent_work(int64_t conv_id, int user_id, int64_t *holder) {
   (void)conv_id, (void)user_id;
   *holder = 0;
   return AUTH_DB_SUCCESS;
}

/* --- the session and the turn ---------------------------------------------- */

static session_t g_session_store;
static session_t *g_session = &g_session_store;
static uint64_t g_turn = 1;

session_t *session_get_command_context(void) {
   return g_session;
}
bool turn_origin_capture(turn_origin_t *out) {
   memset(out, 0, sizeof(*out));
   out->session_id = g_session->session_id;
   out->turn_token = g_turn;
   out->turn_number = (uint32_t)g_turn;
   return true;
}
void turn_origin_note_answered(struct session *session, uint32_t made_turn) {
   (void)session, (void)made_turn;
}
bool session_posts_told_by_turn(struct session *session,
                                uint64_t turn_token,
                                int64_t conv_id,
                                int64_t msg_id) {
   (void)session, (void)turn_token, (void)conv_id, (void)msg_id;
   return false;
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

/* --- the tool's other parts (their own tests) -------------------------------- */

#define NOT_HERE(fn)                                                            \
   char *fn(struct json_object *details, int user_id, const turn_origin_t *o) { \
      (void)details, (void)user_id, (void)o;                                    \
      return strdup("stub");                                                    \
   }
NOT_HERE(coding_delegate_confirm)
NOT_HERE(coding_revise_propose)
NOT_HERE(coding_revise_confirm)
NOT_HERE(coding_publish_propose)
NOT_HERE(coding_publish_confirm)
NOT_HERE(coding_close)
NOT_HERE(coding_close_confirm)
NOT_HERE(coding_requests_approve)
NOT_HERE(coding_requests_confirm_approve)
char *coding_delegate_propose(struct json_object *details,
                              int user_id,
                              int64_t parent_conv,
                              const turn_origin_t *origin) {
   (void)details, (void)user_id, (void)parent_conv, (void)origin;
   return strdup("stub");
}
char *coding_requests_deny(struct json_object *details, int user_id) {
   (void)details, (void)user_id;
   return strdup("stub");
}
char *coding_requests_answer(struct json_object *details, int user_id) {
   (void)details, (void)user_id;
   return strdup("stub");
}
char *coding_requests_grants(int user_id) {
   (void)user_id;
   return strdup("stub");
}
char *coding_requests_revoke(struct json_object *details, int user_id) {
   (void)details, (void)user_id;
   return strdup("stub");
}
int coding_delegate_describe(const char *value, char *out, size_t out_len, int *valid_for_sec) {
   (void)value, (void)out, (void)out_len, (void)valid_for_sec;
   return TOOL_DESCRIBE_DEFAULT;
}
int coding_revise_describe(const char *value, char *out, size_t out_len, int *valid_for_sec) {
   (void)value, (void)out, (void)out_len, (void)valid_for_sec;
   return TOOL_DESCRIBE_DEFAULT;
}
int coding_requests_describe(const char *value, char *out, size_t out_len, int *valid_for_sec) {
   (void)value, (void)out, (void)out_len, (void)valid_for_sec;
   return TOOL_DESCRIBE_DEFAULT;
}
int coding_publish_describe(const char *value,
                            bool close,
                            char *out,
                            size_t out_len,
                            int *valid_for_sec) {
   (void)value, (void)close, (void)out, (void)out_len, (void)valid_for_sec;
   return TOOL_DESCRIBE_DEFAULT;
}
bool coding_publish_pr_url_ok(const char *url) {
   (void)url;
   return true;
}
void coding_report_stop(void) {
}
int coding_watch_start(void) {
   return SUCCESS;
}
void coding_watch_stop(void) {
}

/* --- the registry ------------------------------------------------------------ */

static tool_metadata_t g_meta;
int tool_registry_register(const tool_metadata_t *metadata) {
   g_meta = *metadata;
   return 0;
}
static const tool_action_kind_entry_t *kind_entry(const char *action) {
   for (int i = 0; i < g_meta.action_kind_count; i++) {
      if (strcmp(g_meta.action_kinds[i].action, action) == 0) {
         return &g_meta.action_kinds[i];
      }
   }
   return NULL;
}
tool_action_kind_t tool_action_kind(const tool_metadata_t *meta,
                                    const char *device,
                                    const char *action,
                                    const char *value) {
   (void)meta, (void)device, (void)value;
   const tool_action_kind_entry_t *e = kind_entry(action);
   return e ? e->kind : TOOL_KIND_ACT;
}
struct json_object *tool_parse_details(const char *value, bool no_required_fields) {
   (void)no_required_fields;
   return value && value[0] ? json_tokener_parse(value) : json_object_new_object();
}

/* --- helpers ------------------------------------------------------------------ */

static char *call(const char *action, const char *args) {
   int respond = 0;
   char *v = strdup(args);
   char *out = g_meta.callback(action, v, &respond);
   free(v);
   return out;
}

static bool is_error(const char *s) {
   return s && strncmp(s, TOOL_RESULT_ERROR_MARK, strlen(TOOL_RESULT_ERROR_MARK)) == 0;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.mcp.enabled = true;
   g_config.mcp.server_count = 1;
   g_config.mcp.servers[0].enabled = true;
   snprintf(g_config.mcp.servers[0].alias, sizeof(g_config.mcp.servers[0].alias), "agents");
   snprintf(g_config.mcp.servers[0].role, sizeof(g_config.mcp.servers[0].role), "agent_gateway");
   g_session_store.session_id = 3;
   g_tool[0] = g_args[0] = '\0';
   g_answer = NULL;
   g_refusal = NULL;
   g_turn++;
}

void tearDown(void) {
}

/* --- tests -------------------------------------------------------------------- */

/* GitHub's text is someone else's: each read is framed as web content, and a
 * confirm in the same turn would rest on it. */
static void test_reads_are_web_and_mark_the_turn(void) {
   static const struct {
      const char *action, *args, *answer, *tool;
   } reads[] = {
      { "pr_status", "{\"ref\": \"o/dawn#115\"}", STATUS, "pr_status" },
      { "pr_threads", "{\"ref\": \"o/dawn#115\"}", THREADS, "pr_threads" },
      { "pr_log", "{\"ref\": \"o/dawn#115\", \"check\": \"format-check\"}", LOG, "pr_check_log" },
   };
   for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
      const tool_action_kind_entry_t *e = kind_entry(reads[i].action);
      TEST_ASSERT_NOT_NULL(e);
      TEST_ASSERT_EQUAL(TOOL_KIND_READ, e->kind);
      TEST_ASSERT_EQUAL_STRING(TOOL_FRAME_WEB, e->third_party);
      g_turn++;
      turn_origin_t o;
      turn_origin_capture(&o);
      TEST_ASSERT_FALSE(coding_agent_turn_read(&o));
      g_answer = reads[i].answer;
      char *out = call(reads[i].action, reads[i].args);
      TEST_ASSERT_FALSE_MESSAGE(is_error(out), out);
      TEST_ASSERT_EQUAL_STRING(reads[i].tool, g_tool);
      TEST_ASSERT_TRUE(coding_agent_turn_read(&o));
      free(out);
   }
}

/* {job} names the job's task at the gateway, which finds its pull request. */
static void test_job_is_sent_as_its_client_ref(void) {
   g_answer = STATUS;
   free(call("pr_status", "{\"job\": 42}"));
   struct json_object *sent = json_tokener_parse(g_args);
   TEST_ASSERT_EQUAL_STRING("job:42.1000",
                            json_object_get_string(json_object_object_get(sent, "client_ref")));
   TEST_ASSERT_NULL(json_object_object_get(sent, "ref"));
   TEST_ASSERT_EQUAL_INT(5, json_object_get_int(json_object_object_get(sent, "dawn_user_id")));
   json_object_put(sent);

   g_answer = LOG;
   free(call("pr_log", "{\"job\": 42, \"check\": \"133129755659\"}"));
   sent = json_tokener_parse(g_args);
   TEST_ASSERT_EQUAL_STRING("133129755659",
                            json_object_get_string(json_object_object_get(sent, "check")));
   json_object_put(sent);

   g_tool[0] = '\0';
   char *out = call("pr_status", "{\"job\": 7}"); /* not the user's coding job */
   TEST_ASSERT_TRUE(is_error(out));
   TEST_ASSERT_EQUAL_STRING("", g_tool);
   free(out);
}

/* ref and repo pass through; a call naming nothing, or too much, is refused here. */
static void test_ref_and_repo_pass_through(void) {
   g_answer = THREADS;
   free(call("pr_threads", "{\"ref\": 115, \"repo\": \"dawn\", \"all\": true}"));
   TEST_ASSERT_EQUAL_STRING("pr_threads", g_tool);
   struct json_object *sent = json_tokener_parse(g_args);
   TEST_ASSERT_EQUAL_STRING("115", json_object_get_string(json_object_object_get(sent, "ref")));
   TEST_ASSERT_EQUAL_STRING("dawn", json_object_get_string(json_object_object_get(sent, "repo")));
   TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(sent, "all")));
   json_object_put(sent);

   char long_ref[400];
   memset(long_ref, 'a', sizeof(long_ref) - 1);
   long_ref[sizeof(long_ref) - 1] = '\0';
   char args[512];
   snprintf(args, sizeof(args), "{\"ref\": \"%s\"}", long_ref);
   const char *refused[][2] = { { "pr_status", "{}" },
                                { "pr_status", args },
                                { "pr_log", "{\"ref\": \"o/dawn#1\"}" } };
   for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
      g_tool[0] = '\0';
      char *out = call(refused[i][0], refused[i][1]);
      TEST_ASSERT_TRUE(is_error(out));
      TEST_ASSERT_EQUAL_STRING("", g_tool);
      free(out);
   }
}

/* The blockers come first, in plain words. */
static void test_status_says_the_blockers(void) {
   g_answer = STATUS;
   char *out = call("pr_status", "{\"ref\": \"o/dawn#115\"}");
   TEST_ASSERT_NOT_NULL(strstr(out, "Pull request o/dawn#115: Fix it\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Required check failing: format-check (job 133129755659). "
                                    "Behind main. 4 unresolved threads. "
                                    "Copilot reviewed the current head.\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "- format-check (job 133129755659): failure (required)\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 of the 2 listed passed"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Coding job #42 published it."));
   free(out);

   /* Nothing failing, up to date: said so, with GitHub's own word for it. */
   g_answer = "{\"pr\":{\"owner\":\"x\",\"repo\":\"y\",\"number\":3,\"state\":\"open\","
              "\"merge_state\":\"clean\",\"base\":\"main\",\"head_sha\":\"" HEAD "\"},"
              "\"checks\":[],\"checks_total\":0,\"checks_note\":\"checks unreadable: the token "
              "lacks Actions: Read-only and Commit statuses: Read-only for x/y\","
              "\"copilot\":\"reviewed_head\",\"threads\":{\"unresolved\":0,\"resolved\":0},"
              "\"configured_repo\":null,\"job\":null}";
   out = call("pr_status", "{\"ref\": \"x/y#3\"}");
   TEST_ASSERT_NOT_NULL(strstr(out, "Nothing DAWN can see blocks it (GitHub's merge state: "
                                    "clean). Copilot reviewed the current head.\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Checks: checks unreadable: the token lacks Actions"));
   TEST_ASSERT_NOT_NULL(strstr(out, "It isn't in a repo the coding agent works in."));
   free(out);

   /* Just after main moved: not yet known, and not said to be clear. */
   g_answer = "{\"pr\":{\"owner\":\"x\",\"repo\":\"y\",\"number\":4,\"state\":\"open\","
              "\"merge_state\":\"unknown\",\"mergeable\":\"unknown\",\"base\":\"main\"},"
              "\"checks\":[],\"checks_total\":0,\"copilot\":\"none\","
              "\"threads\":{\"unresolved\":0,\"resolved\":0},\"configured_repo\":null,"
              "\"job\":null}";
   out = call("pr_status", "{\"ref\": \"x/y#4\"}");
   TEST_ASSERT_NOT_NULL(strstr(out, "GitHub is still working out whether it is behind main or "
                                    "conflicts; ask again in a moment.\n"));
   TEST_ASSERT_NULL(strstr(out, "Nothing DAWN can see"));
   free(out);
}

static void test_threads_and_log_texts(void) {
   g_answer = THREADS;
   char *out = call("pr_threads", "{\"ref\": \"o/dawn#115\"}");
   TEST_ASSERT_NOT_NULL(strstr(out, "Unresolved review threads at head ed4167b0: 1.\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "1) comment_id 2501, src/x.c:42\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "   copilot-pull-request-reviewer (bot): Use safe_strscpy "
                                    "here.\n   (7 comments between not shown)\n   kris: Done.\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "- [changes_requested] alice: Split it.\n"));
   free(out);

   g_answer = LOG;
   out = call("pr_log", "{\"ref\": \"o/dawn#115\", \"check\": \"format-check\"}");
   TEST_ASSERT_NOT_NULL(strstr(out, "Log of check format-check (failure)"));
   TEST_ASSERT_NOT_NULL(strstr(out, "of 20645 bytes read:\nsrc/x.c:4: error: bad indent\n"));
   free(out);
}

/* The gateway's reason reaches the model; the turn isn't marked by a refusal. */
static void test_gateway_refusal_is_passed_on(void) {
   g_refusal = "the token lacks Actions: Read-only for o/private";
   turn_origin_t o;
   turn_origin_capture(&o);
   char *out = call("pr_log", "{\"ref\": \"o/private#2\", \"check\": \"build\"}");
   TEST_ASSERT_TRUE(is_error(out));
   TEST_ASSERT_NOT_NULL(strstr(out, "the token lacks Actions: Read-only for o/private"));
   TEST_ASSERT_FALSE(coding_agent_turn_read(&o));
   free(out);
}

int main(void) {
   coding_agent_tool_register();
   UNITY_BEGIN();
   RUN_TEST(test_reads_are_web_and_mark_the_turn);
   RUN_TEST(test_job_is_sent_as_its_client_ref);
   RUN_TEST(test_ref_and_repo_pass_through);
   RUN_TEST(test_status_says_the_blockers);
   RUN_TEST(test_threads_and_log_texts);
   RUN_TEST(test_gateway_refusal_is_passed_on);
   return UNITY_END();
}
