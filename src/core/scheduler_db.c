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
 * Scheduler Database Layer - SQLite CRUD for scheduled_events table
 *
 * Accesses s_db directly (same pattern as auth_db_conv.c).
 * All functions acquire the auth_db mutex.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "core/scheduler_db.h"

#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * String Conversion Tables
 * ============================================================================= */

static const char *const event_type_strings[] = { "timer", "alarm", "reminder", "task",
                                                  "briefing" };
static const char *const status_strings[] = { "pending", "ringing", "fired",     "cancelled",
                                              "snoozed", "missed",  "dismissed", "timed_out" };
static const char *const recurrence_strings[] = { "once",     "daily",  "weekdays",
                                                  "weekends", "weekly", "custom" };

const char *sched_event_type_to_str(sched_event_type_t type) {
   if (type >= 0 && type <= SCHED_EVENT_BRIEFING)
      return event_type_strings[type];
   return "timer";
}

sched_event_type_t sched_event_type_from_str(const char *str) {
   if (!str)
      return SCHED_EVENT_TIMER;
   for (int i = 0; i <= SCHED_EVENT_BRIEFING; i++) {
      if (strcmp(str, event_type_strings[i]) == 0)
         return (sched_event_type_t)i;
   }
   return SCHED_EVENT_TIMER;
}

const char *sched_status_to_str(sched_status_t status) {
   if (status >= 0 && status <= SCHED_STATUS_TIMED_OUT)
      return status_strings[status];
   return "pending";
}

sched_status_t sched_status_from_str(const char *str) {
   if (!str)
      return SCHED_STATUS_PENDING;
   for (int i = 0; i <= SCHED_STATUS_TIMED_OUT; i++) {
      if (strcmp(str, status_strings[i]) == 0)
         return (sched_status_t)i;
   }
   return SCHED_STATUS_PENDING;
}

const char *sched_recurrence_to_str(sched_recurrence_t recurrence) {
   if (recurrence >= 0 && recurrence <= SCHED_RECUR_CUSTOM)
      return recurrence_strings[recurrence];
   return "once";
}

sched_recurrence_t sched_recurrence_from_str(const char *str) {
   if (!str)
      return SCHED_RECUR_ONCE;
   for (int i = 0; i <= SCHED_RECUR_CUSTOM; i++) {
      if (strcmp(str, recurrence_strings[i]) == 0)
         return (sched_recurrence_t)i;
   }
   return SCHED_RECUR_ONCE;
}

static const char *const source_type_strings[] = { "local", "webui", "dap2" };

const char *sched_source_type_to_str(sched_source_type_t type) {
   if (type >= 0 && type <= SCHED_SOURCE_DAP2)
      return source_type_strings[type];
   return "local";
}

sched_source_type_t sched_source_type_from_str(const char *str) {
   if (!str)
      return SCHED_SOURCE_LOCAL;
   for (int i = 0; i <= SCHED_SOURCE_DAP2; i++) {
      if (strcmp(str, source_type_strings[i]) == 0)
         return (sched_source_type_t)i;
   }
   return SCHED_SOURCE_LOCAL;
}

/* =============================================================================
 * Internal: Row extraction helper
 * ============================================================================= */

/* Read a TEXT column into a bounded char buffer with NUL-termination on
 * truncation.  NULL/empty column leaves dst unchanged (caller zero-inits
 * the struct so this means "stays empty").  Folded out of the prior
 * inline pattern that repeated 9 times in extract_event_row; the
 * explicit NUL-term closes a latent silent-overrun if SQLite ever
 * returned a string longer than dst_size-1. */
static void read_text_col(sqlite3_stmt *stmt, int idx, char *dst, size_t dst_size) {
   const char *src = (const char *)sqlite3_column_text(stmt, idx);
   if (!src || !src[0] || dst_size == 0) {
      return;
   }
   strncpy(dst, src, dst_size - 1);
   dst[dst_size - 1] = '\0';
}

static void extract_event_row(sqlite3_stmt *stmt, sched_event_t *event) {
   memset(event, 0, sizeof(*event));
   event->id = sqlite3_column_int64(stmt, 0);
   event->user_id = sqlite3_column_int(stmt, 1);

   const char *type_str = (const char *)sqlite3_column_text(stmt, 2);
   event->event_type = sched_event_type_from_str(type_str);

   const char *status_str = (const char *)sqlite3_column_text(stmt, 3);
   event->status = sched_status_from_str(status_str);

   read_text_col(stmt, 4, event->name, SCHED_NAME_MAX);
   read_text_col(stmt, 5, event->message, SCHED_MESSAGE_MAX);

   event->fire_at = (time_t)sqlite3_column_int64(stmt, 6);
   event->created_at = (time_t)sqlite3_column_int64(stmt, 7);
   event->duration_sec = sqlite3_column_int(stmt, 8);
   event->snoozed_until = (time_t)sqlite3_column_int64(stmt, 9);

   const char *recur_str = (const char *)sqlite3_column_text(stmt, 10);
   event->recurrence = sched_recurrence_from_str(recur_str);

   read_text_col(stmt, 11, event->recurrence_days, SCHED_RECURRENCE_DAYS_MAX);
   read_text_col(stmt, 12, event->original_time, SCHED_ORIGINAL_TIME_MAX);
   read_text_col(stmt, 13, event->source_uuid, SCHED_UUID_MAX);
   read_text_col(stmt, 14, event->source_location, SCHED_LOCATION_MAX);

   event->source_client_type = (sched_source_type_t)sqlite3_column_int(stmt, 15);
   event->announce_all = sqlite3_column_int(stmt, 16) != 0;

   read_text_col(stmt, 17, event->tool_name, SCHED_TOOL_NAME_MAX);
   read_text_col(stmt, 18, event->tool_action, SCHED_TOOL_NAME_MAX);
   read_text_col(stmt, 19, event->tool_value, SCHED_TOOL_VALUE_MAX);

   event->fired_at = (time_t)sqlite3_column_int64(stmt, 20);
   event->snooze_count = sqlite3_column_int(stmt, 21);
   /* v53: say_aloud is the 22nd column.  Implicit assumption — this read is
    * unconditional, so any DB at schema_version < 53 would fail the row
    * extraction.  The migration policy (auth_db_schema.c:2867-2891) gates
    * the v53 ALTER on `current_version >= 18 && < 53` and the fresh-install
    * SCHEMA_SQL path always creates the column.  So by the time
    * scheduler_db reads here, either the column exists (post-migration) or
    * the daemon refused to start (auth_db init aborts on a failed bump).
    * Do NOT add columns to extract_event_row without the same migration
    * guarantee. */
   event->say_aloud = (sched_say_aloud_t)sqlite3_column_int(stmt, 22);
   /* v54: deliver_to is the 23rd column.  Same migration-guarantee
    * argument as say_aloud. */
   read_text_col(stmt, 23, event->deliver_to, SCHED_DELIVER_TO_MAX);
   /* v84: briefing_instructions is the 24th column.  NULL (legacy row) reads as
    * empty → default summarization prompt. */
   read_text_col(stmt, 24, event->instructions, SCHED_INSTRUCTIONS_MAX);
}

/* Bind every column of the INSERT statement.  Both insert call sites
 * share this — the prepare + step + finalize stay at the call site
 * because lock semantics differ (insert_event_unlocked assumes caller
 * holds s_db.mutex; scheduler_db_insert_checked acquires it inline
 * after running its limit checks).  Position 20 (fired_at) and 21
 * (snooze_count) are pinned to 0 — this is by design for fresh inserts
 * (event hasn't fired yet, no snoozes). */
static void bind_event_columns(sqlite3_stmt *stmt, const sched_event_t *event) {
   sqlite3_bind_int(stmt, 1, event->user_id);
   sqlite3_bind_text(stmt, 2, sched_event_type_to_str(event->event_type), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, sched_status_to_str(event->status), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, event->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, event->message, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, (int64_t)event->fire_at);
   sqlite3_bind_int64(stmt, 7, (int64_t)event->created_at);
   sqlite3_bind_int(stmt, 8, event->duration_sec);
   sqlite3_bind_int64(stmt, 9, (int64_t)event->snoozed_until);
   sqlite3_bind_text(stmt, 10, sched_recurrence_to_str(event->recurrence), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 11, event->recurrence_days, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 12, event->original_time, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 13, event->source_uuid[0] ? event->source_uuid : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 14, event->source_location[0] ? event->source_location : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 15, (int)event->source_client_type);
   sqlite3_bind_int(stmt, 16, event->announce_all ? 1 : 0);
   sqlite3_bind_text(stmt, 17, event->tool_name[0] ? event->tool_name : NULL, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 18, event->tool_action[0] ? event->tool_action : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 19, event->tool_value[0] ? event->tool_value : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 20, 0);
   sqlite3_bind_int(stmt, 21, 0);
   sqlite3_bind_int(stmt, 22, (int)event->say_aloud);
   sqlite3_bind_text(stmt, 23, event->deliver_to[0] ? event->deliver_to : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 24, event->instructions[0] ? event->instructions : NULL, -1,
                     SQLITE_TRANSIENT);
}

/* Select all columns in consistent order */
#define SCHED_SELECT_COLS                                                      \
   "id, user_id, event_type, status, name, message, fire_at, created_at, "     \
   "duration_sec, snoozed_until, recurrence, recurrence_days, original_time, " \
   "source_uuid, source_location, source_client_type, announce_all, "          \
   "tool_name, tool_action, tool_value, fired_at, snooze_count, say_aloud, "   \
   "deliver_to, briefing_instructions"

/* =============================================================================
 * CRUD Operations
 * ============================================================================= */

/* Inserts the event row.  Caller holds s_db.mutex; no lock management here.
 * Used by both scheduler_db_insert (acquires/releases lock around it) and
 * scheduler_db_insert_with_step_clone (acquires lock once for the whole
 * transaction).  Sets event->created_at and event->id; populates id_out. */
static int insert_event_unlocked(sched_event_t *event, int64_t *id_out) {
   event->created_at = time(NULL);

   const char *sql = "INSERT INTO scheduled_events "
                     "(user_id, event_type, status, name, message, fire_at, created_at, "
                     "duration_sec, snoozed_until, recurrence, recurrence_days, original_time, "
                     "source_uuid, source_location, source_client_type, announce_all, "
                     "tool_name, tool_action, tool_value, fired_at, snooze_count, say_aloud, "
                     "deliver_to, briefing_instructions) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                     "?)";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("scheduler_db: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      return SCHED_DB_FAILURE;
   }

   bind_event_columns(stmt, event);

   rc = sqlite3_step(stmt);
   int result = SCHED_DB_FAILURE;
   if (rc == SQLITE_DONE) {
      int64_t id = sqlite3_last_insert_rowid(s_db.db);
      event->id = id;
      if (id_out)
         *id_out = id;
      result = SCHED_DB_SUCCESS;
   } else {
      OLOG_ERROR("scheduler_db: insert failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   return result;
}

int scheduler_db_insert(sched_event_t *event, int64_t *id_out) {
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);
   int result = insert_event_unlocked(event, id_out);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_insert_checked(sched_event_t *event,
                                int max_per_user,
                                int max_total,
                                int64_t *id_out) {
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   /* Check per-user limit under the same lock as insert */
   const char *count_user_sql = "SELECT COUNT(*) FROM scheduled_events "
                                "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing')";
   sqlite3_stmt *cnt_stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, count_user_sql, -1, &cnt_stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(cnt_stmt, 1, event->user_id);
      if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
         int user_count = sqlite3_column_int(cnt_stmt, 0);
         if (user_count >= max_per_user) {
            sqlite3_finalize(cnt_stmt);
            AUTH_DB_UNLOCK();
            return SCHED_DB_USER_LIMIT;
         }
      }
      sqlite3_finalize(cnt_stmt);
   }

   /* Check global limit */
   const char *count_total_sql = "SELECT COUNT(*) FROM scheduled_events "
                                 "WHERE status IN ('pending', 'snoozed', 'ringing')";
   cnt_stmt = NULL;
   rc = sqlite3_prepare_v2(s_db.db, count_total_sql, -1, &cnt_stmt, NULL);
   if (rc == SQLITE_OK) {
      if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
         int total_count = sqlite3_column_int(cnt_stmt, 0);
         if (total_count >= max_total) {
            sqlite3_finalize(cnt_stmt);
            AUTH_DB_UNLOCK();
            return SCHED_DB_GLOBAL_LIMIT;
         }
      }
      sqlite3_finalize(cnt_stmt);
   }

   /* Perform insert under the same lock */
   event->created_at = time(NULL);

   const char *sql = "INSERT INTO scheduled_events "
                     "(user_id, event_type, status, name, message, fire_at, created_at, "
                     "duration_sec, snoozed_until, recurrence, recurrence_days, original_time, "
                     "source_uuid, source_location, source_client_type, announce_all, "
                     "tool_name, tool_action, tool_value, fired_at, snooze_count, say_aloud, "
                     "deliver_to, briefing_instructions) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                     "?)";

   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("scheduler_db: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   bind_event_columns(stmt, event);

   rc = sqlite3_step(stmt);
   int result = SCHED_DB_FAILURE;
   if (rc == SQLITE_DONE) {
      int64_t id = sqlite3_last_insert_rowid(s_db.db);
      event->id = id;
      if (id_out)
         *id_out = id;
      result = SCHED_DB_SUCCESS;
   } else {
      OLOG_ERROR("scheduler_db: insert failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_get(int64_t id, sched_event_t *event) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   rc = sqlite3_step(stmt);

   int result = FAILURE;
   if (rc == SQLITE_ROW) {
      extract_event_row(stmt, event);
      result = SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_update_status(int64_t id, sched_status_t status) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "UPDATE scheduled_events SET status = ? WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_text(stmt, 1, sched_status_to_str(status), -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, id);

   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? SUCCESS : FAILURE;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_update_status_fired(int64_t id, sched_status_t status, time_t fired_at) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "UPDATE scheduled_events SET status = ?, fired_at = ? WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_text(stmt, 1, sched_status_to_str(status), -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, (int64_t)fired_at);
   sqlite3_bind_int64(stmt, 3, id);

   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? SUCCESS : FAILURE;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_snooze(int64_t id, time_t new_fire_at) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "UPDATE scheduled_events SET status = 'snoozed', fire_at = ?, "
                     "snoozed_until = ?, snooze_count = snooze_count + 1 "
                     "WHERE id = ? AND status IN ('ringing', 'pending', 'snoozed')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)new_fire_at);
   sqlite3_bind_int64(stmt, 2, (int64_t)new_fire_at);
   sqlite3_bind_int64(stmt, 3, id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? SUCCESS : FAILURE;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_cancel(int64_t id) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "UPDATE scheduled_events SET status = 'cancelled' "
                     "WHERE id = ? AND status IN ('pending', 'snoozed')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? SUCCESS : FAILURE;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_clear_missed(int64_t id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   /* Flips 'missed' rows to 'dismissed' so they stop surfacing in the panel
    * (the panel shows status IN pending/snoozed/ringing/missed).  The row is
    * preserved for cleanup_old_events history; just hidden from the queue.
    *
    * SQL predicate enforces user ownership at the data layer (defense in
    * depth — the dispatcher gate in webui_scheduler.c already authorizes
    * via authorize_event_for_user, but the DB-level predicate prevents any
    * future bypass of that gate from leaking across users). */
   const char *sql = "UPDATE scheduled_events SET status = 'dismissed' "
                     "WHERE id = ? AND user_id = ? AND status = 'missed'";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   sqlite3_bind_int(stmt, 2, user_id);
   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? SUCCESS : FAILURE;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_dismiss(int64_t id) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "UPDATE scheduled_events SET status = 'dismissed', fired_at = ? "
                     "WHERE id = ? AND status = 'ringing'";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int64(stmt, 2, id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? SUCCESS : FAILURE;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Query Operations
 * ============================================================================= */

time_t scheduler_db_next_fire_time(void) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT MIN(fire_at) FROM scheduled_events "
                     "WHERE status IN ('pending', 'snoozed')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   time_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      result = (time_t)sqlite3_column_int64(stmt, 0);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_get_due_events(sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE fire_at <= ? AND status IN ('pending', 'snoozed') "
                     "ORDER BY fire_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_list_user_events(int user_id, int type, sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql;
   if (type >= 0) {
      sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
            "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing') "
            "AND event_type = ? ORDER BY fire_at ASC LIMIT ?";
   } else {
      sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
            "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing') "
            "ORDER BY fire_at ASC LIMIT ?";
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   if (type >= 0) {
      sqlite3_bind_text(stmt, 2, sched_event_type_to_str((sched_event_type_t)type), -1,
                        SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, max_count);
   } else {
      sqlite3_bind_int(stmt, 2, max_count);
   }

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_list_user_missed(int user_id, sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   /* Separate helper from _list_user_events because the active queue and the
    * missed bucket are sorted differently — active ascends to "what fires
    * next", missed descends to "what just happened". */
   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE user_id = ? AND status = 'missed' "
                     "ORDER BY fire_at DESC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_find_by_name(int user_id, const char *name, sched_event_t *event) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE user_id = ? AND name = ? COLLATE NOCASE "
                     "AND status IN ('pending', 'snoozed', 'ringing') "
                     "ORDER BY created_at DESC LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      extract_event_row(stmt, event);
      result = SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_count_user_events(int user_id, int *count_out) {
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   const char *sql = "SELECT COUNT(*) FROM scheduled_events "
                     "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);

   int result = SCHED_DB_FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      if (count_out)
         *count_out = sqlite3_column_int(stmt, 0);
      result = SCHED_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_count_total_events(int *count_out) {
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   const char *sql = "SELECT COUNT(*) FROM scheduled_events "
                     "WHERE status IN ('pending', 'snoozed', 'ringing')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   int result = SCHED_DB_FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      if (count_out)
         *count_out = sqlite3_column_int(stmt, 0);
      result = SCHED_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_get_ringing(sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE status = 'ringing' ORDER BY fired_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int(stmt, 1, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_get_active_by_uuid(const char *uuid, sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE source_uuid = ? AND status IN ('pending', 'snoozed') "
                     "AND event_type = 'timer' ORDER BY fire_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

/* =============================================================================
 * Briefing Steps (schema v50+)
 * ============================================================================= */

/* Caller holds s_db.mutex.  Used inside transactions. */
static int briefing_steps_delete_unlocked(int64_t event_id) {
   const char *sql = "DELETE FROM briefing_steps WHERE event_id = ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return SCHED_DB_FAILURE;
   sqlite3_bind_int64(stmt, 1, event_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? SCHED_DB_SUCCESS : SCHED_DB_FAILURE;
}

/* Caller holds s_db.mutex.  Inserts steps for event_id in seq order starting
 * at 0.  Does NOT delete existing steps first — pair with the delete helper
 * inside a transaction when replacing. */
static int briefing_steps_insert_unlocked(int64_t event_id,
                                          const sched_briefing_step_t *steps,
                                          int step_count) {
   if (step_count <= 0)
      return SCHED_DB_SUCCESS;

   const char *sql = "INSERT INTO briefing_steps "
                     "(event_id, seq, tool_name, tool_action, tool_value) "
                     "VALUES (?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("scheduler_db: prepare briefing_steps insert failed: %s", sqlite3_errmsg(s_db.db));
      return SCHED_DB_FAILURE;
   }
   int result = SCHED_DB_SUCCESS;
   for (int i = 0; i < step_count; i++) {
      sqlite3_reset(stmt);
      sqlite3_bind_int64(stmt, 1, event_id);
      sqlite3_bind_int(stmt, 2, i);
      sqlite3_bind_text(stmt, 3, steps[i].tool_name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, steps[i].tool_action, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, steps[i].tool_value, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
         OLOG_ERROR("scheduler_db: briefing_steps insert step %d failed: %s", i,
                    sqlite3_errmsg(s_db.db));
         result = SCHED_DB_FAILURE;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return result;
}

int scheduler_db_briefing_steps_set(int64_t event_id,
                                    const sched_briefing_step_t *steps,
                                    int step_count) {
   if (step_count < 0 || step_count > SCHED_BRIEFING_STEPS_MAX)
      return SCHED_DB_FAILURE;
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   /* Transaction so set() is atomic — never observe a half-replaced step list. */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
   int rc = briefing_steps_delete_unlocked(event_id);
   if (rc == SCHED_DB_SUCCESS && step_count > 0)
      rc = briefing_steps_insert_unlocked(event_id, steps, step_count);
   if (rc == SCHED_DB_SUCCESS)
      sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   else
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);

   AUTH_DB_UNLOCK();
   return rc;
}

/* Caller holds s_db.mutex.  Reads steps for event_id (seq order) into out[],
 * up to SCHED_BRIEFING_STEPS_MAX.  Returns the count, or -1 on prepare error. */
static int briefing_steps_read_unlocked(int64_t event_id, sched_briefing_step_t *out) {
   const char *sql = "SELECT tool_name, COALESCE(tool_action,''), COALESCE(tool_value,'') "
                     "FROM briefing_steps WHERE event_id = ? ORDER BY seq ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, event_id);
   sqlite3_bind_int(stmt, 2, SCHED_BRIEFING_STEPS_MAX);
   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && n < SCHED_BRIEFING_STEPS_MAX) {
      memset(&out[n], 0, sizeof(out[n]));
      const unsigned char *tname = sqlite3_column_text(stmt, 0);
      const unsigned char *tact = sqlite3_column_text(stmt, 1);
      const unsigned char *tval = sqlite3_column_text(stmt, 2);
      if (tname)
         strncpy(out[n].tool_name, (const char *)tname, SCHED_TOOL_NAME_MAX - 1);
      if (tact)
         strncpy(out[n].tool_action, (const char *)tact, SCHED_TOOL_NAME_MAX - 1);
      if (tval)
         strncpy(out[n].tool_value, (const char *)tval, SCHED_TOOL_VALUE_MAX - 1);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int scheduler_db_briefing_steps_update(int64_t event_id,
                                       int user_id,
                                       const sched_briefing_step_t *steps,
                                       int step_count,
                                       bool append) {
   if (step_count < 0 || step_count > SCHED_BRIEFING_STEPS_MAX)
      return SCHED_DB_FAILURE;
   if (step_count > 0 && !steps)
      return SCHED_DB_FAILURE;
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* 1. Read the guard columns + legacy single-tool fields under the txn. */
   const char *sel_sql =
       "SELECT user_id, status, event_type, "
       "COALESCE(tool_name,''), COALESCE(tool_action,''), COALESCE(tool_value,'') "
       "FROM scheduled_events WHERE id = ?";
   sqlite3_stmt *sel = NULL;
   if (sqlite3_prepare_v2(s_db.db, sel_sql, -1, &sel, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }
   sqlite3_bind_int64(sel, 1, event_id);

   int guard_rc = SCHED_DB_NOT_EDITABLE;
   sched_briefing_step_t legacy_step;
   bool have_legacy = false;
   memset(&legacy_step, 0, sizeof(legacy_step));

   if (sqlite3_step(sel) == SQLITE_ROW) {
      int row_user = sqlite3_column_int(sel, 0);
      const char *status_str = (const char *)sqlite3_column_text(sel, 1);
      const char *type_str = (const char *)sqlite3_column_text(sel, 2);
      sched_status_t status = sched_status_from_str(status_str);
      sched_event_type_t etype = sched_event_type_from_str(type_str);
      bool editable = (status == SCHED_STATUS_PENDING || status == SCHED_STATUS_SNOOZED);
      if (row_user == user_id && etype == SCHED_EVENT_BRIEFING && editable) {
         guard_rc = SCHED_DB_SUCCESS;
         /* Capture the legacy single-tool fields for append materialization. */
         const unsigned char *ltn = sqlite3_column_text(sel, 3);
         if (ltn && ltn[0]) {
            strncpy(legacy_step.tool_name, (const char *)ltn, SCHED_TOOL_NAME_MAX - 1);
            const unsigned char *lta = sqlite3_column_text(sel, 4);
            const unsigned char *ltv = sqlite3_column_text(sel, 5);
            if (lta)
               strncpy(legacy_step.tool_action, (const char *)lta, SCHED_TOOL_NAME_MAX - 1);
            if (ltv)
               strncpy(legacy_step.tool_value, (const char *)ltv, SCHED_TOOL_VALUE_MAX - 1);
            have_legacy = true;
         }
      }
   }
   sqlite3_finalize(sel);

   if (guard_rc != SCHED_DB_SUCCESS) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return guard_rc;
   }

   /* 2. Build the final step list. */
   sched_briefing_step_t final[SCHED_BRIEFING_STEPS_MAX];
   int final_count = 0;

   if (append) {
      int existing = briefing_steps_read_unlocked(event_id, final);
      if (existing < 0) {
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return SCHED_DB_FAILURE;
      }
      final_count = existing;
      /* Materialize a legacy single-tool briefing as step[0] so appending does
       * not silently orphan the original tool (steps table wins at fire time). */
      if (final_count == 0 && have_legacy) {
         final[0] = legacy_step;
         final_count = 1;
      }
      for (int i = 0; i < step_count; i++) {
         /* Idempotent retry: skip an exact duplicate of an existing step. */
         bool dup = false;
         for (int j = 0; j < final_count; j++) {
            if (strcmp(final[j].tool_name, steps[i].tool_name) == 0 &&
                strcmp(final[j].tool_action, steps[i].tool_action) == 0 &&
                strcmp(final[j].tool_value, steps[i].tool_value) == 0) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         if (final_count >= SCHED_BRIEFING_STEPS_MAX) {
            sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
            AUTH_DB_UNLOCK();
            return SCHED_DB_FAILURE; /* would exceed the step cap */
         }
         final[final_count++] = steps[i];
      }
   } else {
      final_count = step_count;
      for (int i = 0; i < step_count; i++)
         final[i] = steps[i];
   }

   /* 3. Replace the stored steps atomically (delete + insert in this txn). */
   int rc = briefing_steps_delete_unlocked(event_id);
   if (rc == SCHED_DB_SUCCESS && final_count > 0)
      rc = briefing_steps_insert_unlocked(event_id, final, final_count);

   if (rc == SCHED_DB_SUCCESS)
      sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   else
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);

   AUTH_DB_UNLOCK();
   return rc;
}

int scheduler_db_update_fields(int64_t id,
                               int user_id,
                               const sched_event_t *fields,
                               uint32_t field_mask) {
   if (!fields || field_mask == 0)
      return SCHED_DB_FAILURE;
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   /* Build the SET clause from the masked columns.  The bind loop below MUST
    * visit the same columns in the same order. */
   char sql[512];
   int off = snprintf(sql, sizeof(sql), "UPDATE scheduled_events SET ");
   bool first = true;
#define SCHED_SET_COL(bit, col)                                                               \
   do {                                                                                       \
      if (field_mask & (bit)) {                                                               \
         off += snprintf(sql + off, sizeof(sql) - off, "%s%s = ?", first ? "" : ", ", (col)); \
         first = false;                                                                       \
      }                                                                                       \
   } while (0)
   SCHED_SET_COL(SCHED_FIELD_NAME, "name");
   SCHED_SET_COL(SCHED_FIELD_MESSAGE, "message");
   SCHED_SET_COL(SCHED_FIELD_FIRE_AT, "fire_at");
   SCHED_SET_COL(SCHED_FIELD_ORIGINAL_TIME, "original_time");
   SCHED_SET_COL(SCHED_FIELD_RECURRENCE, "recurrence");
   SCHED_SET_COL(SCHED_FIELD_RECURRENCE_DAYS, "recurrence_days");
   SCHED_SET_COL(SCHED_FIELD_DELIVER_TO, "deliver_to");
   SCHED_SET_COL(SCHED_FIELD_SAY_ALOUD, "say_aloud");
   SCHED_SET_COL(SCHED_FIELD_INSTRUCTIONS, "briefing_instructions");
#undef SCHED_SET_COL
   off += snprintf(sql + off, sizeof(sql) - off,
                   " WHERE id = ? AND user_id = ? AND status IN ('pending', 'snoozed')");
   /* Guard the snprintf accumulator: unreachable at the current 9 columns
    * (~245 bytes < 512), but keeps a future column addition from wrapping
    * `sizeof(sql) - off` to a huge size_t on the next append. */
   if (off < 0 || (size_t)off >= sizeof(sql)) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   int idx = 1;
   if (field_mask & SCHED_FIELD_NAME)
      sqlite3_bind_text(stmt, idx++, fields->name, -1, SQLITE_TRANSIENT);
   if (field_mask & SCHED_FIELD_MESSAGE)
      sqlite3_bind_text(stmt, idx++, fields->message, -1, SQLITE_TRANSIENT);
   if (field_mask & SCHED_FIELD_FIRE_AT)
      sqlite3_bind_int64(stmt, idx++, (sqlite3_int64)fields->fire_at);
   if (field_mask & SCHED_FIELD_ORIGINAL_TIME)
      sqlite3_bind_text(stmt, idx++, fields->original_time, -1, SQLITE_TRANSIENT);
   if (field_mask & SCHED_FIELD_RECURRENCE)
      sqlite3_bind_text(stmt, idx++, sched_recurrence_to_str(fields->recurrence), -1,
                        SQLITE_STATIC);
   if (field_mask & SCHED_FIELD_RECURRENCE_DAYS)
      sqlite3_bind_text(stmt, idx++, fields->recurrence_days, -1, SQLITE_TRANSIENT);
   if (field_mask & SCHED_FIELD_DELIVER_TO)
      sqlite3_bind_text(stmt, idx++, fields->deliver_to, -1, SQLITE_TRANSIENT);
   if (field_mask & SCHED_FIELD_SAY_ALOUD)
      sqlite3_bind_int(stmt, idx++, (int)fields->say_aloud);
   if (field_mask & SCHED_FIELD_INSTRUCTIONS)
      /* Bind NULL when cleared so the column stays canonical (NULL = unset). */
      sqlite3_bind_text(stmt, idx++, fields->instructions[0] ? fields->instructions : NULL, -1,
                        SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, idx++, id);
   sqlite3_bind_int(stmt, idx++, user_id);

   int sq = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   int rc = SCHED_DB_FAILURE;
   if (sq == SQLITE_DONE)
      rc = (changes > 0) ? SCHED_DB_SUCCESS : SCHED_DB_NOT_EDITABLE;

   AUTH_DB_UNLOCK();
   return rc;
}

int scheduler_db_briefing_steps_list(int64_t event_id,
                                     sched_briefing_step_t *out,
                                     int max_count,
                                     int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max_count <= 0)
      return SCHED_DB_FAILURE;
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   const char *sql = "SELECT tool_name, COALESCE(tool_action,''), COALESCE(tool_value,'') "
                     "FROM briefing_steps WHERE event_id = ? ORDER BY seq ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, event_id);
   sqlite3_bind_int(stmt, 2, max_count);

   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && n < max_count) {
      memset(&out[n], 0, sizeof(out[n]));
      const unsigned char *tname = sqlite3_column_text(stmt, 0);
      const unsigned char *tact = sqlite3_column_text(stmt, 1);
      const unsigned char *tval = sqlite3_column_text(stmt, 2);
      if (tname)
         strncpy(out[n].tool_name, (const char *)tname, SCHED_TOOL_NAME_MAX - 1);
      if (tact)
         strncpy(out[n].tool_action, (const char *)tact, SCHED_TOOL_NAME_MAX - 1);
      if (tval)
         strncpy(out[n].tool_value, (const char *)tval, SCHED_TOOL_VALUE_MAX - 1);
      n++;
   }
   sqlite3_finalize(stmt);

   if (count_out)
      *count_out = n;
   AUTH_DB_UNLOCK();
   return SCHED_DB_SUCCESS;
}

int scheduler_db_insert_with_step_clone(sched_event_t *next,
                                        int64_t src_event_id,
                                        int64_t *new_id_out) {
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   /* Read source steps under the same lock acquisition so we have a
    * consistent snapshot to clone from. */
   sched_briefing_step_t steps[SCHED_BRIEFING_STEPS_MAX];
   int step_count = 0;
   const char *sel_sql = "SELECT tool_name, COALESCE(tool_action,''), COALESCE(tool_value,'') "
                         "FROM briefing_steps WHERE event_id = ? ORDER BY seq ASC LIMIT ?";
   sqlite3_stmt *sel = NULL;
   if (sqlite3_prepare_v2(s_db.db, sel_sql, -1, &sel, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(sel, 1, src_event_id);
      sqlite3_bind_int(sel, 2, SCHED_BRIEFING_STEPS_MAX);
      while (sqlite3_step(sel) == SQLITE_ROW && step_count < SCHED_BRIEFING_STEPS_MAX) {
         memset(&steps[step_count], 0, sizeof(steps[step_count]));
         const unsigned char *tname = sqlite3_column_text(sel, 0);
         const unsigned char *tact = sqlite3_column_text(sel, 1);
         const unsigned char *tval = sqlite3_column_text(sel, 2);
         if (tname)
            strncpy(steps[step_count].tool_name, (const char *)tname, SCHED_TOOL_NAME_MAX - 1);
         if (tact)
            strncpy(steps[step_count].tool_action, (const char *)tact, SCHED_TOOL_NAME_MAX - 1);
         if (tval)
            strncpy(steps[step_count].tool_value, (const char *)tval, SCHED_TOOL_VALUE_MAX - 1);
         step_count++;
      }
   }
   sqlite3_finalize(sel);

   /* Single transaction wrapping insert + step clone so a clone failure
    * rolls back the new pending row — never leaves a zero-step briefing. */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
   int rc = insert_event_unlocked(next, new_id_out);
   if (rc == SCHED_DB_SUCCESS && step_count > 0) {
      int64_t new_id = new_id_out ? *new_id_out : next->id;
      rc = briefing_steps_insert_unlocked(new_id, steps, step_count);
   }
   if (rc == SCHED_DB_SUCCESS)
      sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   else
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);

   AUTH_DB_UNLOCK();
   return rc;
}

int scheduler_db_cancel_and_insert_next(int64_t cancel_id,
                                        sched_event_t *next,
                                        int64_t src_event_id,
                                        bool clone_steps,
                                        int64_t *new_id_out) {
   if (!next)
      return SCHED_DB_FAILURE;
   if (new_id_out)
      *new_id_out = 0;

   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   /* Single transaction wrapping (optional) source-steps SELECT + cancel +
    * insert + (optional) step clone.  Pulling the SELECT inside BEGIN
    * IMMEDIATE makes the clone read consistent with the rest of the
    * operation — a concurrent briefing_steps_set on src_event_id can't
    * race against the snapshot we're cloning from.  If the cancel UPDATE
    * matches 0 rows (row already terminal) we ROLLBACK without inserting,
    * avoiding a phantom successor for a row another actor (fire path,
    * prior cancel) already handled. */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   sched_briefing_step_t steps[SCHED_BRIEFING_STEPS_MAX];
   int step_count = 0;
   if (clone_steps) {
      const char *sel_sql = "SELECT tool_name, COALESCE(tool_action,''), COALESCE(tool_value,'') "
                            "FROM briefing_steps WHERE event_id = ? ORDER BY seq ASC LIMIT ?";
      sqlite3_stmt *sel = NULL;
      if (sqlite3_prepare_v2(s_db.db, sel_sql, -1, &sel, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(sel, 1, src_event_id);
         sqlite3_bind_int(sel, 2, SCHED_BRIEFING_STEPS_MAX);
         while (sqlite3_step(sel) == SQLITE_ROW && step_count < SCHED_BRIEFING_STEPS_MAX) {
            memset(&steps[step_count], 0, sizeof(steps[step_count]));
            const unsigned char *tname = sqlite3_column_text(sel, 0);
            const unsigned char *tact = sqlite3_column_text(sel, 1);
            const unsigned char *tval = sqlite3_column_text(sel, 2);
            if (tname)
               strncpy(steps[step_count].tool_name, (const char *)tname, SCHED_TOOL_NAME_MAX - 1);
            if (tact)
               strncpy(steps[step_count].tool_action, (const char *)tact, SCHED_TOOL_NAME_MAX - 1);
            if (tval)
               strncpy(steps[step_count].tool_value, (const char *)tval, SCHED_TOOL_VALUE_MAX - 1);
            step_count++;
         }
      }
      sqlite3_finalize(sel);
   }

   const char *cancel_sql = "UPDATE scheduled_events SET status = 'cancelled' "
                            "WHERE id = ? AND status IN ('pending', 'snoozed')";
   sqlite3_stmt *cstmt = NULL;
   int rc = SCHED_DB_FAILURE;
   if (sqlite3_prepare_v2(s_db.db, cancel_sql, -1, &cstmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(cstmt, 1, cancel_id);
      int sq = sqlite3_step(cstmt);
      int changes = sqlite3_changes(s_db.db);
      sqlite3_finalize(cstmt);
      if (sq == SQLITE_DONE && changes > 0) {
         rc = insert_event_unlocked(next, new_id_out);
         if (rc == SCHED_DB_SUCCESS && clone_steps && step_count > 0) {
            int64_t new_id = new_id_out ? *new_id_out : next->id;
            rc = briefing_steps_insert_unlocked(new_id, steps, step_count);
         }
      }
   }

   if (rc == SCHED_DB_SUCCESS) {
      sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   } else {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      if (new_id_out)
         *new_id_out = 0;
   }

   AUTH_DB_UNLOCK();
   return rc;
}

int scheduler_db_briefing_steps_list_many(const int64_t *event_ids,
                                          int n_events,
                                          sched_briefing_step_t *out_steps,
                                          int *out_counts) {
   if (!event_ids || !out_steps || !out_counts || n_events <= 0)
      return SCHED_DB_FAILURE;
   for (int i = 0; i < n_events; i++)
      out_counts[i] = 0;

   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   /* Single lock acquisition + one prepared statement reused across all
    * event_ids.  Eliminates the N+1 auth_db_mutex cycles the per-event
    * caller path (handle_list / handle_scheduler_list_events) used to
    * incur — at SCHED_MAX_RESULTS=50 that's 1 lock instead of 50. */
   const char *sql = "SELECT tool_name, COALESCE(tool_action,''), COALESCE(tool_value,'') "
                     "FROM briefing_steps WHERE event_id = ? ORDER BY seq ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   for (int i = 0; i < n_events; i++) {
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
      sqlite3_bind_int64(stmt, 1, event_ids[i]);
      sqlite3_bind_int(stmt, 2, SCHED_BRIEFING_STEPS_MAX);

      sched_briefing_step_t *base = &out_steps[(size_t)i * SCHED_BRIEFING_STEPS_MAX];
      int n = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW && n < SCHED_BRIEFING_STEPS_MAX) {
         memset(&base[n], 0, sizeof(base[n]));
         const unsigned char *tname = sqlite3_column_text(stmt, 0);
         const unsigned char *tact = sqlite3_column_text(stmt, 1);
         const unsigned char *tval = sqlite3_column_text(stmt, 2);
         if (tname)
            strncpy(base[n].tool_name, (const char *)tname, SCHED_TOOL_NAME_MAX - 1);
         if (tact)
            strncpy(base[n].tool_action, (const char *)tact, SCHED_TOOL_NAME_MAX - 1);
         if (tval)
            strncpy(base[n].tool_value, (const char *)tval, SCHED_TOOL_VALUE_MAX - 1);
         n++;
      }
      out_counts[i] = n;
   }
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();
   return SCHED_DB_SUCCESS;
}

int scheduler_db_cleanup_old_events(int retention_days, int *deleted_out) {
   AUTH_DB_LOCK_OR_RETURN(SCHED_DB_FAILURE);

   time_t cutoff = time(NULL) - (time_t)retention_days * 86400;

   const char *sql = "DELETE FROM scheduled_events "
                     "WHERE status IN ('fired', 'cancelled', 'missed', 'dismissed', 'timed_out') "
                     "AND ((fired_at > 0 AND fired_at < ?) OR "
                     "(fired_at = 0 AND created_at < ?))";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return SCHED_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)cutoff);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   rc = sqlite3_step(stmt);
   int result = SCHED_DB_FAILURE;
   if (rc == SQLITE_DONE) {
      if (deleted_out)
         *deleted_out = sqlite3_changes(s_db.db);
      result = SCHED_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_get_missed_events(sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE fire_at < ? AND status IN ('pending', 'snoozed') "
                     "ORDER BY fire_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}
