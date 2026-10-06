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
 * Unit tests for calendar_query_window: which stretch of the calendar a
 * message asks about.  Pinned to UTC and a fixed Wednesday noon.
 */

#include <stdlib.h>
#include <time.h>

#include "tools/calendar_query_window.h"
#include "unity.h"

/* 2026-09-30 12:00 UTC, a Wednesday. */
static const time_t NOW = 1790769600;
static const time_t DAY = 86400;
/* Midnight starting that Wednesday. */
static const time_t WED = 1790769600 - 12 * 3600;

void setUp(void) {
   setenv("TZ", "UTC", 1);
   tzset();
}

void tearDown(void) {
}

static calendar_window_t win(const char *q) {
   calendar_window_t w;
   calendar_query_window(q, NULL, NOW, &w);
   return w;
}

static void test_days(void) {
   calendar_window_t w = win("what's on today?");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(WED, w.start);
   TEST_ASSERT_EQUAL_INT64(WED + DAY, w.end);

   w = win("anything tomorrow");
   TEST_ASSERT_EQUAL_INT64(WED + DAY, w.start);
   TEST_ASSERT_EQUAL_INT64(WED + 2 * DAY, w.end);

   w = win("what did I have yesterday");
   TEST_ASSERT_EQUAL_INT64(WED - DAY, w.start);

   w = win("am I free tonight");
   TEST_ASSERT_EQUAL_INT64(WED, w.start);
}

static void test_weeks_and_weekends(void) {
   calendar_window_t w = win("what's happening next week");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(WED + 5 * DAY, w.start); /* the coming Monday */
   TEST_ASSERT_EQUAL_INT64(WED + 12 * DAY, w.end);

   w = win("anything else this week?");
   TEST_ASSERT_EQUAL_INT64(NOW, w.start);
   TEST_ASSERT_EQUAL_INT64(WED + 5 * DAY, w.end);

   w = win("plans for the weekend");
   TEST_ASSERT_EQUAL_INT64(WED + 3 * DAY, w.start); /* Saturday */
   TEST_ASSERT_EQUAL_INT64(WED + 5 * DAY, w.end);

   w = win("and next weekend?");
   TEST_ASSERT_EQUAL_INT64(WED + 10 * DAY, w.start);

   w = win("what did I do last weekend");
   TEST_ASSERT_EQUAL_INT64(WED - 4 * DAY, w.start); /* the Saturday before */
   TEST_ASSERT_EQUAL_INT64(WED - 2 * DAY, w.end);

   /* On a Sunday, last weekend is the one before this one. */
   calendar_window_t s;
   calendar_query_window("last weekend", NULL, NOW + 4 * DAY, &s);
   TEST_ASSERT_EQUAL_INT64(WED - 4 * DAY, s.start);
}

static void test_weekdays(void) {
   calendar_window_t w = win("what do I have on friday");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(WED + 2 * DAY, w.start);
   TEST_ASSERT_EQUAL_INT64(WED + 3 * DAY, w.end);

   w = win("anything wednesday?"); /* today */
   TEST_ASSERT_EQUAL_INT64(WED, w.start);

   w = win("next wednesday"); /* not today */
   TEST_ASSERT_EQUAL_INT64(WED + 7 * DAY, w.start);

   w = win("what happened last monday");
   TEST_ASSERT_EQUAL_INT64(WED - 2 * DAY, w.start);
}

static void test_months(void) {
   calendar_window_t w = win("anything in november");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(1793491200, w.start); /* 2026-11-01 */
   TEST_ASSERT_EQUAL_INT64(1796083200, w.end);   /* 2026-12-01 */

   /* "may" and "march" are ordinary words unless placed as months. */
   TEST_ASSERT_FALSE(win("may I ask something").asks);
   TEST_ASSERT_FALSE(win("they march on").asks);
   TEST_ASSERT_TRUE(win("trips in may").asks);

   /* A month already past, named alone, is the coming one; "last" looks back. */
   w = win("anything in january");
   TEST_ASSERT_EQUAL_INT64(1798761600, w.start); /* 2027-01-01 */
   TEST_ASSERT_EQUAL_INT64(1801440000, w.end);
   w = win("what happened last march");
   TEST_ASSERT_EQUAL_INT64(1772323200, w.start); /* 2026-03-01 */
   TEST_ASSERT_EQUAL_INT64(1775001600, w.end);

   /* A written year pins it. */
   w = win("plans for november 2027");
   TEST_ASSERT_EQUAL_INT64(1825027200, w.start);
   TEST_ASSERT_EQUAL_INT64(1827619200, w.end);

   /* Abbreviations, and a later month word after an ordinary one. */
   w = win("what's on oct 3");
   TEST_ASSERT_EQUAL_INT64(1790812800, w.start); /* 2026-10-01 */
   w = win("may I see what's on may 5");
   TEST_ASSERT_EQUAL_INT64(1809129600, w.start); /* 2027-05-01 */
   TEST_ASSERT_EQUAL_INT64(1811808000, w.end);
   TEST_ASSERT_FALSE(win("ask jan about the budget").asks);

   /* The parser's own placing words place a month too. */
   w = win("what did I do during march");
   TEST_ASSERT_EQUAL_INT64(1772323200, w.start); /* past tense: the last March */
   w = win("what happened in november");
   TEST_ASSERT_EQUAL_INT64(1761955200, w.start); /* 2025-11-01 */
   TEST_ASSERT_TRUE(win("anything around may 20").asks);
   TEST_ASSERT_TRUE(win("trips in mid march").asks);
}

/* All-day date bounds: the window's local dates, end exclusive. */
static void test_window_dates(void) {
   char s[CALENDAR_DATE_LEN];
   char e[CALENDAR_DATE_LEN];
   calendar_window_dates(WED, WED + DAY, s, e); /* one day, ending at midnight */
   TEST_ASSERT_EQUAL_STRING("2026-09-30", s);
   TEST_ASSERT_EQUAL_STRING("2026-10-01", e);
   calendar_window_dates(NOW, NOW + 3 * DAY, s, e); /* ends mid-Saturday */
   TEST_ASSERT_EQUAL_STRING("2026-09-30", s);
   TEST_ASSERT_EQUAL_STRING("2026-10-04", e);
}

/* An assistant called by a weekday's name: the word is a day only where
 * placed as one. */
static void test_assistant_named_after_a_weekday(void) {
   calendar_window_t w;
   calendar_query_window("friday, how does the memory system work", "Friday", NOW, &w);
   TEST_ASSERT_FALSE(w.asks);
   calendar_query_window("friday, anything on friday?", "Friday", NOW, &w);
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(WED + 2 * DAY, w.start);
   calendar_query_window("what do I have next friday", "friday", NOW, &w);
   TEST_ASSERT_TRUE(w.asks);
   /* Other weekdays stay bare-word days. */
   calendar_query_window("friday, anything monday?", "friday", NOW, &w);
   TEST_ASSERT_EQUAL_INT64(WED + 5 * DAY, w.start);
}

static void test_upcoming_schedule_and_past(void) {
   calendar_window_t w = win("anything upcoming?");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(NOW, w.start);
   TEST_ASSERT_EQUAL_INT64(NOW + 14 * DAY, w.end);

   w = win("what's on my calendar");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_EQUAL_INT64(NOW - CALENDAR_WINDOW_DEFAULT_PAST_SECONDS, w.start);
   TEST_ASSERT_EQUAL_INT64(NOW + CALENDAR_WINDOW_DEFAULT_FUTURE_SECONDS, w.end);

   w = win("what did I do last week");
   TEST_ASSERT_TRUE(w.asks);
   TEST_ASSERT_TRUE(w.end <= NOW + DAY);
   TEST_ASSERT_TRUE(w.start < NOW - DAY);
}

static void test_unrelated_messages_ask_nothing(void) {
   TEST_ASSERT_FALSE(win("recall the marigold project plan").asks);
   TEST_ASSERT_FALSE(win("how does the memory system work").asks);
   TEST_ASSERT_FALSE(win("").asks);
   TEST_ASSERT_FALSE(win(NULL).asks);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_days);
   RUN_TEST(test_weeks_and_weekends);
   RUN_TEST(test_weekdays);
   RUN_TEST(test_months);
   RUN_TEST(test_assistant_named_after_a_weekday);
   RUN_TEST(test_window_dates);
   RUN_TEST(test_upcoming_schedule_and_past);
   RUN_TEST(test_unrelated_messages_ask_nothing);
   return UNITY_END();
}
