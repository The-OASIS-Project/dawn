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
 * The email executor's admission: what a new request does to its slot.
 */

#include "unity.h"
#include "webui/webui_email_exec_policy.h"

void setUp(void) {
}
void tearDown(void) {
}

static void test_a_list_replaces_the_one_before_it(void) {
   email_exec_admit_t a = email_exec_admit(EMAIL_EXEC_SLOT_LIST, true, false, 1);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_RUN, a.action);
   TEST_ASSERT_TRUE(a.cancel_running);
   TEST_ASSERT_FALSE(a.replace_waiting);

   a = email_exec_admit(EMAIL_EXEC_SLOT_LIST, false, false, 0);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_RUN, a.action);
   TEST_ASSERT_FALSE(a.cancel_running);
}

static void test_a_read_waits_behind_the_open_one(void) {
   email_exec_admit_t a = email_exec_admit(EMAIL_EXEC_SLOT_READ, true, false, 1);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_WAIT, a.action);
   TEST_ASSERT_FALSE(a.cancel_running);

   /* The waiting read hasn't marked anything: the newer one takes its place. */
   a = email_exec_admit(EMAIL_EXEC_SLOT_READ, true, true, 2);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_WAIT, a.action);
   TEST_ASSERT_TRUE(a.replace_waiting);
   TEST_ASSERT_FALSE(a.cancel_running);

   a = email_exec_admit(EMAIL_EXEC_SLOT_READ, false, false, 0);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_RUN, a.action);
}

static void test_counts_flags_and_admin_run_one_at_a_time(void) {
   const email_exec_slot_t slots[] = { EMAIL_EXEC_SLOT_COUNTS, EMAIL_EXEC_SLOT_FLAGS,
                                       EMAIL_EXEC_SLOT_ADMIN };
   for (int i = 0; i < 3; i++) {
      TEST_ASSERT_EQUAL(EMAIL_EXEC_REFUSE, email_exec_admit(slots[i], true, false, 1).action);
      TEST_ASSERT_EQUAL(EMAIL_EXEC_RUN, email_exec_admit(slots[i], false, false, 1).action);
   }
}

static void test_the_user_cap_counts_what_stays(void) {
   const int full = EMAIL_EXEC_USER_LIVE_MAX;
   TEST_ASSERT_EQUAL(EMAIL_EXEC_REFUSE,
                     email_exec_admit(EMAIL_EXEC_SLOT_COUNTS, false, false, full).action);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_RUN,
                     email_exec_admit(EMAIL_EXEC_SLOT_COUNTS, false, false, full - 1).action);
   /* A list that replaces a waiting one frees that place; the running one it
    * cancels keeps its place until it drains. */
   TEST_ASSERT_EQUAL(EMAIL_EXEC_RUN,
                     email_exec_admit(EMAIL_EXEC_SLOT_LIST, true, true, full).action);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_REFUSE,
                     email_exec_admit(EMAIL_EXEC_SLOT_LIST, true, false, full).action);
   /* A refusal cancels nothing. */
   email_exec_admit_t a = email_exec_admit(EMAIL_EXEC_SLOT_LIST, true, false, full + 1);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_REFUSE, a.action);
   TEST_ASSERT_FALSE(a.cancel_running);
   a = email_exec_admit(EMAIL_EXEC_SLOT_READ, true, true, full + 1);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_REFUSE, a.action);
   TEST_ASSERT_FALSE(a.replace_waiting);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_list_replaces_the_one_before_it);
   RUN_TEST(test_a_read_waits_behind_the_open_one);
   RUN_TEST(test_counts_flags_and_admin_run_one_at_a_time);
   RUN_TEST(test_the_user_cap_counts_what_stays);
   return UNITY_END();
}
