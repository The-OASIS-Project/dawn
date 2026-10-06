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
 * The session reaper (session_reaper.c): a destroyed session is finished
 * when its last reference goes, never on the destroying caller's time.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_reaper.h"
#include "unity.h"

/* ---- stubs for what the reaper calls ---- */

static atomic_int s_torn_down;
static atomic_int s_finalized;
static atomic_uint s_last_finalized;

void session_compaction_teardown(session_t *session) {
   (void)session;
   atomic_fetch_add(&s_torn_down, 1);
}

void session_manager_finalize(session_t *session) {
   atomic_store(&s_last_finalized, session->session_id);
   atomic_fetch_add(&s_finalized, 1);
   pthread_mutex_destroy(&session->ref_mutex);
   pthread_cond_destroy(&session->ref_zero_cond);
   free(session);
}

/* ---- helpers ---- */

static session_t *ended_session(uint32_t id, int refs) {
   session_t *s = calloc(1, sizeof(*s));
   TEST_ASSERT_NOT_NULL(s);
   s->session_id = id;
   s->ref_count = refs;
   pthread_mutex_init(&s->ref_mutex, NULL);
   pthread_cond_init(&s->ref_zero_cond, NULL);
   atomic_store(&s->being_destroyed, true);
   return s;
}

/* As session_release() does for a destroyed session's last reference. */
static void release(session_t *s) {
   pthread_mutex_lock(&s->ref_mutex);
   s->ref_count--;
   const bool last = s->ref_count <= 0;
   pthread_cond_broadcast(&s->ref_zero_cond);
   pthread_mutex_unlock(&s->ref_mutex);
   if (last) {
      session_reaper_wake();
   }
}

void setUp(void) {
   atomic_store(&s_torn_down, 0);
   atomic_store(&s_finalized, 0);
   atomic_store(&s_last_finalized, 0);
}

void tearDown(void) {
}

/* Nothing holds it: finished soon, its compaction joined first. */
static void test_an_unreferenced_session_is_finished(void) {
   session_reaper_enqueue(ended_session(11, 0));
   TEST_ASSERT_TRUE(session_reaper_drain(2000));
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_finalized));
   TEST_ASSERT_EQUAL_UINT(11, atomic_load(&s_last_finalized));
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_torn_down));
}

/* Enqueueing never waits for the reference (the caller may hold it), and the
 * release finishes the session. */
static void test_a_held_session_waits_for_its_last_reference(void) {
   session_t *s = ended_session(12, 1);
   session_reaper_enqueue(s);
   TEST_ASSERT_FALSE(session_reaper_drain(300));
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_finalized));
   TEST_ASSERT_EQUAL_INT(1, session_reaper_pending());

   release(s);
   TEST_ASSERT_TRUE(session_reaper_drain(2000));
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_finalized));
   TEST_ASSERT_EQUAL_UINT(12, atomic_load(&s_last_finalized));
}

/* Once held (shutdown, while the database closes), nothing is finished until
 * stop; stop finishes what nothing holds and leaks what something still does
 * (never freed under its holder). */
static void test_held_until_stop_which_finishes_the_free_and_leaks_the_held(void) {
   session_reaper_hold();
   session_t *held = ended_session(13, 1);
   session_reaper_enqueue(held);
   session_reaper_enqueue(ended_session(14, 0));
   TEST_ASSERT_FALSE(session_reaper_drain(300));
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_finalized));
   session_reaper_stop();
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_finalized));
   TEST_ASSERT_EQUAL_UINT(14, atomic_load(&s_last_finalized));
   TEST_ASSERT_EQUAL_INT(0, session_reaper_pending());
   /* The leaked one is still intact (the test owns it now). */
   TEST_ASSERT_EQUAL_UINT(13, held->session_id);
   pthread_mutex_destroy(&held->ref_mutex);
   pthread_cond_destroy(&held->ref_zero_cond);
   free(held);
}

/* Without the thread, a destroy finishes on the caller, as it used to. */
static void test_without_the_thread_the_caller_finishes_it(void) {
   session_reaper_enqueue(ended_session(15, 0));
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_finalized));
   TEST_ASSERT_EQUAL_INT(0, session_reaper_pending());
}

int main(void) {
   UNITY_BEGIN();
   TEST_ASSERT_EQUAL_INT(0, session_reaper_start());
   RUN_TEST(test_an_unreferenced_session_is_finished);
   RUN_TEST(test_a_held_session_waits_for_its_last_reference);
   RUN_TEST(test_held_until_stop_which_finishes_the_free_and_leaks_the_held);
   RUN_TEST(test_without_the_thread_the_caller_finishes_it);
   return UNITY_END();
}
