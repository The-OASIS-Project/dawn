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
 * Reading, building and editing iCalendar events (calendar_ical.c): an edit
 * changes only what it was asked to, and refuses what it can't do safely.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tools/calendar_ical.h"
#include "unity.h"

/* America/New_York, as Google sends it. */
#define VTZ_NY                                  \
   "BEGIN:VTIMEZONE\r\n"                        \
   "TZID:America/New_York\r\n"                  \
   "X-LIC-LOCATION:America/New_York\r\n"        \
   "BEGIN:DAYLIGHT\r\n"                         \
   "TZOFFSETFROM:-0500\r\n"                     \
   "TZOFFSETTO:-0400\r\n"                       \
   "TZNAME:EDT\r\n"                             \
   "DTSTART:19700308T020000\r\n"                \
   "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\r\n"  \
   "END:DAYLIGHT\r\n"                           \
   "BEGIN:STANDARD\r\n"                         \
   "TZOFFSETFROM:-0400\r\n"                     \
   "TZOFFSETTO:-0500\r\n"                       \
   "TZNAME:EST\r\n"                             \
   "DTSTART:19701101T020000\r\n"                \
   "RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\r\n" \
   "END:STANDARD\r\n"                           \
   "END:VTIMEZONE\r\n"

#define ALARM_30                               \
   "BEGIN:VALARM\r\n"                          \
   "ACTION:DISPLAY\r\n"                        \
   "DESCRIPTION:This is an event reminder\r\n" \
   "TRIGGER:-P0DT0H30M0S\r\n"                  \
   "END:VALARM\r\n"

/* A weekly Mon-Thu series, Oct 19-22 2026, 17:30-21:00 New York. */
static const char *TECH = "BEGIN:VCALENDAR\r\n"
                          "PRODID:-//Google Inc//Google Calendar 70.9054//EN\r\n"
                          "VERSION:2.0\r\n"
                          "CALSCALE:GREGORIAN\r\n" VTZ_NY "BEGIN:VEVENT\r\n"
                          "DTSTART;TZID=America/New_York:20261019T173000\r\n"
                          "DTEND;TZID=America/New_York:20261019T210000\r\n"
                          "RRULE:FREQ=WEEKLY;WKST=SU;UNTIL=20261023T035959Z;INTERVAL=1;BYDAY=MO,"
                          "TU,WE,TH\r\n"
                          "DTSTAMP:20260829T235533Z\r\n"
                          "UID:tech-series@example.com\r\n"
                          "SEQUENCE:0\r\n"
                          "STATUS:CONFIRMED\r\n"
                          "DESCRIPTION:\r\n" /* Google writes empty ones */
                          "LOCATION:\r\n"
                          "SUMMARY:TECH REHEARSAL Spring Show\r\n"
                          "X-EXAMPLE-KEEP:kept as it was\r\n"
                          "TRANSP:OPAQUE\r\n" ALARM_30 "END:VEVENT\r\n"
                          "END:VCALENDAR\r\n";

/* A Tue-Thu series, Oct 1-15 2026, with five dates cancelled. */
static const char *PRE = "BEGIN:VCALENDAR\r\n"
                         "PRODID:-//Google Inc//Google Calendar 70.9054//EN\r\n"
                         "VERSION:2.0\r\n" VTZ_NY "BEGIN:VEVENT\r\n"
                         "DTSTART;TZID=America/New_York:20261001T173000\r\n"
                         "DTEND;TZID=America/New_York:20261001T210000\r\n"
                         "RRULE:FREQ=WEEKLY;WKST=SU;UNTIL=20261016T035959Z;INTERVAL=1;BYDAY=TU,"
                         "WE,TH\r\n"
                         "EXDATE;TZID=America/New_York:20261001T173000\r\n"
                         "EXDATE;TZID=America/New_York:20261006T173000\r\n"
                         "EXDATE;TZID=America/New_York:20261007T173000\r\n"
                         "EXDATE;TZID=America/New_York:20261008T173000\r\n"
                         "EXDATE;TZID=America/New_York:20261015T173000\r\n"
                         "DTSTAMP:20261014T160105Z\r\n"
                         "UID:pre-series@example.com\r\n"
                         "SEQUENCE:0\r\n"
                         "SUMMARY:PRE-TECH Spring Show Rehearsal\r\n" ALARM_30 "END:VEVENT\r\n"
                         "END:VCALENDAR\r\n";

/* TECH after the old update: UTC times, no rule, no zone, no reminder. */
static const char *DAMAGED = "BEGIN:VCALENDAR\r\n"
                             "VERSION:2.0\r\n"
                             "BEGIN:VEVENT\r\n"
                             "DTSTART:20261019T213000Z\r\n"
                             "DTEND:20261020T010000Z\r\n"
                             "DTSTAMP:20261015T192113Z\r\n"
                             "UID:tech-series@example.com\r\n"
                             "SEQUENCE:0\r\n"
                             "SUMMARY:TECH REHEARSAL Spring Show\r\n"
                             "END:VEVENT\r\n"
                             "END:VCALENDAR\r\n";

static time_t utc(int y, int mo, int d, int h, int mi) {
   struct tm t = { .tm_year = y - 1900,
                   .tm_mon = mo - 1,
                   .tm_mday = d,
                   .tm_hour = h,
                   .tm_min = mi };
   return timegm(&t);
}

/* @p ical without its line folding, so a search sees whole lines. */
static char *unfold(const char *ical) {
   char *out = malloc(strlen(ical) + 1);
   size_t n = 0;
   for (const char *p = ical; *p; p++) {
      if (p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')) {
         p += 2;
         continue;
      }
      out[n++] = *p;
   }
   out[n] = '\0';
   return out;
}

static int count_of(const char *hay, const char *needle) {
   int n = 0;
   for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
      n++;
   return n;
}

/* Edit @p ical with @p c; the result, unfolded (NULL when refused). */
static char *edit(const char *ical, const calendar_ical_change_t *c, calendar_ical_rc_t *rc) {
   char *out = NULL;
   *rc = calendar_ical_edit(ical, c, &out);
   if (!out)
      return NULL;
   char *u = unfold(out);
   free(out);
   return u;
}

static void read_event(const char *ical, caldav_event_t *evt) {
   memset(evt, 0, sizeof(*evt));
   TEST_ASSERT_TRUE(calendar_ical_read(ical, evt, NULL));
}

void setUp(void) {
}

void tearDown(void) {
}

/* ---- reading ---- */

static void test_read_cancelled_dates(void) {
   caldav_event_t e;
   read_event(PRE, &e);
   TEST_ASSERT_EQUAL_STRING("America/New_York", e.tzid);
   TEST_ASSERT_EQUAL_INT(5, e.exdate_count);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 1, 21, 30), e.exdates[0]);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 15, 21, 30), e.exdates[4]);
   caldav_event_free(&e);
}

static void test_read_moved_and_cancelled_instances(void) {
   static const char *ical =
       "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" VTZ_NY
       "BEGIN:VEVENT\r\nUID:s@x\r\nDTSTART;TZID=America/New_York:20261019T173000\r\n"
       "DTEND;TZID=America/New_York:20261019T210000\r\nRRULE:FREQ=DAILY;COUNT=4\r\n"
       "SUMMARY:Series\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:s@x\r\nRECURRENCE-ID;TZID=America/New_York:20261020T173000\r\n"
       "DTSTART;TZID=America/New_York:20261020T190000\r\n"
       "DTEND;TZID=America/New_York:20261020T220000\r\nSEQUENCE:1\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:s@x\r\nRECURRENCE-ID:20261021T213000Z\r\nSTATUS:CANCELLED\r\n"
       "DTSTART:20261021T213000Z\r\nEND:VEVENT\r\n"
       "END:VCALENDAR\r\n";
   caldav_event_t e;
   read_event(ical, &e);
   TEST_ASSERT_EQUAL_INT(2, e.override_count);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 21, 30), e.overrides[0].original);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 23, 0), e.overrides[0].dtstart);
   TEST_ASSERT_FALSE(e.overrides[0].cancelled);
   TEST_ASSERT_EQUAL_STRING("Series", e.overrides[0].summary); /* the series' title */
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 21, 21, 30), e.overrides[1].original);
   TEST_ASSERT_TRUE(e.overrides[1].cancelled);
   caldav_event_free(&e);
}

/* A TZID with ".." on a cancelled date is never looked up: the date reads as UTC. */
static void test_read_unsafe_tzid_on_exdate(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:u@x\r\n"
                             "DTSTART:20261019T213000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"
                             "EXDATE;TZID=../../etc/passwd:20261020T213000\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
   caldav_event_t e;
   read_event(ical, &e);
   TEST_ASSERT_EQUAL_INT(1, e.exdate_count);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 21, 30), e.exdates[0]);
   caldav_event_free(&e);
}

/* ---- editing the series ---- */

static void test_rename_keeps_everything_else(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "[Guest] PRE-TECH Spring Show Rehearsal";
   calendar_ical_rc_t rc;
   char *out = edit(PRE, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "SUMMARY:[Guest] PRE-TECH Spring Show Rehearsal"));
   TEST_ASSERT_NOT_NULL(strstr(out, "RRULE:FREQ=WEEKLY"));
   TEST_ASSERT_EQUAL_INT(5, count_of(out, "EXDATE;TZID=America/New_York:"));
   TEST_ASSERT_NOT_NULL(strstr(out, "BEGIN:VTIMEZONE"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TRIGGER:-PT30M"));
   TEST_ASSERT_NOT_NULL(strstr(out, "SEQUENCE:0")); /* a rename isn't a reschedule */
   TEST_ASSERT_NOT_NULL(strstr(out, "LAST-MODIFIED:"));

   caldav_event_t e;
   read_event(out, &e);
   TEST_ASSERT_EQUAL_INT(5, e.exdate_count);
   TEST_ASSERT_EQUAL_STRING("America/New_York", e.tzid);
   TEST_ASSERT_TRUE(strstr(e.rrule, "UNTIL=20261016T035959Z") != NULL);
   caldav_event_free(&e);
   free(out);

   out = edit(TECH, &c, &rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "X-EXAMPLE-KEEP:kept as it was"));
   TEST_ASSERT_NULL(strstr(out, "X-LIC-ERROR")); /* the empty lines' markers aren't written */
   free(out);
}

/* libical drops a property it can't read (an RRULE with a part it doesn't
 * know); writing that back would lose the series. */
static void test_unreadable_part_refused(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:u@x\r\n"
                             "DTSTART:20261019T213000Z\r\n"
                             "RRULE:FREQ=WEEKLY;UNTIL=20261023T035959Z;X-VENDOR=1\r\n"
                             "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "b";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_UNREADABLE, rc);
}

/* Moving the series an hour moves its cancelled dates with it. */
static void test_move_series_shifts_exceptions(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 1, 22, 30); /* 18:30 New York */
   calendar_ical_rc_t rc;
   char *out = edit(PRE, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261001T183000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=America/New_York:20261001T220000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "SEQUENCE:1"));
   caldav_event_t e;
   read_event(out, &e);
   TEST_ASSERT_EQUAL_INT(5, e.exdate_count);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 1, 22, 30), e.exdates[0]);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 15, 22, 30), e.exdates[4]);
   caldav_event_free(&e);
   free(out);
}

/* The repair: a rule on a UTC event moves it to the user's zone, so it
 * repeats at 17:30 local; a date UNTIL becomes the end of that day in UTC. */
static void test_rule_on_utc_event_reanchors(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.rrule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH;UNTIL=20261022";
   c.user_tzid = "America/New_York";
   c.reminder_minutes = 30;
   calendar_ical_rc_t rc;
   char *out = edit(DAMAGED, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261019T173000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=America/New_York:20261019T210000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TZID:America/New_York\r\n")); /* its VTIMEZONE */
   TEST_ASSERT_NULL(strstr(out, "freeassociation"));
   TEST_ASSERT_NOT_NULL(strstr(out, "UNTIL=20261023T035959Z"));
   TEST_ASSERT_NOT_NULL(strstr(out, "ACTION:DISPLAY"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TRIGGER:-PT30M"));
   caldav_event_t e;
   read_event(out, &e);
   TEST_ASSERT_EQUAL_STRING("America/New_York", e.tzid);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 19, 21, 30), e.dtstart);
   caldav_event_free(&e);
   free(out);
}

/* "" isn't a rule (a model fills fields it leaves alone with ""); ending a
 * series is a rule with UNTIL. */
static void test_empty_rule_refused(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.rrule = "";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(PRE, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_RULE, rc);
}

/* On a repeating series, a start names a time of day: its date (often
 * today's) isn't a new first date, so the series and its cancelled dates stay
 * on their days.  A new rule with the start moves the days. */
static void test_series_start_is_time_of_day(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 15, 22, 30); /* "today" at 18:30 New York */
   calendar_ical_rc_t rc;
   char *out = edit(PRE, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261001T183000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "EXDATE;TZID=America/New_York:20261006T183000"));
   free(out);

   c.start = utc(2026, 10, 7, 21, 30); /* Wednesday, with its new rule */
   c.rrule = "FREQ=WEEKLY;BYDAY=WE,TH,FR;UNTIL=20261017T035959Z";
   out = edit(PRE, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261007T173000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "BYDAY=WE,TH,FR"));
   free(out);
}

/* A new end on a series is a time of day: with a new rule and today's date it
 * still ends the same day (not two weeks later); a multi-day event keeps its
 * days. */
static void test_series_end_is_time_of_day(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.rrule = "FREQ=WEEKLY;BYDAY=TU,WE,TH;UNTIL=20261016T035959Z";
   c.set_end = true;
   c.end = utc(2026, 10, 15, 23, 0); /* "today" at 19:00 New York */
   calendar_ical_rc_t rc;
   char *out = edit(PRE, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=America/New_York:20261001T190000"));
   free(out);

   static const char *weekend = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:w@x\r\n"
                                "DTSTART:20261016T220000Z\r\nDTEND:20261018T220000Z\r\n"
                                "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:a\r\n"
                                "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t e = CALENDAR_ICAL_CHANGE_INIT;
   e.set_end = true;
   e.end = utc(2026, 10, 18, 23, 0);
   e.user_tzid = "UTC"; /* keep it in UTC for an exact check */
   out = edit(weekend, &e, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND:20261018T230000Z"));
   free(out);
}

/* An event crossing midnight by its hours moved to an evening ends that
 * evening (its length had no whole day); one more than a day long keeps it. */
static void test_series_overnight_moves(void) {
   static const char *late = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:n@x\r\n"
                             "DTSTART:20261016T230000Z\r\nDTEND:20261017T010000Z\r\n"
                             "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:a\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = c.set_end = true;
   c.start = utc(2026, 10, 15, 20, 0);
   c.end = utc(2026, 10, 15, 22, 0);
   c.user_tzid = "UTC";
   calendar_ical_rc_t rc;
   char *out = edit(late, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART:20261016T200000Z"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND:20261016T220000Z"));
   free(out);

   static const char *long_night = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\n"
                                   "UID:l@x\r\nDTSTART:20261016T220000Z\r\n"
                                   "DTEND:20261018T010000Z\r\nRRULE:FREQ=WEEKLY;COUNT=4\r\n"
                                   "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t d = CALENDAR_ICAL_CHANGE_INIT;
   d.set_start = d.set_end = true;
   d.start = utc(2026, 10, 15, 22, 30);
   d.end = utc(2026, 10, 15, 1, 0);
   d.user_tzid = "UTC";
   out = edit(long_night, &d, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART:20261016T223000Z"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND:20261018T010000Z"));
   free(out);
}

/* A new time of day keeps the series on the day the user sees it: a UTC
 * series at 6 pm New York moved to 8:30 pm (past UTC midnight) stays on its
 * Friday; one written in London moved to 9 pm New York lands on the London
 * day that is that Friday evening in New York. */
static void test_series_time_on_users_day(void) {
   static const char *utc_series = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\n"
                                   "UID:u@x\r\nDTSTART:20261016T220000Z\r\n"
                                   "DTEND:20261016T230000Z\r\nRRULE:FREQ=WEEKLY;COUNT=4\r\n"
                                   "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = c.set_end = true;
   c.start = utc(2026, 10, 17, 0, 30);
   c.end = utc(2026, 10, 17, 1, 30);
   c.user_tzid = "America/New_York";
   calendar_ical_rc_t rc;
   char *out = edit(utc_series, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261016T203000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=America/New_York:20261016T213000"));
   free(out);

   static const char *london = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:l@x\r\n"
                               "DTSTART;TZID=Europe/London:20261016T100000\r\n"
                               "DTEND;TZID=Europe/London:20261016T110000\r\n"
                               "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:a\r\n"
                               "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t d = CALENDAR_ICAL_CHANGE_INIT;
   d.set_start = d.set_end = true;
   d.start = utc(2026, 10, 1, 1, 0); /* 9 pm New York, another date */
   d.end = utc(2026, 10, 1, 2, 0);
   d.user_tzid = "America/New_York";
   out = edit(london, &d, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=Europe/London:20261017T020000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=Europe/London:20261017T030000"));
   free(out);
}

/* "Make it 2 pm" on a series still written in UTC means 2 pm local, all year:
 * the series moves to the user's zone first. */
static void test_utc_series_time_is_local(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:u@x\r\n"
                             "DTSTART:20260706T140000Z\r\nDTEND:20260706T150000Z\r\n"
                             "RRULE:FREQ=WEEKLY\r\nSUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 15, 18, 0); /* 14:00 New York */
   c.user_tzid = "America/New_York";
   calendar_ical_rc_t rc;
   char *out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20260706T140000"));
   free(out);
}

/* A new first date past the new rule's own end would leave nothing: refused. */
static void test_move_past_end_refused(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 11, 2, 22, 30);
   c.rrule = "FREQ=WEEKLY;BYDAY=MO;UNTIL=20261101";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(TECH, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_RULE, rc);
}

/* An instance only retitled (never moved) moves with the series. */
static void test_retitled_instance_moves_with_series(void) {
   static const char *ical =
       "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" VTZ_NY
       "BEGIN:VEVENT\r\nUID:w@x\r\nDTSTART;TZID=America/New_York:20261006T100000\r\n"
       "DTEND;TZID=America/New_York:20261006T110000\r\nRRULE:FREQ=WEEKLY;COUNT=5\r\n"
       "SUMMARY:Weekly\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:w@x\r\nRECURRENCE-ID;TZID=America/New_York:20261013T100000\r\n"
       "DTSTART;TZID=America/New_York:20261013T100000\r\n"
       "DTEND;TZID=America/New_York:20261013T110000\r\nSUMMARY:Special\r\nEND:VEVENT\r\n"
       "END:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 6, 15, 0); /* 11:00 New York */
   calendar_ical_rc_t rc;
   char *out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "RECURRENCE-ID;TZID=America/New_York:20261013T110000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261013T110000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=America/New_York:20261013T120000"));
   free(out);
}

/* A UTC series moved to a zone: its cancelled dates follow it to the same
 * local time on their own dates (after the daylight-saving change too). */
static void test_reanchor_moves_exceptions(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:u@x\r\n"
                             "DTSTART:20260706T140000Z\r\nDTEND:20260706T150000Z\r\n"
                             "RRULE:FREQ=WEEKLY\r\nEXDATE:20261214T140000Z\r\n"
                             "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.rrule = "FREQ=WEEKLY;UNTIL=20270101";
   c.user_tzid = "America/New_York";
   calendar_ical_rc_t rc;
   char *out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20260706T100000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "EXDATE;TZID=America/New_York:20261214T100000"));
   free(out);
}

/* An alias of a zone name is never looked up: read as UTC, and not
 * rewritten. */
/* UTC by name, and an old zone name Google still writes, are zones (the old
 * name through the system's zone files, which link it to the current one). */
static void test_utc_and_old_zone_names(void) {
   static const char *utc_named = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:t@x\r\n"
                                  "DTSTART;TZID=UTC:20261019T173000\r\nSUMMARY:a\r\n"
                                  "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.reminder_minutes = 5;
   calendar_ical_rc_t rc;
   char *out = edit(utc_named, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(out);

   static const char *kiev = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:k@x\r\n"
                             "DTSTART;TZID=Europe/Kiev:20261019T173000\r\n"
                             "RRULE:FREQ=WEEKLY;COUNT=3\r\nSUMMARY:a\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
   caldav_event_t e;
   read_event(kiev, &e);
   TEST_ASSERT_NOT_EQUAL(0, e.tzid[0]); /* resolved, under its current name */
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 19, 14, 30), e.dtstart);
   caldav_event_free(&e);

   /* The host's own zone file isn't a zone a calendar names. */
   static const char *local = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:l@x\r\n"
                              "DTSTART;TZID=localtime:20261019T173000\r\nSUMMARY:a\r\n"
                              "END:VEVENT\r\nEND:VCALENDAR\r\n";
   read_event(local, &e);
   TEST_ASSERT_EQUAL_STRING("", e.tzid);
   caldav_event_free(&e);
}

/* A rule on hours doesn't follow a moved start either. */
static void test_move_under_hour_rule_refused(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:h@x\r\n"
                             "DTSTART:20261019T090000Z\r\nDTEND:20261019T093000Z\r\n"
                             "RRULE:FREQ=DAILY;BYHOUR=9,17;COUNT=6\r\nSUMMARY:a\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 19, 10, 0);
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_RULE, rc);
}

/* An all-day instance only retitled moves with its series too. */
static void test_all_day_retitled_instance_moves(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:a@x\r\n"
                             "DTSTART;VALUE=DATE:20261019\r\nDTEND;VALUE=DATE:20261020\r\n"
                             "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:a\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:a@x\r\nRECURRENCE-ID;VALUE=DATE:20261026\r\n"
                             "DTSTART;VALUE=DATE:20261026\r\nDTEND;VALUE=DATE:20261027\r\n"
                             "SUMMARY:b\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start_date = "2026-10-20";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc)); /* an all-day series' days are its rule */
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_RULE, rc);
   c.rrule = "FREQ=WEEKLY;COUNT=4";
   char *out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "RECURRENCE-ID;VALUE=DATE:20261027"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;VALUE=DATE:20261027"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;VALUE=DATE:20261028"));
   free(out);
}

/* An empty text line is dropped harmlessly; an empty date is a broken event. */
static void test_empty_values(void) {
   static const char *empty_date = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:e@x\r\n"
                                   "DTSTART:20261019T090000Z\r\nRECURRENCE-ID:\r\nSUMMARY:a\r\n"
                                   "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "b";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(empty_date, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_UNREADABLE, rc);

   static const char *empty_x = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:x@x\r\n"
                                "DTSTART:20261019T090000Z\r\nX-VENDOR-NOTE:\r\nlocation:\r\n"
                                "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   char *out = edit(empty_x, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(out);
}

static void test_has_uid(void) {
   TEST_ASSERT_TRUE(calendar_ical_has_uid(TECH, "tech-series@example.com"));
   TEST_ASSERT_FALSE(calendar_ical_has_uid(TECH, "pre-series@example.com"));
   TEST_ASSERT_FALSE(calendar_ical_has_uid("not ical", "tech-series@example.com"));
}

/* Ending a series sooner drops the moved instances past its new end. */
static void test_shorter_rule_drops_later_overrides(void) {
   calendar_ical_change_t m = CALENDAR_ICAL_CHANGE_INIT;
   m.occurrence = utc(2026, 10, 22, 21, 30);
   m.set_start = true;
   m.start = utc(2026, 10, 22, 23, 0);
   calendar_ical_rc_t rc;
   char *moved = edit(TECH, &m, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);

   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.rrule = "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH;UNTIL=20261020";
   char *out = edit(moved, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(1, count_of(out, "BEGIN:VEVENT"));
   free(out);
   free(moved);
}

/* A series' new title reaches a moved instance that still had the old one,
 * not one given its own. */
static void test_rename_reaches_moved_instances(void) {
   calendar_ical_change_t m = CALENDAR_ICAL_CHANGE_INIT;
   m.occurrence = utc(2026, 10, 20, 21, 30);
   m.set_start = true;
   m.start = utc(2026, 10, 20, 23, 0);
   calendar_ical_rc_t rc;
   char *one = edit(TECH, &m, &rc);
   calendar_ical_change_t own = CALENDAR_ICAL_CHANGE_INIT;
   own.occurrence = utc(2026, 10, 21, 21, 30);
   own.summary = "Its own title";
   char *two = edit(one, &own, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);

   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "Renamed";
   char *out = edit(two, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(2, count_of(out, "SUMMARY:Renamed"));
   TEST_ASSERT_EQUAL_INT(1, count_of(out, "SUMMARY:Its own title"));
   free(out);
   free(two);
   free(one);
}

/* A date that doesn't exist is refused, not rolled into the next month. */
static void test_impossible_date_refused(void) {
   static const char *all_day = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:d@x\r\n"
                                "DTSTART;VALUE=DATE:20261120\r\nDTEND;VALUE=DATE:20261121\r\n"
                                "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start_date = "2026-11-31";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(all_day, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_TIME, rc);
}

/* An override governing a range of instances isn't edited as one. */
static void test_range_override_refused(void) {
   static const char *ical =
       "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:r@x\r\n"
       "DTSTART:20261019T090000Z\r\nRRULE:FREQ=DAILY;COUNT=5\r\nSUMMARY:a\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:r@x\r\nRECURRENCE-ID;RANGE=THISANDFUTURE:20261021T090000Z\r\n"
       "DTSTART:20261021T100000Z\r\nSUMMARY:b\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.occurrence = utc(2026, 10, 21, 9, 0);
   c.cancel = true;
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_UNREADABLE, rc);
}

/* Many unknown zones in one response: warned about up to a point, safely. */
static void test_many_unknown_zones(void) {
   calendar_ical_warned_t warned = { 0 };
   for (int i = 0; i < 12; i++) {
      char ical[512];
      snprintf(ical, sizeof(ical),
               "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:z%d@x\r\n"
               "DTSTART;TZID=Nowhere/Zone%d:20261019T090000\r\nSUMMARY:a\r\n"
               "END:VEVENT\r\nEND:VCALENDAR\r\n",
               i, i);
      caldav_event_t e;
      memset(&e, 0, sizeof(e));
      TEST_ASSERT_TRUE(calendar_ical_read(ical, &e, &warned));
      caldav_event_free(&e);
   }
   TEST_ASSERT_EQUAL_INT(9, warned.count);
}

/* A moved instance is dropped only when the new rule provably lacks it: kept
 * when its date is an extra date (RDATE), or when the rule can't be expanded
 * far enough to tell. */
static void test_rule_change_keeps_unproven_overrides(void) {
   static const char *rdate =
       "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:rd@x\r\n"
       "DTSTART:20261019T090000Z\r\nRRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=4\r\n"
       "RDATE:20261021T090000Z\r\nSUMMARY:a\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:rd@x\r\nRECURRENCE-ID:20261021T090000Z\r\n"
       "DTSTART:20261021T110000Z\r\nSUMMARY:moved\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.rrule = "FREQ=WEEKLY;BYDAY=MO;COUNT=8";
   calendar_ical_rc_t rc;
   char *out = edit(rdate, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(2, count_of(out, "BEGIN:VEVENT"));
   free(out);

   static const char *old_count =
       "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:oc@x\r\n"
       "DTSTART:19900101T090000Z\r\nRRULE:FREQ=DAILY;COUNT=20000\r\nSUMMARY:a\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:oc@x\r\nRECURRENCE-ID:20261019T090000Z\r\n"
       "DTSTART:20261019T100000Z\r\nSUMMARY:moved\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   c.rrule = "FREQ=DAILY;COUNT=19000";
   out = edit(old_count, &c, &rc); /* 2026 is past the expansion's step cap */
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(2, count_of(out, "BEGIN:VEVENT"));
   free(out);
}

/* An extra date that can't be compared (a period) doesn't make the rule's own
 * instances uneditable. */
static void test_period_rdate_does_not_block(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:p@x\r\n"
                             "DTSTART:20261019T090000Z\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                             "RDATE;VALUE=PERIOD:20261030T090000Z/PT1H\r\nSUMMARY:a\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.occurrence = utc(2026, 10, 20, 9, 0);
   c.cancel = true;
   calendar_ical_rc_t rc;
   char *out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(out);
}

static void test_zone_alias_not_looked_up(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:z@x\r\n"
                             "DTSTART;TZID=America//New_York:20261019T173000\r\n"
                             "SUMMARY:a\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   caldav_event_t e;
   read_event(ical, &e);
   TEST_ASSERT_EQUAL_STRING("", e.tzid);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 19, 17, 30), e.dtstart);
   caldav_event_free(&e);
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "b";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_UNREADABLE, rc);
}

static void test_bad_rules_refused(void) {
   const char *rules[] = { "FREQ=DAILY;COUNT=3;UNTIL=20261030", "FREQ=MINUTELY",
                           "FREQ=WEEKLY;UNTIL=20200101", "NOT A RULE",
                           "FREQ=MONTHLY;BYMONTHDAY=31;BYMONTH=2" };
   for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
      calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
      c.rrule = rules[i];
      calendar_ical_rc_t rc;
      TEST_ASSERT_NULL_MESSAGE(edit(TECH, &c, &rc), rules[i]);
      TEST_ASSERT_EQUAL_INT_MESSAGE(CALENDAR_ICAL_ERR_BAD_RULE, rc, rules[i]);
   }
}

static void test_reminders(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.reminder_minutes = 10;
   calendar_ical_rc_t rc;
   char *out = edit(TECH, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(1, count_of(out, "BEGIN:VALARM"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TRIGGER:-PT10M"));
   free(out);

   static const char *two = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:r@x\r\n"
                            "DTSTART:20261019T213000Z\r\nSUMMARY:a\r\n" ALARM_30
                            "BEGIN:VALARM\r\nACTION:EMAIL\r\nSUMMARY:x\r\nDESCRIPTION:x\r\n"
                            "ATTENDEE:mailto:a@example.com\r\nTRIGGER:-PT1H\r\nEND:VALARM\r\n"
                            "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t r = CALENDAR_ICAL_CHANGE_INIT;
   r.remove_reminders = true;
   out = edit(two, &r, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(1, count_of(out, "BEGIN:VALARM")); /* the email one is kept */
   TEST_ASSERT_NOT_NULL(strstr(out, "ACTION:EMAIL"));
   free(out);

   calendar_ical_change_t big = CALENDAR_ICAL_CHANGE_INIT;
   big.reminder_minutes = CALENDAR_ICAL_MAX_REMINDER_MIN + 1;
   TEST_ASSERT_NULL(edit(TECH, &big, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_TIME, rc);
}

static void test_duration_kept(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:d@x\r\n"
                             "DTSTART:20261019T213000Z\r\nDURATION:PT1H\r\nSUMMARY:a\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 20, 15, 0);
   calendar_ical_rc_t rc;
   char *out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART:20261020T150000Z"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DURATION:PT1H"));
   TEST_ASSERT_NULL(strstr(out, "DTEND"));
   free(out);
}

static void test_end_before_start_refused(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_end = true;
   c.end = utc(2026, 10, 19, 20, 0);
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(DAMAGED, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_TIME, rc);
}

/* ---- one instance ---- */

static void test_move_one_instance(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.occurrence = utc(2026, 10, 20, 21, 30);
   c.set_start = true;
   c.start = utc(2026, 10, 20, 23, 0); /* 19:00 New York */
   calendar_ical_rc_t rc;
   char *out = edit(TECH, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(2, count_of(out, "BEGIN:VEVENT"));
   TEST_ASSERT_NOT_NULL(strstr(out, "RECURRENCE-ID;TZID=America/New_York:20261020T173000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261020T190000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;TZID=America/New_York:20261020T223000"));
   TEST_ASSERT_EQUAL_INT(1, count_of(out, "RRULE:FREQ=WEEKLY")); /* the series' only */
   TEST_ASSERT_EQUAL_INT(2, count_of(out, "BEGIN:VALARM"));      /* copied to the instance */
   TEST_ASSERT_EQUAL_INT(2, count_of(out, "X-EXAMPLE-KEEP"));

   caldav_event_t e;
   read_event(out, &e);
   TEST_ASSERT_EQUAL_INT(1, e.override_count);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 21, 30), e.overrides[0].original);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 23, 0), e.overrides[0].dtstart);
   caldav_event_free(&e);

   /* Editing it again changes that VEVENT, not a new one. */
   calendar_ical_change_t again = CALENDAR_ICAL_CHANGE_INIT;
   again.occurrence = utc(2026, 10, 20, 21, 30);
   again.summary = "Moved";
   char *out2 = edit(out, &again, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_EQUAL_INT(2, count_of(out2, "BEGIN:VEVENT"));
   TEST_ASSERT_NOT_NULL(strstr(out2, "SUMMARY:Moved"));
   free(out2);
   free(out);
}

static void test_cancel_one_instance(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.occurrence = utc(2026, 10, 21, 21, 30);
   c.cancel = true;
   calendar_ical_rc_t rc;
   char *out = edit(TECH, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "EXDATE;TZID=America/New_York:20261021T173000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "SEQUENCE:1"));
   free(out);
}

static void test_not_an_instance(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.occurrence = utc(2026, 10, 20, 22, 0); /* 18:00: the series is at 17:30 */
   c.cancel = true;
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(TECH, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_NOT_OCCURRENCE, rc);

   c.occurrence = utc(2026, 10, 19, 21, 30);
   TEST_ASSERT_NULL(edit(DAMAGED, &c, &rc)); /* it doesn't repeat */
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_NOT_OCCURRENCE, rc);

   /* A cancelled instance has nothing to change. */
   calendar_ical_change_t u = CALENDAR_ICAL_CHANGE_INIT;
   u.occurrence = utc(2026, 10, 6, 21, 30);
   u.summary = "x";
   TEST_ASSERT_NULL(edit(PRE, &u, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_NOT_OCCURRENCE, rc);
}

/* ---- refusals ---- */

/* Point @p c at one account address. */
static void set_account(calendar_ical_change_t *c, const char **slot, const char *address) {
   *slot = address;
   c->accounts = slot;
   c->account_count = 1;
}

static void test_guest_copy_reminder_only(void) {
   const char *slot = NULL;
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:g@x\r\n"
                             "DTSTART:20261019T213000Z\r\nSUMMARY:Their meeting\r\n"
                             "ORGANIZER;CN=Other:mailto:other@example.com\r\n"
                             "ATTENDEE:mailto:me@example.com\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   set_account(&c, &slot, "me@example.com");
   c.summary = "Mine now";
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_GUEST, rc);

   /* A reminder with the other fields left "" is still just a reminder. */
   calendar_ical_change_t blank = CALENDAR_ICAL_CHANGE_INIT;
   set_account(&blank, &slot, "me@example.com");
   blank.summary = "";
   blank.reminder_minutes = 10;
   char *b = edit(ical, &blank, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(b);

   calendar_ical_change_t r = CALENDAR_ICAL_CHANGE_INIT;
   set_account(&r, &slot, "Me@Example.com");
   r.reminder_minutes = 15;
   char *out = edit(ical, &r, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(out);

   /* The organizer's own copy is theirs to change. */
   set_account(&c, &slot, "other@example.com");
   out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(out);

   /* Another organizer without the account among the attendees (a shared
    * calendar's own event) isn't a guest copy. */
   set_account(&c, &slot, "third@example.com");
   out = edit(ical, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   free(out);
}

/* A login isn't an invitation address: with the account's address set, a
 * username-login account still recognizes its copy of someone's invitation. */
static void test_guest_by_address_set(void) {
   static const char *ical = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:a@x\r\n"
                             "DTSTART:20261019T213000Z\r\nSUMMARY:Their meeting\r\n"
                             "ORGANIZER:mailto:bob@example.com\r\n"
                             "ATTENDEE:mailto:alice@example.com\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
   static const char *const login_only[] = { "alice" };
   static const char *const with_set[] = { "alice", "alice@example.com", "a@example.com" };
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "Mine now";
   c.accounts = login_only;
   c.account_count = 1;
   calendar_ical_rc_t rc;
   char *out = edit(ical, &c, &rc); /* the login alone can't tell */
   free(out);
   c.accounts = with_set;
   c.account_count = 3;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_GUEST, rc);
}

static void test_unknown_zone_times_refused(void) {
   static const char *ical =
       "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
       "BEGIN:VTIMEZONE\r\nTZID:Eastern Standard Time\r\nBEGIN:STANDARD\r\n"
       "DTSTART:16010101T020000\r\nTZOFFSETFROM:-0400\r\nTZOFFSETTO:-0500\r\n"
       "RRULE:FREQ=YEARLY;BYDAY=1SU;BYMONTH=11\r\nEND:STANDARD\r\nBEGIN:DAYLIGHT\r\n"
       "DTSTART:16010101T020000\r\nTZOFFSETFROM:-0500\r\nTZOFFSETTO:-0400\r\n"
       "RRULE:FREQ=YEARLY;BYDAY=2SU;BYMONTH=3\r\nEND:DAYLIGHT\r\nEND:VTIMEZONE\r\n"
       "BEGIN:VEVENT\r\nUID:o@x\r\nDTSTART;TZID=Eastern Standard Time:20261019T173000\r\n"
       "DTEND;TZID=Eastern Standard Time:20261019T183000\r\nSUMMARY:a\r\nEND:VEVENT\r\n"
       "END:VCALENDAR\r\n";
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.set_start = true;
   c.start = utc(2026, 10, 19, 22, 0);
   calendar_ical_rc_t rc;
   TEST_ASSERT_NULL(edit(ical, &c, &rc));
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_ZONE, rc);

   calendar_ical_change_t t = CALENDAR_ICAL_CHANGE_INIT;
   t.summary = "b";
   char *out = edit(ical, &t, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=Eastern Standard Time:20261019T173000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TZID:Eastern Standard Time")); /* its zone kept */
   free(out);
}

static void test_text_without_control_characters(void) {
   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.summary = "a\x01"
               "b";
   c.description = ""; /* left as it is */
   calendar_ical_rc_t rc;
   char *out = edit(PRE, &c, &rc);
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, rc);
   TEST_ASSERT_NOT_NULL(strstr(out, "SUMMARY:ab\r\n"));
   free(out);
}

/* ---- building ---- */

static void test_build_timed_in_zone(void) {
   calendar_ical_new_t n = { .uid = "n@x",
                             .summary = "Weekly",
                             .start = utc(2026, 10, 20, 21, 30),
                             .end = utc(2026, 10, 20, 22, 30),
                             .tzid = "America/New_York",
                             .rrule = "FREQ=WEEKLY;COUNT=4",
                             .reminder_minutes = 30 };
   char *raw = NULL;
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, calendar_ical_build(&n, &raw));
   char *out = unfold(raw);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;TZID=America/New_York:20261020T173000"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TZID:America/New_York\r\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "TRIGGER:-PT30M"));
   caldav_event_t e;
   read_event(raw, &e);
   TEST_ASSERT_EQUAL_STRING("America/New_York", e.tzid);
   TEST_ASSERT_EQUAL_STRING("FREQ=WEEKLY;COUNT=4", e.rrule);
   caldav_event_free(&e);
   free(out);
   free(raw);
}

static void test_build_all_day(void) {
   calendar_ical_new_t n = { .uid = "a@x",
                             .summary = "Holiday",
                             .all_day = true,
                             .start_date = "2026-10-20",
                             .end_date = "2026-10-21",
                             .reminder_minutes = -1 };
   char *raw = NULL;
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_OK, calendar_ical_build(&n, &raw));
   char *out = unfold(raw);
   TEST_ASSERT_NOT_NULL(strstr(out, "DTSTART;VALUE=DATE:20261020"));
   TEST_ASSERT_NOT_NULL(strstr(out, "DTEND;VALUE=DATE:20261021"));
   TEST_ASSERT_NULL(strstr(out, "VALARM"));
   free(out);
   free(raw);

   n.end_date = "2026-10-20";
   TEST_ASSERT_EQUAL_INT(CALENDAR_ICAL_ERR_BAD_TIME, calendar_ical_build(&n, &raw));
   TEST_ASSERT_NULL(raw);
}

static void test_instance_keys(void) {
   char key[32];
   calendar_ical_instance_key(utc(2026, 10, 20, 21, 30), false, key, sizeof(key));
   TEST_ASSERT_EQUAL_STRING("20261020T213000Z", key);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 21, 30), calendar_ical_instance_key_time(key));
   calendar_ical_instance_key(utc(2026, 10, 20, 0, 0), true, key, sizeof(key));
   TEST_ASSERT_EQUAL_STRING("20261020", key);
   TEST_ASSERT_EQUAL_INT64(utc(2026, 10, 20, 0, 0), calendar_ical_instance_key_time(key));
   TEST_ASSERT_EQUAL_INT64(0, calendar_ical_instance_key_time("junk"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_read_cancelled_dates);
   RUN_TEST(test_read_moved_and_cancelled_instances);
   RUN_TEST(test_read_unsafe_tzid_on_exdate);
   RUN_TEST(test_rename_keeps_everything_else);
   RUN_TEST(test_unreadable_part_refused);
   RUN_TEST(test_move_series_shifts_exceptions);
   RUN_TEST(test_rule_on_utc_event_reanchors);
   RUN_TEST(test_empty_rule_refused);
   RUN_TEST(test_series_start_is_time_of_day);
   RUN_TEST(test_series_end_is_time_of_day);
   RUN_TEST(test_series_overnight_moves);
   RUN_TEST(test_series_time_on_users_day);
   RUN_TEST(test_utc_series_time_is_local);
   RUN_TEST(test_move_past_end_refused);
   RUN_TEST(test_retitled_instance_moves_with_series);
   RUN_TEST(test_reanchor_moves_exceptions);
   RUN_TEST(test_zone_alias_not_looked_up);
   RUN_TEST(test_shorter_rule_drops_later_overrides);
   RUN_TEST(test_rename_reaches_moved_instances);
   RUN_TEST(test_impossible_date_refused);
   RUN_TEST(test_range_override_refused);
   RUN_TEST(test_many_unknown_zones);
   RUN_TEST(test_rule_change_keeps_unproven_overrides);
   RUN_TEST(test_period_rdate_does_not_block);
   RUN_TEST(test_empty_values);
   RUN_TEST(test_has_uid);
   RUN_TEST(test_utc_and_old_zone_names);
   RUN_TEST(test_move_under_hour_rule_refused);
   RUN_TEST(test_all_day_retitled_instance_moves);
   RUN_TEST(test_bad_rules_refused);
   RUN_TEST(test_reminders);
   RUN_TEST(test_duration_kept);
   RUN_TEST(test_end_before_start_refused);
   RUN_TEST(test_move_one_instance);
   RUN_TEST(test_cancel_one_instance);
   RUN_TEST(test_not_an_instance);
   RUN_TEST(test_guest_copy_reminder_only);
   RUN_TEST(test_guest_by_address_set);
   RUN_TEST(test_unknown_zone_times_refused);
   RUN_TEST(test_text_without_control_characters);
   RUN_TEST(test_build_timed_in_zone);
   RUN_TEST(test_build_all_day);
   RUN_TEST(test_instance_keys);
   return UNITY_END();
}
