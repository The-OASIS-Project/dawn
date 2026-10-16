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
 * calendar_rrule_expand: a recurring event's occurrences in a window, with
 * bounded work whatever the rule says.
 */

#include <stdbool.h>
#include <time.h>

#include "dawn_error.h"
#include "tools/calendar_rrule.h"
#include "unity.h"

#define DAY 86400
#define OCT_1_2026 1790812800 /* 2026-10-01 00:00 UTC */
#define JAN_1_2020 1577836800
#define JAN_1_1970 0
#define FEB_1_2027 1801440000
#define FEB_28_2027 1803772800
#define MAR_1_2027 1803859200

typedef struct {
   int n;
   time_t first;
   time_t last;
} seen_t;

static void record(time_t start, void *ctx) {
   seen_t *s = ctx;
   if (s->n == 0)
      s->first = start;
   s->last = start;
   s->n++;
}

static int expand(const char *rule,
                  time_t dtstart,
                  time_t from,
                  time_t to,
                  seen_t *seen,
                  int *count) {
   *seen = (seen_t){ 0 };
   return calendar_rrule_expand(rule, dtstart, NULL, false, "", 3600, from, to, record, seen,
                                count);
}

void setUp(void) {
}

void tearDown(void) {
}

static void test_daily_in_window(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=DAILY", JAN_1_2020 + 9 * 3600, OCT_1_2026,
                                         OCT_1_2026 + 7 * DAY - 1, &s, &n));
   TEST_ASSERT_EQUAL_INT(7, n);
   TEST_ASSERT_EQUAL_INT(7, s.n);
   TEST_ASSERT_EQUAL_INT64(OCT_1_2026 + 9 * 3600, (int64_t)s.first);
}

/* Every hour since 1970 is ~500,000 steps to reach the window; a rule with
 * no COUNT starts at it. */
static void test_open_rule_starts_at_window(void) {
   seen_t s;
   int n = 0;
   const time_t t0 = time(NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         expand("FREQ=HOURLY", JAN_1_1970, OCT_1_2026, OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
   TEST_ASSERT_EQUAL_INT(26, n); /* 23:00 (overlapping 00:00) through 00:00 the next day */
}

/* Starting at the window keeps the rule's hours: every 5 hours from 09:00. */
static void test_hourly_keeps_phase(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=HOURLY;INTERVAL=5", JAN_1_2020 + 9 * 3600,
                                         OCT_1_2026, OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(6, n);
   TEST_ASSERT_EQUAL_INT64(1790809200, (int64_t)s.first); /* 2026-09-30 23:00 */
   TEST_ASSERT_EQUAL_INT64(1790809200 + 5 * 5 * 3600, (int64_t)s.last);
}

/* A rule with no match left before the window's end (February 29th, next in
 * 2028) made libical search to its own limit inside one step (~11 s hourly);
 * ending the rule at the window bounds the search. */
static void test_never_matching_rule_bounded(void) {
   seen_t s;
   int n = 0;
   const time_t t0 = time(NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=HOURLY;BYMONTH=2;BYMONTHDAY=29", JAN_1_2020,
                                         OCT_1_2026, OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* A COUNT rule must be counted from its start, so one that starts too many
 * periods before the window is refused. */
static void test_counted_rule_from_long_ago_refused(void) {
   seen_t s;
   int n = 0;
   const time_t t0 = time(NULL);
   TEST_ASSERT_EQUAL_INT(FAILURE, expand("FREQ=HOURLY;COUNT=1000000000", JAN_1_1970, OCT_1_2026,
                                         OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* A COUNT rule within the period limit that still can't reach the window in
 * the step cap is refused (stored once), not taken as "no occurrences". */
static void test_counted_rule_past_step_cap_refused(void) {
   seen_t s;
   int n = 0;
   const time_t jan_1_1990 = 631152000;
   TEST_ASSERT_EQUAL_INT(FAILURE, expand("FREQ=DAILY;COUNT=20000", jan_1_1990, OCT_1_2026,
                                         OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_sub_hourly_refused(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         expand("FREQ=MINUTELY", OCT_1_2026, OCT_1_2026, OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(FAILURE, expand("FREQ=SECONDLY;BYMONTH=1", OCT_1_2026, OCT_1_2026,
                                         OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(0, s.n);
}

/* An ordinary COUNT rule: ten Mondays from 2026-09-28, the first before the
 * window. */
static void test_counted_rule(void) {
   seen_t s;
   int n = 0;
   const time_t sep_28 = OCT_1_2026 - 3 * DAY;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=WEEKLY;BYDAY=MO;COUNT=10", sep_28 + 9 * 3600,
                                         OCT_1_2026, OCT_1_2026 + 120 * DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(9, n);
   TEST_ASSERT_EQUAL_INT64(sep_28 + 7 * DAY + 9 * 3600, (int64_t)s.first);
   TEST_ASSERT_EQUAL_INT64(sep_28 + 63 * DAY + 9 * 3600, (int64_t)s.last);
}

/* A weekly 10:00 in New York stays at 10:00 across the November change:
 * 14:00 UTC before it, 15:00 after. */
static void test_repeats_in_its_zone(void) {
   seen_t s = { 0 };
   int n = 0;
   const time_t oct_5_14z = 1791208800;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, calendar_rrule_expand("FREQ=WEEKLY", oct_5_14z, "America/New_York", false, "", 3600,
                                      OCT_1_2026, OCT_1_2026 + 40 * DAY - 1, record, &s, &n));
   TEST_ASSERT_EQUAL_INT(6, n); /* Oct 5 .. Nov 9 */
   TEST_ASSERT_EQUAL_INT64(oct_5_14z, (int64_t)s.first);
   TEST_ASSERT_EQUAL_INT64(1793631600 + 7 * DAY, (int64_t)s.last); /* Nov 9, 15:00 UTC */

   /* A name that would leave the zone directory isn't looked up: UTC. */
   s = (seen_t){ 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, calendar_rrule_expand("FREQ=WEEKLY", oct_5_14z, "../../etc/x",
                                                        false, "", 3600, OCT_1_2026,
                                                        OCT_1_2026 + 40 * DAY - 1, record, &s, &n));
   TEST_ASSERT_EQUAL_INT64(oct_5_14z + 5 * 7 * DAY, (int64_t)s.last);
}

/* Rules libical would search for seconds (or, with a non-Gregorian RSCALE,
 * forever) are refused at once. */
static void test_impossible_rules_refused(void) {
   static const char *const rules[] = {
      "RSCALE=CHINESE;FREQ=MONTHLY;BYDAY=6MO",
      "RSCALE=CHINESE;FREQ=MONTHLY;BYDAY=1MO",
      "FREQ=MONTHLY;BYDAY=6MO",
      "FREQ=MONTHLY;BYDAY=1MO;BYSETPOS=2",   /* one date a month: no second */
      "FREQ=MONTHLY;BYMONTHDAY=1;BYDAY=2MO", /* the 1st is never a second Monday */
      "FREQ=MONTHLY;BYMONTHDAY=1,2,3,4,5,6,7;BYDAY=MO;BYSETPOS=2", /* one Monday in days 1-7 */
      "FREQ=MONTHLY;BYDAY=MO;BYSETPOS=-6",
      "FREQ=MONTHLY;BYMONTH=2;BYMONTHDAY=30",
      "FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR,SA,SU;BYSETPOS=32",
      "FREQ=YEARLY;BYMONTH=3;BYDAY=6MO",
      "FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30",
      "FREQ=YEARLY;BYDAY=MO,TU,WE,TH,FR,SA,SU;BYMONTHDAY=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,"
      "17,18,19,20,21,22,23,24,25,26,27,28,29,30,31;BYMONTH=1,2,3,4,5,6,7,8,9,10,11;BYSETPOS=366",
   };
   const time_t t0 = time(NULL);
   for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
      seen_t s;
      int n = 7;
      TEST_ASSERT_EQUAL_INT_MESSAGE(
          FAILURE, expand(rules[i], OCT_1_2026, OCT_1_2026, OCT_1_2026 + DAY, &s, &n), rules[i]);
      TEST_ASSERT_EQUAL_INT(0, n);
   }
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
}

/* What calendar apps write is kept: every weekday, the last weekday of the
 * month, the last day of the month. */
static void test_real_rules_kept(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=DAILY;INTERVAL=1;BYDAY=MO,TU,WE,TH,FR", OCT_1_2026,
                                         OCT_1_2026, OCT_1_2026 + 7 * DAY - 1, &s, &n));
   TEST_ASSERT_EQUAL_INT(5, n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1",
                                         JAN_1_2020, OCT_1_2026, OCT_1_2026 + 60 * DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(1793318400, (int64_t)s.first); /* Fri 2026-10-30 */
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=MONTHLY;BYMONTHDAY=-1", JAN_1_2020, OCT_1_2026,
                                         OCT_1_2026 + 60 * DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(1793404800, (int64_t)s.first); /* 2026-10-31 */
}

/* A BY list's entries are alternatives: an impossible one doesn't refuse a
 * rule another can satisfy ("the last of February 28-31"). */
static void test_mixed_alternatives_kept(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=28,29,30,31;BYSETPOS=-1",
                                         JAN_1_2020, FEB_1_2027, MAR_1_2027, &s, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(FEB_28_2027, (int64_t)s.first);
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=MONTHLY;BYDAY=6MO,1MO", JAN_1_2020, FEB_1_2027,
                                         MAR_1_2027, &s, &n));
   TEST_ASSERT_EQUAL_INT(2, n); /* Feb 1 and Mar 1 2027, both Mondays */
}

/* Each date's times count as BYSETPOS candidates (RFC 5545), so a second on
 * the 1st isn't refused.  libical 3.0 picks BYSETPOS among dates only and
 * expands it to nothing, so the event is then stored once. */
static void test_setpos_counts_seconds(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=MONTHLY;BYMONTHDAY=1;BYSECOND=0,30;BYSETPOS=2",
                                         JAN_1_2020, OCT_1_2026, OCT_1_2026 + 40 * DAY, &s, &n));
}

/* An hourly rule started in summer keeps its wall-clock hours in winter: the
 * jump to the window steps whole local hours, not UTC seconds. */
static void test_hourly_phase_across_dst(void) {
   seen_t s = { 0 };
   int n = 0;
   const time_t jun_1_0900_edt = 1780318800;
   const time_t dec_1_2026 = 1796083200;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         calendar_rrule_expand("FREQ=HOURLY;INTERVAL=5", jun_1_0900_edt,
                                               "America/New_York", false, "", 3600, dec_1_2026,
                                               dec_1_2026 + DAY, record, &s, &n));
   TEST_ASSERT_EQUAL_INT(5, n);
   TEST_ASSERT_EQUAL_INT64(1796090400, (int64_t)s.first); /* Nov 30 21:00 EST */
}

/* A weekly 02:30 in New York on the morning clocks skip 02:00-03:00 (Mar 8,
 * 2026) happens at 03:30 EDT (07:30 UTC), as RFC 5545 says; not 01:30. */
static void test_skipped_time_moves_forward(void) {
   seen_t s = { 0 };
   int n = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, calendar_rrule_expand("FREQ=WEEKLY", 1772350200 /* Mar 1 02:30 EST */,
                                      "America/New_York", false, "", 3600, 1772841600 /* Mar 7 */,
                                      1773014400 /* Mar 9 */, record, &s, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(1772955000 /* Mar 8 07:30 UTC */, (int64_t)s.first);
}

/* A huge duration doesn't move where an open rule starts. */
static void test_huge_duration_bounded(void) {
   seen_t s = { 0 };
   int n = 0;
   const time_t t0 = time(NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         calendar_rrule_expand("FREQ=HOURLY;BYMONTH=2;BYMONTHDAY=29", JAN_1_1970,
                                               NULL, false, "", 2147483647, OCT_1_2026,
                                               OCT_1_2026 + DAY, record, &s, &n));
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
}

static void test_occurrence_cap(void) {
   seen_t s;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, expand("FREQ=HOURLY", OCT_1_2026, OCT_1_2026,
                                         OCT_1_2026 + 30 * DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(CALENDAR_RRULE_MAX_OCCURRENCES, n);
}

static void test_all_day_yearly(void) {
   seen_t s = { 0 };
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         calendar_rrule_expand("FREQ=YEARLY", 0, NULL, true, "2020-12-25", DAY,
                                               OCT_1_2026, OCT_1_2026 + 120 * DAY, record, &s, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(1798156800, (int64_t)s.first); /* 2026-12-25 */
}

static void test_bad_rule(void) {
   seen_t s;
   int n = 7;
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         expand("NOT A RULE", JAN_1_2020, OCT_1_2026, OCT_1_2026 + DAY, &s, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_daily_in_window);
   RUN_TEST(test_open_rule_starts_at_window);
   RUN_TEST(test_hourly_keeps_phase);
   RUN_TEST(test_never_matching_rule_bounded);
   RUN_TEST(test_counted_rule_from_long_ago_refused);
   RUN_TEST(test_counted_rule_past_step_cap_refused);
   RUN_TEST(test_sub_hourly_refused);
   RUN_TEST(test_counted_rule);
   RUN_TEST(test_repeats_in_its_zone);
   RUN_TEST(test_impossible_rules_refused);
   RUN_TEST(test_real_rules_kept);
   RUN_TEST(test_mixed_alternatives_kept);
   RUN_TEST(test_setpos_counts_seconds);
   RUN_TEST(test_hourly_phase_across_dst);
   RUN_TEST(test_skipped_time_moves_forward);
   RUN_TEST(test_huge_duration_bounded);
   RUN_TEST(test_occurrence_cap);
   RUN_TEST(test_all_day_yearly);
   RUN_TEST(test_bad_rule);
   return UNITY_END();
}
