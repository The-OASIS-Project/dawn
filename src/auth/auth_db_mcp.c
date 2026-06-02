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
 * Per-user MCP server allowlist CRUD. Uses ad-hoc prepared statements (like
 * memory_db_alias.c) so the shared auth_db_state_t layout is untouched; all
 * access is serialized under s_db.mutex, consistent with the rest of auth_db.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "auth/auth_db_mcp.h"

#include <pthread.h>
#include <sqlite3.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_mcp_grant(int64_t user_id, const char *server_alias) {
   if (server_alias == NULL) {
      return AUTH_DB_FAILURE;
   }

   const char *sql = "INSERT INTO mcp_user_access (user_id, server_alias, enabled, updated_at) "
                     "VALUES (?, ?, 1, ?) "
                     "ON CONFLICT(user_id, server_alias) DO UPDATE SET enabled = 1, "
                     "updated_at = excluded.updated_at";

   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_mcp: grant prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, user_id);
   sqlite3_bind_text(st, 2, server_alias, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);

   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_mcp_revoke(int64_t user_id, const char *server_alias) {
   if (server_alias == NULL) {
      return AUTH_DB_FAILURE;
   }

   const char *sql = "UPDATE mcp_user_access SET enabled = 0, updated_at = ? "
                     "WHERE user_id = ? AND server_alias = ?";

   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_mcp: revoke prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
   sqlite3_bind_int64(st, 2, user_id);
   sqlite3_bind_text(st, 3, server_alias, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);

   /* No matching row is a successful no-op (idempotent revoke). */
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_mcp_check_access(int64_t user_id, const char *server_alias, bool *allowed_out) {
   if (server_alias == NULL || allowed_out == NULL) {
      return AUTH_DB_FAILURE;
   }
   *allowed_out = false;

   const char *sql = "SELECT enabled FROM mcp_user_access WHERE user_id = ? AND server_alias = ?";

   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_mcp: check prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, user_id);
   sqlite3_bind_text(st, 2, server_alias, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(st);
   int result = AUTH_DB_SUCCESS;
   if (rc == SQLITE_ROW) {
      *allowed_out = sqlite3_column_int(st, 0) != 0;
   } else if (rc != SQLITE_DONE) {
      result = AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);

   return result;
}

int auth_db_mcp_user_is_admin(int64_t user_id, bool *is_admin_out) {
   if (is_admin_out == NULL) {
      return AUTH_DB_FAILURE;
   }
   *is_admin_out = false;

   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT is_admin FROM users WHERE id = ?", -1, &st, NULL) !=
       SQLITE_OK) {
      OLOG_ERROR("auth_db_mcp: is_admin prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, user_id);

   int rc = sqlite3_step(st);
   int result = AUTH_DB_SUCCESS;
   if (rc == SQLITE_ROW) {
      *is_admin_out = sqlite3_column_int(st, 0) != 0;
   } else if (rc != SQLITE_DONE) {
      result = AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);

   return result;
}

int auth_db_mcp_grant_all_admins(const char *server_alias) {
   if (server_alias == NULL) {
      return AUTH_DB_FAILURE;
   }

   const char *sql =
       "INSERT OR IGNORE INTO mcp_user_access (user_id, server_alias, enabled, updated_at) "
       "SELECT id, ?, 1, ? FROM users WHERE is_admin = 1";

   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_mcp: grant_all_admins prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, server_alias, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);

   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}
