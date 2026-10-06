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
 * Unit tests for the US-equity market-session calendar (NYSE holidays/half-days).
 */

#include <stdbool.h>
#include <time.h>

#include "core/market_hours.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Build a UTC time_t for a wall-clock time in US/Eastern. 2026 DST runs Mar 8 –
 * Nov 1, so pass dst=true for Apr–Oct dates, false otherwise. ET = UTC − offset,
 * so UTC = ET + offset. */
static time_t et_utc(int y, int mon0, int mday, int hh, int mm, bool dst) {
   struct tm t = { 0 };
   t.tm_year = y - 1900;
   t.tm_mon = mon0;
   t.tm_mday = mday;
   t.tm_hour = hh;
   t.tm_min = mm;
   return timegm(&t) + (dst ? 4 : 5) * 3600;
}

#define ASSERT_SESSION(expect, y, mo, d, h, m, dst) \
   TEST_ASSERT_EQUAL_INT((expect), market_hours_us_equity(et_utc((y), (mo), (d), (h), (m), (dst))))

/* A normal EDT weekday (Tue 2026-09-08): every session boundary. */
static void test_regular_and_boundaries(void) {
   ASSERT_SESSION(MARKET_CLOSED, 2026, 8, 8, 6, 0, true); /* before pre */
   ASSERT_SESSION(MARKET_PRE, 2026, 8, 8, 8, 0, true);
   ASSERT_SESSION(MARKET_PRE, 2026, 8, 8, 9, 29, true);
   ASSERT_SESSION(MARKET_REGULAR, 2026, 8, 8, 9, 30, true); /* open */
   ASSERT_SESSION(MARKET_REGULAR, 2026, 8, 8, 12, 0, true);
   ASSERT_SESSION(MARKET_REGULAR, 2026, 8, 8, 15, 59, true);
   ASSERT_SESSION(MARKET_POST, 2026, 8, 8, 16, 0, true); /* close */
   ASSERT_SESSION(MARKET_POST, 2026, 8, 8, 19, 59, true);
   ASSERT_SESSION(MARKET_CLOSED, 2026, 8, 8, 20, 0, true); /* after post */
}

static void test_weekend(void) {
   ASSERT_SESSION(MARKET_CLOSED, 2026, 8, 5, 12, 0, true); /* Sat */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 8, 6, 12, 0, true); /* Sun */
}

/* All 2026 full-close holidays at noon ET; plus a normal weekday for contrast. */
static void test_full_holidays(void) {
   ASSERT_SESSION(MARKET_CLOSED, 2026, 0, 1, 12, 0, false);  /* New Year (Thu) */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 0, 19, 12, 0, false); /* MLK */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 1, 16, 12, 0, false); /* Washington's Birthday */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 3, 3, 12, 0, true);   /* Good Friday (Apr 3) */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 4, 25, 12, 0, true);  /* Memorial */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 5, 19, 12, 0, true);  /* Juneteenth */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 6, 3, 12, 0, true);   /* Independence observed (Fri Jul 3) */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 8, 7, 12, 0, true);   /* Labor Day */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 10, 26, 12, 0, false); /* Thanksgiving */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 11, 25, 12, 0, false); /* Christmas (Fri) */
   ASSERT_SESSION(MARKET_REGULAR, 2026, 0, 2, 12, 0, false);  /* Fri Jan 2 — normal day */
   /* Juneteenth was not an NYSE holiday before 2022 (Fri Jun 19 2020 traded). */
   ASSERT_SESSION(MARKET_REGULAR, 2020, 5, 19, 12, 0, true);
}

static void test_half_days(void) {
   /* Day after Thanksgiving (Fri Nov 27) — early close 13:00 ET. */
   ASSERT_SESSION(MARKET_REGULAR, 2026, 10, 27, 12, 30, false);
   ASSERT_SESSION(MARKET_POST, 2026, 10, 27, 13, 30, false);
   /* Christmas Eve (Thu Dec 24) — early close. */
   ASSERT_SESSION(MARKET_REGULAR, 2026, 11, 24, 12, 0, false);
   ASSERT_SESSION(MARKET_POST, 2026, 11, 24, 14, 0, false);
   /* Guard: Jul 3 2026 is the OBSERVED Independence Day (full close), not a half-day
    * — so 13:30 ET is CLOSED, never POST. */
   ASSERT_SESSION(MARKET_CLOSED, 2026, 6, 3, 13, 30, true);
   /* Guard: a normal Friday (Sep 11) is a full session, not an early close. */
   ASSERT_SESSION(MARKET_REGULAR, 2026, 8, 11, 13, 30, true);
}

static void test_session_strings(void) {
   TEST_ASSERT_EQUAL_STRING("regular", market_session_str(MARKET_REGULAR));
   TEST_ASSERT_EQUAL_STRING("pre", market_session_str(MARKET_PRE));
   TEST_ASSERT_EQUAL_STRING("post", market_session_str(MARKET_POST));
   TEST_ASSERT_EQUAL_STRING("closed", market_session_str(MARKET_CLOSED));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_regular_and_boundaries);
   RUN_TEST(test_weekend);
   RUN_TEST(test_full_holidays);
   RUN_TEST(test_half_days);
   RUN_TEST(test_session_strings);
   return UNITY_END();
}
