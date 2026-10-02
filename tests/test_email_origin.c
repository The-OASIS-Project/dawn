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
 * The confirm rule for sending and trashing email: a confirm counts only from
 * the session that prepared the action, in the very next turn (the user's
 * reply to the read-back).
 */

#include "tools/email_service.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_next_turn_same_session(void) {
   const email_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const email_origin_t reply = { .session_id = 7, .turn_token = 160, .turn_number = 5 };
   TEST_ASSERT_EQUAL_INT(EMAIL_RC_OK, email_origin_check(&made, &reply));
}

/* The user moved on (or a background turn ran in between): too late. */
static void test_later_turn_refused(void) {
   const email_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const email_origin_t later = { .session_id = 7, .turn_token = 300, .turn_number = 6 };
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_NOT_NEXT, email_origin_check(&made, &later));
}

/* The model confirming in the turn that prepared the action: refused. */
static void test_same_turn_refused(void) {
   const email_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_SAME_TURN, email_origin_check(&made, &made));
}

/* Drafted on one surface, confirmed from another: refused. */
static void test_other_session_refused(void) {
   const email_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const email_origin_t other = { .session_id = 8, .turn_token = 101, .turn_number = 5 };
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_OTHER_SESSION, email_origin_check(&made, &other));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_OTHER_SESSION, email_origin_check(&made, NULL));
}

/* A confirm with no turn (an MQTT message naming the session gets the session
 * but no turn) never counts, and neither does a record made without one. */
static void test_no_turn_refused(void) {
   const email_origin_t made = { .session_id = 7, .turn_token = 100 };
   const email_origin_t no_turn = { .session_id = 7, .turn_token = 0 };
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_SAME_TURN, email_origin_check(&made, &no_turn));
   const email_origin_t made_untracked = { .session_id = 0, .turn_token = 0 };
   const email_origin_t local = { .session_id = 0, .turn_token = 5 };
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_OTHER_SESSION,
                         email_origin_check(&made_untracked, &local));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_OTHER_SESSION, email_origin_check(NULL, &local));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_next_turn_same_session);
   RUN_TEST(test_later_turn_refused);
   RUN_TEST(test_same_turn_refused);
   RUN_TEST(test_other_session_refused);
   RUN_TEST(test_no_turn_refused);
   return UNITY_END();
}
