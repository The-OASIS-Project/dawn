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
 * Authentication Database - Rate Limiting Module
 *
 * Handles IP-based rate limiting for login attempts:
 * - Counting recent failed attempts per IP
 * - Logging login attempts (success/failure)
 * - Clearing attempt history (for admin unblocking)
 * - Listing blocked IPs
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Rate Limiting
 * ============================================================================= */

int auth_db_count_recent_failures(const char *ip_address, time_t since, int *count_out) {
   if (!ip_address || !count_out) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_count_recent_failures);
   sqlite3_bind_text(s_db.stmt_count_recent_failures, 1, ip_address, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_count_recent_failures, 2, (int64_t)since);

   int rc = sqlite3_step(s_db.stmt_count_recent_failures);
   if (rc == SQLITE_ROW) {
      *count_out = sqlite3_column_int(s_db.stmt_count_recent_failures, 0);
   } else {
      sqlite3_reset(s_db.stmt_count_recent_failures);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_count_recent_failures);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_log_attempt(const char *ip_address, const char *username, bool success) {
   if (!ip_address) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_log_attempt);
   sqlite3_bind_text(s_db.stmt_log_attempt, 1, ip_address, -1, SQLITE_STATIC);

   if (username) {
      sqlite3_bind_text(s_db.stmt_log_attempt, 2, username, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_attempt, 2);
   }

   sqlite3_bind_int64(s_db.stmt_log_attempt, 3, (int64_t)time(NULL));
   sqlite3_bind_int(s_db.stmt_log_attempt, 4, success ? 1 : 0);

   int rc = sqlite3_step(s_db.stmt_log_attempt);
   sqlite3_reset(s_db.stmt_log_attempt);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_clear_login_attempts(const char *ip_address, int *deleted_out) {
   AUTH_DB_LOCK_OR_FAIL();

   int deleted = 0;
   sqlite3_stmt *stmt = NULL;
   int rc;

   if (ip_address) {
      /* Delete attempts for specific IP */
      rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts WHERE ip_address = ?", -1, &stmt,
                              NULL);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: prepare clear_login_attempts failed: %s", sqlite3_errmsg(s_db.db));
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }
      sqlite3_bind_text(stmt, 1, ip_address, -1, SQLITE_STATIC);
   } else {
      /* Delete all attempts */
      rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts", -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: prepare clear_all_login_attempts failed: %s",
                    sqlite3_errmsg(s_db.db));
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }
   }

   rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE) {
      deleted = sqlite3_changes(s_db.db);
   }
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   OLOG_INFO("auth_db: Cleared %d login attempts for IP: %s", deleted,
             ip_address ? ip_address : "all");

   if (deleted_out) {
      *deleted_out = deleted;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_list_blocked_ips(time_t since, auth_ip_status_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Query IPs with failed attempts, grouped by IP, ordered by attempt count descending */
   const char *sql = "SELECT ip_address, COUNT(*) as attempt_count, MAX(timestamp) as last_attempt "
                     "FROM login_attempts "
                     "WHERE success = 0 AND timestamp > ? "
                     "GROUP BY ip_address "
                     "ORDER BY attempt_count DESC "
                     "LIMIT 100";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare list_blocked_ips failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)since);

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_ip_status_t status = { 0 };

      const char *ip = (const char *)sqlite3_column_text(stmt, 0);
      if (ip) {
         safe_strscpy(status.ip_address, ip);
      }
      status.failed_attempts = sqlite3_column_int(stmt, 1);
      status.last_attempt = (time_t)sqlite3_column_int64(stmt, 2);

      /* Call callback with mutex still held - callback should be quick */
      if (callback(&status, ctx) != 0) {
         break;
      }
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}
