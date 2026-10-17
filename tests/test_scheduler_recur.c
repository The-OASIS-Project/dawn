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
 * When a repeating scheduled event fires next, on its owner's clock (New
 * York here; times in the comments are New York's).
 */

#include "core/scheduler_recur.h"
#include "unity.h"

static const tz_t *s_ny;

void setUp(void) {
   s_ny = tz_lookup("America/New_York");
}

void tearDown(void) {
}

static time_t next(time_t fire_at, const char *original, sched_recurrence_t r, time_t now) {
   return sched_next_occurrence(s_ny, fire_at, original, r, NULL, false, now);
}

/* Fired on time at 07:00: tomorrow at 07:00. */
static void test_daily_next_day(void) {
   TEST_ASSERT_EQUAL_INT64(1792234800, next(1792148400, "07:00", SCHED_RECUR_DAILY, 1792148460));
}

/* A 23:50 alarm snoozed to 00:10 and dismissed at 00:11: tonight's 23:50,
 * not tomorrow's (the snooze doesn't hide which day it was for). */
static void test_snoozed_past_midnight(void) {
   TEST_ASSERT_EQUAL_INT64(1792295400,
                           sched_next_occurrence(s_ny, 1792210200, "23:50", SCHED_RECUR_DAILY, NULL,
                                                 true, 1792210260));
}

/* A Sunday 01:55 weekly alarm snoozed within the hour clocks go back (01:55
 * EDT + 10 min is 01:05 EST): still Sunday's occurrence, so next Sunday, not
 * Saturday. */
static void test_snoozed_in_repeated_hour(void) {
   TEST_ASSERT_EQUAL_INT64(1794120900,
                           sched_next_occurrence(s_ny, 1793513100, "01:55", SCHED_RECUR_WEEKLY,
                                                 NULL, true, 1793513160));
}

/* A row never snoozed whose fire_at is earlier in the day than its time (an
 * older daemon set them apart): its own date's occurrence, so it doesn't fire
 * again at 07:00 the same day. */
static void test_unsnoozed_early_fire_at(void) {
   TEST_ASSERT_EQUAL_INT64(1792234800, next(1792134000, "07:00", SCHED_RECUR_DAILY, 1792134060));
}

/* Missed for six days while the daemon was down: the next one after now,
 * once. */
static void test_catches_up_once(void) {
   TEST_ASSERT_EQUAL_INT64(1792234800, next(1791630000, "07:00", SCHED_RECUR_DAILY, 1792166400));
}

/* A Monday weekly missed until Thursday: next Monday, not Thursday. */
static void test_weekly_keeps_its_day(void) {
   TEST_ASSERT_EQUAL_INT64(1792414800, next(1791205200, "09:00", SCHED_RECUR_WEEKLY, 1792080000));
}

/* 07:00 the day before clocks go back: 07:00 the next day too (EST). */
static void test_keeps_time_across_dst(void) {
   TEST_ASSERT_EQUAL_INT64(1793534400, next(1793444400, "07:00", SCHED_RECUR_DAILY, 1793444700));
}

/* Tomorrow's occurrence cancelled this evening: the one after it. */
static void test_cancel_ahead_skips_only_itself(void) {
   TEST_ASSERT_EQUAL_INT64(1792321200, next(1792234800, "07:00", SCHED_RECUR_DAILY, 1792195200));
}

/* Weekdays from a Friday: Monday. */
static void test_weekdays_skip_weekend(void) {
   TEST_ASSERT_EQUAL_INT64(1792407600, next(1792148400, "07:00", SCHED_RECUR_WEEKDAYS, 1792148460));
}

/* No original_time: fire_at's time of day. */
static void test_no_original_time(void) {
   TEST_ASSERT_EQUAL_INT64(1792234800, next(1792148400, "", SCHED_RECUR_DAILY, 1792148460));
}

/* Custom days from a Friday: "mon, wed" is Monday, "wed" Wednesday; no days
 * never fires. */
static void test_custom_days(void) {
   TEST_ASSERT_EQUAL_INT64(1792407600,
                           sched_next_occurrence(s_ny, 1792148400, "07:00", SCHED_RECUR_CUSTOM,
                                                 "mon, wed", false, 1792148460));
   TEST_ASSERT_EQUAL_INT64(1792580400,
                           sched_next_occurrence(s_ny, 1792148400, "07:00", SCHED_RECUR_CUSTOM,
                                                 "wed", false, 1792148460));
   TEST_ASSERT_EQUAL_INT64(0, sched_next_occurrence(s_ny, 1792148400, "07:00", SCHED_RECUR_CUSTOM,
                                                    "", false, 1792148460));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_daily_next_day);
   RUN_TEST(test_snoozed_past_midnight);
   RUN_TEST(test_snoozed_in_repeated_hour);
   RUN_TEST(test_unsnoozed_early_fire_at);
   RUN_TEST(test_catches_up_once);
   RUN_TEST(test_weekly_keeps_its_day);
   RUN_TEST(test_keeps_time_across_dst);
   RUN_TEST(test_cancel_ahead_skips_only_itself);
   RUN_TEST(test_weekdays_skip_weekend);
   RUN_TEST(test_no_original_time);
   RUN_TEST(test_custom_days);
   return UNITY_END();
}
