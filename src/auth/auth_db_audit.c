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
 * Authentication Database - Audit Logging Module
 *
 * Handles security audit logging:
 * - Recording security events (login, logout, password change, etc.)
 * - Querying audit log with filters
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Audit Logging
 * ============================================================================= */

void auth_db_log_event(const char *event,
                       const char *username,
                       const char *ip_address,
                       const char *details) {
   if (!event) {
      return;
   }

   AUTH_DB_LOCK_OR_RETURN_VOID();

   sqlite3_reset(s_db.stmt_log_event);
   sqlite3_bind_int64(s_db.stmt_log_event, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_log_event, 2, event, -1, SQLITE_STATIC);

   if (username) {
      sqlite3_bind_text(s_db.stmt_log_event, 3, username, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_event, 3);
   }

   if (ip_address) {
      sqlite3_bind_text(s_db.stmt_log_event, 4, ip_address, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_event, 4);
   }

   if (details) {
      sqlite3_bind_text(s_db.stmt_log_event, 5, details, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_event, 5);
   }

   sqlite3_step(s_db.stmt_log_event);
   sqlite3_reset(s_db.stmt_log_event);

   AUTH_DB_UNLOCK();
}

int auth_db_query_audit_log(const auth_log_filter_t *filter,
                            auth_log_callback_t callback,
                            void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Build dynamic SQL query based on filters */
   char sql[512];
   int sql_len = snprintf(sql, sizeof(sql),
                          "SELECT timestamp, event, username, ip_address, details "
                          "FROM auth_log WHERE 1=1");

   /* Apply filters */
   time_t since = 0, until = 0;
   const char *event_filter = NULL;
   const char *user_filter = NULL;
   int limit = AUTH_LOG_DEFAULT_LIMIT;
   int offset = 0;

   if (filter) {
      since = filter->since;
      until = filter->until;
      event_filter = filter->event;
      user_filter = filter->username;
      limit = (filter->limit > 0) ? filter->limit : AUTH_LOG_DEFAULT_LIMIT;
      if (limit > AUTH_LOG_MAX_LIMIT)
         limit = AUTH_LOG_MAX_LIMIT;
      offset = (filter->offset > 0) ? filter->offset : 0;
   }

   if (since > 0) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND timestamp >= ?");
   }
   if (until > 0) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND timestamp <= ?");
   }
   if (event_filter) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND event = ?");
   }
   if (user_filter) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND username = ?");
   }

   snprintf(sql + sql_len, sizeof(sql) - sql_len, " ORDER BY timestamp DESC LIMIT ? OFFSET ?");

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Bind parameters */
   int param = 1;
   if (since > 0) {
      sqlite3_bind_int64(stmt, param++, (int64_t)since);
   }
   if (until > 0) {
      sqlite3_bind_int64(stmt, param++, (int64_t)until);
   }
   if (event_filter) {
      sqlite3_bind_text(stmt, param++, event_filter, -1, SQLITE_STATIC);
   }
   if (user_filter) {
      sqlite3_bind_text(stmt, param++, user_filter, -1, SQLITE_STATIC);
   }
   sqlite3_bind_int(stmt, param++, limit);
   sqlite3_bind_int(stmt, param++, offset);

   /* Execute and call callback for each row */
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_log_entry_t entry = { 0 };

      entry.timestamp = (time_t)sqlite3_column_int64(stmt, 0);

      const char *ev = (const char *)sqlite3_column_text(stmt, 1);
      if (ev) {
         safe_strscpy(entry.event, ev);
      }

      const char *user = (const char *)sqlite3_column_text(stmt, 2);
      if (user) {
         safe_strscpy(entry.username, user);
      }

      const char *ip = (const char *)sqlite3_column_text(stmt, 3);
      if (ip) {
         safe_strscpy(entry.ip_address, ip);
      }

      const char *details = (const char *)sqlite3_column_text(stmt, 4);
      if (details) {
         safe_strscpy(entry.details, details);
      }

      if (callback(&entry, ctx) != 0) {
         break;
      }
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}
