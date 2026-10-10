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
 * The confirm rule for actions prepared for the user's yes (an email send or
 * trash, a call, a delete): a confirm counts only from the session that
 * prepared the action, in the very next turn (the user's reply to the
 * read-back), or in any later turn when the user approved it by reply code.
 */

#include <string.h>

#include "core/turn_origin.h"
#include "tools/email_service.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_next_turn_same_session(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const turn_origin_t reply = { .session_id = 7, .turn_token = 160, .turn_number = 5 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OK, turn_origin_check(&made, &reply));
}

/* The user moved on (or a background turn ran in between): too late. */
static void test_later_turn_refused(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const turn_origin_t later = { .session_id = 7, .turn_token = 300, .turn_number = 6 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_NOT_NEXT, turn_origin_check(&made, &later));
}

/* The model confirming in the turn that prepared the action: refused. */
static void test_same_turn_refused(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_SAME_TURN, turn_origin_check(&made, &made));
}

/* Prepared on one surface, confirmed from another: refused. */
static void test_other_session_refused(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const turn_origin_t other = { .session_id = 8, .turn_token = 101, .turn_number = 5 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OTHER_SESSION, turn_origin_check(&made, &other));
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OTHER_SESSION, turn_origin_check(&made, NULL));
}

/* A confirm with no turn (an MQTT message naming the session gets the session
 * but no turn) never counts, and neither does a record made without one. */
static void test_no_turn_refused(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100 };
   const turn_origin_t no_turn = { .session_id = 7, .turn_token = 0 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_SAME_TURN, turn_origin_check(&made, &no_turn));
   const turn_origin_t made_untracked = { .session_id = 0, .turn_token = 0 };
   const turn_origin_t local = { .session_id = 0, .turn_token = 5 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OTHER_SESSION, turn_origin_check(&made_untracked, &local));
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OTHER_SESSION, turn_origin_check(NULL, &local));
}

/* Approved by reply code: any later turn of the same session, never the turn
 * that made it, never another session. */
static void test_code_redeemed(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   const turn_origin_t later = { .session_id = 7,
                                 .turn_token = 300,
                                 .turn_number = 6,
                                 .code_redeemed = true };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OK, turn_origin_check(&made, &later));
   turn_origin_t same = made;
   same.code_redeemed = true;
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_SAME_TURN, turn_origin_check(&made, &same));
   const turn_origin_t other = { .session_id = 8,
                                 .turn_token = 300,
                                 .turn_number = 6,
                                 .code_redeemed = true };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OTHER_SESSION, turn_origin_check(&made, &other));
}

/* A turn a rendered visual started confirms nothing, whatever it says: not the
 * next turn, not with a code, and it is refused before any other check. */
static void test_from_visual_refused(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   turn_origin_t reply = { .session_id = 7, .turn_token = 160, .turn_number = 5 };
   reply.from_visual = true;
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_FROM_VISUAL, turn_origin_check(&made, &reply));
   reply.code_redeemed = true;
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_FROM_VISUAL, turn_origin_check(&made, &reply));
   /* Another session stays another session. */
   reply.session_id = 8;
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OTHER_SESSION, turn_origin_check(&made, &reply));
   /* Staging in a visual's turn is fine: a stored origin drops the flag, so the
    * user's own reply next turn confirms it. */
   turn_origin_t staged = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   staged.from_visual = true;
   const turn_origin_t stored = turn_origin_stored(&staged);
   TEST_ASSERT_FALSE(stored.from_visual);
   const turn_origin_t user = { .session_id = 7, .turn_token = 160, .turn_number = 5 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OK, turn_origin_check(&stored, &user));
   TEST_ASSERT_NOT_NULL(strstr(turn_origin_retry_hint(TURN_ORIGIN_FROM_VISUAL), "visual"));
}

/* Email reports a refusal with its own confirm codes. */
static void test_email_codes(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_RC_OK, email_confirm_rc(TURN_ORIGIN_OK));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_SAME_TURN, email_confirm_rc(TURN_ORIGIN_SAME_TURN));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_NOT_NEXT, email_confirm_rc(TURN_ORIGIN_NOT_NEXT));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_OTHER_SESSION,
                         email_confirm_rc(TURN_ORIGIN_OTHER_SESSION));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_FROM_VISUAL, email_confirm_rc(TURN_ORIGIN_FROM_VISUAL));
   TEST_ASSERT_EQUAL_INT(EMAIL_CONFIRM_RC_THIRD_PARTY, email_confirm_rc(TURN_ORIGIN_THIRD_PARTY));
}

/* A turn carrying an attached email confirms nothing (the email could say
 * yes); staging in one is fine, and the user's own next reply confirms. */
static void test_third_party_refused(void) {
   const turn_origin_t made = { .session_id = 7, .turn_token = 100, .turn_number = 4 };
   turn_origin_t reply = { .session_id = 7, .turn_token = 160, .turn_number = 5 };
   reply.third_party = true;
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_THIRD_PARTY, turn_origin_check(&made, &reply));
   reply.code_redeemed = true;
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_THIRD_PARTY, turn_origin_check(&made, &reply));
   turn_origin_t staged = made;
   staged.third_party = true;
   const turn_origin_t stored = turn_origin_stored(&staged);
   TEST_ASSERT_FALSE(stored.third_party);
   const turn_origin_t user = { .session_id = 7, .turn_token = 160, .turn_number = 5 };
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OK, turn_origin_check(&stored, &user));
   TEST_ASSERT_NOT_NULL(strstr(turn_origin_retry_hint(TURN_ORIGIN_THIRD_PARTY), "email"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_next_turn_same_session);
   RUN_TEST(test_later_turn_refused);
   RUN_TEST(test_same_turn_refused);
   RUN_TEST(test_other_session_refused);
   RUN_TEST(test_no_turn_refused);
   RUN_TEST(test_code_redeemed);
   RUN_TEST(test_from_visual_refused);
   RUN_TEST(test_third_party_refused);
   RUN_TEST(test_email_codes);
   return UNITY_END();
}
