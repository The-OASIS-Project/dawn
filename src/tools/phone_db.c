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
 * Phone Database — SQLite CRUD for call and SMS logs.
 * Uses the shared auth_db handle (Pattern A, same as calendar_db.c).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/phone_db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Helper: normalize a phone number for DB matching
 *
 * LLMs emit phone numbers in many formats ("+1-555-123-4567", "(555) 123-4567",
 * "1 555 123 4567"). Stored rows are E.164 without punctuation ("+15551234567").
 * Strip whitespace/dashes/parens, keep digits and an optional leading '+',
 * normalize a bare 10-digit US number to +1 prefix.
 * ============================================================================= */

void phone_number_format_for_tts(const char *in, char *out, size_t out_size) {
   if (!out || out_size == 0)
      return;
   out[0] = '\0';
   if (!in)
      return;

   size_t pos = 0;
   for (const char *p = in; *p && pos + 2 < out_size; p++) {
      if (isdigit((unsigned char)*p)) {
         if (pos > 0)
            out[pos++] = ' ';
         out[pos++] = *p;
      }
   }
   out[pos] = '\0';
}

void phone_number_redact(const char *in, char *out, size_t out_size) {
   if (!out || out_size == 0)
      return;
   out[0] = '\0';
   if (!in || !*in) {
      snprintf(out, out_size, "(none)");
      return;
   }
   size_t len = strlen(in);
   if (len <= 4) {
      snprintf(out, out_size, "%s", in);
      return;
   }
   snprintf(out, out_size, "...%s", in + len - 4);
}

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

static void row_to_call_log(sqlite3_stmt *stmt, phone_call_log_t *entry) {
   entry->id = sqlite3_column_int64(stmt, 0);
   entry->user_id = sqlite3_column_int(stmt, 1);
   entry->direction = sqlite3_column_int(stmt, 2);
   col_str(entry->number, sizeof(entry->number), stmt, 3);
   col_str(entry->contact_name, sizeof(entry->contact_name), stmt, 4);
   entry->duration_sec = sqlite3_column_int(stmt, 5);
   entry->timestamp = (time_t)sqlite3_column_int64(stmt, 6);
   entry->status = sqlite3_column_int(stmt, 7);
}

static void row_to_sms_log(sqlite3_stmt *stmt, phone_sms_log_t *entry) {
   entry->id = sqlite3_column_int64(stmt, 0);
   entry->user_id = sqlite3_column_int(stmt, 1);
   entry->direction = sqlite3_column_int(stmt, 2);
   col_str(entry->number, sizeof(entry->number), stmt, 3);
   col_str(entry->contact_name, sizeof(entry->contact_name), stmt, 4);
   col_str(entry->body, sizeof(entry->body), stmt, 5);
   entry->timestamp = (time_t)sqlite3_column_int64(stmt, 6);
   entry->read = sqlite3_column_int(stmt, 7);
}

/* =============================================================================
 * Call Log
 * ============================================================================= */

int phone_db_call_log_insert(int user_id,
                             int direction,
                             const char *number,
                             const char *contact_name,
                             int duration_sec,
                             time_t timestamp,
                             int status,
                             int64_t *id_out) {
   if (!s_db.initialized) {
      return PHONE_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "INSERT INTO phone_call_log "
                     "(user_id, direction, number, contact_name, duration_sec, timestamp, status) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call_log insert prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, direction);
   sqlite3_bind_text(stmt, 3, number ? number : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, contact_name ? contact_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, duration_sec);
   sqlite3_bind_int64(stmt, 6, (int64_t)timestamp);
   sqlite3_bind_int(stmt, 7, status);

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("phone_db: call_log insert failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   if (id_out) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return PHONE_DB_SUCCESS;
}

int phone_db_call_log_update(int64_t id, int duration_sec, int status) {
   if (!s_db.initialized) {
      return 1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "UPDATE phone_call_log SET duration_sec = ?, status = ? WHERE id = ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call_log update prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return 1;
   }

   sqlite3_bind_int(stmt, 1, duration_sec);
   sqlite3_bind_int(stmt, 2, status);
   sqlite3_bind_int64(stmt, 3, id);

   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? 0 : 1;
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("phone_db: call_log update failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_call_log_recent(int user_id, phone_call_log_t *out, int max, int *count_out) {
   if (!s_db.initialized || !out || max <= 0 || !count_out) {
      return PHONE_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "SELECT id, user_id, direction, number, contact_name, duration_sec, "
                     "timestamp, status FROM phone_call_log "
                     "WHERE user_id = ? ORDER BY timestamp DESC LIMIT ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call_log query prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
      row_to_call_log(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   *count_out = count;
   return PHONE_DB_SUCCESS;
}

/* =============================================================================
 * SMS Log
 * ============================================================================= */

int phone_db_sms_log_insert(int user_id,
                            int direction,
                            const char *number,
                            const char *contact_name,
                            const char *body,
                            time_t timestamp,
                            int64_t *id_out) {
   if (!s_db.initialized) {
      return PHONE_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "INSERT INTO phone_sms_log "
                     "(user_id, direction, number, contact_name, body, timestamp, read) "
                     "VALUES (?, ?, ?, ?, ?, ?, 0)";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms_log insert prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, direction);
   sqlite3_bind_text(stmt, 3, number ? number : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, contact_name ? contact_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, body ? body : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, (int64_t)timestamp);

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("phone_db: sms_log insert failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   if (id_out) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return PHONE_DB_SUCCESS;
}

int phone_db_sms_get_unread(int user_id, phone_sms_log_t *out, int max, int *count_out) {
   if (!s_db.initialized || !out || max <= 0 || !count_out) {
      return PHONE_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   /* Only unread INBOUND messages — outbound (things the user sent) is not
    * something they need to "read" again. */
   const char *sql = "SELECT id, user_id, direction, number, contact_name, body, "
                     "timestamp, read FROM phone_sms_log "
                     "WHERE user_id = ? AND read = 0 AND direction = 1 "
                     "ORDER BY timestamp DESC LIMIT ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms unread query failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
      row_to_sms_log(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   *count_out = count;
   return PHONE_DB_SUCCESS;
}

int phone_db_sms_log_recent(int user_id, phone_sms_log_t *out, int max, int *count_out) {
   if (!s_db.initialized || !out || max <= 0 || !count_out) {
      return PHONE_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "SELECT id, user_id, direction, number, contact_name, body, "
                     "timestamp, read FROM phone_sms_log "
                     "WHERE user_id = ? ORDER BY timestamp DESC LIMIT ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms recent query failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
      row_to_sms_log(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   *count_out = count;
   return PHONE_DB_SUCCESS;
}

int phone_db_sms_mark_read(int64_t id) {
   if (!s_db.initialized) {
      return 1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "UPDATE phone_sms_log SET read = 1 WHERE id = ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return 1;
   }

   sqlite3_bind_int64(stmt, 1, id);
   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? 0 : 1;

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_cleanup(int call_retention_days, int sms_retention_days) {
   if (!s_db.initialized) {
      return 1;
   }

   pthread_mutex_lock(&s_db.mutex);

   time_t now = time(NULL);
   int result = 0;

   if (call_retention_days > 0) {
      time_t call_cutoff = now - (time_t)call_retention_days * 86400;
      sqlite3_stmt *stmt;
      int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM phone_call_log WHERE timestamp < ?", -1,
                                  &stmt, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, (int64_t)call_cutoff);
         if (sqlite3_step(stmt) != SQLITE_DONE) {
            OLOG_ERROR("phone_db: call cleanup failed: %s", sqlite3_errmsg(s_db.db));
            result = 1;
         }
         sqlite3_finalize(stmt);
      }
   }

   if (sms_retention_days > 0) {
      time_t sms_cutoff = now - (time_t)sms_retention_days * 86400;
      sqlite3_stmt *stmt;
      int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM phone_sms_log WHERE timestamp < ?", -1,
                                  &stmt, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, (int64_t)sms_cutoff);
         if (sqlite3_step(stmt) != SQLITE_DONE) {
            OLOG_ERROR("phone_db: sms cleanup failed: %s", sqlite3_errmsg(s_db.db));
            result = 1;
         }
         sqlite3_finalize(stmt);
      }
   }

   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

/* =============================================================================
 * Delete operations (user-scoped, for the LLM deletion tool)
 * ============================================================================= */

int phone_db_sms_log_delete(int user_id, int64_t id) {
   if (!s_db.initialized)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM phone_sms_log WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms delete prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result;
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_ERROR("phone_db: sms delete failed: %s", sqlite3_errmsg(s_db.db));
      result = PHONE_DB_FAILURE;
   } else {
      result = (sqlite3_changes(s_db.db) > 0) ? PHONE_DB_SUCCESS : PHONE_DB_NOT_FOUND;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_sms_log_delete_by_number(int user_id, const char *number, int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!s_db.initialized || !number)
      return PHONE_DB_FAILURE;

   char normalized[32];
   phone_number_normalize(number, normalized, sizeof(normalized));
   if (normalized[0] == '\0')
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM phone_sms_log WHERE user_id = ? AND number = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms delete_by_number prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, normalized, -1, SQLITE_TRANSIENT);

   int result;
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_ERROR("phone_db: sms delete_by_number failed: %s", sqlite3_errmsg(s_db.db));
      result = PHONE_DB_FAILURE;
   } else {
      if (out_count)
         *out_count = sqlite3_changes(s_db.db);
      result = PHONE_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_sms_log_count_by_number(int user_id, const char *number, int *out_count) {
   if (!out_count)
      return PHONE_DB_FAILURE;
   *out_count = 0;
   if (!s_db.initialized || !number)
      return PHONE_DB_FAILURE;

   char normalized[32];
   phone_number_normalize(number, normalized, sizeof(normalized));
   if (normalized[0] == '\0')
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(
       s_db.db, "SELECT COUNT(*) FROM phone_sms_log WHERE user_id = ? AND number = ?", -1, &stmt,
       NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, normalized, -1, SQLITE_TRANSIENT);

   int result = PHONE_DB_FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *out_count = sqlite3_column_int(stmt, 0);
      result = PHONE_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_call_log_delete(int user_id, int64_t id) {
   if (!s_db.initialized)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM phone_call_log WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call delete prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result;
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_ERROR("phone_db: call delete failed: %s", sqlite3_errmsg(s_db.db));
      result = PHONE_DB_FAILURE;
   } else {
      result = (sqlite3_changes(s_db.db) > 0) ? PHONE_DB_SUCCESS : PHONE_DB_NOT_FOUND;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_call_log_delete_older_than(int user_id, time_t cutoff, int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!s_db.initialized)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM phone_call_log WHERE user_id = ? AND timestamp < ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call delete_older_than prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int result;
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_ERROR("phone_db: call delete_older_than failed: %s", sqlite3_errmsg(s_db.db));
      result = PHONE_DB_FAILURE;
   } else {
      if (out_count)
         *out_count = sqlite3_changes(s_db.db);
      result = PHONE_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_sms_log_get_by_id(int user_id, int64_t id, phone_sms_log_t *out) {
   if (!s_db.initialized || !out)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, user_id, direction, number, contact_name, body, "
                               "timestamp, read FROM phone_sms_log "
                               "WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_sms_log(stmt, out);
      result = PHONE_DB_SUCCESS;
   } else {
      result = PHONE_DB_NOT_FOUND;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_sms_log_delete_older_than(int user_id, time_t cutoff, int *out_count) {
   if (out_count)
      *out_count = 0;
   if (!s_db.initialized)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM phone_sms_log WHERE user_id = ? AND timestamp < ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms delete_older_than prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int result;
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_ERROR("phone_db: sms delete_older_than failed: %s", sqlite3_errmsg(s_db.db));
      result = PHONE_DB_FAILURE;
   } else {
      if (out_count)
         *out_count = sqlite3_changes(s_db.db);
      result = PHONE_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_sms_log_count_older_than(int user_id, time_t cutoff, int *out_count) {
   if (!out_count)
      return PHONE_DB_FAILURE;
   *out_count = 0;
   if (!s_db.initialized)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(
       s_db.db, "SELECT COUNT(*) FROM phone_sms_log WHERE user_id = ? AND timestamp < ?", -1, &stmt,
       NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int result = PHONE_DB_FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *out_count = sqlite3_column_int(stmt, 0);
      result = PHONE_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_call_log_get_by_id(int user_id, int64_t id, phone_call_log_t *out) {
   if (!s_db.initialized || !out)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, user_id, direction, number, contact_name, "
                               "duration_sec, timestamp, status FROM phone_call_log "
                               "WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_call_log(stmt, out);
      result = PHONE_DB_SUCCESS;
   } else {
      result = PHONE_DB_NOT_FOUND;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_call_log_count_older_than(int user_id, time_t cutoff, int *out_count) {
   if (!out_count)
      return PHONE_DB_FAILURE;
   *out_count = 0;
   if (!s_db.initialized)
      return PHONE_DB_FAILURE;

   pthread_mutex_lock(&s_db.mutex);

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(
       s_db.db, "SELECT COUNT(*) FROM phone_call_log WHERE user_id = ? AND timestamp < ?", -1,
       &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return PHONE_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int result = PHONE_DB_FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *out_count = sqlite3_column_int(stmt, 0);
      result = PHONE_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}
