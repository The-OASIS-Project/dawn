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
 * Unit tests for reading CalDAV responses (caldav_parse_events,
 * caldav_parse_sync_page): what each field becomes, without a server.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tools/caldav_client.h"
#include "unity.h"

#define BASE "https://cal.example.com/dav/home/"

void setUp(void) {
}

void tearDown(void) {
}

static const char EVENTS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
    "<d:response><d:href>/dav/home/a1.ics</d:href><d:propstat><d:prop>"
    "<d:getetag>\"etag-1\"</d:getetag><c:calendar-data>BEGIN:VCALENDAR\r\n"
    "BEGIN:VEVENT\r\nUID:a1@example.com\r\nSUMMARY:Weekly sync\r\n"
    "DESCRIPTION:Agenda: notes\\, decisions\r\nLOCATION:Room 1\r\n"
    "DTSTART:20261001T120000Z\r\nDTEND:20261001T130000Z\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=10\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"
    "</c:calendar-data></d:prop></d:propstat></d:response>"
    "<d:response><d:href>b2.ics</d:href><d:propstat><d:prop><d:getetag>e2</d:getetag>"
    "<c:calendar-data>BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:b2\r\nSUMMARY:Holiday\r\n"
    "DTSTART;VALUE=DATE:20261225\r\nDTEND;VALUE=DATE:20261226\r\nEND:VEVENT\r\n"
    "END:VCALENDAR\r\n</c:calendar-data></d:prop></d:propstat></d:response>"
    "<d:response><d:href>/dav/home/no-data.ics</d:href><d:propstat><d:prop>"
    "<d:getetag>\"e3\"</d:getetag></d:prop></d:propstat></d:response>"
    "</d:multistatus>";

static void test_events_fields(void) {
   caldav_event_list_t list;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK, caldav_parse_events(EVENTS, strlen(EVENTS), BASE, &list));
   /* The response with no calendar-data is skipped. */
   TEST_ASSERT_EQUAL_INT(2, list.count);

   const caldav_event_t *e = &list.events[0];
   TEST_ASSERT_EQUAL_STRING("https://cal.example.com/dav/home/a1.ics", e->href);
   TEST_ASSERT_EQUAL_STRING("\"etag-1\"", e->etag); /* as sent: it goes back in If-Match */
   TEST_ASSERT_EQUAL_STRING("a1@example.com", e->uid);
   TEST_ASSERT_EQUAL_STRING("Weekly sync", e->summary);
   TEST_ASSERT_EQUAL_STRING("Agenda: notes, decisions", e->description);
   TEST_ASSERT_EQUAL_STRING("Room 1", e->location);
   TEST_ASSERT_FALSE(e->all_day);
   TEST_ASSERT_EQUAL_INT64(1790856000, (int64_t)e->dtstart);
   TEST_ASSERT_EQUAL_INT64(1790859600, (int64_t)e->dtend);
   TEST_ASSERT_EQUAL_INT(3600, e->duration_sec);
   TEST_ASSERT_NOT_NULL(strstr(e->rrule, "FREQ=WEEKLY"));
   TEST_ASSERT_NOT_NULL(strstr(e->rrule, "COUNT=10"));
   TEST_ASSERT_NOT_NULL(e->raw_ical);

   e = &list.events[1];
   TEST_ASSERT_EQUAL_STRING("https://cal.example.com/dav/home/b2.ics", e->href);
   TEST_ASSERT_EQUAL_STRING("e2", e->etag);
   TEST_ASSERT_TRUE(e->all_day);
   TEST_ASSERT_EQUAL_STRING("2026-12-25", e->dtstart_date);
   TEST_ASSERT_EQUAL_STRING("2026-12-26", e->dtend_date);
   TEST_ASSERT_EQUAL_INT64(1798156800, (int64_t)e->dtstart);
   TEST_ASSERT_EQUAL_STRING("", e->rrule);

   caldav_event_list_free(&list);
   TEST_ASSERT_NULL(list.events);
}

static void test_events_bad_xml(void) {
   caldav_event_list_t list;
   const char bad[] = "<d:multistatus xmlns:d=\"DAV:\"><d:response>";
   TEST_ASSERT_EQUAL_INT(CALDAV_ERR_PARSE, caldav_parse_events(bad, strlen(bad), BASE, &list));
   TEST_ASSERT_EQUAL_INT(0, list.count);
   caldav_event_list_free(&list);

   const char none[] = "<d:multistatus xmlns:d=\"DAV:\"></d:multistatus>";
   TEST_ASSERT_EQUAL_INT(CALDAV_OK, caldav_parse_events(none, strlen(none), BASE, &list));
   TEST_ASSERT_EQUAL_INT(0, list.count);
   caldav_event_list_free(&list);
}

static void test_sync_page(void) {
   const char page[] =
       "<d:multistatus xmlns:d=\"DAV:\">"
       "<d:response><d:href>/dav/home/a1.ics</d:href><d:propstat><d:prop><d:getetag>\"2\""
       "</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
       "<d:response><d:href>/dav/home/gone.ics</d:href>"
       "<d:status>HTTP/1.1 404 Not Found</d:status></d:response>"
       "<d:sync-token>http://example.com/ns/sync/1234</d:sync-token></d:multistatus>";
   caldav_sync_change_t *changes = NULL;
   int count = 0, cap = 0;
   char token[256] = "";
   bool more = true;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK,
                         caldav_parse_sync_page(page, (int)strlen(page), BASE, &changes, &count,
                                                &cap, token, sizeof(token), &more));
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("https://cal.example.com/dav/home/a1.ics", changes[0].href);
   TEST_ASSERT_FALSE(changes[0].gone);
   TEST_ASSERT_EQUAL_STRING("https://cal.example.com/dav/home/gone.ics", changes[1].href);
   TEST_ASSERT_TRUE(changes[1].gone);
   TEST_ASSERT_EQUAL_STRING("http://example.com/ns/sync/1234", token);
   TEST_ASSERT_FALSE(more);
   free(changes);
}

static void test_sync_page_truncated(void) {
   const char page[] = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/dav/home/</d:href>"
                       "<d:status>HTTP/1.1 507 Insufficient Storage</d:status></d:response>"
                       "<d:sync-token>tok-2</d:sync-token></d:multistatus>";
   caldav_sync_change_t *changes = NULL;
   int count = 0, cap = 0;
   char token[256] = "";
   bool more = false;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK,
                         caldav_parse_sync_page(page, (int)strlen(page), BASE, &changes, &count,
                                                &cap, token, sizeof(token), &more));
   TEST_ASSERT_TRUE(more);
   TEST_ASSERT_EQUAL_STRING("tok-2", token);
   free(changes);
}

/* One event whose calendar-data carries @p vtimezone, starting 2026-10-05
 * 10:00 in zone @p tzid. */
static char *event_in_zone(const char *tzid, const char *vtimezone) {
   static const char fmt[] =
       "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
       "<d:response><d:href>z.ics</d:href><d:propstat><d:prop><d:getetag>e</d:getetag>"
       "<c:calendar-data>BEGIN:VCALENDAR\r\nVERSION:2.0\r\n%s"
       "BEGIN:VEVENT\r\nUID:z\r\nDTSTART;TZID=%s:20261005T100000\r\nEND:VEVENT\r\n"
       "END:VCALENDAR\r\n</c:calendar-data></d:prop></d:propstat></d:response>"
       "</d:multistatus>";
   size_t n = sizeof(fmt) + strlen(tzid) + strlen(vtimezone);
   char *xml = malloc(n);
   TEST_ASSERT_NOT_NULL(xml);
   snprintf(xml, n, fmt, vtimezone, tzid);
   return xml;
}

static time_t parsed_start(const char *xml) {
   caldav_event_list_t list;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK, caldav_parse_events(xml, strlen(xml), BASE, &list));
   TEST_ASSERT_EQUAL_INT(1, list.count);
   time_t start = list.events[0].dtstart;
   caldav_event_list_free(&list);
   return start;
}

/* A calendar's time zone whose rules don't have a real zone's shape (an
 * hourly rule took libical ~20 s per event, a fuzz finding) is ignored, and
 * the built-in zone of that name gives the right time at once.  A real-shaped
 * custom zone is still used. */
static void test_events_unbounded_timezone_ignored(void) {
   char *xml = event_in_zone("America/New_York",
                             "BEGIN:VTIMEZONE\r\nTZID:America/New_York\r\n"
                             "BEGIN:STANDARD\r\nDTSTART:19701101T020000\r\n"
                             "RRULE:FREQ=HOURLY;BYMONTH=11;BYDAY=1SU\r\n"
                             "TZOFFSETFROM:-0400\r\nTZOFFSETTO:-0500\r\nEND:STANDARD\r\n"
                             "END:VTIMEZONE\r\n");
   const time_t t0 = time(NULL);
   TEST_ASSERT_EQUAL_INT64(1791208800, (int64_t)parsed_start(xml)); /* 10:00 EDT */
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
   free(xml);

   /* "Yearly" but on every weekday: 365 changes a year from 1601. */
   xml = event_in_zone("America/New_York",
                       "BEGIN:VTIMEZONE\r\nTZID:America/New_York\r\n"
                       "BEGIN:STANDARD\r\nDTSTART:16010101T000000\r\n"
                       "RRULE:FREQ=YEARLY;BYDAY=MO,TU,WE,TH,FR,SA,SU\r\n"
                       "TZOFFSETFROM:-0500\r\nTZOFFSETTO:-0500\r\nEND:STANDARD\r\n"
                       "END:VTIMEZONE\r\n");
   const time_t t1 = time(NULL);
   TEST_ASSERT_EQUAL_INT64(1791208800, (int64_t)parsed_start(xml));
   TEST_ASSERT_TRUE(time(NULL) - t1 <= 5);
   free(xml);

   /* The same zone nested in the event, where libical looks first. */
   static const char nested[] =
       "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
       "<d:response><d:href>n.ics</d:href><d:propstat><d:prop><d:getetag>e</d:getetag>"
       "<c:calendar-data>BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:n\r\n"
       "BEGIN:VTIMEZONE\r\nTZID:America/New_York\r\nBEGIN:STANDARD\r\n"
       "DTSTART:19701101T020000\r\nRRULE:FREQ=HOURLY;BYMONTH=11;BYDAY=1SU\r\n"
       "TZOFFSETFROM:-0400\r\nTZOFFSETTO:-0500\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
       "DTSTART;TZID=America/New_York:20261005T100000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"
       "</c:calendar-data></d:prop></d:propstat></d:response></d:multistatus>";
   const time_t t2 = time(NULL);
   TEST_ASSERT_EQUAL_INT64(1791208800, (int64_t)parsed_start(nested));
   TEST_ASSERT_TRUE(time(NULL) - t2 <= 5);

   xml = event_in_zone("Example Standard Time",
                       "BEGIN:VTIMEZONE\r\nTZID:Example Standard Time\r\n"
                       "BEGIN:STANDARD\r\nDTSTART:16010101T000000\r\n"
                       "RRULE:FREQ=YEARLY;BYMONTH=1;BYMONTHDAY=1\r\n"
                       "TZOFFSETFROM:+0300\r\nTZOFFSETTO:+0300\r\nEND:STANDARD\r\n"
                       "END:VTIMEZONE\r\n");
   TEST_ASSERT_EQUAL_INT64(1791183600, (int64_t)parsed_start(xml)); /* 10:00 at +03 */
   free(xml);

   /* An Outlook-style zone (a name libical doesn't know) is used. */
   xml = event_in_zone("Example Eastern Time",
                       "BEGIN:VTIMEZONE\r\nTZID:Example Eastern Time\r\n"
                       "BEGIN:STANDARD\r\nDTSTART:16010101T020000\r\n"
                       "RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\r\n"
                       "TZOFFSETFROM:-0400\r\nTZOFFSETTO:-0500\r\nEND:STANDARD\r\n"
                       "BEGIN:DAYLIGHT\r\nDTSTART:16010101T020000\r\n"
                       "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\r\n"
                       "TZOFFSETFROM:-0500\r\nTZOFFSETTO:-0400\r\nEND:DAYLIGHT\r\n"
                       "END:VTIMEZONE\r\n");
   TEST_ASSERT_EQUAL_INT64(1791208800, (int64_t)parsed_start(xml)); /* 10:00 EDT */
   free(xml);

   /* One whose rule names a date that doesn't exist is ignored: read as UTC. */
   xml = event_in_zone("Example Broken Time",
                       "BEGIN:VTIMEZONE\r\nTZID:Example Broken Time\r\n"
                       "BEGIN:STANDARD\r\nDTSTART:16010101T000000\r\n"
                       "RRULE:FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30\r\n"
                       "TZOFFSETFROM:-0500\r\nTZOFFSETTO:-0500\r\nEND:STANDARD\r\n"
                       "END:VTIMEZONE\r\n");
   TEST_ASSERT_EQUAL_INT64(1791194400, (int64_t)parsed_start(xml));
   free(xml);
}

/* One VTIMEZONE named @p tzid holding @p blocks (CRLF-separated lines). */
static char *zone_of(const char *tzid, const char *blocks) {
   const size_t n = strlen(tzid) + strlen(blocks) + 64;
   char *z = malloc(n);
   TEST_ASSERT_NOT_NULL(z);
   snprintf(z, n, "BEGIN:VTIMEZONE\r\nTZID:%s\r\n%sEND:VTIMEZONE\r\n", tzid, blocks);
   return z;
}

/* The start of an event at 2026-10-05 10:00 in a zone named @p tzid with
 * @p blocks, and whether reading it stayed fast. */
static time_t start_in_zone(const char *tzid, const char *blocks) {
   char *zone = zone_of(tzid, blocks);
   char *xml = event_in_zone(tzid, zone);
   const time_t t0 = time(NULL);
   const time_t start = parsed_start(xml);
   TEST_ASSERT_TRUE(time(NULL) - t0 <= 5);
   free(xml);
   free(zone);
   return start;
}

#define AT_UTC_10 1791194400  /* 2026-10-05 10:00 UTC: a zone set aside */
#define AT_EDT_10 1791208800  /* 10:00 at -04 */
#define AT_CEST_10 1791187200 /* 10:00 at +02 */

#define STD_BLOCK(rule, from, to)                                                      \
   "BEGIN:STANDARD\r\nDTSTART:16010101T020000\r\nRRULE:" rule "\r\nTZOFFSETFROM:" from \
   "\r\nTZOFFSETTO:" to "\r\nEND:STANDARD\r\n"
#define DST_BLOCK(rule, from, to)                                                      \
   "BEGIN:DAYLIGHT\r\nDTSTART:16010101T020000\r\nRRULE:" rule "\r\nTZOFFSETFROM:" from \
   "\r\nTZOFFSETTO:" to "\r\nEND:DAYLIGHT\r\n"

/* Zones of names libical doesn't know: real shapes are used, others are set
 * aside before libical expands them. */
static void test_custom_zone_shapes(void) {
   /* Kept: a European zone written with days counted from the month's end,
    * and a US one written with BYSETPOS. */
   TEST_ASSERT_EQUAL_INT64(
       AT_CEST_10,
       start_in_zone(
           "Example Europe",
           DST_BLOCK("FREQ=YEARLY;BYMONTH=3;BYMONTHDAY=-1,-2,-3,-4,-5,-6,-7;BYDAY=SU", "+0100",
                     "+0200")
               STD_BLOCK("FREQ=YEARLY;BYMONTH=10;BYMONTHDAY=-7,-6,-5,-4,-3,-2,-1;BYDAY=SU", "+0200",
                         "+0100")));
   TEST_ASSERT_EQUAL_INT64(AT_EDT_10,
                           start_in_zone("Example US",
                                         DST_BLOCK("FREQ=YEARLY;BYMONTH=3;BYDAY=SU;BYSETPOS=2",
                                                   "-0500", "-0400")
                                             STD_BLOCK("FREQ=YEARLY;BYMONTH=11;BYDAY=SU;BYSETPOS=1",
                                                       "-0400", "-0500")));

   /* Set aside: a non-Gregorian scale (libical's search for it can run
    * forever), a sixth Sunday, a two-year interval, too many rules. */
   TEST_ASSERT_EQUAL_INT64(AT_UTC_10,
                           start_in_zone("Example RScale",
                                         STD_BLOCK("RSCALE=CHINESE;FREQ=YEARLY;BYMONTH=3;BYDAY=6SU",
                                                   "-0500", "-0500")));
   TEST_ASSERT_EQUAL_INT64(AT_UTC_10, start_in_zone("Example Sixth",
                                                    STD_BLOCK("FREQ=YEARLY;BYMONTH=2;BYDAY=6SU",
                                                              "-0500", "-0500")));
   TEST_ASSERT_EQUAL_INT64(AT_UTC_10,
                           start_in_zone("Example Interval",
                                         STD_BLOCK("FREQ=YEARLY;INTERVAL=2;BYMONTH=2;BYMONTHDAY=29",
                                                   "-0500", "-0500")));
   char rules[4096] = "BEGIN:STANDARD\r\nDTSTART:16010101T020000\r\n";
   for (int m = 0; m < 17; m++) {
      char line[64];
      snprintf(line, sizeof(line), "RRULE:FREQ=YEARLY;BYMONTH=%d;BYDAY=2SU\r\n", m % 12 + 1);
      strcat(rules, line);
   }
   strcat(rules, "TZOFFSETFROM:-0500\r\nTZOFFSETTO:-0500\r\nEND:STANDARD\r\n");
   TEST_ASSERT_EQUAL_INT64(AT_UTC_10, start_in_zone("Example Many", rules));
}

/* libical opens a zone file by the TZID's name: one that leaves the zone
 * directory (here, the test's own stdin) is never looked up. */
static void test_unsafe_tzid(void) {
   /* stdin becomes a pipe nobody writes: reading it would block for good
    * (the alarm fails the test instead). */
   int fds[2];
   TEST_ASSERT_EQUAL_INT(0, pipe(fds));
   const int saved = dup(0);
   dup2(fds[0], 0);
   alarm(10);
   char *xml = event_in_zone("../../../../proc/self/fd/0", "");
   TEST_ASSERT_EQUAL_INT64(AT_UTC_10, (int64_t)parsed_start(xml));
   alarm(0);
   free(xml);
   dup2(saved, 0);
   close(saved);
   close(fds[0]);
   close(fds[1]);
}

/* libical's handling of many time zones is quadratic: an event with more
 * than a few is skipped, counted through folded lines. */
static void test_too_many_vtimezones(void) {
   char *zones = calloc(1, 32768);
   TEST_ASSERT_NOT_NULL(zones);
   for (int i = 0; i < 17; i++) {
      char z[160];
      snprintf(z, sizeof(z), "BEGIN:VTIMEZ%sONE\r\nTZID:Z%d\r\nEND:VTIMEZONE\r\n",
               i == 3 ? "\r\n " : "", i);
      strcat(zones, z);
   }
   char *xml = event_in_zone("Z0", zones);
   caldav_event_list_t list;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK, caldav_parse_events(xml, strlen(xml), BASE, &list));
   TEST_ASSERT_EQUAL_INT(0, list.count);
   caldav_event_list_free(&list);
   free(xml);
   free(zones);
}

/* The series is read even when a changed occurrence comes first; the zone it
 * repeats in is kept; a recurrence rule too long to store whole isn't cut. */
static void test_series_master_and_zone(void) {
   static const char xml[] =
       "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
       "<d:response><d:href>s.ics</d:href><d:propstat><d:prop><d:getetag>e</d:getetag>"
       "<c:calendar-data>BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
       "BEGIN:VEVENT\r\nUID:s\r\nRECURRENCE-ID;TZID=America/New_York:20261012T100000\r\n"
       "DTSTART;TZID=America/New_York:20261012T110000\r\nSUMMARY:Moved\r\nEND:VEVENT\r\n"
       "BEGIN:VEVENT\r\nUID:s\r\nSUMMARY:Standup\r\n"
       "DTSTART;TZID=America/New_York:20261005T100000\r\nRRULE:FREQ=WEEKLY\r\n"
       "END:VEVENT\r\nEND:VCALENDAR\r\n</c:calendar-data></d:prop></d:propstat></d:response>"
       "</d:multistatus>";
   caldav_event_list_t list;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK, caldav_parse_events(xml, strlen(xml), BASE, &list));
   TEST_ASSERT_EQUAL_INT(1, list.count);
   TEST_ASSERT_EQUAL_STRING("Standup", list.events[0].summary);
   TEST_ASSERT_EQUAL_STRING("FREQ=WEEKLY", list.events[0].rrule);
   TEST_ASSERT_EQUAL_STRING("America/New_York", list.events[0].tzid);
   TEST_ASSERT_EQUAL_INT64(AT_EDT_10, (int64_t)list.events[0].dtstart);
   caldav_event_list_free(&list);
}

/* The account's own addresses: hrefs of calendar-user-address-set, "mailto:"
 * removed, non-mailto ids kept as they are. */
static void test_user_addresses(void) {
   static const char *xml =
       "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
       "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response><d:href>/p/alice/</d:href>"
       "<d:propstat><d:prop><c:calendar-user-address-set>"
       "<d:href>mailto:alice@example.com</d:href><d:href> MAILTO:a@example.com </d:href>"
       "<d:href>/p/alice/</d:href></c:calendar-user-address-set></d:prop>"
       "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
   char addrs[CALDAV_MAX_USER_ADDRESSES][CALDAV_USER_ADDRESS_LEN];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(CALDAV_OK, caldav_parse_user_addresses(xml, strlen(xml), addrs,
                                                                CALDAV_MAX_USER_ADDRESSES, &n));
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_STRING("alice@example.com", addrs[0]);
   TEST_ASSERT_EQUAL_STRING("a@example.com", addrs[1]);
   TEST_ASSERT_EQUAL_STRING("/p/alice/", addrs[2]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_events_fields);
   RUN_TEST(test_events_bad_xml);
   RUN_TEST(test_events_unbounded_timezone_ignored);
   RUN_TEST(test_custom_zone_shapes);
   RUN_TEST(test_unsafe_tzid);
   RUN_TEST(test_too_many_vtimezones);
   RUN_TEST(test_series_master_and_zone);
   RUN_TEST(test_sync_page);
   RUN_TEST(test_sync_page_truncated);
   RUN_TEST(test_user_addresses);
   return UNITY_END();
}
