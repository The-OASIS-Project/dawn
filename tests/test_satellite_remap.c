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
 * Unit tests for satellite user remapping: the previous user's conversation is
 * saved behind any running query, before the session becomes the new user's.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "core/turn_queue.h"
#include "unity.h"
#include "webui/satellite_remap.h"

dawn_config_t g_config;

/* ---- session-layer stubs, recording what the remap did ---- */

static session_t *s;
static int s_save_result;   /* what session_save_voice_conversation returns */
static int s_saved_as_user; /* the session's user when the save ran (-1: never) */
static int s_cleared;       /* session_clear_history calls */
static atomic_int s_refs;

void session_set_metrics_user(session_t *session, int user_id) {
   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.user_id = user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
}
int session_save_voice_conversation(session_t *session, int64_t *conv_id_out) {
   pthread_mutex_lock(&session->metrics_mutex);
   s_saved_as_user = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   *conv_id_out = s_save_result == 0 ? 77 : 0;
   return s_save_result;
}
void session_clear_history(session_t *session) {
   (void)session;
   s_cleared++;
}
void session_retain(session_t *session) {
   (void)session;
   atomic_fetch_add(&s_refs, 1);
}
void session_release(session_t *session) {
   (void)session;
   atomic_fetch_sub(&s_refs, 1);
}
/* ---- helpers ---- */

static int user_of(session_t *session) {
   pthread_mutex_lock(&session->metrics_mutex);
   const int u = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   return u;
}

/* Wait (bounded) for the queued remap to finish: it releases its reference. */
static bool wait_for_remap(void) {
   for (int i = 0; i < 2000; i++) {
      if (atomic_load(&s_refs) == 0) {
         return true;
      }
      struct timespec ts = { 0, 1000000 };
      nanosleep(&ts, NULL);
   }
   return false;
}

/* A query in progress on the session's turn queue, finished by the test. */
static void hold_spawn(void *work) {
   (void)work;
}
static void hold_free(void *work) {
   (void)work;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.default_voice_user_id = 1;
   s = calloc(1, sizeof(*s));
   pthread_mutex_init(&s->metrics_mutex, NULL);
   s->session_id = 42;
   s_save_result = 0;
   s_saved_as_user = -1;
   s_cleared = 0;
   atomic_store(&s_refs, 0);
}

void tearDown(void) {
   turn_queue_purge_session(42);
   pthread_mutex_destroy(&s->metrics_mutex);
   free(s);
}

/* ---- tests ---- */

void test_unmapped_is_a_guest_not_the_default_voice_user(void) {
   /* Unmapped speech is a guest's: mapping it to anyone, the default voice
    * user included, changes whose it is. */
   session_set_metrics_user(s, 0);
   TEST_ASSERT_TRUE(satellite_owner_changes(s, 1)); /* default voice user is 1 */
   TEST_ASSERT_TRUE(satellite_owner_changes(s, 5));
   TEST_ASSERT_FALSE(satellite_owner_changes(s, 0));
   session_set_metrics_user(s, 5);
   TEST_ASSERT_TRUE(satellite_owner_changes(s, 0));
   TEST_ASSERT_FALSE(satellite_owner_changes(s, 5));
}

void test_remap_saves_previous_users_conversation_first(void) {
   session_set_metrics_user(s, 5);
   satellite_queue_remap(s, 7);
   TEST_ASSERT_TRUE(wait_for_remap());
   TEST_ASSERT_EQUAL_INT(5, s_saved_as_user); /* saved as the previous user */
   TEST_ASSERT_EQUAL_INT(7, user_of(s));
   TEST_ASSERT_EQUAL_INT(0, s_cleared); /* the save started the new context */
}

void test_remap_waits_for_the_query_in_progress(void) {
   session_set_metrics_user(s, 5);
   static int running;
   TEST_ASSERT_EQUAL_INT(TURN_QUEUE_OK,
                         turn_queue_enqueue(42, TURN_SOURCE_USER, &running, hold_spawn, hold_free));
   satellite_queue_remap(s, 7);
   struct timespec ts = { 0, 50000000 };
   nanosleep(&ts, NULL);
   /* The running query still belongs to the user it began as. */
   TEST_ASSERT_EQUAL_INT(-1, s_saved_as_user);
   TEST_ASSERT_EQUAL_INT(5, user_of(s));
   turn_queue_turn_done(42); /* the query ends: the remap runs */
   TEST_ASSERT_TRUE(wait_for_remap());
   TEST_ASSERT_EQUAL_INT(5, s_saved_as_user);
   TEST_ASSERT_EQUAL_INT(7, user_of(s));
}

void test_unmap_with_nothing_to_save_starts_a_new_context(void) {
   /* Nothing to save (or the save failed): a new context starts, so the next
    * speaker gets neither the previous user's history nor their prompt. */
   session_set_metrics_user(s, 5);
   s_save_result = 1;
   satellite_queue_remap(s, 0);
   TEST_ASSERT_TRUE(wait_for_remap());
   TEST_ASSERT_EQUAL_INT(1, s_cleared);
   TEST_ASSERT_EQUAL_INT(0, user_of(s));
}

void test_apply_mapping_without_owner_change(void) {
   session_set_metrics_user(s, 1);
   satellite_apply_mapping(s, 1);
   TEST_ASSERT_EQUAL_INT(1, user_of(s));
   TEST_ASSERT_EQUAL_INT(-1, s_saved_as_user); /* same owner: nothing saved */
   TEST_ASSERT_EQUAL_INT(0, s_cleared);        /* nor a new context */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_unmapped_is_a_guest_not_the_default_voice_user);
   RUN_TEST(test_remap_saves_previous_users_conversation_first);
   RUN_TEST(test_remap_waits_for_the_query_in_progress);
   RUN_TEST(test_unmap_with_nothing_to_save_starts_a_new_context);
   RUN_TEST(test_apply_mapping_without_owner_change);
   return UNITY_END();
}
