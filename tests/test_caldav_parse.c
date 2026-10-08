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
#include <stdlib.h>
#include <string.h>

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
   TEST_ASSERT_EQUAL_STRING("etag-1", e->etag); /* quotes stripped */
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

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_events_fields);
   RUN_TEST(test_events_bad_xml);
   RUN_TEST(test_sync_page);
   RUN_TEST(test_sync_page_truncated);
   return UNITY_END();
}
