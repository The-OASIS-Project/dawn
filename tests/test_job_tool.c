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
 * The `job` tool's guards: a call with no session context (an MQTT publish
 * naming no session) is refused for every action, before any job is read,
 * changed or started; and a resume asks as the tool, so it can't restart a job
 * the user cancelled.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/job_worker.h"
#include "core/memory_filter.h"
#include "core/scheduled_context.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "tools/job_tool.h"
#include "tools/tool_registry.h"
#include "unity.h"

/* ---- stubs: the caller ---- */

static session_t *s_ctx;

session_t *session_get_command_context(void) {
   return s_ctx;
}
int session_effective_user_id(session_t *session) {
   return session ? 1 : 0;
}
int session_default_voice_user_id(void) {
   return 1; /* what a context-less call would otherwise run as */
}
bool scheduled_context_get(int *user_id_out) {
   (void)user_id_out;
   return false;
}

/* ---- stubs: what the actions reach (counted, to prove a refusal reached none) ---- */

static int s_reached;
static job_resume_result_t s_resume_result;

int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   (void)conv_id, (void)user_id;
   s_reached++;
   memset(out, 0, sizeof(*out));
   return AUTH_DB_NOT_FOUND;
}
int conv_db_job_list_by_user(int user_id, job_record_t *out, int max, int *count_out) {
   (void)user_id, (void)out, (void)max;
   s_reached++;
   *count_out = 0;
   return AUTH_DB_SUCCESS;
}
int conv_db_get_messages(int64_t conv_id, int user_id, message_callback_t callback, void *ctx) {
   (void)conv_id, (void)user_id, (void)callback, (void)ctx;
   s_reached++;
   return AUTH_DB_SUCCESS;
}
int conv_db_create_job_ex(int user_id,
                          const char *title,
                          int64_t parent_id,
                          const char *spawn_mode,
                          const char *on_complete,
                          const char *deliver_to,
                          int spawn_depth,
                          const char *goal,
                          const char *origin,
                          const conv_llm_columns_t *llm,
                          int64_t *conv_id_out) {
   (void)user_id, (void)title, (void)parent_id, (void)spawn_mode, (void)on_complete;
   (void)deliver_to, (void)spawn_depth, (void)goal, (void)origin, (void)llm;
   s_reached++;
   *conv_id_out = 0;
   return AUTH_DB_FAILURE;
}
job_resume_result_t job_worker_resume(int64_t conv_id,
                                      int user_id,
                                      job_resume_origin_t origin,
                                      char *reason,
                                      size_t reason_len) {
   (void)conv_id, (void)user_id;
   (void)reason, (void)reason_len;
   /* The model may not undo a Stop the user asked for: the tool asks as the tool. */
   TEST_ASSERT_EQUAL_INT(JOB_RESUME_ORIGIN_TOOL, origin);
   s_reached++;
   return s_resume_result;
}
job_cancel_result_t job_manager_cancel_or_retire(int64_t conv_id,
                                                 int user_id,
                                                 char *status_out,
                                                 size_t status_n) {
   (void)conv_id, (void)user_id, (void)status_out, (void)status_n;
   s_reached++;
   return JOB_CANCEL_NOT_FOUND;
}
int job_worker_spawn(int user_id, int64_t conv_id, const char *goal) {
   (void)user_id, (void)conv_id, (void)goal;
   s_reached++;
   return FAILURE;
}
int job_manager_capacity(int user_id, job_provider_class_t provider) {
   (void)user_id, (void)provider;
   s_reached++;
   return JOB_MGR_OK;
}
int job_manager_set_terminal(int64_t conv_id,
                             int user_id,
                             const char *status,
                             const char *error_or_null,
                             time_t finished_at,
                             int64_t final_message_id) {
   (void)conv_id, (void)user_id, (void)status, (void)error_or_null, (void)finished_at;
   (void)final_message_id;
   return SUCCESS;
}
void job_update_emit(int64_t conv_id, int user_id) {
   (void)conv_id, (void)user_id;
}
void job_manager_mark_dirty(void) {
}
const char *job_spawn_origin_string(void) {
   return "job";
}
void job_selection_of_spawner(session_llm_config_t *out) {
   memset(out, 0, sizeof(*out));
}
void job_selection_columns(const session_llm_config_t *cfg, conv_llm_columns_t *out) {
   (void)cfg;
   memset(out, 0, sizeof(*out));
}
job_provider_class_t job_provider_for(const session_llm_config_t *cfg) {
   (void)cfg;
   return JOB_PROVIDER_CLOUD;
}
char *event_payload_spawn(int64_t child_conv_id, const char *title) {
   (void)child_conv_id, (void)title;
   return NULL;
}
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id, (void)user_id, (void)kind;
   free(payload_owned);
}
void conv_generate_title(const char *content, char *title_out, size_t max_len) {
   (void)content;
   snprintf(title_out, max_len, "job");
}
bool memory_filter_check(const char *text) {
   (void)text;
   return false;
}
bool memory_filter_check_injection_commands(const char *text) {
   (void)text;
   return false;
}
const char *memory_filter_injection_command_match(const char *text) {
   (void)text;
   return NULL;
}

/* ---- stubs: the registry (captures the callback) ---- */

static tool_callback_fn s_callback;

int tool_registry_register(const tool_metadata_t *metadata) {
   s_callback = metadata->callback;
   return SUCCESS;
}
struct json_object *tool_parse_details(const char *value, bool no_required_fields) {
   (void)no_required_fields;
   struct json_object *o = value ? json_tokener_parse(value) : NULL;
   return o ? o : json_object_new_object();
}

/* ---- helpers ---- */

static char *call(const char *action, const char *args) {
   char *value = args ? strdup(args) : NULL;
   int respond = 0;
   char *out = s_callback(action, value, &respond);
   free(value);
   return out;
}

void setUp(void) {
   s_ctx = NULL;
   s_reached = 0;
   s_resume_result = JOB_RESUME_STARTED;
   if (s_callback == NULL) {
      TEST_ASSERT_EQUAL_INT(SUCCESS, job_tool_register());
   }
   TEST_ASSERT_NOT_NULL(s_callback);
}

void tearDown(void) {
   free(s_ctx);
   s_ctx = NULL;
}

/* With no session context every action is refused, reads included, and none
 * reaches a job: `list`/`status` would show the default user's jobs to an MQTT
 * publisher, `cancel` would stop them. */
static void test_no_context_refuses_every_action(void) {
   static const char *const actions[] = { "list", "status", "cancel", "resume", "spawn" };
   for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
      char *out = call(actions[i], "{\"job_id\": 5, \"goal\": \"look it up\"}");
      TEST_ASSERT_NOT_NULL(out);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "only be used from a user session"), actions[i]);
      free(out);
   }
   TEST_ASSERT_EQUAL_INT(0, s_reached);
}

/* A caller with a session still reaches the job. */
static void test_session_caller_reaches_the_job(void) {
   s_ctx = calloc(1, sizeof(*s_ctx));
   TEST_ASSERT_NOT_NULL(s_ctx);
   s_ctx->type = SESSION_TYPE_WEBUI;
   char *out = call("status", "{\"job_id\": 5}");
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NULL(strstr(out, "only be used from a user session"));
   free(out);
   TEST_ASSERT_TRUE(s_reached > 0);
}

/* A resume from the tool reaches job_worker_resume as the tool (the stub checks
 * the origin) and says it's resuming. */
static void test_resume_asks_as_the_tool(void) {
   s_ctx = calloc(1, sizeof(*s_ctx));
   TEST_ASSERT_NOT_NULL(s_ctx);
   s_ctx->type = SESSION_TYPE_WEBUI;
   char *out = call("resume", "{\"job_id\": 5}");
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Resuming background job #5"));
   free(out);
   TEST_ASSERT_EQUAL_INT(1, s_reached);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_no_context_refuses_every_action);
   RUN_TEST(test_session_caller_reaches_the_job);
   RUN_TEST(test_resume_asks_as_the_tool);
   return UNITY_END();
}
