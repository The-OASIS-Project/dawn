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
 * Calendar Database Layer — SQLite CRUD for accounts, calendars, events,
 * and pre-expanded occurrences. Uses the shared auth_db handle (s_db).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/calendar_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Helper: safe string copy from SQLite column
 * ============================================================================= */

static void col_str(char *dst, size_t dst_size, sqlite3_stmt *stmt, int col) {
   const char *src = (const char *)sqlite3_column_text(stmt, col);
   if (src) {
      size_t len = (size_t)sqlite3_column_bytes(stmt, col);
      if (len >= dst_size)
         len = dst_size - 1;
      memcpy(dst, src, len);
      dst[len] = '\0';
   } else {
      dst[0] = '\0';
   }
}

/* =============================================================================
 * Row Mappers
 * ============================================================================= */

static void row_to_account(sqlite3_stmt *stmt, calendar_account_t *a) {
   a->id = sqlite3_column_int64(stmt, 0);
   a->user_id = sqlite3_column_int(stmt, 1);
   col_str(a->name, sizeof(a->name), stmt, 2);
   col_str(a->caldav_url, sizeof(a->caldav_url), stmt, 3);
   col_str(a->username, sizeof(a->username), stmt, 4);
   /* Column 5: encrypted_password (BLOB) */
   const void *blob = sqlite3_column_blob(stmt, 5);
   int blob_len = sqlite3_column_bytes(stmt, 5);
   if (blob && blob_len > 0 && blob_len <= (int)sizeof(a->encrypted_password)) {
      memcpy(a->encrypted_password, blob, blob_len);
      a->encrypted_password_len = blob_len;
   } else {
      memset(a->encrypted_password, 0, sizeof(a->encrypted_password));
      a->encrypted_password_len = 0;
   }
   col_str(a->auth_type, sizeof(a->auth_type), stmt, 6);
   col_str(a->principal_url, sizeof(a->principal_url), stmt, 7);
   col_str(a->calendar_home_url, sizeof(a->calendar_home_url), stmt, 8);
   a->enabled = sqlite3_column_int(stmt, 9) != 0;
   a->last_sync = (time_t)sqlite3_column_int64(stmt, 10);
   a->sync_interval_sec = sqlite3_column_int(stmt, 11);
   a->created_at = (time_t)sqlite3_column_int64(stmt, 12);
   a->read_only = sqlite3_column_int(stmt, 13) != 0;
   /* Column 14: oauth_account_key (may not exist in older schemas) */
   int col_count = sqlite3_column_count(stmt);
   if (col_count >= 15)
      col_str(a->oauth_account_key, sizeof(a->oauth_account_key), stmt, 14);
   else
      a->oauth_account_key[0] = '\0';
}

static void row_to_calendar(sqlite3_stmt *stmt, calendar_calendar_t *c) {
   c->id = sqlite3_column_int64(stmt, 0);
   c->account_id = sqlite3_column_int64(stmt, 1);
   col_str(c->caldav_path, sizeof(c->caldav_path), stmt, 2);
   col_str(c->display_name, sizeof(c->display_name), stmt, 3);
   col_str(c->color, sizeof(c->color), stmt, 4);
   c->is_active = sqlite3_column_int(stmt, 5) != 0;
   col_str(c->ctag, sizeof(c->ctag), stmt, 6);
   c->created_at = (time_t)sqlite3_column_int64(stmt, 7);
   /* Column 8 (account read_only) only present in active_for_user JOIN query.
    * Standard calendar queries return 8 columns; the JOIN query returns 9. */
   int col_count = sqlite3_column_count(stmt);
   c->account_read_only = (col_count >= 9) ? (sqlite3_column_int(stmt, 8) != 0) : false;
}

static void row_to_event(sqlite3_stmt *stmt, calendar_event_t *e) {
   e->id = sqlite3_column_int64(stmt, 0);
   e->calendar_id = sqlite3_column_int64(stmt, 1);
   col_str(e->uid, sizeof(e->uid), stmt, 2);
   col_str(e->etag, sizeof(e->etag), stmt, 3);
   col_str(e->summary, sizeof(e->summary), stmt, 4);
   col_str(e->description, sizeof(e->description), stmt, 5);
   col_str(e->location, sizeof(e->location), stmt, 6);
   e->dtstart = (time_t)sqlite3_column_int64(stmt, 7);
   e->dtend = (time_t)sqlite3_column_int64(stmt, 8);
   e->duration_sec = sqlite3_column_int(stmt, 9);
   e->all_day = sqlite3_column_int(stmt, 10) != 0;
   col_str(e->dtstart_date, sizeof(e->dtstart_date), stmt, 11);
   col_str(e->dtend_date, sizeof(e->dtend_date), stmt, 12);
   col_str(e->rrule, sizeof(e->rrule), stmt, 13);
   /* raw_ical is heap-allocated */
   const char *ical = (const char *)sqlite3_column_text(stmt, 14);
   e->raw_ical = ical ? strdup(ical) : NULL;
   e->last_synced = (time_t)sqlite3_column_int64(stmt, 15);
}

static void row_to_occurrence(sqlite3_stmt *stmt, calendar_occurrence_t *o) {
   o->id = sqlite3_column_int64(stmt, 0);
   o->event_id = sqlite3_column_int64(stmt, 1);
   o->dtstart = (time_t)sqlite3_column_int64(stmt, 2);
   o->dtend = (time_t)sqlite3_column_int64(stmt, 3);
   o->all_day = sqlite3_column_int(stmt, 4) != 0;
   col_str(o->dtstart_date, sizeof(o->dtstart_date), stmt, 5);
   col_str(o->dtend_date, sizeof(o->dtend_date), stmt, 6);
   col_str(o->summary, sizeof(o->summary), stmt, 7);
   col_str(o->location, sizeof(o->location), stmt, 8);
   o->is_override = sqlite3_column_int(stmt, 9) != 0;
   o->is_cancelled = sqlite3_column_int(stmt, 10) != 0;
   col_str(o->recurrence_id, sizeof(o->recurrence_id), stmt, 11);
   /* Columns 12 (e.uid) and 13 (e.calendar_id) present in JOIN queries; the
    * calendar id is selected only by the range queries, so guard each on the
    * actual column count. */
   int col_count = sqlite3_column_count(stmt);
   if (col_count >= 13)
      col_str(o->event_uid, sizeof(o->event_uid), stmt, 12);
   o->calendar_id = (col_count >= 14) ? sqlite3_column_int64(stmt, 13) : 0;
}

/* =============================================================================
 * Helper: build JSON array of calendar IDs for json_each() binding
 * ============================================================================= */

static int build_cal_id_json(const int64_t *ids, int count, char *buf, size_t buf_len) {
   if (buf_len < 32) {
      buf[0] = '\0';
      return 0;
   }
   int pos = 0;
   pos += snprintf(buf + pos, buf_len - pos, "[");
   for (int i = 0; i < count && pos < (int)buf_len - 22; i++) {
      if (i > 0)
         pos += snprintf(buf + pos, buf_len - pos, ",");
      pos += snprintf(buf + pos, buf_len - pos, "%lld", (long long)ids[i]);
   }
   pos += snprintf(buf + pos, buf_len - pos, "]");
   return pos;
}

/* =============================================================================
 * Account CRUD
 * ============================================================================= */

int calendar_db_account_create(const calendar_account_t *acct, int64_t *id_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_create;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, acct->user_id);
   sqlite3_bind_text(st, 2, acct->name, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, acct->caldav_url, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, acct->username, -1, SQLITE_STATIC);
   sqlite3_bind_blob(st, 5, acct->encrypted_password, acct->encrypted_password_len, SQLITE_STATIC);
   sqlite3_bind_text(st, 6, acct->auth_type[0] ? acct->auth_type : "basic", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 7, acct->principal_url, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 8, acct->calendar_home_url, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 9, acct->enabled ? 1 : 0);
   sqlite3_bind_int64(st, 10, (int64_t)acct->last_sync);
   sqlite3_bind_int(st, 11, acct->sync_interval_sec > 0 ? acct->sync_interval_sec : 900);
   sqlite3_bind_int64(st, 12, (int64_t)time(NULL));
   sqlite3_bind_int(st, 13, acct->read_only ? 1 : 0);
   sqlite3_bind_text(st, 14, acct->oauth_account_key, -1, SQLITE_STATIC);

   int result = FAILURE;
   if (sqlite3_step(st) == SQLITE_DONE) {
      if (id_out)
         *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("calendar_db: account create failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_get(int64_t id, calendar_account_t *out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_get;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int result = 1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      row_to_account(st, out);
      result = 0;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_list(int user_id, calendar_account_t *out, int max_count, int *count_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_list;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_account(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int calendar_db_account_list_enabled(calendar_account_t *out, int max_count, int *count_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_list_enabled;
   sqlite3_reset(st);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_account(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int calendar_db_account_update(const calendar_account_t *acct) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_update;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, acct->name, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, acct->caldav_url, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, acct->username, -1, SQLITE_STATIC);
   sqlite3_bind_blob(st, 4, acct->encrypted_password, acct->encrypted_password_len, SQLITE_STATIC);
   sqlite3_bind_text(st, 5, acct->auth_type, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 6, acct->enabled ? 1 : 0);
   sqlite3_bind_int(st, 7, acct->sync_interval_sec);
   sqlite3_bind_int64(st, 8, acct->id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   if (result != 0)
      OLOG_ERROR("calendar_db: account update failed: %s", sqlite3_errmsg(s_db.db));
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_delete(int64_t id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_delete;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_update_sync(int64_t id, time_t last_sync) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_update_sync;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, (int64_t)last_sync);
   sqlite3_bind_int64(st, 2, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_update_discovery(int64_t id,
                                         const char *principal_url,
                                         const char *calendar_home_url) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_update_discovery;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, principal_url ? principal_url : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, calendar_home_url ? calendar_home_url : "", -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_set_read_only(int64_t id, bool read_only) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_set_read_only;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, read_only ? 1 : 0);
   sqlite3_bind_int64(st, 2, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   if (result != 0)
      OLOG_ERROR("calendar_db: set_read_only failed: %s", sqlite3_errmsg(s_db.db));
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_account_set_enabled(int64_t id, bool enabled) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_acct_set_enabled;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, enabled ? 1 : 0);
   sqlite3_bind_int64(st, 2, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   if (result != 0)
      OLOG_ERROR("calendar_db: set_enabled failed: %s", sqlite3_errmsg(s_db.db));
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Calendar CRUD
 * ============================================================================= */

int calendar_db_calendar_create(const calendar_calendar_t *cal, int64_t *id_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_create;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, cal->account_id);
   sqlite3_bind_text(st, 2, cal->caldav_path, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, cal->display_name, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, cal->color, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 5, cal->is_active ? 1 : 0);
   sqlite3_bind_text(st, 6, cal->ctag, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 7, (int64_t)time(NULL));

   int result = FAILURE;
   if (sqlite3_step(st) == SQLITE_DONE) {
      if (id_out)
         *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("calendar_db: calendar create failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_calendar_get(int64_t id, calendar_calendar_t *out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_get;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int result = 1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      row_to_calendar(st, out);
      result = 0;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_calendar_list(int64_t account_id,
                              calendar_calendar_t *out,
                              int max_count,
                              int *count_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_list;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, account_id);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_calendar(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int calendar_db_calendar_update_ctag(int64_t id, const char *ctag) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_update_ctag;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, ctag ? ctag : "", -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_calendar_set_active(int64_t id, bool active) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_set_active;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, active ? 1 : 0);
   sqlite3_bind_int64(st, 2, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_calendar_delete(int64_t id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_delete;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_active_calendars_for_user(int user_id,
                                          calendar_calendar_t *out,
                                          int max_count,
                                          int *count_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_cal_active_for_user;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_calendar(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

/* =============================================================================
 * Event CRUD
 * ============================================================================= */

int calendar_db_event_upsert(const calendar_event_t *event, int64_t *id_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_evt_upsert;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, event->calendar_id);
   sqlite3_bind_text(st, 2, event->uid, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, event->etag, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 4, event->summary, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 5, event->description, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 6, event->location, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 7, (int64_t)event->dtstart);
   sqlite3_bind_int64(st, 8, (int64_t)event->dtend);
   sqlite3_bind_int(st, 9, event->duration_sec);
   sqlite3_bind_int(st, 10, event->all_day ? 1 : 0);
   sqlite3_bind_text(st, 11, event->dtstart_date, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 12, event->dtend_date, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 13, event->rrule, -1, SQLITE_STATIC);
   if (event->raw_ical)
      sqlite3_bind_text(st, 14, event->raw_ical, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(st, 14);
   sqlite3_bind_int64(st, 15, (int64_t)event->last_synced);

   int result = FAILURE;
   if (sqlite3_step(st) == SQLITE_DONE) {
      if (id_out)
         *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("calendar_db: event upsert failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_event_get_by_uid(const char *uid, calendar_event_t *out) {
   /* This query requires user_id — but the header says uid only.
    * We use a version that joins through to accounts for access control.
    * Caller must set out->calendar_id to user_id before calling (overloaded). */
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_evt_get_by_uid;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, uid, -1, SQLITE_STATIC);
   /* Bind user_id from the calendar_id field (overloaded for this call) */
   sqlite3_bind_int64(st, 2, out->calendar_id);

   int result = 1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      row_to_event(st, out);
      result = 0;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_event_delete(int64_t id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_evt_delete;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_event_delete_by_calendar(int64_t calendar_id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_evt_delete_by_cal;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, calendar_id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Occurrence CRUD
 * ============================================================================= */

int calendar_db_occurrence_insert(const calendar_occurrence_t *occ, int64_t *id_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_occ_insert;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, occ->event_id);
   sqlite3_bind_int64(st, 2, (int64_t)occ->dtstart);
   sqlite3_bind_int64(st, 3, (int64_t)occ->dtend);
   sqlite3_bind_int(st, 4, occ->all_day ? 1 : 0);
   sqlite3_bind_text(st, 5, occ->dtstart_date, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 6, occ->dtend_date, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 7, occ->summary, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 8, occ->location, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 9, occ->is_override ? 1 : 0);
   sqlite3_bind_int(st, 10, occ->is_cancelled ? 1 : 0);
   sqlite3_bind_text(st, 11, occ->recurrence_id, -1, SQLITE_STATIC);

   int result = FAILURE;
   if (sqlite3_step(st) == SQLITE_DONE) {
      if (id_out)
         *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("calendar_db: occurrence insert failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_occurrence_delete_for_event(int64_t event_id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_cal_occ_delete_for_event;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, event_id);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int calendar_db_occurrences_in_range(const int64_t *calendar_ids,
                                     int calendar_count,
                                     time_t range_start,
                                     time_t range_end,
                                     calendar_occurrence_t *out,
                                     int max_count,
                                     int *count_out) {
   if (calendar_count <= 0) {
      if (count_out)
         *count_out = 0;
      return SUCCESS;
   }

   AUTH_DB_LOCK_OR_FAIL();

   char id_json[768];
   build_cal_id_json(calendar_ids, calendar_count, id_json, sizeof(id_json));

   sqlite3_stmt *st = s_db.stmt_cal_occ_in_range;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, id_json, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, (int64_t)range_end);
   sqlite3_bind_int64(st, 3, (int64_t)range_start);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_occurrence(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int calendar_db_allday_occurrences_in_range(const int64_t *calendar_ids,
                                            int calendar_count,
                                            const char *start_date,
                                            const char *end_date,
                                            calendar_occurrence_t *out,
                                            int max_count,
                                            int *count_out) {
   if (calendar_count <= 0) {
      if (count_out)
         *count_out = 0;
      return SUCCESS;
   }

   AUTH_DB_LOCK_OR_FAIL();

   char id_json[768];
   build_cal_id_json(calendar_ids, calendar_count, id_json, sizeof(id_json));

   sqlite3_stmt *st = s_db.stmt_cal_occ_allday_in_range;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, id_json, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, end_date, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, start_date, -1, SQLITE_STATIC);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_occurrence(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int calendar_db_occurrences_search(const int64_t *calendar_ids,
                                   int calendar_count,
                                   const char *query,
                                   calendar_occurrence_t *out,
                                   int max_count,
                                   int *count_out) {
   if (calendar_count <= 0 || !query) {
      if (count_out)
         *count_out = 0;
      return SUCCESS;
   }

   AUTH_DB_LOCK_OR_FAIL();

   char id_json[768];
   build_cal_id_json(calendar_ids, calendar_count, id_json, sizeof(id_json));

   /* Escape LIKE metacharacters (%, _, \) before building pattern */
   if (strlen(query) > 200) {
      AUTH_DB_UNLOCK();
      if (count_out)
         *count_out = 0;
      return SUCCESS;
   }
   char escaped[512];
   size_t ej = 0;
   for (size_t ei = 0; query[ei] && ej < sizeof(escaped) - 2; ei++) {
      if (query[ei] == '%' || query[ei] == '_' || query[ei] == '\\')
         escaped[ej++] = '\\';
      escaped[ej++] = query[ei];
   }
   escaped[ej] = '\0';

   char pattern[600];
   snprintf(pattern, sizeof(pattern), "%%%s%%", escaped);

   sqlite3_stmt *st = s_db.stmt_cal_occ_search;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, id_json, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 4, max_count);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_occurrence(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int calendar_db_next_occurrence(const int64_t *calendar_ids,
                                int calendar_count,
                                time_t after,
                                calendar_occurrence_t *out) {
   if (calendar_count <= 0)
      return 1;

   AUTH_DB_LOCK_OR_FAIL();

   char id_json[768];
   build_cal_id_json(calendar_ids, calendar_count, id_json, sizeof(id_json));

   sqlite3_stmt *st = s_db.stmt_cal_occ_next;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, id_json, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, (int64_t)after);

   int result = 1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      row_to_occurrence(st, out);
      result = 0;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}
