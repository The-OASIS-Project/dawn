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
 * Unit tests for the iso8601 absolute timestamp parser.
 *
 * Pins TZ=UTC for the duration of the test process so local-TZ paths
 * (no-suffix datetimes, time-only "today/tomorrow") give deterministic
 * results.  Explicit-offset paths verify the arithmetic is correct
 * regardless of local TZ.
 */

#define _GNU_SOURCE /* timegm */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "core/iso8601.h"
#include "unity.h"

/* Reference epochs computed via timegm() in setUp() so the values aren't
 * brittle to host endianness or platform-specific time_t width. */
static time_t s_2026_02_19_utc;        /* 2026-02-19 00:00:00 UTC */
static time_t s_2026_02_19_153000_utc; /* 2026-02-19 15:30:00 UTC */
static int64_t s_2026_jan1_utc;        /* 2026-01-01 00:00:00 UTC */

static time_t make_utc(int year, int mon, int mday, int hour, int min, int sec) {
   struct tm tm = { 0 };
   tm.tm_year = year - 1900;
   tm.tm_mon = mon - 1;
   tm.tm_mday = mday;
   tm.tm_hour = hour;
   tm.tm_min = min;
   tm.tm_sec = sec;
   return timegm(&tm);
}

void setUp(void) {
   setenv("TZ", "UTC", 1);
   tzset();

   s_2026_02_19_utc = make_utc(2026, 2, 19, 0, 0, 0);
   s_2026_02_19_153000_utc = make_utc(2026, 2, 19, 15, 30, 0);
   s_2026_jan1_utc = (int64_t)make_utc(2026, 1, 1, 0, 0, 0);
}

void tearDown(void) {
}

/* ── iso8601_parse: error paths ─────────────────────────────────────────── */

static void test_parse_null_returns_minus_one(void) {
   TEST_ASSERT_EQUAL_INT64((time_t)-1, iso8601_parse(NULL));
}

static void test_parse_empty_returns_minus_one(void) {
   TEST_ASSERT_EQUAL_INT64((time_t)-1, iso8601_parse(""));
}

static void test_parse_garbage_returns_minus_one(void) {
   TEST_ASSERT_EQUAL_INT64((time_t)-1, iso8601_parse("not a date"));
}

static void test_parse_bad_time_only_returns_minus_one(void) {
   /* Hour 25, minute 99: out-of-range time-only input. */
   TEST_ASSERT_EQUAL_INT64((time_t)-1, iso8601_parse("25:99"));
}

/* ── iso8601_parse: date-only ───────────────────────────────────────────── */

static void test_parse_date_only_utc(void) {
   /* With TZ=UTC, midnight local == midnight UTC. */
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_utc, iso8601_parse("2026-02-19"));
}

/* ── iso8601_parse: full datetime ───────────────────────────────────────── */

static void test_parse_no_tz_local_is_utc(void) {
   /* With TZ=UTC, no-suffix datetime is interpreted as UTC. */
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_153000_utc, iso8601_parse("2026-02-19T15:30:00"));
}

static void test_parse_z_suffix_is_utc(void) {
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_153000_utc, iso8601_parse("2026-02-19T15:30:00Z"));
}

static void test_parse_lowercase_z_suffix_is_utc(void) {
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_153000_utc, iso8601_parse("2026-02-19T15:30:00z"));
}

static void test_parse_negative_offset(void) {
   /* 15:30 at -05:00 == 20:30 UTC.  Independent of local TZ. */
   time_t expected = make_utc(2026, 2, 19, 20, 30, 0);
   TEST_ASSERT_EQUAL_INT64(expected, iso8601_parse("2026-02-19T15:30:00-05:00"));
}

static void test_parse_positive_offset(void) {
   /* 15:30 at +02:00 == 13:30 UTC. */
   time_t expected = make_utc(2026, 2, 19, 13, 30, 0);
   TEST_ASSERT_EQUAL_INT64(expected, iso8601_parse("2026-02-19T15:30:00+02:00"));
}

/* ── iso8601_parse: time-only ───────────────────────────────────────────── */

static void test_parse_basic_format_offset(void) {
   /* -0500 without the colon: 15:30 at -05:00 == 20:30 UTC (not 500 hours). */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc + 5 * 3600,
                           (int64_t)iso8601_parse("2026-02-19T15:30:00-0500"));
   /* Read by position: +0030 is thirty minutes, not a 30-hour offset. */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc - 30 * 60,
                           (int64_t)iso8601_parse("2026-02-19T15:30:00+0030"));
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc - (5 * 3600 + 30 * 60),
                           (int64_t)iso8601_parse("2026-02-19T15:30:00+0530"));
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc + 5 * 3600,
                           (int64_t)iso8601_parse("2026-02-19T15:30:00-05"));
}

static void test_parse_space_separator_keeps_the_time(void) {
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc,
                           (int64_t)iso8601_parse("2026-02-19 15:30"));
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc + 5 * 3600,
                           (int64_t)iso8601_parse("2026-02-19 15:30:00-05:00"));
}

static void test_parse_out_of_range_returns_minus_one(void) {
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-13-19T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-02-40T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-02-19T25:00"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-02-19T15:61"));
   /* A day the month doesn't have isn't rolled into the next month. */
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-02-31T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-02-29T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2026-04-31T15:30"));
   TEST_ASSERT_NOT_EQUAL((int64_t)-1, (int64_t)iso8601_parse("2024-02-29T15:30"));
   TEST_ASSERT_NOT_EQUAL((int64_t)-1, (int64_t)iso8601_parse("2000-02-29T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)iso8601_parse("2100-02-29T15:30"));
}

static void test_parse_out_of_range_offset_is_ignored(void) {
   /* +25:00 isn't an offset: the time is read as local (UTC here). */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc,
                           (int64_t)iso8601_parse("2026-02-19T15:30:00+25:00"));
   /* Offsets stop at +14:00. */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc,
                           (int64_t)iso8601_parse("2026-02-19T15:30:00+14:30"));
}

static void test_parse_time_only_returns_today_or_tomorrow(void) {
   /* Time-only resolves to today (if not yet past) or tomorrow.  Either way,
    * the result must be in [now, now + 24h]. */
   time_t now = time(NULL);
   time_t result = iso8601_parse("12:00");
   TEST_ASSERT_NOT_EQUAL((time_t)-1, result);
   TEST_ASSERT_TRUE(result >= now);
   TEST_ASSERT_TRUE(result <= now + 86400 + 60); /* small slack for clock drift */
}

/* ── iso8601_parse_date_utc ─────────────────────────────────────────────── */

static void test_date_utc_null_returns_zero(void) {
   TEST_ASSERT_EQUAL_INT64(0, iso8601_parse_date_utc(NULL));
}

static void test_date_utc_empty_returns_zero(void) {
   TEST_ASSERT_EQUAL_INT64(0, iso8601_parse_date_utc(""));
}

static void test_date_utc_garbage_returns_zero(void) {
   TEST_ASSERT_EQUAL_INT64(0, iso8601_parse_date_utc("not a date"));
}

static void test_date_utc_full_date(void) {
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_utc, iso8601_parse_date_utc("2026-02-19"));
}

static void test_date_utc_year_only(void) {
   TEST_ASSERT_EQUAL_INT64(s_2026_jan1_utc, iso8601_parse_date_utc("2026"));
}

/* The 0 sentinel is the documented "no bound" value — distinct from
 * iso8601_parse()'s -1 sentinel.  Verify the contracts don't collide. */
static void test_date_utc_sentinel_is_zero_not_minus_one(void) {
   int64_t result = iso8601_parse_date_utc(NULL);
   TEST_ASSERT_EQUAL_INT64(0, result);
   TEST_ASSERT_NOT_EQUAL(-1, result);
}

int main(void) {
   UNITY_BEGIN();

   /* iso8601_parse */
   RUN_TEST(test_parse_null_returns_minus_one);
   RUN_TEST(test_parse_empty_returns_minus_one);
   RUN_TEST(test_parse_garbage_returns_minus_one);
   RUN_TEST(test_parse_bad_time_only_returns_minus_one);
   RUN_TEST(test_parse_date_only_utc);
   RUN_TEST(test_parse_basic_format_offset);
   RUN_TEST(test_parse_space_separator_keeps_the_time);
   RUN_TEST(test_parse_out_of_range_returns_minus_one);
   RUN_TEST(test_parse_out_of_range_offset_is_ignored);
   RUN_TEST(test_parse_no_tz_local_is_utc);
   RUN_TEST(test_parse_z_suffix_is_utc);
   RUN_TEST(test_parse_lowercase_z_suffix_is_utc);
   RUN_TEST(test_parse_negative_offset);
   RUN_TEST(test_parse_positive_offset);
   RUN_TEST(test_parse_time_only_returns_today_or_tomorrow);

   /* iso8601_parse_date_utc */
   RUN_TEST(test_date_utc_null_returns_zero);
   RUN_TEST(test_date_utc_empty_returns_zero);
   RUN_TEST(test_date_utc_garbage_returns_zero);
   RUN_TEST(test_date_utc_full_date);
   RUN_TEST(test_date_utc_year_only);
   RUN_TEST(test_date_utc_sentinel_is_zero_not_minus_one);

   return UNITY_END();
}
