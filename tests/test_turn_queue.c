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
 * Unit tests for the per-session turn queue: serialization, and dropping a
 * producer's queued turns (a satellite query superseding the ones before it).
 */

#include "core/turn_queue.h"
#include "unity.h"

#define SID 7

static int s_spawned;
static int s_freed;
static int s_discarded;
static int s_other_freed;
static int s_last_spawned;

static void spawn(void *work) {
   s_spawned++;
   s_last_spawned = *(int *)work;
}
static void free_work(void *work) {
   (void)work;
   s_freed++;
}
static void other_free(void *work) {
   (void)work;
   s_other_freed++;
}
static void discard(void *work) {
   (void)work;
   s_discarded++;
}

void setUp(void) {
   s_spawned = s_freed = s_discarded = s_other_freed = s_last_spawned = 0;
}
void tearDown(void) {
   turn_queue_purge_session(SID);
   turn_queue_turn_done(SID);
}

void test_second_turn_waits_for_the_first(void) {
   static int a = 1, b = 2;
   TEST_ASSERT_EQUAL_INT(TURN_QUEUE_OK,
                         turn_queue_enqueue(SID, TURN_SOURCE_USER, &a, spawn, free_work));
   TEST_ASSERT_EQUAL_INT(TURN_QUEUE_OK,
                         turn_queue_enqueue(SID, TURN_SOURCE_USER, &b, spawn, free_work));
   TEST_ASSERT_EQUAL_INT(1, s_spawned);
   turn_queue_turn_done(SID);
   TEST_ASSERT_EQUAL_INT(2, s_spawned);
   TEST_ASSERT_EQUAL_INT(2, s_last_spawned);
}

void test_discard_drops_only_that_producers_queued_turns(void) {
   static int running = 1, old1 = 2, other = 3, old2 = 4, newest = 5;
   turn_queue_enqueue(SID, TURN_SOURCE_USER, &running, spawn, free_work);
   turn_queue_enqueue(SID, TURN_SOURCE_USER, &old1, spawn, free_work);
   turn_queue_enqueue(SID, TURN_SOURCE_USER, &other, spawn, other_free);
   turn_queue_enqueue(SID, TURN_SOURCE_USER, &old2, spawn, free_work);

   TEST_ASSERT_EQUAL_INT(2, turn_queue_discard_queued(SID, free_work, discard));
   TEST_ASSERT_EQUAL_INT(2, s_discarded);
   TEST_ASSERT_EQUAL_INT(0, s_freed); /* discarded, not freed */

   /* The queue still works after unlinking (tail included). */
   turn_queue_enqueue(SID, TURN_SOURCE_USER, &newest, spawn, free_work);
   turn_queue_turn_done(SID); /* running finished: "other" starts */
   TEST_ASSERT_EQUAL_INT(3, s_last_spawned);
   turn_queue_turn_done(SID);
   TEST_ASSERT_EQUAL_INT(5, s_last_spawned);
   TEST_ASSERT_EQUAL_INT(3, s_spawned);
}

void test_discard_leaves_the_running_turn(void) {
   static int running = 1;
   turn_queue_enqueue(SID, TURN_SOURCE_USER, &running, spawn, free_work);
   TEST_ASSERT_EQUAL_INT(0, turn_queue_discard_queued(SID, free_work, discard));
   TEST_ASSERT_EQUAL_INT(0, s_discarded);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_second_turn_waits_for_the_first);
   RUN_TEST(test_discard_drops_only_that_producers_queued_turns);
   RUN_TEST(test_discard_leaves_the_running_turn);
   return UNITY_END();
}
