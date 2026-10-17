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
 * Times are read in a named zone (core/tz.h), never the process's: TZ is
 * set to Asia/Tokyo for the whole run to prove it doesn't matter.
 */

#define _GNU_SOURCE /* timegm */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "core/iso8601.h"
#include "core/tz.h"
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

/* tz_parse() in UTC, with -1 for an error (the old contract these tests
 * were written against). */
static time_t parse_utc(const char *text) {
   time_t t;
   return tz_parse(text, tz_lookup("UTC"), &t) ? t : (time_t)-1;
}

/* tz_parse() in @p zone, or -1. */
static time_t parse_in(const char *zone, const char *text) {
   time_t t;
   return tz_parse(text, tz_lookup(zone), &t) ? t : (time_t)-1;
}

void setUp(void) {
   setenv("TZ", "Asia/Tokyo", 1);
   tzset();

   s_2026_02_19_utc = make_utc(2026, 2, 19, 0, 0, 0);
   s_2026_02_19_153000_utc = make_utc(2026, 2, 19, 15, 30, 0);
   s_2026_jan1_utc = (int64_t)make_utc(2026, 1, 1, 0, 0, 0);
}

void tearDown(void) {
}

/* ── tz_parse in UTC: error paths ─────────────────────────────────────────── */

static void test_parse_null_fails(void) {
   TEST_ASSERT_EQUAL_INT64((time_t)-1, parse_utc(NULL));
}

static void test_parse_empty_fails(void) {
   TEST_ASSERT_EQUAL_INT64((time_t)-1, parse_utc(""));
}

static void test_parse_garbage_fails(void) {
   TEST_ASSERT_EQUAL_INT64((time_t)-1, parse_utc("not a date"));
}

static void test_parse_bad_time_only_fails(void) {
   /* Hour 25, minute 99: out-of-range time-only input. */
   TEST_ASSERT_EQUAL_INT64((time_t)-1, parse_utc("25:99"));
}

/* ── tz_parse in UTC: date-only ───────────────────────────────────────────── */

static void test_parse_date_only_utc(void) {
   /* With TZ=UTC, midnight local == midnight UTC. */
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_utc, parse_utc("2026-02-19"));
}

/* ── tz_parse in UTC: full datetime ───────────────────────────────────────── */

static void test_parse_no_tz_local_is_utc(void) {
   /* With TZ=UTC, no-suffix datetime is interpreted as UTC. */
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_153000_utc, parse_utc("2026-02-19T15:30:00"));
}

static void test_parse_z_suffix_is_utc(void) {
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_153000_utc, parse_utc("2026-02-19T15:30:00Z"));
}

static void test_parse_lowercase_z_suffix_is_utc(void) {
   TEST_ASSERT_EQUAL_INT64(s_2026_02_19_153000_utc, parse_utc("2026-02-19T15:30:00z"));
}

static void test_parse_negative_offset(void) {
   /* 15:30 at -05:00 == 20:30 UTC.  Independent of local TZ. */
   time_t expected = make_utc(2026, 2, 19, 20, 30, 0);
   TEST_ASSERT_EQUAL_INT64(expected, parse_utc("2026-02-19T15:30:00-05:00"));
}

static void test_parse_positive_offset(void) {
   /* 15:30 at +02:00 == 13:30 UTC. */
   time_t expected = make_utc(2026, 2, 19, 13, 30, 0);
   TEST_ASSERT_EQUAL_INT64(expected, parse_utc("2026-02-19T15:30:00+02:00"));
}

/* ── tz_parse in UTC: time-only ───────────────────────────────────────────── */

static void test_parse_basic_format_offset(void) {
   /* -0500 without the colon: 15:30 at -05:00 == 20:30 UTC (not 500 hours). */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc + 5 * 3600,
                           (int64_t)parse_utc("2026-02-19T15:30:00-0500"));
   /* Read by position: +0030 is thirty minutes, not a 30-hour offset. */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc - 30 * 60,
                           (int64_t)parse_utc("2026-02-19T15:30:00+0030"));
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc - (5 * 3600 + 30 * 60),
                           (int64_t)parse_utc("2026-02-19T15:30:00+0530"));
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc + 5 * 3600,
                           (int64_t)parse_utc("2026-02-19T15:30:00-05"));
}

static void test_parse_space_separator_keeps_the_time(void) {
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc,
                           (int64_t)parse_utc("2026-02-19 15:30"));
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc + 5 * 3600,
                           (int64_t)parse_utc("2026-02-19 15:30:00-05:00"));
}

static void test_parse_out_of_range_fails(void) {
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-13-19T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-02-40T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-02-19T25:00"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-02-19T15:61"));
   /* A day the month doesn't have isn't rolled into the next month. */
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-02-31T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-02-29T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2026-04-31T15:30"));
   TEST_ASSERT_NOT_EQUAL((int64_t)-1, (int64_t)parse_utc("2024-02-29T15:30"));
   TEST_ASSERT_NOT_EQUAL((int64_t)-1, (int64_t)parse_utc("2000-02-29T15:30"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("2100-02-29T15:30"));
   /* Years outside 1-9999 (a far-off year from a model must not overflow) */
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("-2147483648-01-01T00:00"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("10000-01-01T00:00"));
   TEST_ASSERT_EQUAL_INT64(-1, (int64_t)parse_utc("0000-01-01T00:00"));
}

static void test_parse_out_of_range_offset_is_ignored(void) {
   /* +25:00 isn't an offset: the time is read as local (UTC here). */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc,
                           (int64_t)parse_utc("2026-02-19T15:30:00+25:00"));
   /* Offsets stop at +14:00. */
   TEST_ASSERT_EQUAL_INT64((int64_t)s_2026_02_19_153000_utc,
                           (int64_t)parse_utc("2026-02-19T15:30:00+14:30"));
}

static void test_parse_time_only_returns_today_or_tomorrow(void) {
   /* Time-only resolves to today (if not yet past) or tomorrow.  Either way,
    * the result must be in [now, now + 24h]. */
   time_t now = time(NULL);
   time_t result = parse_utc("12:00");
   TEST_ASSERT_NOT_EQUAL((time_t)-1, result);
   TEST_ASSERT_TRUE(result >= now);
   TEST_ASSERT_TRUE(result <= now + 86400 + 60); /* small slack for clock drift */
}


/* ── Times in a named zone ──────────────────────────────────────────────── */

static void test_zone_reads_local_time(void) {
   /* 15:30 in New York in February is EST (-5): 20:30 UTC; in Los Angeles
    * (-8) 23:30; in Kolkata (+5:30) 10:00. */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 2, 19, 20, 30, 0),
                           parse_in("America/New_York", "2026-02-19T15:30:00"));
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 2, 19, 23, 30, 0),
                           parse_in("America/Los_Angeles", "2026-02-19T15:30"));
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 2, 19, 10, 0, 0),
                           parse_in("Asia/Kolkata", "2026-02-19T15:30"));
   /* Summer in New York is EDT (-4). */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 7, 1, 19, 30, 0),
                           parse_in("America/New_York", "2026-07-01T15:30"));
}

static void test_offset_ignores_zone(void) {
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 2, 19, 20, 30, 0),
                           parse_in("Asia/Kolkata", "2026-02-19T15:30:00-05:00"));
   /* An offset time on a New York spring-forward morning: plain arithmetic
    * (the old parser was an hour off here). */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 3, 8, 2, 30, 0),
                           parse_in("America/New_York", "2026-03-08T02:30:00Z"));
}

static void test_fractional_seconds_keep_offset(void) {
   /* Discord sends ...T12:00:00.123000+00:00; the fraction must not hide
    * the offset. */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 2, 19, 12, 0, 0),
                           parse_in("America/New_York", "2026-02-19T12:00:00.123000+00:00"));
}

static void test_skipped_time_moves_forward(void) {
   /* New York, 2026-03-08: clocks go 02:00 EST -> 03:00 EDT.  02:30 doesn't
    * exist; it is 03:30 EDT (07:30 UTC), as RFC 5545 and mktime say. */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 3, 8, 7, 30, 0),
                           parse_in("America/New_York", "2026-03-08T02:30"));
   /* Lord Howe moves by 30 minutes: 2026-10-04 02:00 -> 02:30 (+10:30 ->
    * +11).  02:15 doesn't exist; it is 02:45 (+11), 15:45 UTC the day before. */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 10, 3, 15, 45, 0),
                           parse_in("Australia/Lord_Howe", "2026-10-04T02:15"));
}

static void test_repeated_time_is_earlier(void) {
   /* New York, 2026-11-01: 01:30 happens twice (EDT then EST); the first,
    * 05:30 UTC. */
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 11, 1, 5, 30, 0),
                           parse_in("America/New_York", "2026-11-01T01:30"));
}

static void test_wall_round_trip(void) {
   const tz_t *ny = tz_lookup("America/New_York");
   const time_t t = make_utc(2026, 7, 1, 19, 30, 0);
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 7, 1, 15, 30, 0), tz_wall(ny, t));
   TEST_ASSERT_EQUAL_INT64(t, tz_from_wall(ny, tz_wall(ny, t)));
   struct tm tm;
   tz_local(ny, t, &tm);
   TEST_ASSERT_EQUAL_INT(15, tm.tm_hour);
   TEST_ASSERT_EQUAL_INT(3, tm.tm_wday); /* Wednesday */
   TEST_ASSERT_EQUAL_INT(-4 * 3600, (int)tm.tm_gmtoff);
   TEST_ASSERT_EQUAL_INT64(t, tz_mktime(ny, &tm));
}

static void test_mktime_normalizes(void) {
   /* Day 32 of January is February 1st, as with mktime. */
   struct tm tm = { 0 };
   tm.tm_year = 126;
   tm.tm_mon = 0;
   tm.tm_mday = 32;
   tm.tm_hour = 9;
   TEST_ASSERT_EQUAL_INT64(make_utc(2026, 2, 1, 14, 0, 0),
                           tz_mktime(tz_lookup("America/New_York"), &tm));
}

static void test_lookup(void) {
   TEST_ASSERT_NOT_NULL(tz_lookup("America/New_York"));
   TEST_ASSERT_NOT_NULL(tz_lookup("Etc/UTC"));
   TEST_ASSERT_EQUAL_STRING("Asia/Kolkata", tz_name(tz_lookup("Asia/Calcutta"))); /* old name */
   TEST_ASSERT_NULL(tz_lookup("Asia/Mumbai"));                                    /* not a zone */
   TEST_ASSERT_NULL(tz_lookup("../etc/passwd"));
   TEST_ASSERT_NULL(tz_lookup(""));
   TEST_ASSERT_NULL(tz_lookup(NULL));
}

static void test_daemon_zone(void) {
   tz_daemon_init("America/Chicago");
   TEST_ASSERT_EQUAL_STRING("America/Chicago", tz_name(tz_daemon()));
   /* A zoneinfo name libical doesn't list is accepted for the daemon. */
   tz_daemon_init("CET");
   TEST_ASSERT_EQUAL_STRING("CET", tz_name(tz_daemon()));
   /* One that isn't a zone falls back to UTC (with a warning). */
   tz_daemon_init("Not/A_Zone");
   TEST_ASSERT_EQUAL_STRING("UTC", tz_name(tz_daemon()));
   tz_daemon_init("EST5EDT,M3.2.0,M11.1.0");
   TEST_ASSERT_EQUAL_STRING("UTC", tz_name(tz_daemon()));
}

/* A zone's name stays valid after libical loads the zone (libical frees
 * and replaces its own copy then). */
static void test_name_outlives_first_load(void) {
   const tz_t *paris = tz_lookup("Europe/Paris");
   const char *name = tz_name(paris);
   (void)tz_wall(paris, make_utc(2026, 7, 1, 12, 0, 0)); /* first load */
   TEST_ASSERT_EQUAL_STRING("Europe/Paris", name);
}

/* The daemon's zone from TZ=:<file>, a link into zoneinfo or a copy of a
 * zone file (systemd and Docker setups), as the C library reads it. */
static void test_daemon_zone_from_file(void) {
   setenv("TZ", ":/usr/share/zoneinfo/Europe/Paris", 1);
   tz_daemon_init("");
   TEST_ASSERT_EQUAL_STRING("Europe/Paris", tz_name(tz_daemon()));

   char copy[] = "/tmp/tz_copy_XXXXXX";
   const int fd = mkstemp(copy);
   TEST_ASSERT_TRUE(fd >= 0);
   FILE *in = fopen("/usr/share/zoneinfo/America/Denver", "rb");
   TEST_ASSERT_NOT_NULL(in);
   char buf[4096];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      TEST_ASSERT_EQUAL_INT((int)n, (int)write(fd, buf, n));
   fclose(in);
   close(fd);
   char tz[64];
   snprintf(tz, sizeof(tz), ":%s", copy);
   setenv("TZ", tz, 1);
   tz_daemon_init("");
   unlink(copy);
   TEST_ASSERT_EQUAL_STRING("America/Denver", tz_name(tz_daemon()));
   setenv("TZ", "Asia/Tokyo", 1);
}

static void test_time_only_steps_a_calendar_day(void) {
   /* "HH:MM" is the next time that clock time comes, in the zone. */
   const tz_t *ny = tz_lookup("America/New_York");
   time_t t;
   TEST_ASSERT_TRUE(tz_parse("07:00", ny, &t));
   const time_t now = time(NULL);
   TEST_ASSERT_TRUE(t > now);
   TEST_ASSERT_TRUE(t <= now + 25 * 3600);
   struct tm tm;
   tz_local(ny, t, &tm);
   TEST_ASSERT_EQUAL_INT(7, tm.tm_hour);
   TEST_ASSERT_EQUAL_INT(0, tm.tm_min);
}

/* Conversions run on many threads at once (tools, the scheduler, turns),
 * including a zone's first use; run under ThreadSanitizer in CI. */
static void *convert_many(void *arg) {
   const tz_t *zone = tz_lookup((const char *)arg);
   long bad = 0;
   for (int i = 0; i < 2000; i++) {
      const time_t t = make_utc(2026, 1 + i % 12, 1 + i % 28, i % 24, 0, 0);
      if (tz_from_wall(zone, tz_wall(zone, t)) != t)
         bad++;
   }
   return (void *)bad;
}

static void test_concurrent_conversions(void) {
   static const char *const zones[] = { "Europe/Oslo", "Asia/Kolkata", "America/Halifax",
                                        "Australia/Adelaide" };
   pthread_t threads[4];
   for (int i = 0; i < 4; i++)
      TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, convert_many, (void *)zones[i]));
   for (int i = 0; i < 4; i++) {
      void *bad = NULL;
      pthread_join(threads[i], &bad);
      TEST_ASSERT_EQUAL_INT64(0, (long)bad);
   }
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

/* The 0 sentinel is the documented "no bound" value — distinct from the
 * -1 these tests use for a time that doesn't parse.  Verify the contracts don't collide. */
static void test_date_utc_sentinel_is_zero_not_minus_one(void) {
   int64_t result = iso8601_parse_date_utc(NULL);
   TEST_ASSERT_EQUAL_INT64(0, result);
   TEST_ASSERT_NOT_EQUAL(-1, result);
}

int main(void) {
   UNITY_BEGIN();

   /* tz_parse in UTC */
   RUN_TEST(test_parse_null_fails);
   RUN_TEST(test_parse_empty_fails);
   RUN_TEST(test_parse_garbage_fails);
   RUN_TEST(test_parse_bad_time_only_fails);
   RUN_TEST(test_parse_date_only_utc);
   RUN_TEST(test_parse_basic_format_offset);
   RUN_TEST(test_parse_space_separator_keeps_the_time);
   RUN_TEST(test_parse_out_of_range_fails);
   RUN_TEST(test_parse_out_of_range_offset_is_ignored);
   RUN_TEST(test_parse_no_tz_local_is_utc);
   RUN_TEST(test_parse_z_suffix_is_utc);
   RUN_TEST(test_parse_lowercase_z_suffix_is_utc);
   RUN_TEST(test_parse_negative_offset);
   RUN_TEST(test_parse_positive_offset);
   RUN_TEST(test_parse_time_only_returns_today_or_tomorrow);

   /* tz */
   RUN_TEST(test_zone_reads_local_time);
   RUN_TEST(test_offset_ignores_zone);
   RUN_TEST(test_fractional_seconds_keep_offset);
   RUN_TEST(test_skipped_time_moves_forward);
   RUN_TEST(test_repeated_time_is_earlier);
   RUN_TEST(test_wall_round_trip);
   RUN_TEST(test_mktime_normalizes);
   RUN_TEST(test_lookup);
   RUN_TEST(test_daemon_zone);
   RUN_TEST(test_time_only_steps_a_calendar_day);
   RUN_TEST(test_name_outlives_first_load);
   RUN_TEST(test_daemon_zone_from_file);
   RUN_TEST(test_concurrent_conversions);

   /* iso8601_parse_date_utc */
   RUN_TEST(test_date_utc_null_returns_zero);
   RUN_TEST(test_date_utc_empty_returns_zero);
   RUN_TEST(test_date_utc_garbage_returns_zero);
   RUN_TEST(test_date_utc_full_date);
   RUN_TEST(test_date_utc_year_only);
   RUN_TEST(test_date_utc_sentinel_is_zero_not_minus_one);

   return UNITY_END();
}
