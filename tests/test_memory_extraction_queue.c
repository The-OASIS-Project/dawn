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
 * Unit tests for the memory extraction queue (memory_extraction_queue.c).
 */

#include <string.h>

#include "memory/memory_extraction_queue.h"
#include "unity.h"

void setUp(void) {
   extraction_queue_clear();
}

void tearDown(void) {
}

static extraction_queue_entry_t entry(int user, int64_t conv) {
   extraction_queue_entry_t e;
   memset(&e, 0, sizeof(e));
   e.user_id = user;
   e.conversation_id = conv;
   return e;
}

static bool always(int user_id, void *ctx) {
   (void)user_id;
   (void)ctx;
   return true;
}

/* Ready only for the user named by ctx. */
static bool only_user(int user_id, void *ctx) {
   return user_id == *(const int *)ctx;
}

static void test_oldest_first_and_one_per_conversation(void) {
   extraction_queue_entry_t e = entry(1, 10);
   TEST_ASSERT_TRUE(extraction_queue_push(&e));
   e = entry(2, 20);
   TEST_ASSERT_TRUE(extraction_queue_push(&e));
   /* The same conversation again refreshes its entry and keeps its place. */
   e = entry(1, 10);
   strcpy(e.session_id, "newer");
   TEST_ASSERT_TRUE(extraction_queue_push(&e));
   TEST_ASSERT_EQUAL_INT(2, extraction_queue_count());

   extraction_queue_entry_t out;
   TEST_ASSERT_TRUE(extraction_queue_take(always, NULL, &out));
   TEST_ASSERT_EQUAL_INT64(10, out.conversation_id);
   TEST_ASSERT_EQUAL_STRING("newer", out.session_id);
   TEST_ASSERT_TRUE(extraction_queue_take(always, NULL, &out));
   TEST_ASSERT_EQUAL_INT64(20, out.conversation_id);
   TEST_ASSERT_FALSE(extraction_queue_take(always, NULL, &out));
}

static void test_take_skips_users_not_ready(void) {
   extraction_queue_entry_t e = entry(1, 10);
   extraction_queue_push(&e);
   e = entry(2, 20);
   extraction_queue_push(&e);
   const int ready_user = 2;
   extraction_queue_entry_t out;
   TEST_ASSERT_TRUE(extraction_queue_take(only_user, (void *)&ready_user, &out));
   TEST_ASSERT_EQUAL_INT64(20, out.conversation_id);
   TEST_ASSERT_EQUAL_INT(1, extraction_queue_count());
}

static void test_full_queue_refuses(void) {
   for (int i = 0; i < EXTRACTION_QUEUE_MAX; i++) {
      extraction_queue_entry_t e = entry(1, 100 + i);
      TEST_ASSERT_TRUE(extraction_queue_push(&e));
   }
   extraction_queue_entry_t e = entry(1, 999);
   TEST_ASSERT_FALSE(extraction_queue_push(&e));
   /* A conversation already waiting is still refreshed. */
   e = entry(1, 100);
   TEST_ASSERT_TRUE(extraction_queue_push(&e));
   TEST_ASSERT_EQUAL_INT(EXTRACTION_QUEUE_MAX, extraction_queue_count());
}

static void test_invalid_entries_refused(void) {
   extraction_queue_entry_t e = entry(0, 10);
   TEST_ASSERT_FALSE(extraction_queue_push(&e));
   e = entry(1, 0);
   TEST_ASSERT_FALSE(extraction_queue_push(&e));
   TEST_ASSERT_FALSE(extraction_queue_push(NULL));
}

static void test_drops(void) {
   extraction_queue_entry_t e = entry(1, 10);
   extraction_queue_push(&e);
   e = entry(1, 11);
   extraction_queue_push(&e);
   e = entry(2, 10 + 100);
   extraction_queue_push(&e);
   e = entry(1, 12);
   extraction_queue_push(&e);

   /* Another user's id in the list drops nothing of theirs. */
   const int64_t ids[] = { 11, 110 };
   extraction_queue_drop_conversations(1, ids, 2);
   TEST_ASSERT_EQUAL_INT(3, extraction_queue_count());

   extraction_queue_drop_user(1);
   TEST_ASSERT_EQUAL_INT(1, extraction_queue_count());
   extraction_queue_entry_t out;
   TEST_ASSERT_TRUE(extraction_queue_take(always, NULL, &out));
   TEST_ASSERT_EQUAL_INT(2, out.user_id);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_oldest_first_and_one_per_conversation);
   RUN_TEST(test_take_skips_users_not_ready);
   RUN_TEST(test_full_queue_refuses);
   RUN_TEST(test_invalid_entries_refused);
   RUN_TEST(test_drops);
   return UNITY_END();
}
