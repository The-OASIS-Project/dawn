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
 * Tests for the reserve-aware idle-worker selector behind the non-blocking ASR
 * borrow. Pins the boundary that matters for the default 2-context pool: with
 * keep_idle=1, the LAST idle context is never lent to speculative work.
 */

#include "core/worker_pool_select.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static void test_all_busy_returns_none(void) {
   unsigned char idle[4] = { 0, 0, 0, 0 };
   TEST_ASSERT_EQUAL_INT(-1, worker_pool_select_idle(idle, 4, 1));
   TEST_ASSERT_EQUAL_INT(-1, worker_pool_select_idle(idle, 4, 0));
}

/* keep_idle=1: lend only when >=2 are idle; never the last one. */
static void test_reserve_one_holds_last(void) {
   unsigned char one_idle[2] = { 0, 1 };
   TEST_ASSERT_EQUAL_INT(-1, worker_pool_select_idle(one_idle, 2, 1)); /* reserve breached */

   unsigned char two_idle[2] = { 1, 1 };
   TEST_ASSERT_EQUAL_INT(0, worker_pool_select_idle(two_idle, 2, 1)); /* first idle */
}

/* keep_idle=0: lend whenever anything is idle. */
static void test_no_reserve_lends_any(void) {
   unsigned char one_idle[2] = { 0, 1 };
   TEST_ASSERT_EQUAL_INT(1, worker_pool_select_idle(one_idle, 2, 0));
}

static void test_returns_first_idle_index(void) {
   unsigned char idle[4] = { 0, 0, 1, 1 };
   TEST_ASSERT_EQUAL_INT(2, worker_pool_select_idle(idle, 4, 1));
}

static void test_negative_keep_idle_treated_as_zero(void) {
   unsigned char one_idle[2] = { 1, 0 };
   TEST_ASSERT_EQUAL_INT(0, worker_pool_select_idle(one_idle, 2, -5));
}

static void test_degenerate_inputs(void) {
   TEST_ASSERT_EQUAL_INT(-1, worker_pool_select_idle(NULL, 4, 1));
   unsigned char idle[1] = { 1 };
   TEST_ASSERT_EQUAL_INT(-1, worker_pool_select_idle(idle, 0, 0));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_all_busy_returns_none);
   RUN_TEST(test_reserve_one_holds_last);
   RUN_TEST(test_no_reserve_lends_any);
   RUN_TEST(test_returns_first_idle_index);
   RUN_TEST(test_negative_keep_idle_treated_as_zero);
   RUN_TEST(test_degenerate_inputs);
   return UNITY_END();
}
