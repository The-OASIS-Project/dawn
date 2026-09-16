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
 * Authentication Database - Session Module
 *
 * Handles authentication session management:
 * - Session creation and deletion
 * - Session lookup and validation
 * - Activity tracking
 * - Session listing and enumeration
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Extract session summary from current SQLite row
 *
 * Expects columns in order: token, user_id, username, created_at,
 * last_activity, ip_address, user_agent
 *
 * @param stmt Statement positioned on a valid row
 * @param session Output session summary struct
 */
static void extract_session_summary(sqlite3_stmt *stmt, auth_session_summary_t *session) {
   memset(session, 0, sizeof(*session));

   /* Only copy token prefix for security */
   const char *tok = (const char *)sqlite3_column_text(stmt, 0);
   if (tok) {
      safe_strscpy(session->token_prefix, tok);
   }

   session->user_id = sqlite3_column_int(stmt, 1);

   const char *uname = (const char *)sqlite3_column_text(stmt, 2);
   if (uname) {
      safe_strscpy(session->username, uname);
   }

   session->created_at = (time_t)sqlite3_column_int64(stmt, 3);
   session->last_activity = (time_t)sqlite3_column_int64(stmt, 4);

   const char *ip = (const char *)sqlite3_column_text(stmt, 5);
   if (ip) {
      safe_strscpy(session->ip_address, ip);
   }

   const char *ua = (const char *)sqlite3_column_text(stmt, 6);
   if (ua) {
      safe_strscpy(session->user_agent, ua);
   }
}

/* =============================================================================
 * Session Operations
 * ============================================================================= */

int auth_db_create_session(int user_id,
                           const char *token,
                           const char *ip_address,
                           const char *user_agent,
                           bool remember_me) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   time_t now = time(NULL);
   time_t expires_at = now +
                       (remember_me ? AUTH_REMEMBER_ME_TIMEOUT_SEC : AUTH_SESSION_TIMEOUT_SEC);

   sqlite3_reset(s_db.stmt_create_session);
   sqlite3_bind_text(s_db.stmt_create_session, 1, token, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_create_session, 2, user_id);
   sqlite3_bind_int64(s_db.stmt_create_session, 3, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_create_session, 4, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_create_session, 5, (int64_t)expires_at);

   if (ip_address) {
      sqlite3_bind_text(s_db.stmt_create_session, 6, ip_address, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_create_session, 6);
   }

   if (user_agent) {
      /* Truncate user agent to max length */
      size_t ua_len = strlen(user_agent);
      if (ua_len >= AUTH_USER_AGENT_MAX) {
         ua_len = AUTH_USER_AGENT_MAX - 1;
      }
      sqlite3_bind_text(s_db.stmt_create_session, 7, user_agent, (int)ua_len, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_create_session, 7);
   }

   int rc = sqlite3_step(s_db.stmt_create_session);
   sqlite3_reset(s_db.stmt_create_session);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_get_session(const char *token, auth_session_t *session_out) {
   if (!token || !session_out) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Note: Cleanup is now handled by the background maintenance thread
    * (auth_maintenance.c) rather than lazily during session lookups.
    * This avoids conflicts between concurrent cleanup attempts. */

   sqlite3_reset(s_db.stmt_get_session);
   sqlite3_bind_text(s_db.stmt_get_session, 1, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_get_session);

   if (rc == SQLITE_ROW) {
      const char *tok = (const char *)sqlite3_column_text(s_db.stmt_get_session, 0);
      if (tok) {
         safe_strscpy(session_out->token, tok);
      }

      session_out->user_id = sqlite3_column_int(s_db.stmt_get_session, 1);

      const char *uname = (const char *)sqlite3_column_text(s_db.stmt_get_session, 2);
      if (uname) {
         safe_strscpy(session_out->username, uname);
      }

      session_out->is_admin = sqlite3_column_int(s_db.stmt_get_session, 3) != 0;
      session_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_get_session, 4);
      session_out->last_activity = (time_t)sqlite3_column_int64(s_db.stmt_get_session, 5);
      session_out->expires_at = (time_t)sqlite3_column_int64(s_db.stmt_get_session, 6);

      const char *ip = (const char *)sqlite3_column_text(s_db.stmt_get_session, 7);
      if (ip) {
         safe_strscpy(session_out->ip_address, ip);
      } else {
         session_out->ip_address[0] = '\0';
      }

      const char *ua = (const char *)sqlite3_column_text(s_db.stmt_get_session, 8);
      if (ua) {
         safe_strscpy(session_out->user_agent, ua);
      } else {
         session_out->user_agent[0] = '\0';
      }

      session_out->keepalive_enabled = sqlite3_column_int(s_db.stmt_get_session, 9) != 0;

      sqlite3_reset(s_db.stmt_get_session);

      /* Check if session has expired */
      if (session_out->expires_at > 0 && session_out->expires_at < time(NULL)) {
         AUTH_DB_UNLOCK();
         return AUTH_DB_NOT_FOUND; /* Treat expired as not found */
      }

      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_get_session);
   AUTH_DB_UNLOCK();

   if (rc == SQLITE_DONE) {
      return AUTH_DB_NOT_FOUND;
   }

   OLOG_ERROR("auth_db_get_session: failed: %s", sqlite3_errmsg(s_db.db));
   return AUTH_DB_FAILURE;
}

int auth_db_renew_session(const char *token, time_t new_expires_at) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_renew_session);
   sqlite3_bind_int64(s_db.stmt_renew_session, 1, (int64_t)new_expires_at);
   /* Absolute-cap clamp lives in the SQL (MIN(?, created_at + ?cap)) so the
    * primitive itself can never write an unbounded expiry. */
   sqlite3_bind_int64(s_db.stmt_renew_session, 2, (int64_t)AUTH_SESSION_ABSOLUTE_CAP_SEC);
   sqlite3_bind_text(s_db.stmt_renew_session, 3, token, -1, SQLITE_STATIC);
   /* Guard: never renew an already-expired/revoked-by-time row. */
   sqlite3_bind_int64(s_db.stmt_renew_session, 4, (int64_t)time(NULL));

   int rc = sqlite3_step(s_db.stmt_renew_session);
   sqlite3_reset(s_db.stmt_renew_session);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_set_session_keepalive(const char *token, bool enabled) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_set_session_keepalive);
   sqlite3_bind_int(s_db.stmt_set_session_keepalive, 1, enabled ? 1 : 0);
   sqlite3_bind_text(s_db.stmt_set_session_keepalive, 2, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_set_session_keepalive);
   sqlite3_reset(s_db.stmt_set_session_keepalive);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_update_session_activity(const char *token) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_update_session_activity);
   sqlite3_bind_int64(s_db.stmt_update_session_activity, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_update_session_activity, 2, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_update_session_activity);
   sqlite3_reset(s_db.stmt_update_session_activity);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_delete_session(const char *token) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_delete_session);
   sqlite3_bind_text(s_db.stmt_delete_session, 1, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_delete_session);
   sqlite3_reset(s_db.stmt_delete_session);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_delete_session_by_prefix(const char *prefix) {
   if (!prefix || strlen(prefix) < AUTH_TOKEN_PREFIX_LEN) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Find full token matching prefix (use SUBSTR for exact matching, not LIKE)
    * Note: SQL substr length must match AUTH_TOKEN_PREFIX_LEN (16) */
   const char *find_sql = "SELECT token FROM sessions WHERE substr(token, 1, 16) = ? LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, find_sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Bind only the first AUTH_TOKEN_PREFIX_LEN characters */
   char prefix_buf[AUTH_TOKEN_PREFIX_LEN + 1] = { 0 };
   safe_strscpy(prefix_buf, prefix);
   sqlite3_bind_text(stmt, 1, prefix_buf, AUTH_TOKEN_PREFIX_LEN, SQLITE_STATIC);

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }

   /* Get full token and delete it */
   const char *full_token = (const char *)sqlite3_column_text(stmt, 0);
   if (!full_token) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Delete the session */
   sqlite3_reset(s_db.stmt_delete_session);
   sqlite3_bind_text(s_db.stmt_delete_session, 1, full_token, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(s_db.stmt_delete_session);
   sqlite3_reset(s_db.stmt_delete_session);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

bool auth_db_session_belongs_to_user(const char *prefix, int user_id) {
   if (!prefix || strlen(prefix) < AUTH_TOKEN_PREFIX_LEN || user_id <= 0) {
      return false;
   }

   AUTH_DB_LOCK_OR_RETURN(false);

   /* Single query to check if session with prefix belongs to user
    * Note: SQL substr length must match AUTH_TOKEN_PREFIX_LEN (16) */
   const char *sql =
       "SELECT 1 FROM sessions WHERE substr(token, 1, 16) = ? AND user_id = ? LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return false;
   }

   char prefix_buf[AUTH_TOKEN_PREFIX_LEN + 1] = { 0 };
   safe_strscpy(prefix_buf, prefix);
   sqlite3_bind_text(stmt, 1, prefix_buf, AUTH_TOKEN_PREFIX_LEN, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, user_id);

   rc = sqlite3_step(stmt);
   bool belongs = (rc == SQLITE_ROW);

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return belongs;
}

int auth_db_delete_sessions_by_username(const char *username, int *deleted_out) {
   if (!username) {
      return AUTH_DB_FAILURE;
   }

   /* Look up user to get their ID */
   auth_user_t user;
   int rc = auth_db_get_user(username, &user);
   if (rc == AUTH_DB_NOT_FOUND) {
      if (deleted_out) {
         *deleted_out = 0;
      }
      return AUTH_DB_SUCCESS;
   }
   if (rc != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }

   return auth_db_delete_user_sessions(user.id, deleted_out);
}

int auth_db_delete_user_sessions(int user_id, int *deleted_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_delete_user_sessions);
   sqlite3_bind_int(s_db.stmt_delete_user_sessions, 1, user_id);

   int rc = sqlite3_step(s_db.stmt_delete_user_sessions);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_delete_user_sessions);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   if (deleted_out) {
      *deleted_out = changes;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_list_sessions(auth_session_summary_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT s.token, s.user_id, u.username, s.created_at, "
                     "s.last_activity, s.ip_address, s.user_agent "
                     "FROM sessions s "
                     "JOIN users u ON s.user_id = u.id "
                     "ORDER BY s.last_activity DESC";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_session_summary_t session;
      extract_session_summary(stmt, &session);

      if (callback(&session, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_list_user_sessions(int user_id, auth_session_summary_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT s.token, s.user_id, u.username, s.created_at, "
                     "s.last_activity, s.ip_address, s.user_agent "
                     "FROM sessions s "
                     "JOIN users u ON s.user_id = u.id "
                     "WHERE s.user_id = ? "
                     "ORDER BY s.last_activity DESC";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_session_summary_t session;
      extract_session_summary(stmt, &session);

      if (callback(&session, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_count_sessions(int *count_out) {
   if (!count_out) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT COUNT(*) FROM sessions";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *count_out = sqlite3_column_int(stmt, 0);
   } else {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}
