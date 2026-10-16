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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for calendar_db CRUD operations.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "tools/calendar_db.h"
#include "unity.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static calendar_account_t make_account(int user_id, const char *name, const char *url) {
   calendar_account_t a;
   memset(&a, 0, sizeof(a));
   a.user_id = user_id;
   strncpy(a.name, name, sizeof(a.name) - 1);
   strncpy(a.caldav_url, url, sizeof(a.caldav_url) - 1);
   strncpy(a.username, "testuser", sizeof(a.username) - 1);
   strncpy(a.auth_type, "basic", sizeof(a.auth_type) - 1);
   a.enabled = true;
   a.sync_interval_sec = 900;
   return a;
}

static calendar_calendar_t make_calendar(int64_t account_id, const char *path, const char *name) {
   calendar_calendar_t c;
   memset(&c, 0, sizeof(c));
   c.account_id = account_id;
   strncpy(c.caldav_path, path, sizeof(c.caldav_path) - 1);
   strncpy(c.display_name, name, sizeof(c.display_name) - 1);
   strncpy(c.color, "#0000FF", sizeof(c.color) - 1);
   c.is_active = true;
   return c;
}

static calendar_event_t make_event(int64_t calendar_id, const char *uid, const char *summary) {
   calendar_event_t e;
   memset(&e, 0, sizeof(e));
   e.calendar_id = calendar_id;
   strncpy(e.uid, uid, sizeof(e.uid) - 1);
   strncpy(e.etag, "etag1", sizeof(e.etag) - 1);
   strncpy(e.summary, summary, sizeof(e.summary) - 1);
   e.dtstart = 1700000000;
   e.dtend = 1700003600;
   e.last_synced = time(NULL);
   return e;
}

/* ── setUp / tearDown ────────────────────────────────────────────────────── */

void setUp(void) {
   auth_db_init(":memory:");
   auth_db_create_user("testuser1", "hash1", true);
   auth_db_create_user("testuser2", "hash2", false);
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ── Account Tests ───────────────────────────────────────────────────────── */

static void test_account_create(void) {
   calendar_account_t a = make_account(1, "Work", "https://caldav.example.com");
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_create(&a, &id));
   TEST_ASSERT_TRUE(id > 0);
}

static void test_account_get(void) {
   calendar_account_t a = make_account(1, "Personal", "https://cal.personal.dev");
   int64_t id = 0;
   calendar_db_account_create(&a, &id);

   calendar_account_t out;
   memset(&out, 0, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_get(id, &out));
   TEST_ASSERT_EQUAL_STRING("Personal", out.name);
   TEST_ASSERT_EQUAL_STRING("https://cal.personal.dev", out.caldav_url);
   TEST_ASSERT_EQUAL_INT(1, out.user_id);
   TEST_ASSERT_TRUE(out.enabled);
}

static void test_account_list(void) {
   calendar_account_t a1 = make_account(1, "A1", "https://a1.example.com");
   calendar_account_t a2 = make_account(1, "A2", "https://a2.example.com");
   calendar_db_account_create(&a1, NULL);
   calendar_db_account_create(&a2, NULL);

   calendar_account_t out[8];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_list(1, out, 8, &count));
   TEST_ASSERT_EQUAL_INT(2, count);
}

static void test_account_delete(void) {
   calendar_account_t a = make_account(1, "ToDelete", "https://del.example.com");
   int64_t id = 0;
   calendar_db_account_create(&a, &id);

   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_delete(id));

   calendar_account_t out;
   TEST_ASSERT_EQUAL_INT(1, calendar_db_account_get(id, &out));
}

static void test_account_list_empty(void) {
   calendar_account_t out[4];
   int count = -1;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_list(1, out, 4, &count));
   TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_account_user_isolation(void) {
   calendar_account_t a1 = make_account(1, "User1Cal", "https://u1.example.com");
   calendar_account_t a2 = make_account(2, "User2Cal", "https://u2.example.com");
   calendar_db_account_create(&a1, NULL);
   calendar_db_account_create(&a2, NULL);

   calendar_account_t out[4];
   int count = 0;
   calendar_db_account_list(1, out, 4, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("User1Cal", out[0].name);
}

/* ── Calendar Tests ──────────────────────────────────────────────────────── */

static void test_calendar_create(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/work", "Work");
   int64_t cal_id = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_calendar_create(&c, &cal_id));
   TEST_ASSERT_TRUE(cal_id > 0);
}

static void test_calendar_get(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/personal", "Personal");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);

   calendar_calendar_t out;
   memset(&out, 0, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, calendar_db_calendar_get(cal_id, &out));
   TEST_ASSERT_EQUAL_STRING("Personal", out.display_name);
   TEST_ASSERT_EQUAL_STRING("/cal/personal", out.caldav_path);
   TEST_ASSERT_EQUAL_INT64(acct_id, out.account_id);
}

static void test_calendar_list(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c1 = make_calendar(acct_id, "/cal/a", "CalA");
   calendar_calendar_t c2 = make_calendar(acct_id, "/cal/b", "CalB");
   calendar_db_calendar_create(&c1, NULL);
   calendar_db_calendar_create(&c2, NULL);

   calendar_calendar_t out[4];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_calendar_list(acct_id, out, 4, &count));
   TEST_ASSERT_EQUAL_INT(2, count);
}

static void test_calendar_delete(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/del", "ToDelete");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);

   TEST_ASSERT_EQUAL_INT(0, calendar_db_calendar_delete(cal_id));

   calendar_calendar_t out;
   TEST_ASSERT_EQUAL_INT(1, calendar_db_calendar_get(cal_id, &out));
}

static void test_calendar_list_empty(void) {
   calendar_calendar_t out[4];
   int count = -1;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_calendar_list(999, out, 4, &count));
   TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_calendar_cascade_on_account_delete(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/c", "Child");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);

   calendar_db_account_delete(acct_id);

   calendar_calendar_t out;
   TEST_ASSERT_EQUAL_INT(1, calendar_db_calendar_get(cal_id, &out));
}

/* ── Event Tests ─────────────────────────────────────────────────────────── */

static void test_event_upsert_create(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/c", "Cal");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);

   calendar_event_t e = make_event(cal_id, "uid-001@example.com", "Meeting");
   int64_t evt_id = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_event_upsert(&e, &evt_id));
   TEST_ASSERT_TRUE(evt_id > 0);
}

/* An invitation can sit in two of the user's calendars: finding it by UID
 * says so, and a calendar narrows it to one. */
static void test_event_find_in_two_calendars(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);
   calendar_calendar_t c1 = make_calendar(acct_id, "/cal/one", "One");
   calendar_calendar_t c2 = make_calendar(acct_id, "/cal/two", "Two");
   int64_t id1 = 0, id2 = 0;
   calendar_db_calendar_create(&c1, &id1);
   calendar_db_calendar_create(&c2, &id2);
   calendar_event_t e1 = make_event(id1, "invite@example.com", "In one");
   calendar_event_t e2 = make_event(id2, "invite@example.com", "In two");
   int64_t ev = 0;
   calendar_db_event_upsert(&e1, &ev);
   calendar_db_event_upsert(&e2, &ev);

   calendar_event_t out = { 0 };
   int matches = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_event_find(1, "invite@example.com", 0, &out, &matches));
   TEST_ASSERT_EQUAL_INT(2, matches);
   free(out.raw_ical);
   memset(&out, 0, sizeof(out));
   TEST_ASSERT_EQUAL_INT(0, calendar_db_event_find(1, "invite@example.com", id2, &out, &matches));
   TEST_ASSERT_EQUAL_INT(1, matches);
   TEST_ASSERT_EQUAL_STRING("In two", out.summary);
   free(out.raw_ical);
   /* Another user's UID is not found. */
   TEST_ASSERT_NOT_EQUAL_INT(0, calendar_db_event_find(2, "invite@example.com", 0, &out, &matches));
   TEST_ASSERT_EQUAL_INT(0, matches);
}

static void test_event_upsert_update(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/c", "Cal");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);

   calendar_event_t e = make_event(cal_id, "uid-upd@example.com", "Original");
   int64_t id1 = 0;
   calendar_db_event_upsert(&e, &id1);

   strncpy(e.summary, "Updated", sizeof(e.summary));
   strncpy(e.etag, "etag2", sizeof(e.etag));
   int64_t id2 = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_event_upsert(&e, &id2));

   /* INSERT OR REPLACE replaces the row — verify via UID lookup */
   calendar_event_t out;
   memset(&out, 0, sizeof(out));
   int matches = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_event_find(1, "uid-upd@example.com", 0, &out, &matches));
   TEST_ASSERT_EQUAL_INT(1, matches);
   TEST_ASSERT_EQUAL_STRING("Updated", out.summary);
   TEST_ASSERT_EQUAL_STRING("etag2", out.etag);
   free(out.raw_ical);
}

/* An event and its occurrences are stored as one: storing again replaces
 * the occurrences with the new set. */
static void test_event_store(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);
   calendar_calendar_t c = make_calendar(acct_id, "/cal/c", "Cal");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);
   calendar_event_t e = make_event(cal_id, "uid-rep@example.com", "Standup");

   calendar_occurrence_t occ[3];
   memset(occ, 0, sizeof(occ));
   for (int i = 0; i < 3; i++) {
      occ[i].dtstart = 1790812800 + i * 86400;
      occ[i].dtend = occ[i].dtstart + 3600;
   }
   int64_t id1 = 0;
   int64_t id2 = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, calendar_db_event_store(&e, occ, 3, &id1));
   TEST_ASSERT_EQUAL_INT(SUCCESS, calendar_db_event_store(&e, occ + 1, 2, &id2));
   TEST_ASSERT_TRUE(id2 > 0);
   TEST_ASSERT_EQUAL_INT64(id2, occ[1].event_id);

   calendar_occurrence_t out[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_occurrences_in_range(&cal_id, 1, 1790812800 - 86400,
                                                             1790812800 + 10 * 86400, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(1790812800 + 86400, (int64_t)out[0].dtstart);
}

static void test_refetch_forgets_ctags(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);
   calendar_calendar_t c = make_calendar(acct_id, "/cal/c", "Cal");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);
   calendar_db_calendar_update_ctag(cal_id, "ctag-1");

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_calendar_refetch(s_db.db));
   calendar_calendar_t out;
   TEST_ASSERT_EQUAL_INT(0, calendar_db_calendar_get(cal_id, &out));
   TEST_ASSERT_EQUAL_STRING("", out.ctag);
}

static void test_event_delete(void) {
   calendar_account_t a = make_account(1, "Acct", "https://x.com");
   int64_t acct_id = 0;
   calendar_db_account_create(&a, &acct_id);

   calendar_calendar_t c = make_calendar(acct_id, "/cal/c", "Cal");
   int64_t cal_id = 0;
   calendar_db_calendar_create(&c, &cal_id);

   calendar_event_t e = make_event(cal_id, "uid-del@example.com", "ToDelete");
   int64_t evt_id = 0;
   calendar_db_event_upsert(&e, &evt_id);

   TEST_ASSERT_EQUAL_INT(0, calendar_db_event_delete(evt_id));
}

static void test_account_set_read_only(void) {
   calendar_account_t a = make_account(1, "RO", "https://ro.example.com");
   int64_t id = 0;
   calendar_db_account_create(&a, &id);

   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_set_read_only(id, true));

   calendar_account_t out;
   calendar_db_account_get(id, &out);
   TEST_ASSERT_TRUE(out.read_only);
}

static void test_account_set_enabled(void) {
   calendar_account_t a = make_account(1, "Dis", "https://dis.example.com");
   int64_t id = 0;
   calendar_db_account_create(&a, &id);

   TEST_ASSERT_EQUAL_INT(0, calendar_db_account_set_enabled(id, false));

   calendar_account_t out;
   calendar_db_account_get(id, &out);
   TEST_ASSERT_FALSE(out.enabled);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* Accounts */
   RUN_TEST(test_account_create);
   RUN_TEST(test_account_get);
   RUN_TEST(test_account_list);
   RUN_TEST(test_account_delete);
   RUN_TEST(test_account_list_empty);
   RUN_TEST(test_account_user_isolation);

   /* Calendars */
   RUN_TEST(test_calendar_create);
   RUN_TEST(test_calendar_get);
   RUN_TEST(test_calendar_list);
   RUN_TEST(test_calendar_delete);
   RUN_TEST(test_calendar_list_empty);
   RUN_TEST(test_calendar_cascade_on_account_delete);

   /* Events */
   RUN_TEST(test_event_upsert_create);
   RUN_TEST(test_event_upsert_update);
   RUN_TEST(test_event_find_in_two_calendars);
   RUN_TEST(test_event_delete);
   RUN_TEST(test_event_store);
   RUN_TEST(test_refetch_forgets_ctags);

   /* Account flags */
   RUN_TEST(test_account_set_read_only);
   RUN_TEST(test_account_set_enabled);

   return UNITY_END();
}
