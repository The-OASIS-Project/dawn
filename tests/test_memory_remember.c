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
 *
 * Unit tests for the remember tool (memoryCallback) on a real schema: the
 * conversation a remembered fact is recorded as learned in, and the refusal
 * when a turn has more facts waiting for its conversation than it can track.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "unity.h"

char *memoryCallback(const char *actionName, char *value, int *should_respond);

/* Test hooks in bench_retrieval_stub.c. */
extern session_t *g_stub_command_context;
extern int g_stub_defer_result;
extern int64_t g_stub_defer_conv;

static session_t s_session;
static int s_user;

static char *remember(const char *fact) {
   char value[256];
   snprintf(value, sizeof(value), "%s", fact);
   int respond = 0;
   return memoryCallback("remember", value, &respond);
}

/* The fact row for @p text: its source conversation (0 = none) and whether it
 * counts as learned outside any conversation.  False when there is no row. */
static bool fact_row(const char *text, int64_t *conv_out, int *unsourced_out) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id, origin_unsourced FROM memory_facts "
                      "WHERE user_id = ? AND fact_text = ?",
                      -1, &st, NULL);
   sqlite3_bind_int(st, 1, s_user);
   sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
   const bool found = sqlite3_step(st) == SQLITE_ROW;
   if (found) {
      *conv_out = sqlite3_column_type(st, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 0);
      *unsourced_out = sqlite3_column_int(st, 1);
   }
   sqlite3_finalize(st);
   return found;
}

void setUp(void) {
   config_set_defaults(&g_config);
   g_config.memory.enabled = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   auth_db_create_user("rememberer", "hash", false);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("rememberer", &user);
   s_user = user.id;
   memset(&s_session, 0, sizeof(s_session));
   s_session.type = SESSION_TYPE_WEBUI;
   s_session.metrics.user_id = s_user;
   g_stub_command_context = &s_session;
   g_stub_defer_result = FAILURE;
   g_stub_defer_conv = 0;
}

void tearDown(void) {
   g_stub_command_context = NULL;
   auth_db_shutdown();
}

void test_remember_in_a_known_conversation_is_learned_there(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Chat", &conv));
   g_stub_defer_result = SUCCESS;
   g_stub_defer_conv = conv;
   free(remember("My locker number is 314"));
   int64_t source = 0;
   int unsourced = -1;
   TEST_ASSERT_TRUE(fact_row("My locker number is 314", &source, &unsourced));
   TEST_ASSERT_EQUAL_INT64(conv, source);
   TEST_ASSERT_EQUAL_INT(0, unsourced); /* forgetting that conversation removes it */
}

void test_remember_outside_a_turn_is_stated_outside_any_conversation(void) {
   g_stub_defer_result = FAILURE; /* the scheduler, MQTT */
   free(remember("My bike is green"));
   int64_t source = -1;
   int unsourced = -1;
   TEST_ASSERT_TRUE(fact_row("My bike is green", &source, &unsourced));
   TEST_ASSERT_EQUAL_INT64(0, source);
   TEST_ASSERT_EQUAL_INT(1, unsourced);
}

void test_remember_waiting_for_its_conversation_is_kept(void) {
   g_stub_defer_result = SESSION_FACT_SOURCE_QUEUED; /* a new chat's first message */
   char *r = remember("My hat size is seven");
   TEST_ASSERT_NULL(strstr(r, "Too many facts"));
   free(r);
   int64_t source = -1;
   int unsourced = -1;
   TEST_ASSERT_TRUE(fact_row("My hat size is seven", &source, &unsourced));
   TEST_ASSERT_EQUAL_INT64(0, source); /* recorded once the conversation exists */
}

void test_remember_past_the_waiting_limit_is_refused_and_not_stored(void) {
   /* Stored, it could never be tied to its conversation, and forgetting that
    * conversation would keep it: refused, so the model can say so. */
   g_stub_defer_result = SESSION_FACT_SOURCE_DROPPED;
   char *r = remember("My gate code is 2468");
   TEST_ASSERT_NOT_NULL(strstr(r, "Too many facts"));
   free(r);
   int64_t source = 0;
   int unsourced = 0;
   TEST_ASSERT_FALSE(fact_row("My gate code is 2468", &source, &unsourced));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_remember_in_a_known_conversation_is_learned_there);
   RUN_TEST(test_remember_outside_a_turn_is_stated_outside_any_conversation);
   RUN_TEST(test_remember_waiting_for_its_conversation_is_kept);
   RUN_TEST(test_remember_past_the_waiting_limit_is_refused_and_not_stored);
   return UNITY_END();
}
