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
 * Unit tests for scheduler_db.c — CRUD, queries, and string conversions.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/scheduler_db.h"
#include "unity.h"

/* Must match auth_db_core.c v18 migration */
static const char *DDL =
    "CREATE TABLE IF NOT EXISTS users ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  username TEXT UNIQUE NOT NULL,"
    "  real_name TEXT DEFAULT NULL,"
    "  preferred_address TEXT DEFAULT NULL,"
    "  identity_aliases TEXT DEFAULT NULL"
    ");"
    "INSERT INTO users (id, username) VALUES (1, 'testuser');"
    "INSERT INTO users (id, username) VALUES (2, 'otheruser');"
    "CREATE TABLE IF NOT EXISTS scheduled_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  event_type TEXT NOT NULL DEFAULT 'timer',"
    "  status TEXT NOT NULL DEFAULT 'pending',"
    "  name TEXT NOT NULL,"
    "  message TEXT,"
    "  fire_at INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  duration_sec INTEGER DEFAULT 0,"
    "  snoozed_until INTEGER DEFAULT 0,"
    "  recurrence TEXT DEFAULT 'once',"
    "  recurrence_days TEXT,"
    "  original_time TEXT,"
    "  source_uuid TEXT,"
    "  source_location TEXT,"
    "  source_client_type INTEGER DEFAULT 0,"
    "  announce_all INTEGER DEFAULT 0,"
    "  tool_name TEXT,"
    "  tool_action TEXT,"
    "  tool_value TEXT,"
    "  fired_at INTEGER DEFAULT 0,"
    "  snooze_count INTEGER DEFAULT 0,"
    "  say_aloud INTEGER NOT NULL DEFAULT 0," /* v53 tri-state TTS override */
    "  deliver_to TEXT,"                      /* v54 messaging channel fan-out */
    "  briefing_instructions TEXT,"           /* v84 per-briefing summarization steering */
    "  FOREIGN KEY (user_id) REFERENCES users(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sched_status_fire "
    "  ON scheduled_events(status, fire_at);"
    "CREATE INDEX IF NOT EXISTS idx_sched_user "
    "  ON scheduled_events(user_id, status);"
    "CREATE INDEX IF NOT EXISTS idx_sched_user_name "
    "  ON scheduled_events(user_id, status, name);"
    "CREATE INDEX IF NOT EXISTS idx_sched_source "
    "  ON scheduled_events(source_uuid);"
    "CREATE TABLE IF NOT EXISTS briefing_steps ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  event_id INTEGER NOT NULL,"
    "  seq INTEGER NOT NULL,"
    "  tool_name TEXT NOT NULL,"
    "  tool_action TEXT NOT NULL DEFAULT '',"
    "  tool_value TEXT NOT NULL DEFAULT '',"
    "  FOREIGN KEY (event_id) REFERENCES scheduled_events(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_briefing_steps_event "
    "  ON briefing_steps(event_id, seq);";

static void setup_db(void) {
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }
   s_db.initialized = true;
}

static void teardown_db(void) {
   s_db.initialized = false;
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }
}

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

/* ============================================================================
 * Helper: create a populated event with sensible defaults
 * ============================================================================ */

static sched_event_t make_event(void) {
   sched_event_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.user_id = 1;
   ev.event_type = SCHED_EVENT_ALARM;
   ev.status = SCHED_STATUS_PENDING;
   strncpy(ev.name, "Test Alarm", SCHED_NAME_MAX - 1);
   strncpy(ev.message, "Wake up!", SCHED_MESSAGE_MAX - 1);
   ev.fire_at = time(NULL) + 3600;
   ev.recurrence = SCHED_RECUR_ONCE;
   return ev;
}

/* ============================================================================
 * Test: String Conversions
 * ============================================================================ */

static void test_string_conversions(void) {
   /* Event type round-trip */
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TIMER, sched_event_type_from_str("timer"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_ALARM, sched_event_type_from_str("alarm"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_REMINDER, sched_event_type_from_str("reminder"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TASK, sched_event_type_from_str("task"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TIMER, sched_event_type_from_str("bogus"));
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_TIMER, sched_event_type_from_str(NULL));
   TEST_ASSERT_EQUAL_STRING("alarm", sched_event_type_to_str(SCHED_EVENT_ALARM));
   TEST_ASSERT_EQUAL_STRING("timer", sched_event_type_to_str(99));

   /* Status round-trip */
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, sched_status_from_str("pending"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_RINGING, sched_status_from_str("ringing"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_FIRED, sched_status_from_str("fired"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_CANCELLED, sched_status_from_str("cancelled"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_SNOOZED, sched_status_from_str("snoozed"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_MISSED, sched_status_from_str("missed"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, sched_status_from_str("dismissed"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_TIMED_OUT, sched_status_from_str("timed_out"));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, sched_status_from_str("nope"));
   TEST_ASSERT_EQUAL_STRING("dismissed", sched_status_to_str(SCHED_STATUS_DISMISSED));

   /* Recurrence round-trip */
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_ONCE, sched_recurrence_from_str("once"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_DAILY, sched_recurrence_from_str("daily"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKDAYS, sched_recurrence_from_str("weekdays"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKENDS, sched_recurrence_from_str("weekends"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKLY, sched_recurrence_from_str("weekly"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_CUSTOM, sched_recurrence_from_str("custom"));
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_ONCE, sched_recurrence_from_str("xyz"));
   TEST_ASSERT_EQUAL_STRING("weekly", sched_recurrence_to_str(SCHED_RECUR_WEEKLY));
}

/* ============================================================================
 * Test: Insert and Get
 * ============================================================================ */

static void test_insert_and_get(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   int irc = scheduler_db_insert(&ev, &id);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, irc);
   TEST_ASSERT_TRUE(id > 0);
   TEST_ASSERT_EQUAL_INT64(id, ev.id);
   TEST_ASSERT_TRUE(ev.created_at > 0);

   sched_event_t got;
   int rc = scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT64(id, got.id);
   TEST_ASSERT_EQUAL_INT(1, got.user_id);
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_ALARM, got.event_type);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, got.status);
   TEST_ASSERT_EQUAL_STRING("Test Alarm", got.name);
   TEST_ASSERT_EQUAL_STRING("Wake up!", got.message);
   TEST_ASSERT_EQUAL_INT64(ev.fire_at, got.fire_at);
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_ONCE, got.recurrence);

   rc = scheduler_db_get(99999, &got);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Insert Checked (per-user and global limits)
 * ============================================================================ */

static void test_insert_checked_limits(void) {
   int max_per_user = 3;
   int max_total = 5;

   for (int i = 0; i < max_per_user; i++) {
      sched_event_t ev = make_event();
      snprintf(ev.name, SCHED_NAME_MAX, "User1 Event %d", i);
      int64_t id = 0;
      int irc = scheduler_db_insert_checked(&ev, max_per_user, max_total, &id);
      TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, irc);
      TEST_ASSERT_TRUE(id > 0);
   }

   sched_event_t ev_over = make_event();
   strncpy(ev_over.name, "User1 Over Limit", SCHED_NAME_MAX - 1);
   int64_t dummy = 0;
   int rc = scheduler_db_insert_checked(&ev_over, max_per_user, max_total, &dummy);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_USER_LIMIT, rc);

   for (int i = 0; i < 2; i++) {
      sched_event_t ev = make_event();
      ev.user_id = 2;
      snprintf(ev.name, SCHED_NAME_MAX, "User2 Event %d", i);
      int64_t id = 0;
      int irc = scheduler_db_insert_checked(&ev, max_per_user, max_total, &id);
      TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, irc);
      TEST_ASSERT_TRUE(id > 0);
   }

   sched_event_t ev_global = make_event();
   ev_global.user_id = 2;
   strncpy(ev_global.name, "User2 Over Global", SCHED_NAME_MAX - 1);
   rc = scheduler_db_insert_checked(&ev_global, max_per_user, max_total, &dummy);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_GLOBAL_LIMIT, rc);

   int u1_count = 0, u2_count = 0;
   scheduler_db_count_user_events(1, &u1_count);
   scheduler_db_count_user_events(2, &u2_count);
   TEST_ASSERT_EQUAL_INT(3, u1_count);
   TEST_ASSERT_EQUAL_INT(2, u2_count);
}

/* ============================================================================
 * Test: Update Status
 * ============================================================================ */

static void test_update_status(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_update_status(id, SCHED_STATUS_RINGING);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_RINGING, got.status);

   rc = scheduler_db_update_status(id, SCHED_STATUS_DISMISSED);
   TEST_ASSERT_EQUAL_INT(0, rc);

   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, got.status);
}

/* ============================================================================
 * Test: Update Status with Fired At
 * ============================================================================ */

static void test_update_status_fired(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   time_t now = time(NULL);
   int rc = scheduler_db_update_status_fired(id, SCHED_STATUS_RINGING, now);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_RINGING, got.status);
   TEST_ASSERT_EQUAL_INT64(now, got.fired_at);
}

/* ============================================================================
 * Test: Cancel (Optimistic)
 * ============================================================================ */

static void test_cancel_optimistic(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_cancel(id);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_CANCELLED, got.status);

   rc = scheduler_db_cancel(id);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   sched_event_t ev2 = make_event();
   int64_t id2 = 0;
   scheduler_db_insert(&ev2, &id2);
   scheduler_db_update_status(id2, SCHED_STATUS_DISMISSED);
   rc = scheduler_db_cancel(id2);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Dismiss (Optimistic)
 * ============================================================================ */

static void test_dismiss_optimistic(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   int rc = scheduler_db_dismiss(id);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   scheduler_db_update_status(id, SCHED_STATUS_RINGING);
   rc = scheduler_db_dismiss(id);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, got.status);
   TEST_ASSERT_TRUE(got.fired_at > 0);

   rc = scheduler_db_dismiss(id);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Snooze
 * ============================================================================ */

static void test_snooze(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   scheduler_db_update_status(id, SCHED_STATUS_RINGING);

   time_t new_fire = time(NULL) + 600;
   int rc = scheduler_db_snooze(id, new_fire);
   TEST_ASSERT_EQUAL_INT(0, rc);

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_SNOOZED, got.status);
   TEST_ASSERT_EQUAL_INT64(new_fire, got.fire_at);
   TEST_ASSERT_EQUAL_INT(1, got.snooze_count);

   time_t new_fire2 = time(NULL) + 1200;
   rc = scheduler_db_snooze(id, new_fire2);
   TEST_ASSERT_EQUAL_INT(0, rc);

   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(2, got.snooze_count);
   TEST_ASSERT_EQUAL_INT64(new_fire2, got.fire_at);
}

/* ============================================================================
 * Test: Due Events
 * ============================================================================ */

static void test_due_events(void) {
   time_t now = time(NULL);

   sched_event_t ev1 = make_event();
   ev1.fire_at = now - 3600;
   strncpy(ev1.name, "Past Event 1", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now - 1800;
   strncpy(ev2.name, "Past Event 2", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t ev3 = make_event();
   ev3.fire_at = now + 7200;
   strncpy(ev3.name, "Future Event", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   sched_event_t results[10];
   int count = scheduler_db_get_due_events(results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);

   if (count == 2) {
      TEST_ASSERT_TRUE(results[0].fire_at <= results[1].fire_at);
   }
}

/* ============================================================================
 * Test: List User Events
 * ============================================================================ */

static void test_list_user_events(void) {
   sched_event_t ev1 = make_event();
   ev1.event_type = SCHED_EVENT_ALARM;
   strncpy(ev1.name, "User1 Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_TIMER;
   strncpy(ev2.name, "User1 Timer", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t ev3 = make_event();
   ev3.user_id = 2;
   strncpy(ev3.name, "User2 Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   sched_event_t results[10];

   int count = scheduler_db_list_user_events(1, -1, results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);

   count = scheduler_db_list_user_events(1, SCHED_EVENT_ALARM, results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);

   count = scheduler_db_list_user_events(2, -1, results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);
}

/* ============================================================================
 * Test: Find by Name
 * ============================================================================ */

static void test_find_by_name(void) {
   sched_event_t ev = make_event();
   strncpy(ev.name, "Morning Alarm", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev, NULL);

   sched_event_t found;

   int rc = scheduler_db_find_by_name(1, "morning alarm", &found);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_STRING("Morning Alarm", found.name);

   rc = scheduler_db_find_by_name(1, "morning alarm%", &found);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   rc = scheduler_db_find_by_name(1, "nonexistent", &found);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   rc = scheduler_db_find_by_name(2, "Morning Alarm", &found);
   TEST_ASSERT_NOT_EQUAL(0, rc);
}

/* ============================================================================
 * Test: Count Events
 * ============================================================================ */

static void test_count_events(void) {
   for (int i = 0; i < 3; i++) {
      sched_event_t ev = make_event();
      snprintf(ev.name, SCHED_NAME_MAX, "Count Event %d", i);
      scheduler_db_insert(&ev, NULL);
   }

   sched_event_t ev_cancel = make_event();
   strncpy(ev_cancel.name, "Cancelled One", SCHED_NAME_MAX - 1);
   int64_t cancel_id = 0;
   scheduler_db_insert(&ev_cancel, &cancel_id);
   scheduler_db_update_status(cancel_id, SCHED_STATUS_CANCELLED);

   int u1_count = 0;
   scheduler_db_count_user_events(1, &u1_count);
   TEST_ASSERT_EQUAL_INT(3, u1_count);

   int total = 0;
   scheduler_db_count_total_events(&total);
   TEST_ASSERT_EQUAL_INT(3, total);
}

/* ============================================================================
 * Test: Get Ringing
 * ============================================================================ */

static void test_get_ringing(void) {
   sched_event_t ev1 = make_event();
   strncpy(ev1.name, "Ringing One", SCHED_NAME_MAX - 1);
   int64_t id1 = 0;
   scheduler_db_insert(&ev1, &id1);

   sched_event_t ev2 = make_event();
   strncpy(ev2.name, "Still Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   scheduler_db_update_status(id1, SCHED_STATUS_RINGING);

   sched_event_t results[10];
   int count = scheduler_db_get_ringing(results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);
   if (count == 1) {
      TEST_ASSERT_EQUAL_INT64(id1, results[0].id);
   }
}

/* ============================================================================
 * Test: Cleanup Old Events
 * ============================================================================ */

static void test_cleanup_old_events(void) {
   time_t now = time(NULL);

   sched_event_t ev_old = make_event();
   strncpy(ev_old.name, "Old Fired", SCHED_NAME_MAX - 1);
   int64_t id_old = 0;
   scheduler_db_insert(&ev_old, &id_old);
   time_t old_time = now - 86400 * 10;
   scheduler_db_update_status_fired(id_old, SCHED_STATUS_FIRED, old_time);

   sched_event_t ev_recent = make_event();
   strncpy(ev_recent.name, "Recent Fired", SCHED_NAME_MAX - 1);
   int64_t id_recent = 0;
   scheduler_db_insert(&ev_recent, &id_recent);
   scheduler_db_update_status_fired(id_recent, SCHED_STATUS_FIRED, now);

   sched_event_t ev_pending = make_event();
   strncpy(ev_pending.name, "Old Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev_pending, NULL);

   int deleted = 0;
   scheduler_db_cleanup_old_events(1, &deleted);
   TEST_ASSERT_EQUAL_INT(1, deleted);

   sched_event_t got;
   int rc = scheduler_db_get(id_old, &got);
   TEST_ASSERT_NOT_EQUAL(0, rc);

   rc = scheduler_db_get(id_recent, &got);
   TEST_ASSERT_EQUAL_INT(0, rc);
}

/* ============================================================================
 * Test: Next Fire Time
 * ============================================================================ */

static void test_next_fire_time(void) {
   time_t now = time(NULL);

   sched_event_t ev1 = make_event();
   ev1.fire_at = now + 1000;
   strncpy(ev1.name, "Earliest", SCHED_NAME_MAX - 1);
   int64_t id1 = 0;
   scheduler_db_insert(&ev1, &id1);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now + 2000;
   strncpy(ev2.name, "Middle", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t ev3 = make_event();
   ev3.fire_at = now + 3000;
   strncpy(ev3.name, "Latest", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev3, NULL);

   time_t next = scheduler_db_next_fire_time();
   TEST_ASSERT_EQUAL_INT64(now + 1000, next);

   scheduler_db_cancel(id1);
   next = scheduler_db_next_fire_time();
   TEST_ASSERT_EQUAL_INT64(now + 2000, next);
}

/* ============================================================================
 * Test: Get Active by UUID
 * ============================================================================ */

static void test_get_active_by_uuid(void) {
   sched_event_t ev1 = make_event();
   ev1.event_type = SCHED_EVENT_TIMER;
   strncpy(ev1.source_uuid, "sat-001", SCHED_UUID_MAX - 1);
   strncpy(ev1.name, "Timer for sat-001", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_TIMER;
   strncpy(ev2.source_uuid, "sat-001", SCHED_UUID_MAX - 1);
   strncpy(ev2.name, "Timer 2 for sat-001", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t results[10];

   int count = scheduler_db_get_active_by_uuid("sat-001", results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);

   count = scheduler_db_get_active_by_uuid("sat-999", results, 10);
   TEST_ASSERT_EQUAL_INT(0, count);
}

/* ============================================================================
 * Test: Get Missed Events
 * ============================================================================ */

static void test_get_missed_events(void) {
   time_t now = time(NULL);

   sched_event_t ev1 = make_event();
   ev1.fire_at = now - 3600;
   strncpy(ev1.name, "Missed One", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev1, NULL);

   sched_event_t ev2 = make_event();
   ev2.fire_at = now + 3600;
   strncpy(ev2.name, "Future One", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&ev2, NULL);

   sched_event_t results[10];
   int count = scheduler_db_get_missed_events(results, 10);
   TEST_ASSERT_EQUAL_INT(1, count);
   if (count == 1) {
      TEST_ASSERT_EQUAL_STRING("Missed One", results[0].name);
   }
}

/* ============================================================================
 * Test: list_user_missed — user-scoped filter, status='missed' only, DESC sort
 *
 * Regression test against the bug where the WebUI panel briefly used
 * scheduler_db_get_missed_events (a DB-wide pending/snoozed-overdue helper)
 * and would have leaked across users + returned zero rows in steady state.
 * ============================================================================ */

static void test_list_user_missed(void) {
   time_t now = time(NULL);

   /* User 1: one missed, one pending, one cancelled */
   sched_event_t m1 = make_event();
   m1.user_id = 1;
   m1.fire_at = now - 7200;
   m1.status = SCHED_STATUS_MISSED;
   strncpy(m1.name, "U1 Missed Older", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&m1, NULL);

   sched_event_t m2 = make_event();
   m2.user_id = 1;
   m2.fire_at = now - 3600;
   m2.status = SCHED_STATUS_MISSED;
   strncpy(m2.name, "U1 Missed Newer", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&m2, NULL);

   sched_event_t p1 = make_event();
   p1.user_id = 1;
   p1.fire_at = now + 3600;
   p1.status = SCHED_STATUS_PENDING;
   strncpy(p1.name, "U1 Pending", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&p1, NULL);

   sched_event_t c1 = make_event();
   c1.user_id = 1;
   c1.fire_at = now - 1800;
   c1.status = SCHED_STATUS_CANCELLED;
   strncpy(c1.name, "U1 Cancelled", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&c1, NULL);

   /* User 2: one missed — must NOT appear in user 1's results */
   sched_event_t u2m = make_event();
   u2m.user_id = 2;
   u2m.fire_at = now - 5400;
   u2m.status = SCHED_STATUS_MISSED;
   strncpy(u2m.name, "U2 Missed", SCHED_NAME_MAX - 1);
   scheduler_db_insert(&u2m, NULL);

   sched_event_t results[10];
   int count = scheduler_db_list_user_missed(1, results, 10);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* DESC by fire_at — newest first */
   TEST_ASSERT_EQUAL_STRING("U1 Missed Newer", results[0].name);
   TEST_ASSERT_EQUAL_STRING("U1 Missed Older", results[1].name);

   /* User 2 sees only their own */
   int u2_count = scheduler_db_list_user_missed(2, results, 10);
   TEST_ASSERT_EQUAL_INT(1, u2_count);
   TEST_ASSERT_EQUAL_STRING("U2 Missed", results[0].name);

   /* Limit honored */
   int limited = scheduler_db_list_user_missed(1, results, 1);
   TEST_ASSERT_EQUAL_INT(1, limited);
   TEST_ASSERT_EQUAL_STRING("U1 Missed Newer", results[0].name);

   /* User with no missed rows returns zero */
   sched_event_t results_empty[10];
   int none = scheduler_db_list_user_missed(99, results_empty, 10);
   TEST_ASSERT_EQUAL_INT(0, none);
}

/* ============================================================================
 * Test: scheduler_db_clear_missed
 *
 * Covers the four branches: missed → dismissed success, wrong status (pending),
 * wrong status (fired), wrong owner.  The user_id predicate added in the
 * post-/review fix is the defense-in-depth gate beneath the dispatcher auth.
 * ============================================================================ */

static void test_clear_missed(void) {
   time_t now = time(NULL);

   /* (a) status='missed' → flips to 'dismissed', returns SUCCESS */
   sched_event_t m = make_event();
   m.user_id = 1;
   m.fire_at = now - 3600;
   m.status = SCHED_STATUS_MISSED;
   strncpy(m.name, "U1 Missed", SCHED_NAME_MAX - 1);
   int64_t m_id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&m, &m_id));
   TEST_ASSERT_EQUAL_INT(SUCCESS, scheduler_db_clear_missed(m_id, 1));
   sched_event_t after;
   TEST_ASSERT_EQUAL_INT(SUCCESS, scheduler_db_get(m_id, &after));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_DISMISSED, after.status);

   /* (b) status='pending' → no-op, FAILURE, row unchanged */
   sched_event_t p = make_event();
   p.user_id = 1;
   p.fire_at = now + 3600;
   p.status = SCHED_STATUS_PENDING;
   strncpy(p.name, "U1 Pending", SCHED_NAME_MAX - 1);
   int64_t p_id = 0;
   scheduler_db_insert(&p, &p_id);
   TEST_ASSERT_EQUAL_INT(FAILURE, scheduler_db_clear_missed(p_id, 1));
   scheduler_db_get(p_id, &after);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, after.status);

   /* (c) status='fired' → no-op, FAILURE, row unchanged */
   sched_event_t f = make_event();
   f.user_id = 1;
   f.status = SCHED_STATUS_FIRED;
   strncpy(f.name, "U1 Fired", SCHED_NAME_MAX - 1);
   int64_t f_id = 0;
   scheduler_db_insert(&f, &f_id);
   TEST_ASSERT_EQUAL_INT(FAILURE, scheduler_db_clear_missed(f_id, 1));
   scheduler_db_get(f_id, &after);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_FIRED, after.status);

   /* (d) wrong user → FAILURE, row unchanged.  Defense in depth: the
    *     dispatcher gate would already have rejected, but the DB-level
    *     predicate prevents leakage across users if the gate is ever
    *     bypassed. */
   sched_event_t u2m = make_event();
   u2m.user_id = 2;
   u2m.fire_at = now - 1800;
   u2m.status = SCHED_STATUS_MISSED;
   strncpy(u2m.name, "U2 Missed", SCHED_NAME_MAX - 1);
   int64_t u2_id = 0;
   scheduler_db_insert(&u2m, &u2_id);
   TEST_ASSERT_EQUAL_INT(FAILURE, scheduler_db_clear_missed(u2_id, 1));
   scheduler_db_get(u2_id, &after);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_MISSED, after.status);
   /* And the actual owner can still clear it */
   TEST_ASSERT_EQUAL_INT(SUCCESS, scheduler_db_clear_missed(u2_id, 2));
}

/* ============================================================================
 * Test: briefing_steps set/list round-trip
 * ============================================================================ */

static void test_briefing_steps_roundtrip(void) {
   /* Insert a briefing event row to attach steps to */
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   int64_t event_id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev, &event_id));

   /* Empty list before set */
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = -1;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_briefing_steps_list(
                                               event_id, out, SCHED_BRIEFING_STEPS_MAX, &count));
   TEST_ASSERT_EQUAL_INT(0, count);

   /* Set 3 steps */
   sched_briefing_step_t steps[3];
   memset(steps, 0, sizeof(steps));
   strncpy(steps[0].tool_name, "weather", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[0].tool_action, "get", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[0].tool_value, "Atlanta", SCHED_TOOL_VALUE_MAX - 1);
   strncpy(steps[1].tool_name, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[1].tool_action, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[1].tool_value, "top AI news today", SCHED_TOOL_VALUE_MAX - 1);
   strncpy(steps[2].tool_name, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[2].tool_action, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[2].tool_value, "top world news today", SCHED_TOOL_VALUE_MAX - 1);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_briefing_steps_set(event_id, steps, 3));

   /* List returns them in seq order */
   count = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_briefing_steps_list(
                                               event_id, out, SCHED_BRIEFING_STEPS_MAX, &count));
   TEST_ASSERT_EQUAL_INT(3, count);
   TEST_ASSERT_EQUAL_STRING("weather", out[0].tool_name);
   TEST_ASSERT_EQUAL_STRING("Atlanta", out[0].tool_value);
   TEST_ASSERT_EQUAL_STRING("search", out[1].tool_name);
   TEST_ASSERT_EQUAL_STRING("top AI news today", out[1].tool_value);
   TEST_ASSERT_EQUAL_STRING("top world news today", out[2].tool_value);

   /* Re-set replaces (atomic transaction inside the helper) */
   sched_briefing_step_t replaced[1];
   memset(replaced, 0, sizeof(replaced));
   strncpy(replaced[0].tool_name, "url_fetch", SCHED_TOOL_NAME_MAX - 1);
   strncpy(replaced[0].tool_value, "https://example.com", SCHED_TOOL_VALUE_MAX - 1);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_briefing_steps_set(event_id, replaced, 1));
   count = 0;
   scheduler_db_briefing_steps_list(event_id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("url_fetch", out[0].tool_name);

   /* Reject above cap */
   sched_briefing_step_t too_many[SCHED_BRIEFING_STEPS_MAX + 1];
   memset(too_many, 0, sizeof(too_many));
   for (int i = 0; i < SCHED_BRIEFING_STEPS_MAX + 1; i++)
      strncpy(too_many[i].tool_name, "weather", SCHED_TOOL_NAME_MAX - 1);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE, scheduler_db_briefing_steps_set(
                                               event_id, too_many, SCHED_BRIEFING_STEPS_MAX + 1));
}

/* ============================================================================
 * Test: insert_with_step_clone copies source briefing's steps atomically
 * ============================================================================ */

static void test_insert_with_step_clone(void) {
   /* Source briefing with 2 steps */
   sched_event_t src = make_event();
   src.event_type = SCHED_EVENT_BRIEFING;
   int64_t src_id = 0;
   scheduler_db_insert(&src, &src_id);
   sched_briefing_step_t steps[2];
   memset(steps, 0, sizeof(steps));
   strncpy(steps[0].tool_name, "weather", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[1].tool_name, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[1].tool_value, "AI news", SCHED_TOOL_VALUE_MAX - 1);
   scheduler_db_briefing_steps_set(src_id, steps, 2);

   /* Build the next-occurrence row and clone */
   sched_event_t next = src;
   next.id = 0;
   next.status = SCHED_STATUS_PENDING;
   next.fire_at = time(NULL) + 86400;
   next.fired_at = 0;
   int64_t new_id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_insert_with_step_clone(&next, src_id, &new_id));
   TEST_ASSERT_TRUE(new_id > src_id);

   /* Cloned steps present on the new row */
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(new_id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("weather", out[0].tool_name);
   TEST_ASSERT_EQUAL_STRING("search", out[1].tool_name);
   TEST_ASSERT_EQUAL_STRING("AI news", out[1].tool_value);

   /* Source steps still intact (clone is read-only against src) */
   count = 0;
   scheduler_db_briefing_steps_list(src_id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(2, count);
}

/* ============================================================================
 * Test: clone with zero source steps inserts but copies nothing
 * ============================================================================ */

static void test_insert_with_step_clone_no_source_steps(void) {
   sched_event_t src = make_event();
   src.event_type = SCHED_EVENT_BRIEFING;
   int64_t src_id = 0;
   scheduler_db_insert(&src, &src_id);

   sched_event_t next = src;
   next.id = 0;
   next.fire_at = time(NULL) + 86400;
   int64_t new_id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_insert_with_step_clone(&next, src_id, &new_id));

   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = -1;
   scheduler_db_briefing_steps_list(new_id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(0, count);
}

/* ============================================================================
 * Test: scheduler_db_cancel_and_insert_next — recurring chain stays atomic
 * ============================================================================ */

static void test_cancel_and_insert_next_briefing(void) {
   /* Recurring briefing with 2 steps — the cancel path needs to clone steps. */
   sched_event_t src = make_event();
   src.event_type = SCHED_EVENT_BRIEFING;
   src.recurrence = SCHED_RECUR_DAILY;
   int64_t src_id = 0;
   scheduler_db_insert(&src, &src_id);
   sched_briefing_step_t steps[2];
   memset(steps, 0, sizeof(steps));
   strncpy(steps[0].tool_name, "weather", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[1].tool_name, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(steps[1].tool_value, "AI news", SCHED_TOOL_VALUE_MAX - 1);
   scheduler_db_briefing_steps_set(src_id, steps, 2);

   sched_event_t next = src;
   next.id = 0;
   next.status = SCHED_STATUS_PENDING;
   next.fire_at = src.fire_at + 86400;
   next.fired_at = 0;

   int64_t new_id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_cancel_and_insert_next(src_id, &next, src_id,
                                                             /*clone_steps=*/true, &new_id));
   TEST_ASSERT_TRUE(new_id > src_id);

   /* Source now cancelled */
   sched_event_t verify;
   scheduler_db_get(src_id, &verify);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_CANCELLED, verify.status);

   /* New row pending + steps cloned */
   scheduler_db_get(new_id, &verify);
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_PENDING, verify.status);
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(new_id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("weather", out[0].tool_name);
   TEST_ASSERT_EQUAL_STRING("AI news", out[1].tool_value);
}

static void test_cancel_and_insert_next_already_cancelled_aborts(void) {
   /* Row already in a terminal state — cancel-and-insert must NOT silently
    * insert a phantom successor.  Pre-fix the two-call sequence happily
    * scheduled a next row even when the cancel was a no-op. */
   sched_event_t src = make_event();
   src.event_type = SCHED_EVENT_TIMER;
   src.recurrence = SCHED_RECUR_DAILY;
   int64_t src_id = 0;
   scheduler_db_insert(&src, &src_id);
   /* Move row out of pending/snoozed (fire it). */
   scheduler_db_update_status(src_id, SCHED_STATUS_FIRED);

   sched_event_t next = src;
   next.id = 0;
   next.status = SCHED_STATUS_PENDING;
   next.fire_at = src.fire_at + 86400;
   next.fired_at = 0;

   int64_t new_id = 12345; /* sentinel value — must be cleared to 0 on failure */
   int rc = scheduler_db_cancel_and_insert_next(src_id, &next, src_id, /*clone_steps=*/false,
                                                &new_id);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, new_id);

   /* Source row is still FIRED — the failed cancel did NOT silently flip it. */
   sched_event_t verify;
   TEST_ASSERT_EQUAL_INT(SUCCESS, scheduler_db_get(src_id, &verify));
   TEST_ASSERT_EQUAL_INT(SCHED_STATUS_FIRED, verify.status);

   /* No phantom successor row inserted — count rows via direct SQL since
    * list_user_events filters to pending/snoozed/ringing only. */
   sqlite3_stmt *cnt = NULL;
   int row_count = -1;
   if (sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM scheduled_events WHERE user_id = 1", -1,
                          &cnt, NULL) == SQLITE_OK) {
      if (sqlite3_step(cnt) == SQLITE_ROW)
         row_count = sqlite3_column_int(cnt, 0);
   }
   sqlite3_finalize(cnt);
   TEST_ASSERT_EQUAL_INT(1, row_count);
}

/* ============================================================================
 * Test: scheduler_db_briefing_steps_list_many returns identical results to
 * the single-row helper across a mixed batch of briefings + non-briefings.
 * ============================================================================ */

static void test_briefing_steps_list_many_mixed(void) {
   /* Three briefings with varying step counts, one timer (no steps). */
   int64_t b1_id = 0, b2_id = 0, b3_id = 0, t_id = 0;
   sched_event_t b = make_event();
   b.event_type = SCHED_EVENT_BRIEFING;
   scheduler_db_insert(&b, &b1_id);
   scheduler_db_insert(&b, &b2_id);
   scheduler_db_insert(&b, &b3_id);
   sched_event_t t = make_event();
   t.event_type = SCHED_EVENT_TIMER;
   scheduler_db_insert(&t, &t_id);

   sched_briefing_step_t s1[2];
   memset(s1, 0, sizeof(s1));
   strncpy(s1[0].tool_name, "weather", SCHED_TOOL_NAME_MAX - 1);
   strncpy(s1[0].tool_value, "Atlanta", SCHED_TOOL_VALUE_MAX - 1);
   strncpy(s1[1].tool_name, "search", SCHED_TOOL_NAME_MAX - 1);
   strncpy(s1[1].tool_value, "today", SCHED_TOOL_VALUE_MAX - 1);
   scheduler_db_briefing_steps_set(b1_id, s1, 2);

   /* b2 deliberately empty (0 steps) */

   sched_briefing_step_t s3[1];
   memset(s3, 0, sizeof(s3));
   strncpy(s3[0].tool_name, "url_fetch", SCHED_TOOL_NAME_MAX - 1);
   strncpy(s3[0].tool_value, "https://x", SCHED_TOOL_VALUE_MAX - 1);
   scheduler_db_briefing_steps_set(b3_id, s3, 1);

   int64_t ids[4] = { b1_id, b2_id, b3_id, t_id };
   sched_briefing_step_t table[4 * SCHED_BRIEFING_STEPS_MAX];
   int counts[4] = { -1, -1, -1, -1 };
   memset(table, 0, sizeof(table));

   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_briefing_steps_list_many(ids, 4, table, counts));

   /* Per-id counts match the single-row helper's output */
   TEST_ASSERT_EQUAL_INT(2, counts[0]);
   TEST_ASSERT_EQUAL_INT(0, counts[1]);
   TEST_ASSERT_EQUAL_INT(1, counts[2]);
   TEST_ASSERT_EQUAL_INT(0, counts[3]); /* timer has no steps */

   /* Content matches and lives in the right row slots */
   TEST_ASSERT_EQUAL_STRING("weather", table[0 * SCHED_BRIEFING_STEPS_MAX + 0].tool_name);
   TEST_ASSERT_EQUAL_STRING("Atlanta", table[0 * SCHED_BRIEFING_STEPS_MAX + 0].tool_value);
   TEST_ASSERT_EQUAL_STRING("search", table[0 * SCHED_BRIEFING_STEPS_MAX + 1].tool_name);
   TEST_ASSERT_EQUAL_STRING("url_fetch", table[2 * SCHED_BRIEFING_STEPS_MAX + 0].tool_name);
}

static void test_briefing_steps_list_many_rejects_bad_args(void) {
   sched_briefing_step_t table[SCHED_BRIEFING_STEPS_MAX];
   int counts[1] = { 0 };
   int64_t ids[1] = { 1 };

   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE,
                         scheduler_db_briefing_steps_list_many(NULL, 1, table, counts));
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE,
                         scheduler_db_briefing_steps_list_many(ids, 1, NULL, counts));
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE,
                         scheduler_db_briefing_steps_list_many(ids, 1, table, NULL));
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE,
                         scheduler_db_briefing_steps_list_many(ids, 0, table, counts));
}

/* ============================================================================
 * Test: say_aloud column round-trips through insert + read (schema v53)
 * ============================================================================ */

static void test_say_aloud_persistence(void) {
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   ev.say_aloud = SCHED_SAY_ALOUD_ALWAYS;
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev, &id));

   sched_event_t got;
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_INT(SCHED_SAY_ALOUD_ALWAYS, got.say_aloud);

   /* Default = 0 round-trips */
   ev.say_aloud = SCHED_SAY_ALOUD_DEFAULT;
   id = 0;
   scheduler_db_insert(&ev, &id);
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_SAY_ALOUD_DEFAULT, got.say_aloud);

   /* NEVER round-trips */
   ev.say_aloud = SCHED_SAY_ALOUD_NEVER;
   id = 0;
   scheduler_db_insert(&ev, &id);
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(SCHED_SAY_ALOUD_NEVER, got.say_aloud);
}

/* ============================================================================
 * Test: deliver_to column round-trips through insert + read (schema v54)
 * ============================================================================ */

static void test_deliver_to_persistence(void) {
   /* Non-empty channel name round-trips */
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   strncpy(ev.deliver_to, "slack_main", SCHED_DELIVER_TO_MAX - 1);
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev, &id));

   sched_event_t got;
   memset(&got, 0, sizeof(got));
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("slack_main", got.deliver_to);

   /* Empty deliver_to stays empty (default — no fan-out) */
   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_BRIEFING;
   /* deliver_to[0] == '\0' from make_event's memset */
   id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev2, &id));

   memset(&got, 0xAA, sizeof(got)); /* prove the read actually clears */
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_INT(0, got.deliver_to[0]);

   /* deliver_to also round-trips on SCHED_EVENT_TASK (not briefing-only) */
   sched_event_t ev3 = make_event();
   ev3.event_type = SCHED_EVENT_TASK;
   strncpy(ev3.deliver_to, "telegram_personal", SCHED_DELIVER_TO_MAX - 1);
   id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev3, &id));

   memset(&got, 0, sizeof(got));
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("telegram_personal", got.deliver_to);
}

/* ============================================================================
 * Test: instructions column round-trips through insert + read (schema v84)
 * ============================================================================ */

static void test_instructions_persistence(void) {
   /* Non-empty instructions round-trip */
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   strncpy(ev.instructions, "Lead with unread email; keep it under five bullets.",
           SCHED_INSTRUCTIONS_MAX - 1);
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev, &id));

   sched_event_t got;
   memset(&got, 0, sizeof(got));
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("Lead with unread email; keep it under five bullets.",
                            got.instructions);

   /* Empty instructions stay empty (legacy default — fixed prompt) */
   sched_event_t ev2 = make_event();
   ev2.event_type = SCHED_EVENT_BRIEFING;
   /* instructions[0] == '\0' from make_event's memset */
   id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&ev2, &id));

   memset(&got, 0xAA, sizeof(got)); /* prove the read actually clears */
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_INT(0, got.instructions[0]);
}

/* ============================================================================
 * Test: deliver_to carries through recurrence (struct copy invariant)
 *
 * prepare_next_occurrence_row in scheduler.c does *next_out = *src and only
 * clears the lifecycle fields (id, status, fire_at, fired_at, snooze_count,
 * snoozed_until).  Everything else MUST carry over, including the new
 * deliver_to.  Pin this invariant so a future struct refactor that drops
 * the wholesale copy doesn't silently break recurring fan-out.
 *
 * Operates directly on the struct because prepare_next_occurrence_row is
 * file-static in scheduler.c — we mirror its known semantics rather than
 * call it.  If the function ever moves out of static, switch this test
 * to call it.
 * ============================================================================ */

static void test_deliver_to_recurrence_carry(void) {
   sched_event_t src = make_event();
   src.event_type = SCHED_EVENT_BRIEFING;
   src.recurrence = SCHED_RECUR_DAILY;
   strncpy(src.deliver_to, "slack_morning", SCHED_DELIVER_TO_MAX - 1);
   src.say_aloud = SCHED_SAY_ALOUD_ALWAYS;
   strncpy(src.instructions, "Punchy, lead with anything time-sensitive.",
           SCHED_INSTRUCTIONS_MAX - 1);

   /* Mirror prepare_next_occurrence_row's exact body. */
   sched_event_t next = src;
   next.id = 0;
   next.status = SCHED_STATUS_PENDING;
   next.fire_at = src.fire_at + 86400;
   next.fired_at = 0;
   next.snooze_count = 0;
   next.snoozed_until = 0;

   TEST_ASSERT_EQUAL_STRING("slack_morning", next.deliver_to);
   TEST_ASSERT_EQUAL_INT(SCHED_SAY_ALOUD_ALWAYS, next.say_aloud);
   TEST_ASSERT_EQUAL_INT(SCHED_EVENT_BRIEFING, next.event_type);
   TEST_ASSERT_EQUAL_STRING("Punchy, lead with anything time-sensitive.", next.instructions);

   /* Round-trip the next-occurrence row through the DB too so a struct
    * shape that survives the in-memory copy but fails on DB persist
    * still trips the test. */
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS, scheduler_db_insert(&next, &id));
   sched_event_t got;
   memset(&got, 0, sizeof(got));
   TEST_ASSERT_EQUAL_INT(0, scheduler_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("slack_morning", got.deliver_to);
   TEST_ASSERT_EQUAL_STRING("Punchy, lead with anything time-sensitive.", got.instructions);
}

/* ============================================================================
 * Test: scheduler_db_briefing_steps_update — the guarded editor (update action)
 * ============================================================================ */

/* Small helper: make a pending briefing owned by user 1 with `n` search steps. */
static int64_t make_briefing_with_steps(int user_id, int n) {
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   ev.user_id = user_id;
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);
   if (n > 0) {
      sched_briefing_step_t steps[SCHED_BRIEFING_STEPS_MAX];
      memset(steps, 0, sizeof(steps));
      for (int i = 0; i < n; i++) {
         strncpy(steps[i].tool_name, "search", SCHED_TOOL_NAME_MAX - 1);
         strncpy(steps[i].tool_action, "news", SCHED_TOOL_NAME_MAX - 1);
         snprintf(steps[i].tool_value, SCHED_TOOL_VALUE_MAX, "topic %d", i);
      }
      scheduler_db_briefing_steps_set(id, steps, n);
   }
   return id;
}

static sched_briefing_step_t one_step(const char *name, const char *action, const char *value) {
   sched_briefing_step_t s;
   memset(&s, 0, sizeof(s));
   strncpy(s.tool_name, name, SCHED_TOOL_NAME_MAX - 1);
   if (action)
      strncpy(s.tool_action, action, SCHED_TOOL_NAME_MAX - 1);
   if (value)
      strncpy(s.tool_value, value, SCHED_TOOL_VALUE_MAX - 1);
   return s;
}

static void test_briefing_steps_update_append(void) {
   int64_t id = make_briefing_with_steps(1, 1);
   sched_briefing_step_t add = one_step("search", "news", "stock holdings");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_briefing_steps_update(id, 1, &add, 1, /*append=*/true));

   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("topic 0", out[0].tool_value);        /* original kept */
   TEST_ASSERT_EQUAL_STRING("stock holdings", out[1].tool_value); /* appended last */
}

static void test_briefing_steps_update_replace(void) {
   int64_t id = make_briefing_with_steps(1, 3);
   sched_briefing_step_t repl = one_step("weather", "get", "Atlanta");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_briefing_steps_update(id, 1, &repl, 1, /*append=*/false));

   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("weather", out[0].tool_name);
}

static void test_briefing_steps_update_ownership(void) {
   int64_t id = make_briefing_with_steps(1, 1); /* owned by user 1 */
   sched_briefing_step_t add = one_step("search", "news", "intrusion");
   /* user 2 cannot edit user 1's briefing */
   TEST_ASSERT_EQUAL_INT(SCHED_DB_NOT_EDITABLE,
                         scheduler_db_briefing_steps_update(id, 2, &add, 1, /*append=*/true));
   /* steps unchanged */
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
}

static void test_briefing_steps_update_not_editable_status(void) {
   int64_t id = make_briefing_with_steps(1, 1);
   scheduler_db_update_status(id, SCHED_STATUS_FIRED); /* out of pending/snoozed */
   sched_briefing_step_t add = one_step("search", "news", "late");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_NOT_EDITABLE,
                         scheduler_db_briefing_steps_update(id, 1, &add, 1, /*append=*/true));
}

static void test_briefing_steps_update_rejects_non_briefing(void) {
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_TASK;
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);
   sched_briefing_step_t add = one_step("search", "news", "x");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_NOT_EDITABLE,
                         scheduler_db_briefing_steps_update(id, 1, &add, 1, /*append=*/true));
}

static void test_briefing_steps_update_legacy_materialization(void) {
   /* A pre-v50 briefing: legacy tool_* on the row, zero rows in briefing_steps. */
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   strncpy(ev.tool_name, "weather", SCHED_TOOL_NAME_MAX - 1);
   strncpy(ev.tool_action, "get", SCHED_TOOL_NAME_MAX - 1);
   strncpy(ev.tool_value, "Sugar Hill", SCHED_TOOL_VALUE_MAX - 1);
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   sched_briefing_step_t add = one_step("search", "news", "stock holdings");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_briefing_steps_update(id, 1, &add, 1, /*append=*/true));

   /* The legacy tool must survive as step[0], with the new step appended. */
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("weather", out[0].tool_name);
   TEST_ASSERT_EQUAL_STRING("Sugar Hill", out[0].tool_value);
   TEST_ASSERT_EQUAL_STRING("search", out[1].tool_name);
   TEST_ASSERT_EQUAL_STRING("stock holdings", out[1].tool_value);
}

static void test_briefing_steps_update_cap_overflow(void) {
   int64_t id = make_briefing_with_steps(1, SCHED_BRIEFING_STEPS_MAX); /* already full */
   sched_briefing_step_t add = one_step("search", "news", "one too many");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE,
                         scheduler_db_briefing_steps_update(id, 1, &add, 1, /*append=*/true));
   /* Original list intact (append rolled back). */
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(SCHED_BRIEFING_STEPS_MAX, count);
}

static void test_briefing_steps_update_append_dedup(void) {
   int64_t id = make_briefing_with_steps(1, 1); /* step: search/news/"topic 0" */
   sched_briefing_step_t dup = one_step("search", "news", "topic 0"); /* exact duplicate */
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_briefing_steps_update(id, 1, &dup, 1, /*append=*/true));
   /* Idempotent: no second copy added. */
   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
}

/* Pins design §6: editing the pending row's steps propagates to the next
 * occurrence, because the recurrence clone copies from the just-fired row. */
static void test_briefing_steps_update_clone_propagation(void) {
   sched_event_t src = make_event();
   src.event_type = SCHED_EVENT_BRIEFING;
   src.recurrence = SCHED_RECUR_DAILY;
   int64_t src_id = 0;
   scheduler_db_insert(&src, &src_id);
   sched_briefing_step_t base = one_step("weather", "get", "Sugar Hill");
   scheduler_db_briefing_steps_set(src_id, &base, 1);

   /* Edit the pending row: add stock news. */
   sched_briefing_step_t add = one_step("search", "news", "stock holdings");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_briefing_steps_update(src_id, 1, &add, 1, /*append=*/true));

   /* Simulate the rollover: clone the next occurrence from the edited row. */
   sched_event_t next = src;
   next.id = 0;
   next.status = SCHED_STATUS_PENDING;
   next.fire_at = src.fire_at + 86400;
   next.fired_at = 0;
   int64_t new_id = 0;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_insert_with_step_clone(&next, src_id, &new_id));

   sched_briefing_step_t out[SCHED_BRIEFING_STEPS_MAX];
   int count = 0;
   scheduler_db_briefing_steps_list(new_id, out, SCHED_BRIEFING_STEPS_MAX, &count);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("weather", out[0].tool_name);
   TEST_ASSERT_EQUAL_STRING("stock holdings", out[1].tool_value);
}

static void test_briefing_steps_update_missing_row(void) {
   sched_briefing_step_t add = one_step("search", "news", "x");
   TEST_ASSERT_EQUAL_INT(SCHED_DB_NOT_EDITABLE,
                         scheduler_db_briefing_steps_update(999999, 1, &add, 1, /*append=*/true));
}

/* ============================================================================
 * Test: scheduler_db_update_fields — scalar-field editor (Phase 2)
 * ============================================================================ */

static void test_update_fields_name_and_recurrence(void) {
   sched_event_t ev = make_event();
   ev.recurrence = SCHED_RECUR_DAILY;
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   strncpy(fields.name, "Renamed Alarm", SCHED_NAME_MAX - 1);
   fields.recurrence = SCHED_RECUR_WEEKDAYS;
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_update_fields(id, 1, &fields,
                                                    SCHED_FIELD_NAME | SCHED_FIELD_RECURRENCE));

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_STRING("Renamed Alarm", got.name);
   TEST_ASSERT_EQUAL_INT(SCHED_RECUR_WEEKDAYS, got.recurrence);
   /* Unmasked fields untouched. */
   TEST_ASSERT_EQUAL_STRING("Wake up!", got.message);
}

static void test_update_fields_fire_at_and_original_time(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   time_t new_fire = time(NULL) + 7200;
   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   fields.fire_at = new_fire;
   strncpy(fields.original_time, "09:30", SCHED_ORIGINAL_TIME_MAX - 1);
   TEST_ASSERT_EQUAL_INT(
       SCHED_DB_SUCCESS,
       scheduler_db_update_fields(id, 1, &fields, SCHED_FIELD_FIRE_AT | SCHED_FIELD_ORIGINAL_TIME));

   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT64(new_fire, got.fire_at);
   TEST_ASSERT_EQUAL_STRING("09:30", got.original_time);
}

static void test_update_fields_deliver_to_clear(void) {
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   strncpy(ev.deliver_to, "slack_main", SCHED_DELIVER_TO_MAX - 1);
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   /* Empty deliver_to clears the fan-out. */
   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   fields.deliver_to[0] = '\0';
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_update_fields(id, 1, &fields, SCHED_FIELD_DELIVER_TO));
   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(0, got.deliver_to[0]);
}

static void test_update_fields_instructions_set_and_clear(void) {
   sched_event_t ev = make_event();
   ev.event_type = SCHED_EVENT_BRIEFING;
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   /* Set instructions on a briefing that had none. */
   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   strncpy(fields.instructions, "Terse, no closing line.", SCHED_INSTRUCTIONS_MAX - 1);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_update_fields(id, 1, &fields, SCHED_FIELD_INSTRUCTIONS));
   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_STRING("Terse, no closing line.", got.instructions);

   /* Empty instructions clear the steering (back to the default prompt). */
   memset(&fields, 0, sizeof(fields));
   fields.instructions[0] = '\0';
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_update_fields(id, 1, &fields, SCHED_FIELD_INSTRUCTIONS));
   memset(&got, 0xAA, sizeof(got));
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_INT(0, got.instructions[0]);
}

static void test_update_fields_ownership(void) {
   sched_event_t ev = make_event(); /* user 1 */
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   strncpy(fields.name, "Hijacked", SCHED_NAME_MAX - 1);
   /* user 2 cannot edit user 1's event */
   TEST_ASSERT_EQUAL_INT(SCHED_DB_NOT_EDITABLE,
                         scheduler_db_update_fields(id, 2, &fields, SCHED_FIELD_NAME));
   sched_event_t got;
   scheduler_db_get(id, &got);
   TEST_ASSERT_EQUAL_STRING("Test Alarm", got.name); /* unchanged */
}

static void test_update_fields_not_editable_status(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);
   scheduler_db_update_status(id, SCHED_STATUS_FIRED);

   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   strncpy(fields.name, "Too Late", SCHED_NAME_MAX - 1);
   TEST_ASSERT_EQUAL_INT(SCHED_DB_NOT_EDITABLE,
                         scheduler_db_update_fields(id, 1, &fields, SCHED_FIELD_NAME));
}

static void test_update_fields_empty_mask_rejected(void) {
   sched_event_t ev = make_event();
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);
   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   TEST_ASSERT_EQUAL_INT(SCHED_DB_FAILURE, scheduler_db_update_fields(id, 1, &fields, 0));
}

/* A no-op edit (new value == current value) still matches the WHERE row, so
 * sqlite3_changes() > 0 and the primitive returns SUCCESS — NOT a false
 * NOT_EDITABLE.  Pins the contract against a future switch to a diff-based
 * change count. */
static void test_update_fields_noop_returns_success(void) {
   sched_event_t ev = make_event(); /* name == "Test Alarm" */
   int64_t id = 0;
   scheduler_db_insert(&ev, &id);

   sched_event_t fields;
   memset(&fields, 0, sizeof(fields));
   strncpy(fields.name, "Test Alarm", SCHED_NAME_MAX - 1); /* identical value */
   TEST_ASSERT_EQUAL_INT(SCHED_DB_SUCCESS,
                         scheduler_db_update_fields(id, 1, &fields, SCHED_FIELD_NAME));
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_string_conversions);
   RUN_TEST(test_insert_and_get);
   RUN_TEST(test_insert_checked_limits);
   RUN_TEST(test_update_status);
   RUN_TEST(test_update_status_fired);
   RUN_TEST(test_cancel_optimistic);
   RUN_TEST(test_dismiss_optimistic);
   RUN_TEST(test_snooze);
   RUN_TEST(test_due_events);
   RUN_TEST(test_list_user_events);
   RUN_TEST(test_find_by_name);
   RUN_TEST(test_count_events);
   RUN_TEST(test_get_ringing);
   RUN_TEST(test_cleanup_old_events);
   RUN_TEST(test_next_fire_time);
   RUN_TEST(test_get_active_by_uuid);
   RUN_TEST(test_get_missed_events);
   RUN_TEST(test_list_user_missed);
   RUN_TEST(test_clear_missed);
   RUN_TEST(test_briefing_steps_roundtrip);
   RUN_TEST(test_insert_with_step_clone);
   RUN_TEST(test_insert_with_step_clone_no_source_steps);
   RUN_TEST(test_cancel_and_insert_next_briefing);
   RUN_TEST(test_cancel_and_insert_next_already_cancelled_aborts);
   RUN_TEST(test_briefing_steps_list_many_mixed);
   RUN_TEST(test_briefing_steps_list_many_rejects_bad_args);
   RUN_TEST(test_say_aloud_persistence);
   RUN_TEST(test_deliver_to_persistence);
   RUN_TEST(test_instructions_persistence);
   RUN_TEST(test_deliver_to_recurrence_carry);
   RUN_TEST(test_briefing_steps_update_append);
   RUN_TEST(test_briefing_steps_update_replace);
   RUN_TEST(test_briefing_steps_update_ownership);
   RUN_TEST(test_briefing_steps_update_not_editable_status);
   RUN_TEST(test_briefing_steps_update_rejects_non_briefing);
   RUN_TEST(test_briefing_steps_update_legacy_materialization);
   RUN_TEST(test_briefing_steps_update_cap_overflow);
   RUN_TEST(test_briefing_steps_update_append_dedup);
   RUN_TEST(test_briefing_steps_update_clone_propagation);
   RUN_TEST(test_briefing_steps_update_missing_row);
   RUN_TEST(test_update_fields_name_and_recurrence);
   RUN_TEST(test_update_fields_fire_at_and_original_time);
   RUN_TEST(test_update_fields_deliver_to_clear);
   RUN_TEST(test_update_fields_instructions_set_and_clear);
   RUN_TEST(test_update_fields_ownership);
   RUN_TEST(test_update_fields_not_editable_status);
   RUN_TEST(test_update_fields_empty_mask_rejected);
   RUN_TEST(test_update_fields_noop_returns_success);
   return UNITY_END();
}
