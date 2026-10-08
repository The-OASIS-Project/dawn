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
 * Unit tests for a session's stable memory citation handles (real auth_db):
 * numbered in memory before the conversation exists, saved when the history
 * becomes that conversation, the same after a reload, and started over for
 * another conversation.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "core/focus/focus_handles.h"
#include "core/session_manager.h"
#include "test_tmp.h"
#include "unity.h"

static char TEST_DB[TEST_TMP_PATH_MAX];
static int user_id = 0;

static session_t *new_session(void) {
   session_t *s = calloc(1, sizeof(*s));
   TEST_ASSERT_NOT_NULL(s);
   pthread_mutex_init(&s->history_mutex, NULL);
   return s;
}

static void free_session(session_t *s) {
   focus_handles_free(s->focus_handles);
   pthread_mutex_destroy(&s->history_mutex);
   free(s);
}

void setUp(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   user_id = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(TEST_DB);
}

static int handle_of(session_t *s, int64_t conv, const char *item_id) {
   conv_focus_handle_t item = { .source = "memory_fact", .item_id = item_id };
   TEST_ASSERT_EQUAL_INT(0, focus_handles_assign(s, conv, user_id, &item, 1));
   return item.handle;
}

typedef struct {
   int count;
} stored_t;

static int count_stored(const char *source, const char *item_id, int handle, void *ctx) {
   (void)source;
   (void)item_id;
   (void)handle;
   ((stored_t *)ctx)->count++;
   return 0;
}

static int stored_count(int64_t conv) {
   stored_t n = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_focus_handles_load(conv, user_id, count_stored, &n));
   return n.count;
}

static void test_first_turn_handles_are_saved_with_the_conversation(void) {
   session_t *s = new_session();
   /* A new chat's first turn: no conversation yet. */
   conv_focus_handle_t items[] = {
      { .source = "memory_fact", .item_id = "fact:1" },
      { .source = "memory_fact", .item_id = "fact:2" },
   };
   TEST_ASSERT_EQUAL_INT(0, focus_handles_assign(s, 0, user_id, items, 2));
   TEST_ASSERT_EQUAL_INT(1, items[0].handle);
   TEST_ASSERT_EQUAL_INT(2, items[1].handle);
   TEST_ASSERT_EQUAL_INT(2, handle_of(s, 0, "fact:2"));

   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(user_id, "c", &conv));
   /* Not while the session's history is something else. */
   TEST_ASSERT_EQUAL_INT(0, focus_handles_flush(s, conv, 0));
   TEST_ASSERT_EQUAL_INT(0, stored_count(conv));
   /* Once the history is the conversation, they are its handles. */
   atomic_store(&s->history_conversation_id, conv);
   TEST_ASSERT_EQUAL_INT(0, focus_handles_flush(s, conv, 0));
   TEST_ASSERT_EQUAL_INT(2, stored_count(conv));

   /* The next item gets the next handle, stored. */
   TEST_ASSERT_EQUAL_INT(3, handle_of(s, conv, "fact:3"));
   TEST_ASSERT_EQUAL_INT(3, stored_count(conv));
   char item_id[FOCUS_HANDLE_ITEM_ID_LEN];
   pthread_mutex_lock(&s->history_mutex);
   TEST_ASSERT_TRUE(focus_handles_item_locked(s, 2, item_id));
   TEST_ASSERT_EQUAL_STRING("fact:2", item_id);
   TEST_ASSERT_FALSE(focus_handles_item_locked(s, 9, item_id));
   pthread_mutex_unlock(&s->history_mutex);
   free_session(s);
}

static void test_handles_survive_a_reload(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(user_id, "c", &conv));
   session_t *first = new_session();
   TEST_ASSERT_EQUAL_INT(1, handle_of(first, conv, "fact:10"));
   TEST_ASSERT_EQUAL_INT(2, handle_of(first, conv, "fact:20"));
   free_session(first);

   /* Another session (a restart, another tab) sees the same handles. */
   session_t *second = new_session();
   TEST_ASSERT_EQUAL_INT(2, handle_of(second, conv, "fact:20"));
   TEST_ASSERT_EQUAL_INT(3, handle_of(second, conv, "fact:30"));
   free_session(second);
}

static void test_another_conversation_starts_over(void) {
   int64_t a = 0, b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(user_id, "a", &a));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(user_id, "b", &b));
   session_t *s = new_session();
   TEST_ASSERT_EQUAL_INT(1, handle_of(s, a, "fact:1"));
   TEST_ASSERT_EQUAL_INT(2, handle_of(s, a, "fact:2"));
   TEST_ASSERT_EQUAL_INT(1, handle_of(s, b, "fact:2"));
   /* Unsaved first-turn handles never become a conversation the history
    * merely switches to. */
   TEST_ASSERT_EQUAL_INT(1, handle_of(s, 0, "fact:9"));
   TEST_ASSERT_EQUAL_INT(3, handle_of(s, a, "fact:9"));
   free_session(s);
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_focus_handles.db");
   UNITY_BEGIN();
   RUN_TEST(test_first_turn_handles_are_saved_with_the_conversation);
   RUN_TEST(test_handles_survive_a_reload);
   RUN_TEST(test_another_conversation_starts_over);
   return UNITY_END();
}
