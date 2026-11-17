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
 * Memory extraction end to end, on a real schema and the real history
 * loader, with a fake model: memory_trigger_extraction() and its detached
 * thread run as in the daemon, and the model call (wrapped at link time, so
 * no request ever leaves the process) records the prompt it was sent and
 * returns a scripted reply.  Covers a history that lacks another session's
 * rows, a complete one, another session's web result in the batch, and
 * memory recovery's progress check on a count stored in older units.  The
 * session unit is linked too, so a test can run a turn on a session (the real
 * dispatch, prefix and history, a fake model) and then extract its
 * conversation in the same process, or two sessions on one conversation
 * taking turns, each caught up at its turn's start.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "auth/auth_db_messages.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "core/llm_response_finalize.h"
#include "core/outside_kind.h"
#include "core/prompt_parts.h"
#include "core/session_compaction.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/text_input_dispatch.h"
#include "llm/llm_interface.h"
#include "llm/llm_third_party.h"
#include "llm/llm_tool_history.h"
#include "llm/llm_tools.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
#include "memory/memory_history_loader.h"
#include "memory/memory_recovery.h"
#include "tools/tool_registry.h"
#include "unity.h"
#include "utils/string_utils.h"

/* Never initialized here, so nothing is embedded. */
const embedding_provider_t embedding_provider_onnx = { .name = "onnx" };

#define WAIT_SEC 10

static int s_user;

/* The fake model: what it was last sent, how often it was called, and what
 * it answers.  Written on the extraction thread; read after
 * memory_extraction_wait_idle(), whose lock orders the two. */
static char *s_prompt;
static int s_calls;
static const char *s_reply;
static void (*s_during_call)(void); /* run inside the call (a row saved meanwhile) */

/* A gate the call can wait at, so a test decides when it returns. */
static pthread_mutex_t s_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gate_cond = PTHREAD_COND_INITIALIZER;
static bool s_gated;
static bool s_entered;
static bool s_open;

static struct timespec deadline(void) {
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += WAIT_SEC;
   return ts;
}

char *__wrap_llm_chat_completion_with_config(struct json_object *conversation_history,
                                             const char *input_text,
                                             const llm_resolved_config_t *config) {
   (void)conversation_history;
   (void)config;
   free(s_prompt);
   s_prompt = input_text ? strdup(input_text) : NULL;
   s_calls++;
   if (s_during_call) {
      s_during_call();
   }
   pthread_mutex_lock(&s_gate_mutex);
   s_entered = true;
   pthread_cond_broadcast(&s_gate_cond);
   const struct timespec until = deadline();
   while (s_gated && !s_open) {
      if (pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &until) == ETIMEDOUT) {
         break;
      }
   }
   pthread_mutex_unlock(&s_gate_mutex);
   return s_reply ? strdup(s_reply) : NULL;
}

/* The turn's model (the session unit's call, wrapped like the extraction's):
 * the answer it gives, after a tool exchange when s_turn_tool names its
 * result (run on s_turn_session, saved through its persist hook). */
static const char *s_turn_reply;
static const char *s_turn_tool;
static session_t *s_turn_session;

/* A tool exchange as the tool loop appends and saves it, on a Claude path:
 * the call, then its result in a user message. */
static void run_tool_exchange(struct json_object *history) {
   const int before = (int)json_object_array_length(history);
   llm_tool_response_t response;
   memset(&response, 0, sizeof(response));
   response.text = "Checking.";
   response.tool_calls.count = 1;
   response.has_tool_calls = true;
   snprintf(response.tool_calls.calls[0].id, LLM_TOOLS_ID_LEN, "toolu_1");
   snprintf(response.tool_calls.calls[0].name, LLM_TOOLS_NAME_LEN, "weather");
   snprintf(response.tool_calls.calls[0].arguments, LLM_TOOLS_ARGS_LEN, "{}");
   tool_result_list_t *results = calloc(1, sizeof(*results));
   TEST_ASSERT_NOT_NULL(results);
   results->count = 1;
   snprintf(results->results[0].tool_call_id, LLM_TOOLS_ID_LEN, "toolu_1");
   snprintf(results->results[0].result, sizeof(results->results[0].result), "%s", s_turn_tool);
   results->results[0].success = true;
   llm_tool_history_append_claude(history, &response, results, "api.anthropic.com#0");
   llm_tool_history_persist(s_turn_session, history, before, NULL, 1, results);
   free(results);
}

char *__wrap_llm_chat_completion_streaming_with_config(struct json_object *conversation_history,
                                                       const char *input_text,
                                                       llm_text_chunk_callback chunk_callback,
                                                       void *callback_userdata,
                                                       const llm_resolved_config_t *config) {
   (void)input_text, (void)chunk_callback, (void)callback_userdata;
   (void)config;
   if (s_turn_tool) {
      run_tool_exchange(conversation_history);
   }
   return s_turn_reply ? strdup(s_turn_reply) : NULL;
}

/* The turn's persist hook: each row into the conversation, its id back. */
static int64_t persist_row(void *userdata, const session_tool_row_t *row) {
   const conv_message_row_t db_row = { .role = row->role,
                                       .content = row->content,
                                       .tool_calls = row->tool_calls,
                                       .tool_call_id = row->tool_call_id,
                                       .llm_blocks = row->llm_blocks,
                                       .kind = row->kind };
   int64_t id = 0;
   return conv_db_add_row(*(const int64_t *)userdata, s_user, &db_row, &id) == AUTH_DB_SUCCESS ? id
                                                                                               : 0;
}

/* ---- the runtime a turn calls besides the session unit, stubbed ---- */

/* The prompt each turn applies (session_manager.c's dispatch): a fixed one. */
int session_dispatch_user_turn_ex(session_t *session,
                                  const char *user_turn_text,
                                  const char *turn_note) {
   (void)user_turn_text;
   composed_prompt_t cp = { .directives = strdup("D") };
   prompt_sections_add(&cp, "persona", "your persona", "P");
   cp.stable_prefix = prompt_sections_join(&cp);
   cp.context_head = strdup("[system_time] 09:00\n");
   session_prefix_apply_turn(session, &cp, turn_note);
   composed_prompt_free(&cp);
   return SUCCESS;
}
/* The thread's turn token, as session_manager.c keeps it: a turn's rows are
 * added only while its token is the thread's. */
static __thread uint64_t tl_turn_token;
uint64_t session_turn_token(void) {
   return tl_turn_token;
}
void session_set_turn_token(uint64_t token) {
   tl_turn_token = token;
}
/* The finalizer's own work is tested elsewhere: the text as given. */
int llm_response_finalize(session_t *session, const char *raw_response, response_final_t *out) {
   (void)session;
   out->text = strdup(raw_response);
   return out->text ? SUCCESS : FAILURE;
}
void session_touch(session_t *session) {
   (void)session;
}
void session_manager_for_each_session_any(void (*fn)(session_t *session, void *ctx), void *ctx) {
   (void)fn, (void)ctx;
}
void job_manager_for_each_session(void (*fn)(session_t *session, void *ctx), void *ctx) {
   (void)fn, (void)ctx;
}
bool turn_queue_busy(uint32_t session_id) {
   (void)session_id;
   return false;
}
void session_posts_untell_locked(session_t *session) {
   (void)session;
}
int focus_handles_flush(struct session *session, int64_t conv_id, int user_id) {
   (void)session, (void)conv_id, (void)user_id;
   return SUCCESS;
}
void focus_handles_reset_locked(struct session *session) {
   (void)session;
}
int focus_handles_withdrawn_locked(const struct session *session,
                                   int64_t conv_id,
                                   const char *const *item_ids,
                                   int n_ids,
                                   int *out,
                                   int cap) {
   (void)session, (void)conv_id, (void)item_ids, (void)n_ids, (void)out, (void)cap;
   return 0;
}
int tool_result_store_bind_locked(struct session *session,
                                  int64_t conv_id,
                                  uint64_t turn_token,
                                  bool with_ended) {
   (void)session, (void)conv_id, (void)turn_token, (void)with_ended;
   return 0;
}
void tool_result_store_drop_trees(struct session *session) {
   (void)session;
}
void tool_result_store_reset_locked(struct session *session) {
   (void)session;
}
void tool_result_store_turn_ended_locked(struct session *session,
                                         uint64_t turn_token,
                                         bool to_live) {
   (void)session, (void)turn_token, (void)to_live;
}
char *event_payload_status(bool generating) {
   (void)generating;
   return NULL;
}
void webui_send_stream_delta(struct session *session, const char *text) {
   (void)session, (void)text;
}
void webui_send_stream_end(struct session *session, const char *reason) {
   (void)session, (void)reason;
}
void webui_send_transcript(struct session *session, const char *role, const char *text) {
   (void)session, (void)role, (void)text;
}
void webui_send_transcript_ex(struct session *session,
                              const char *role,
                              const char *text,
                              bool server_saved,
                              int64_t message_id) {
   (void)session, (void)role, (void)text, (void)server_saved, (void)message_id;
}

#define REPLY                                                                        \
   "{\"facts\": [{\"text\": \"The user keeps a placeholder notebook.\"}], "          \
   "\"preferences\": [], \"corrections\": [], "                                      \
   "\"summary\": \"The user and the assistant talked about a placeholder topic.\", " \
   "\"topics\": [\"placeholder\"]}"

void setUp(void) {
   config_set_defaults(&g_config);
   g_config.memory.enabled = true;
   safe_strscpy(g_config.memory.extraction_provider, "local");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   auth_db_create_user("placeholder", "hash", false);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("placeholder", &user);
   s_user = user.id;
   free(s_prompt);
   s_prompt = NULL;
   s_calls = 0;
   s_reply = REPLY;
   s_during_call = NULL;
   s_gated = false;
   s_entered = false;
   s_open = false;
   s_turn_reply = NULL;
   s_turn_tool = NULL;
   s_turn_session = NULL;
}

void tearDown(void) {
   memory_recovery_set_test_wait(NULL);
   TEST_ASSERT_TRUE(memory_extraction_wait_idle(s_user, WAIT_SEC));
   free(s_prompt);
   s_prompt = NULL;
   auth_db_shutdown();
}

/* ---- helpers ---- */

static int64_t add(int64_t conv, const char *role, const char *text) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_message_ex(conv, s_user, role, text, &id));
   return id;
}

/* A session's history: the rows given, as {role, content, id}. */
static struct json_object *history_of(const char *const *roles,
                                      const char *const *texts,
                                      const int64_t *ids,
                                      int n) {
   struct json_object *h = json_object_new_array();
   for (int i = 0; i < n; i++) {
      struct json_object *m = json_object_new_object();
      json_object_object_add(m, "role", json_object_new_string(roles[i]));
      json_object_object_add(m, "content", json_object_new_string(texts[i]));
      json_object_object_add(m, "id", json_object_new_int64(ids[i]));
      json_object_array_add(h, m);
   }
   return h;
}

/* Extraction of @p history, waited for. */
static void extract(int64_t conv, struct json_object *history, int message_count) {
   TEST_ASSERT_EQUAL_INT(0, memory_trigger_extraction(s_user, conv, "s", history, message_count, 0,
                                                      NULL));
   json_object_put(history);
   TEST_ASSERT_TRUE(memory_extraction_wait_idle(s_user, WAIT_SEC));
}

static int64_t q1(const char *sql, int64_t a) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   sqlite3_bind_int64(st, 1, a);
   const int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

static void exec2(const char *sql, int64_t a, int64_t b) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   sqlite3_bind_int64(st, 1, a);
   sqlite3_bind_int64(st, 2, b);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(st));
   sqlite3_finalize(st);
}

static int64_t id_cursor(int64_t conv) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_get_last_extracted_msg_id(conv, &id));
   return id;
}

static int count_cursor(int64_t conv) {
   int count = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_get_last_extracted(conv, &count));
   return count;
}

static void prompt_has(const char *marker) {
   TEST_ASSERT_NOT_NULL(s_prompt);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_prompt, marker), marker);
}

static void prompt_lacks(const char *marker) {
   TEST_ASSERT_NOT_NULL(s_prompt);
   TEST_ASSERT_NULL_MESSAGE(strstr(s_prompt, marker), marker);
}

/* ---- the history a batch reads ---- */

/* Two sessions on one conversation, rows A/B/A, and session A's history (which
 * lacks B's rows) handed to extraction: B's rows reach the model, the id
 * cursor ends at the stored max, the count cursor at the conversation's
 * message_count (so recovery doesn't pick it again), and the summary counts
 * the rows sent. */
void test_history_lacking_another_sessions_rows_sends_them(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Shared", &conv));
   const char *texts[] = { "MARK-A1-Q placeholder question", "MARK-A1-R placeholder answer",
                           "MARK-B-Q placeholder question",  "MARK-B-R placeholder answer",
                           "MARK-A2-Q placeholder question", "MARK-A2-R placeholder answer" };
   const char *roles[] = { "user", "assistant", "user", "assistant", "user", "assistant" };
   int64_t ids[6];
   for (int i = 0; i < 6; i++) {
      ids[i] = add(conv, roles[i], texts[i]);
   }

   /* Session A's history: its own four rows, not B's two. */
   const char *a_roles[] = { roles[0], roles[1], roles[4], roles[5] };
   const char *a_texts[] = { texts[0], texts[1], texts[4], texts[5] };
   const int64_t a_ids[] = { ids[0], ids[1], ids[4], ids[5] };
   extract(conv, history_of(a_roles, a_texts, a_ids, 4), 4);

   TEST_ASSERT_EQUAL_INT(1, s_calls);
   prompt_has("MARK-B-Q");
   prompt_has("MARK-B-R");
   prompt_has("MARK-A1-Q");
   prompt_has("MARK-A2-R");
   TEST_ASSERT_EQUAL_INT64(ids[5], id_cursor(conv));
   const int64_t total = q1("SELECT message_count FROM conversations WHERE id = ?1", conv);
   TEST_ASSERT_EQUAL_INT64(6, total);
   TEST_ASSERT_EQUAL_INT(6, count_cursor(conv));
   TEST_ASSERT_EQUAL_INT64(
       0, q1("SELECT last_extracted_msg_count < message_count FROM conversations WHERE id = ?1",
             conv));
   TEST_ASSERT_EQUAL_INT64(6, q1("SELECT message_count FROM memory_summaries WHERE "
                                 "source_conversation_id = ?1",
                                 conv));
}

/* A complete history: the prompt holds the rows past the cursor and none
 * before it; the cursor ends at the history's max. */
void test_complete_history_sends_only_new_rows(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Own", &conv));
   const char *texts[] = { "MARK-OLD-Q placeholder question", "MARK-OLD-R placeholder answer",
                           "MARK-NEW-Q placeholder question", "MARK-NEW-R placeholder answer" };
   const char *roles[] = { "user", "assistant", "user", "assistant" };
   int64_t ids[4];
   for (int i = 0; i < 4; i++) {
      ids[i] = add(conv, roles[i], texts[i]);
   }
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_set_last_extracted(conv, 2, ids[1], OUTSIDE_NONE));

   extract(conv, history_of(roles, texts, ids, 4), 4);

   TEST_ASSERT_EQUAL_INT(1, s_calls);
   prompt_has("MARK-NEW-Q");
   prompt_has("MARK-NEW-R");
   prompt_lacks("MARK-OLD-Q");
   prompt_lacks("MARK-OLD-R");
   TEST_ASSERT_EQUAL_INT64(ids[3], id_cursor(conv));
   TEST_ASSERT_EQUAL_INT(4, count_cursor(conv));
   TEST_ASSERT_EQUAL_INT64(2, q1("SELECT message_count FROM memory_summaries WHERE "
                                 "source_conversation_id = ?1",
                                 conv));
}

/* Another session's web result in the batch's range, its exchange missing
 * from this session's history: the model is asked (the batch has a question),
 * and the batch's summary and the conversation's floor record the web. */
void test_another_sessions_web_result_taints_a_model_batch(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Shared web", &conv));
   const int64_t qa = add(conv, "user", "MARK-A-Q placeholder question from one tab");

   /* Session B: a web search, its result and its reply. */
   const int64_t qb = add(conv, "user", "MARK-B-Q look something up");
   const conv_message_row_t call = {
      .role = "assistant",
      .content = "",
      .tool_calls = "[{\"id\":\"w1\",\"type\":\"function\",\"function\":{\"name\":\"search\","
                    "\"arguments\":\"{\\\"query\\\":\\\"placeholder\\\"}\"}}]"
   };
   int64_t call_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &call, &call_id));
   char *page = llm_third_party_frame(TOOL_FRAME_WEB, NULL, "MARK-WEB placeholder page\n");
   TEST_ASSERT_NOT_NULL(page);
   const conv_message_row_t result = { .role = "tool", .tool_call_id = "w1", .content = page };
   int64_t tool_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &result, &tool_id));
   free(page);
   const int64_t rb = add(conv, "assistant", "MARK-B-R placeholder answer");

   const int64_t ra = add(conv, "assistant", "MARK-A-R placeholder answer to the first tab");

   const char *roles[] = { "user", "assistant" };
   const char *texts[] = { "MARK-A-Q placeholder question from one tab",
                           "MARK-A-R placeholder answer to the first tab" };
   const int64_t ids[] = { qa, ra };
   extract(conv, history_of(roles, texts, ids, 2), 2);

   TEST_ASSERT_EQUAL_INT(1, s_calls);
   prompt_has("MARK-B-Q");
   prompt_has("MARK-B-R");
   TEST_ASSERT_TRUE(qb > qa && rb > tool_id);
   TEST_ASSERT_EQUAL_INT64(ra, id_cursor(conv));
   int floor = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_outside(conv, s_user, &floor));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, floor);
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q1("SELECT outside FROM memory_summaries WHERE "
                                           "source_conversation_id = ?1",
                                           conv));
}

/* ---- memory recovery ---- */

/* Recovery's waits: the settle sees the extraction running (its model call
 * entered and held at the gate); the first poll opens the gate and waits for
 * the extraction to end. */
static int s_waits;

static void recovery_wait(void) {
   pthread_mutex_lock(&s_gate_mutex);
   if (s_waits++ == 0) {
      const struct timespec until = deadline();
      while (!s_entered) {
         if (pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &until) == ETIMEDOUT) {
            break;
         }
      }
      pthread_mutex_unlock(&s_gate_mutex);
      return;
   }
   s_open = true;
   pthread_cond_broadcast(&s_gate_cond);
   pthread_mutex_unlock(&s_gate_mutex);
   (void)memory_extraction_wait_idle(s_user, WAIT_SEC);
}

static int64_t s_recovery_conv;
static int64_t s_saved_meanwhile;

/* A row another session saves while the model works: it stays unread. */
static void save_meanwhile(void) {
   /* On the extraction thread: no assertion here; the test checks the id. */
   s_saved_meanwhile = 0;
   (void)conv_db_add_message_ex(s_recovery_conv, s_user, "user",
                                "MARK-LATE placeholder question saved during the call",
                                &s_saved_meanwhile);
}

/* A conversation whose stored count is in older units (above what its rows
 * through the id cursor count, below its message_count, so recovery picks it):
 * the pass reads up to the rows it loaded, the count falls to the rows the id
 * cursor passed, and recovery takes that as progress, not as a failed attempt
 * to give up on at the attempt cap. */
void test_recovery_counts_a_falling_count_as_progress(void) {
   g_config.memory.recovery_enabled = true;
   g_config.memory.recovery_max_attempts = 1;
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Old units", &conv));
   s_recovery_conv = conv;
   const char *roles[] = { "user", "assistant", "user", "assistant", "user", "assistant" };
   const char *texts[] = { "MARK-R1-Q placeholder question about a placeholder topic",
                           "MARK-R1-R placeholder answer about a placeholder topic",
                           "MARK-R2-Q placeholder question about a placeholder topic",
                           "MARK-R2-R placeholder answer about a placeholder topic",
                           "MARK-R3-Q placeholder question about a placeholder topic",
                           "MARK-R3-R placeholder answer about a placeholder topic" };
   int64_t ids[6];
   for (int i = 0; i < 6; i++) {
      ids[i] = add(conv, roles[i], texts[i]);
   }
   /* The id cursor after the first exchange; then the counts in older units:
    * 7 stored, 9 in message_count, against 6 rows. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_set_last_extracted(conv, 0, ids[1], OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_set_last_extracted(conv, 7, 0, OUTSIDE_NONE));
   exec2("UPDATE conversations SET message_count = 9, updated_at = updated_at - ?1 * 2 "
         "WHERE id = ?2",
         g_config.memory.recovery_idle_threshold_seconds, conv);
   TEST_ASSERT_EQUAL_INT(7, count_cursor(conv));

   s_waits = 0;
   s_gated = true;
   s_during_call = save_meanwhile;
   memory_recovery_set_test_wait(recovery_wait);
   int triggered = 0, skipped = 0;
   memory_recovery_run_pass(&triggered, &skipped);
   memory_recovery_set_test_wait(NULL);

   TEST_ASSERT_EQUAL_INT(1, triggered);
   TEST_ASSERT_EQUAL_INT(0, skipped);
   TEST_ASSERT_EQUAL_INT(1, s_calls);
   prompt_has("MARK-R2-Q");
   prompt_has("MARK-R3-R");
   prompt_lacks("MARK-R1-Q");
   prompt_lacks("MARK-LATE");
   /* Partway: through the rows loaded, the one saved during the call above. */
   TEST_ASSERT_EQUAL_INT64(ids[5], id_cursor(conv));
   TEST_ASSERT_TRUE(s_saved_meanwhile > ids[5]);
   /* The count fell to the rows the cursor passed; a give-up would have
    * written the message_count the conversation was picked with (9). */
   TEST_ASSERT_EQUAL_INT(6, count_cursor(conv));
   TEST_ASSERT_EQUAL_INT64(10, q1("SELECT message_count FROM conversations WHERE id = ?1", conv));
}

/* ---- a turn, then its extraction ---- */

static session_t *new_session_id(int64_t conv, uint32_t id) {
   session_t *s = calloc(1, sizeof(*s));
   TEST_ASSERT_NOT_NULL(s);
   s->session_id = id;
   s->type = SESSION_TYPE_WEBUI;
   s->metrics.user_id = (uint32_t)s_user;
   pthread_mutex_init(&s->history_mutex, NULL);
   pthread_mutex_init(&s->llm_config_mutex, NULL);
   pthread_mutex_init(&s->metrics_mutex, NULL);
   pthread_mutex_init(&s->tools_mutex, NULL);
   s->conversation_history = json_object_new_array();
   session_bind_history_conversation(s, conv);
   return s;
}

static session_t *new_session(int64_t conv) {
   return new_session_id(conv, 3);
}

static void free_session(session_t *s) {
   session_final_answer_clear(s);
   session_compaction_teardown(s);
   session_prefix_turn_free(s->prefix_turn);
   json_object_put(s->withdraw_pending);
   json_object_put(s->conversation_history);
   pthread_mutex_destroy(&s->history_mutex);
   pthread_mutex_destroy(&s->llm_config_mutex);
   pthread_mutex_destroy(&s->metrics_mutex);
   pthread_mutex_destroy(&s->tools_mutex);
   free(s);
}

/* A turn on @p s: the question saved by the dispatch, a tool exchange (with
 * @p tool_result) through the session's persist hook, the reply by the
 * surface.  Returns the reply's row id. */
static int64_t run_turn(session_t *s,
                        int64_t conv,
                        const char *question,
                        const char *answer,
                        const char *tool_result) {
   s_turn_reply = answer;
   s_turn_tool = tool_result;
   s_turn_session = s;
   int64_t hook_conv = conv;
   s->tool_persist_cb = persist_row;
   s->tool_persist_userdata = &hook_conv;
   session_turn_begin(s, conv, s_user);
   const text_input_dispatch_opts_t opts = { .conversation_id = conv, .auth_user_id = s_user };
   char *reply = core_text_input_dispatch(s, question, &opts);
   TEST_ASSERT_NOT_NULL(reply);
   const conv_message_row_t row = { .role = "assistant", .content = reply };
   int64_t reply_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &row, &reply_id));
   session_stamp_last_message_id(s, "assistant", reply_id);
   free(reply);
   session_turn_end(s);
   s->tool_persist_cb = NULL;
   s->tool_persist_userdata = NULL;
   return reply_id;
}

/* The session's history as extraction is handed it. */
static void extract_session(session_t *s, int64_t conv) {
   pthread_mutex_lock(&s->history_mutex);
   struct json_object *history = NULL;
   TEST_ASSERT_EQUAL_INT(0, json_object_deep_copy(s->conversation_history, &history, NULL));
   pthread_mutex_unlock(&s->history_mutex);
   extract(conv, history, (int)json_object_array_length(history));
}

/* A turn run on a session (the question saved by the dispatch, the reply by
 * the surface), then the session's history handed to extraction: the model is
 * sent both, and the cursor ends at the reply. */
void test_a_turn_on_a_session_then_its_extraction(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Turn", &conv));
   session_t *s = new_session(conv);

   const int64_t reply_id = run_turn(s, conv, "MARK-TURN-Q placeholder question",
                                     "MARK-TURN-R placeholder answer", NULL);
   extract_session(s, conv);

   TEST_ASSERT_EQUAL_INT(1, s_calls);
   prompt_has("MARK-TURN-Q");
   prompt_has("MARK-TURN-R");
   TEST_ASSERT_EQUAL_INT64(reply_id, id_cursor(conv));
   free_session(s);
}

/* A turn with a tool exchange, extracted, then a plain turn on the same
 * session, extracted: the exchange is sent with its own batch only (its rows'
 * ids are on the live history, so the second batch drops them), and the
 * second batch is its two rows. */
void test_a_tool_exchange_is_extracted_once(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Tools", &conv));
   session_t *s = new_session(conv);

   const int64_t first = run_turn(s, conv, "MARK-Q1 placeholder question",
                                  "MARK-R1 placeholder answer", "MARK-TOOL placeholder result");
   extract_session(s, conv);
   TEST_ASSERT_EQUAL_INT(1, s_calls);
   prompt_has("MARK-Q1");
   prompt_has("MARK-TOOL");
   TEST_ASSERT_EQUAL_INT64(first, id_cursor(conv));

   const int64_t second = run_turn(s, conv, "MARK-Q2 placeholder question",
                                   "MARK-R2 placeholder answer", NULL);
   extract_session(s, conv);
   TEST_ASSERT_EQUAL_INT(2, s_calls);
   prompt_has("MARK-Q2");
   prompt_has("MARK-R2");
   prompt_lacks("MARK-TOOL");
   prompt_lacks("MARK-Q1");
   TEST_ASSERT_EQUAL_INT64(second, id_cursor(conv));
   TEST_ASSERT_EQUAL_INT64(2, q1("SELECT message_count FROM memory_summaries WHERE "
                                 "source_conversation_id = ?1 ORDER BY id DESC LIMIT 1",
                                 conv));
   free_session(s);
}


/* ---- two sessions on one conversation ---- */

/* The turn loader the WebUI registers: the conversation as a reload builds it. */
static struct json_object *reload_loader(int user_id,
                                         int64_t conv_id,
                                         const session_llm_config_t *base,
                                         session_llm_config_t *cfg_out,
                                         bool *has_cfg_out) {
   (void)base, (void)cfg_out;
   *has_cfg_out = false;
   return memory_history_load_for_llm(conv_id, user_id, NULL);
}

static int occurrences(const char *hay, const char *needle) {
   int n = 0;
   for (const char *p = hay ? strstr(hay, needle) : NULL; p; p = strstr(p + 1, needle)) {
      n++;
   }
   return n;
}

/* Session A runs a turn, then B (caught up from the conversation); A, idle
 * meanwhile, is extracted with the history it has: the cursor stops at what
 * that batch read, and B's extraction then reads B's rows, once. */
void test_an_idle_session_extracts_the_other_sessions_rows(void) {
   session_set_history_loader(reload_loader);
   session_set_history_tail_loader(memory_history_request_tail);
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Shared", &conv));
   session_t *a = new_session_id(conv, 3);
   session_t *b = new_session_id(conv, 4);
   const int64_t a_reply = run_turn(a, conv, "MARK-A1-Q placeholder question",
                                    "MARK-A1-R placeholder answer", NULL);
   const int64_t b_reply = run_turn(b, conv, "MARK-B-Q placeholder question",
                                    "MARK-B-R placeholder answer", NULL);
   pthread_mutex_lock(&b->history_mutex);
   TEST_ASSERT_EQUAL_INT(1, occurrences(json_object_to_json_string(b->conversation_history),
                                        "MARK-A1-Q"));
   pthread_mutex_unlock(&b->history_mutex);

   extract_session(a, conv);
   TEST_ASSERT_EQUAL_INT(1, s_calls);
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-A1-Q"));
   TEST_ASSERT_EQUAL_INT(0, occurrences(s_prompt, "MARK-B-Q"));
   TEST_ASSERT_EQUAL_INT64(a_reply, id_cursor(conv)); /* not past what it read */

   extract_session(b, conv);
   TEST_ASSERT_EQUAL_INT(2, s_calls);
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-B-Q"));
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-B-R"));
   TEST_ASSERT_EQUAL_INT(0, occurrences(s_prompt, "MARK-A1-Q"));
   TEST_ASSERT_EQUAL_INT64(b_reply, id_cursor(conv));
   free_session(a);
   free_session(b);
   session_set_history_loader(NULL);
   session_set_history_tail_loader(NULL);
}

/* A's next turn begins after B's: caught up, A's history holds B's rows once,
 * and its extraction reads every row once; B's stale history then has nothing
 * past the cursor to send. */
void test_a_caught_up_session_extracts_every_row_once(void) {
   session_set_history_loader(reload_loader);
   session_set_history_tail_loader(memory_history_request_tail);
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Shared", &conv));
   session_t *a = new_session_id(conv, 3);
   session_t *b = new_session_id(conv, 4);
   run_turn(a, conv, "MARK-A1-Q placeholder question", "MARK-A1-R placeholder answer", NULL);
   run_turn(b, conv, "MARK-B-Q placeholder question", "MARK-B-R placeholder answer",
            "MARK-B-TOOL placeholder result");
   const int64_t a_reply = run_turn(a, conv, "MARK-A2-Q placeholder question",
                                    "MARK-A2-R placeholder answer", NULL);
   pthread_mutex_lock(&a->history_mutex);
   const char *live = json_object_to_json_string(a->conversation_history);
   TEST_ASSERT_EQUAL_INT(1, occurrences(live, "MARK-B-Q"));
   TEST_ASSERT_EQUAL_INT(1, occurrences(live, "MARK-B-TOOL"));
   pthread_mutex_unlock(&a->history_mutex);

   extract_session(a, conv);
   TEST_ASSERT_EQUAL_INT(1, s_calls);
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-A1-Q"));
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-B-Q"));
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-B-TOOL"));
   TEST_ASSERT_EQUAL_INT(1, occurrences(s_prompt, "MARK-A2-R"));
   TEST_ASSERT_EQUAL_INT64(a_reply, id_cursor(conv));

   extract_session(b, conv);
   TEST_ASSERT_EQUAL_INT(1, s_calls); /* nothing past the cursor */
   TEST_ASSERT_EQUAL_INT64(a_reply, id_cursor(conv));
   free_session(a);
   free_session(b);
   session_set_history_loader(NULL);
   session_set_history_tail_loader(NULL);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_history_lacking_another_sessions_rows_sends_them);
   RUN_TEST(test_complete_history_sends_only_new_rows);
   RUN_TEST(test_another_sessions_web_result_taints_a_model_batch);
   RUN_TEST(test_recovery_counts_a_falling_count_as_progress);
   RUN_TEST(test_a_turn_on_a_session_then_its_extraction);
   RUN_TEST(test_a_tool_exchange_is_extracted_once);
   RUN_TEST(test_an_idle_session_extracts_the_other_sessions_rows);
   RUN_TEST(test_a_caught_up_session_extracts_every_row_once);
   return UNITY_END();
}
