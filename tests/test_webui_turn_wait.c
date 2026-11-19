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
 * stop_other_reply's decision: which turn holding a conversation a user may
 * stop from another session, and that the stop reaches only that turn.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "core/conv_turn_lease.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "unity.h"
#include "webui/webui_handlers.h"

/* ---- the sessions, by id (session_manager.c isn't linked) ---- */

#define MAX_TEST_SESSIONS 4
static session_t *s_sessions[MAX_TEST_SESSIONS];

session_t *session_get_for_reconnect(uint32_t session_id) {
   for (int i = 0; i < MAX_TEST_SESSIONS; i++) {
      if (s_sessions[i] && s_sessions[i]->session_id == session_id) {
         return s_sessions[i];
      }
   }
   return NULL;
}

void session_release(session_t *session) {
   (void)session;
}

/* Only the turn's own thread is its caller; none here is. */
bool session_turn_reads_elsewhere_locked(const session_t *session) {
   return session && session->turn_active;
}

/* ---- the job's cancel ---- */

static int s_job_cancels;
static job_cancel_result_t s_job_result;

job_cancel_result_t job_manager_cancel_or_retire(int64_t conv_id,
                                                 int user_id,
                                                 char *status_out,
                                                 size_t status_n) {
   (void)conv_id, (void)user_id, (void)status_out, (void)status_n;
   s_job_cancels++;
   return s_job_result;
}

/* ---- what the WebUI would send ---- */

void webui_send_session_json(struct session *session, const char *json_str) {
   (void)session, (void)json_str;
}
void webui_send_state_for_conversation(struct session *session,
                                       const char *state,
                                       const char *detail,
                                       int64_t conversation_id) {
   (void)session, (void)state, (void)detail, (void)conversation_id;
}
void send_json_response(ws_connection_t *conn, json_object *response) {
   (void)conn, (void)response;
}
const char *webui_turn_ref_get(void) {
   return NULL;
}

/* ---- fixture ---- */

#define ALICE 1
#define BOB 2

static session_t *new_session(uint32_t id, session_type_t type) {
   session_t *s = calloc(1, sizeof(*s));
   TEST_ASSERT_NOT_NULL(s);
   s->session_id = id;
   s->type = type;
   pthread_mutex_init(&s->history_mutex, NULL);
   for (int i = 0; i < MAX_TEST_SESSIONS; i++) {
      if (!s_sessions[i]) {
         s_sessions[i] = s;
         break;
      }
   }
   return s;
}

/* @p s runs a turn for @p user holding @p conv's lease. */
static uint64_t run_turn(session_t *s, int user, int64_t conv) {
   const uint64_t token = conv_turn_lease_new_token();
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(conv, s->session_id, token));
   pthread_mutex_lock(&s->history_mutex);
   s->turn_active = true;
   s->turn_user_id = user;
   s->turn_lease_conv = conv;
   s->turn_lease_token = token;
   pthread_mutex_unlock(&s->history_mutex);
   return token;
}

static void end_turn(session_t *s, int64_t conv, uint64_t token) {
   pthread_mutex_lock(&s->history_mutex);
   s->turn_active = false;
   s->turn_lease_conv = 0;
   s->turn_lease_token = 0;
   pthread_mutex_unlock(&s->history_mutex);
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, token));
}

void setUp(void) {
   s_job_cancels = 0;
   s_job_result = JOB_CANCEL_NOT_FOUND;
}

void tearDown(void) {
   for (int i = 0; i < MAX_TEST_SESSIONS; i++) {
      if (s_sessions[i]) {
         pthread_mutex_destroy(&s_sessions[i]->history_mutex);
         free(s_sessions[i]);
         s_sessions[i] = NULL;
      }
   }
}

/* ---- tests ---- */

static void test_nothing_holds_it(void) {
   new_session(1, SESSION_TYPE_WEBUI);
   TEST_ASSERT_EQUAL_STRING("NOT_RUNNING", webui_stop_other_reply_core(1, ALICE, 501));
   TEST_ASSERT_EQUAL_STRING("NOT_RUNNING", webui_stop_other_reply_core(1, ALICE, 0));
}

static void test_the_requesters_own_turn_is_forbidden(void) {
   session_t *me = new_session(1, SESSION_TYPE_WEBUI);
   const uint64_t t = run_turn(me, ALICE, 502);
   TEST_ASSERT_EQUAL_STRING("FORBIDDEN", webui_stop_other_reply_core(1, ALICE, 502));
   TEST_ASSERT_FALSE(atomic_load(&me->cancel_requested));
   end_turn(me, 502, t);
}

static void test_another_users_turn_answers_as_none(void) {
   new_session(1, SESSION_TYPE_WEBUI);
   session_t *other = new_session(2, SESSION_TYPE_WEBUI);
   const uint64_t t = run_turn(other, BOB, 503);
   TEST_ASSERT_EQUAL_STRING("NOT_RUNNING", webui_stop_other_reply_core(1, ALICE, 503));
   TEST_ASSERT_FALSE(atomic_load(&other->cancel_requested));
   end_turn(other, 503, t);
}

static void test_the_users_other_session_is_stopped(void) {
   new_session(1, SESSION_TYPE_WEBUI);
   session_t *other = new_session(2, SESSION_TYPE_WEBUI);
   const uint64_t t = run_turn(other, ALICE, 504);
   pthread_mutex_lock(&other->history_mutex);
   other->turn_owner_token = t;
   pthread_mutex_unlock(&other->history_mutex);
   TEST_ASSERT_NULL(webui_stop_other_reply_core(1, ALICE, 504));
   TEST_ASSERT_TRUE(atomic_load(&other->cancel_requested));
   TEST_ASSERT_EQUAL_INT(0, s_job_cancels);
   /* The user's Stop, like the one pressed on that reply's own client. */
   TEST_ASSERT_EQUAL_UINT64(t, other->stopped_turn_tokens[0]);
   end_turn(other, 504, t);
}

static void test_a_jobs_own_turn_is_cancelled_as_the_job(void) {
   new_session(1, SESSION_TYPE_WEBUI);
   session_t *job = new_session(3, SESSION_TYPE_JOB);
   const uint64_t t = run_turn(job, ALICE, 505);
   s_job_result = JOB_CANCEL_SIGNALLED;
   TEST_ASSERT_NULL(webui_stop_other_reply_core(1, ALICE, 505));
   TEST_ASSERT_EQUAL_INT(1, s_job_cancels);
   TEST_ASSERT_FALSE(atomic_load(&job->cancel_requested));
   end_turn(job, 505, t);
}

/* A turn of the holding session other than the one holding the lease (it
 * ended, and the session's next turn runs) is never stopped. */
static void test_only_the_holding_turn_is_stopped(void) {
   session_t *other = new_session(2, SESSION_TYPE_WEBUI);
   const uint64_t held = run_turn(other, ALICE, 506);
   pthread_mutex_lock(&other->history_mutex);
   other->turn_lease_token = conv_turn_lease_new_token(); /* its next turn's */
   pthread_mutex_unlock(&other->history_mutex);
   TEST_ASSERT_FALSE(session_turn_cancel_if_holds(other, 506, held));
   TEST_ASSERT_FALSE(atomic_load(&other->cancel_requested));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(506, held));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_nothing_holds_it);
   RUN_TEST(test_the_requesters_own_turn_is_forbidden);
   RUN_TEST(test_another_users_turn_answers_as_none);
   RUN_TEST(test_the_users_other_session_is_stopped);
   RUN_TEST(test_a_jobs_own_turn_is_cancelled_as_the_job);
   RUN_TEST(test_only_the_holding_turn_is_stopped);
   return UNITY_END();
}
