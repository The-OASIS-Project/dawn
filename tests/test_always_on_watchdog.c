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
 * When always-on stops waiting for a turn's answer.
 */

#include "unity.h"
#include "webui/always_on_watchdog.h"

void setUp(void) {
}

void tearDown(void) {
}

/* A turn busy in a tool for minutes (it speaks nothing) is still waited for. */
static void test_a_running_turn_is_waited_for(void) {
   const int64_t since = 1000;
   const int64_t now = since + 90000;
   TEST_ASSERT_FALSE(always_on_processing_expired(now, since, since, true));
}

/* With no turn running, 30 s without progress ends the wait. */
static void test_no_turn_running_times_out(void) {
   const int64_t since = 1000;
   TEST_ASSERT_FALSE(always_on_processing_expired(since + ALWAYS_ON_PROCESSING_TIMEOUT_MS - 1,
                                                  since, since, false));
   TEST_ASSERT_TRUE(
       always_on_processing_expired(since + ALWAYS_ON_PROCESSING_TIMEOUT_MS, since, since, false));
   /* Counted from the last progress (a spoken sentence, the turn seen running) */
   const int64_t progress = since + 60000;
   TEST_ASSERT_FALSE(always_on_processing_expired(progress + 1000, progress, since, false));
}

/* However busy, the wait has a hard limit. */
static void test_the_hard_limit_holds(void) {
   const int64_t since = 1000;
   const int64_t now = since + ALWAYS_ON_PROCESSING_MAX_MS;
   TEST_ASSERT_TRUE(always_on_processing_expired(now, now, since, true));
   TEST_ASSERT_FALSE(always_on_processing_expired(now - 1, now - 1, since, true));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_running_turn_is_waited_for);
   RUN_TEST(test_no_turn_running_times_out);
   RUN_TEST(test_the_hard_limit_holds);
   return UNITY_END();
}
