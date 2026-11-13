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
 * memory recovery's progress check on a count stored in older units.
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
#include "core/outside_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_third_party.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
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

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_history_lacking_another_sessions_rows_sends_them);
   RUN_TEST(test_complete_history_sends_only_new_rows);
   RUN_TEST(test_another_sessions_web_result_taints_a_model_batch);
   RUN_TEST(test_recovery_counts_a_falling_count_as_progress);
   return UNITY_END();
}
