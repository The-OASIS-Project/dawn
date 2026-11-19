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
 * Unit tests for conv_post.c: a post into a conversation from outside a turn
 * is written at once when no turn holds it, or at the holding turn's end, with
 * the caller told it was taken either way.
 */

#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "core/conv_post.h"
#include "core/conv_turn_lease.h"
#include "dawn_error.h"
#include "unity.h"

static int s_user;
static int s_appended; /* tabs told of new messages */
static int64_t s_written_conv;
static int64_t s_written_id;

/* The WebUI's broadcast (webui_broadcasts.c) isn't linked. */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id) {
   (void)user_id;
   (void)conv_id;
   s_appended++;
}

static void on_written(int64_t conv_id, int64_t msg_id) {
   s_written_conv = conv_id;
   s_written_id = msg_id;
}

static int64_t newest(int64_t conv) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_max_msg_id(conv, s_user, &id));
   return id;
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   s_user = u.id;
   s_appended = 0;
   s_written_conv = 0;
   s_written_id = 0;
   conv_post_init();
}

void tearDown(void) {
   conv_turn_lease_set_writer(NULL, NULL);
   auth_db_shutdown();
}

/* No turn on it: written now, its id reported, the tabs told. */
static void test_a_post_into_a_free_conversation_is_written_now(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "t", &conv));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_post_unprompted_ex(conv, s_user, "done", on_written));
   TEST_ASSERT_TRUE(newest(conv) > 0);
   TEST_ASSERT_EQUAL_INT64(conv, s_written_conv);
   TEST_ASSERT_EQUAL_INT64(newest(conv), s_written_id);
   TEST_ASSERT_EQUAL_INT(1, s_appended);
}

/* A turn holds it: taken now, written at the turn's end. */
static void test_a_post_into_a_held_conversation_is_written_at_the_turn_end(void) {
   int64_t conv = 0;
   const uint64_t turn = conv_turn_lease_new_token();
   conv_create_claim_t claim = { .session = 3, .token = turn };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_create_claimed(s_user, "t", NULL, false, &claim, &conv));
   TEST_ASSERT_TRUE(claim.claimed);
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_post_unprompted_ex(conv, s_user, "done", on_written));
   TEST_ASSERT_EQUAL_INT64(0, newest(conv));
   TEST_ASSERT_EQUAL_INT64(0, s_written_id);
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, turn));
   TEST_ASSERT_TRUE(newest(conv) > 0);
   TEST_ASSERT_EQUAL_INT64(newest(conv), s_written_id);
   TEST_ASSERT_EQUAL_INT(1, s_appended);
}

/* A write that fails now is reported. */
static void test_a_failed_write_is_reported(void) {
   TEST_ASSERT_EQUAL_INT(FAILURE, conv_post_unprompted(424242, s_user, "done"));
   TEST_ASSERT_EQUAL_INT(0, s_appended);
   TEST_ASSERT_EQUAL_INT(FAILURE, conv_post_unprompted(0, s_user, "done"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_post_into_a_free_conversation_is_written_now);
   RUN_TEST(test_a_post_into_a_held_conversation_is_written_at_the_turn_end);
   RUN_TEST(test_a_failed_write_is_reported);
   return UNITY_END();
}
