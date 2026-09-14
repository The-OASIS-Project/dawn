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
 * Authentication Database User Module
 *
 * Provides user account management operations: creation, lookup, password
 * updates, lockout handling, and admin protection.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * User Operations
 * ============================================================================= */

int auth_db_create_user(const char *username, const char *password_hash, bool is_admin) {
   if (!username || !password_hash) {
      return AUTH_DB_INVALID;
   }

   size_t ulen = strlen(username);
   if (ulen == 0 || ulen >= AUTH_USERNAME_MAX) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_create_user);
   sqlite3_bind_text(s_db.stmt_create_user, 1, username, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_create_user, 2, password_hash, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_create_user, 3, is_admin ? 1 : 0);
   sqlite3_bind_int64(s_db.stmt_create_user, 4, (int64_t)time(NULL));

   int rc = sqlite3_step(s_db.stmt_create_user);
   sqlite3_reset(s_db.stmt_create_user);

   AUTH_DB_UNLOCK();

   if (rc == SQLITE_CONSTRAINT) {
      return AUTH_DB_DUPLICATE;
   } else if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db_create_user: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

int auth_db_get_user(const char *username, auth_user_t *user_out) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_get_user);
   sqlite3_bind_text(s_db.stmt_get_user, 1, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_get_user);

   if (rc == SQLITE_ROW) {
      if (user_out) {
         user_out->id = sqlite3_column_int(s_db.stmt_get_user, 0);

         const char *uname = (const char *)sqlite3_column_text(s_db.stmt_get_user, 1);
         if (uname) {
            safe_strscpy(user_out->username, uname);
         }

         const char *hash = (const char *)sqlite3_column_text(s_db.stmt_get_user, 2);
         if (hash) {
            safe_strscpy(user_out->password_hash, hash);
         }

         user_out->is_admin = sqlite3_column_int(s_db.stmt_get_user, 3) != 0;
         user_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_get_user, 4);
         user_out->last_login = (time_t)sqlite3_column_int64(s_db.stmt_get_user, 5);
         user_out->failed_attempts = sqlite3_column_int(s_db.stmt_get_user, 6);
         user_out->lockout_until = (time_t)sqlite3_column_int64(s_db.stmt_get_user, 7);
      }
      sqlite3_reset(s_db.stmt_get_user);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_get_user);
   AUTH_DB_UNLOCK();

   if (rc == SQLITE_DONE) {
      return AUTH_DB_NOT_FOUND;
   }

   OLOG_ERROR("auth_db_get_user: failed: %s", sqlite3_errmsg(s_db.db));
   return AUTH_DB_FAILURE;
}

int auth_db_user_count(int *count_out) {
   if (!count_out) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_count_users);
   int rc = sqlite3_step(s_db.stmt_count_users);

   if (rc == SQLITE_ROW) {
      *count_out = sqlite3_column_int(s_db.stmt_count_users, 0);
   } else {
      sqlite3_reset(s_db.stmt_count_users);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_count_users);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_validate_username(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   size_t len = strlen(username);
   if (len == 0 || len >= AUTH_USERNAME_MAX) {
      return AUTH_DB_INVALID;
   }

   /* First character must be letter or underscore */
   char c = username[0];
   if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
      return AUTH_DB_INVALID;
   }

   /* Remaining characters: alphanumeric, underscore, hyphen, period */
   for (size_t i = 1; i < len; i++) {
      c = username[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')) {
         return AUTH_DB_INVALID;
      }
   }

   return AUTH_DB_SUCCESS;
}

int auth_db_increment_failed_attempts(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_inc_failed_attempts);
   sqlite3_bind_text(s_db.stmt_inc_failed_attempts, 1, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_inc_failed_attempts);
   sqlite3_reset(s_db.stmt_inc_failed_attempts);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_reset_failed_attempts(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_reset_failed_attempts);
   sqlite3_bind_text(s_db.stmt_reset_failed_attempts, 1, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_reset_failed_attempts);
   sqlite3_reset(s_db.stmt_reset_failed_attempts);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_update_last_login(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_update_last_login);
   sqlite3_bind_int64(s_db.stmt_update_last_login, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_update_last_login, 2, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_update_last_login);
   sqlite3_reset(s_db.stmt_update_last_login);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_set_lockout(const char *username, time_t lockout_until) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_set_lockout);
   sqlite3_bind_int64(s_db.stmt_set_lockout, 1, (int64_t)lockout_until);
   sqlite3_bind_text(s_db.stmt_set_lockout, 2, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_set_lockout);
   sqlite3_reset(s_db.stmt_set_lockout);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_list_users(auth_user_summary_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT id, username, is_admin, created_at, last_login, "
                     "failed_attempts, lockout_until FROM users ORDER BY id";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_user_summary_t user = { 0 };
      user.id = sqlite3_column_int(stmt, 0);

      const char *uname = (const char *)sqlite3_column_text(stmt, 1);
      if (uname) {
         safe_strscpy(user.username, uname);
      }

      user.is_admin = sqlite3_column_int(stmt, 2) != 0;
      user.created_at = (time_t)sqlite3_column_int64(stmt, 3);
      user.last_login = (time_t)sqlite3_column_int64(stmt, 4);
      user.failed_attempts = sqlite3_column_int(stmt, 5);
      user.lockout_until = (time_t)sqlite3_column_int64(stmt, 6);

      if (callback(&user, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_count_admins(int *count_out) {
   if (!count_out) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT COUNT(*) FROM users WHERE is_admin = 1";
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

int auth_db_delete_user(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Start transaction for atomicity */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Get user info (is_admin, id) */
   const char *sql_get = "SELECT id, is_admin FROM users WHERE username = ?";
   sqlite3_stmt *stmt_get = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_get, -1, &stmt_get, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt_get, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_get);

   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt_get);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }

   int user_id = sqlite3_column_int(stmt_get, 0);
   bool is_admin = sqlite3_column_int(stmt_get, 1) != 0;
   sqlite3_finalize(stmt_get);

   /* If admin, check if this is the last admin */
   if (is_admin) {
      const char *sql_count = "SELECT COUNT(*) FROM users WHERE is_admin = 1";
      sqlite3_stmt *stmt_count = NULL;
      rc = sqlite3_prepare_v2(s_db.db, sql_count, -1, &stmt_count, NULL);
      if (rc != SQLITE_OK) {
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }
      int admin_count = 0;
      if (sqlite3_step(stmt_count) == SQLITE_ROW) {
         admin_count = sqlite3_column_int(stmt_count, 0);
      }
      sqlite3_finalize(stmt_count);

      if (admin_count <= 1) {
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return AUTH_DB_LAST_ADMIN;
      }
   }

   /* Delete user's sessions first */
   const char *sql_del_sessions = "DELETE FROM sessions WHERE user_id = ?";
   sqlite3_stmt *stmt_del_sessions = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql_del_sessions, -1, &stmt_del_sessions, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt_del_sessions, 1, user_id);
   sqlite3_step(stmt_del_sessions);
   sqlite3_finalize(stmt_del_sessions);

   /* Delete the user */
   const char *sql_del = "DELETE FROM users WHERE username = ?";
   sqlite3_stmt *stmt_del = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql_del, -1, &stmt_del, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(stmt_del, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_del);
   sqlite3_finalize(stmt_del);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_update_password(const char *username, const char *new_hash) {
   if (!username || !new_hash) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Start transaction for atomicity (password + session invalidation) */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Get user ID */
   const char *sql_get = "SELECT id FROM users WHERE username = ?";
   sqlite3_stmt *stmt_get = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_get, -1, &stmt_get, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt_get, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_get);

   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt_get);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }

   int user_id = sqlite3_column_int(stmt_get, 0);
   sqlite3_finalize(stmt_get);

   /* Update password */
   const char *sql_update = "UPDATE users SET password_hash = ? WHERE username = ?";
   sqlite3_stmt *stmt_update = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql_update, -1, &stmt_update, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(stmt_update, 1, new_hash, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt_update, 2, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_update);
   sqlite3_finalize(stmt_update);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Invalidate all sessions for this user */
   const char *sql_del = "DELETE FROM sessions WHERE user_id = ?";
   sqlite3_stmt *stmt_del = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql_del, -1, &stmt_del, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt_del, 1, user_id);
   sqlite3_step(stmt_del);
   sqlite3_finalize(stmt_del);

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int auth_db_unlock_user(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Check if user exists first */
   const char *sql_check = "SELECT 1 FROM users WHERE username = ?";
   sqlite3_stmt *stmt_check = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_check, -1, &stmt_check, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt_check, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_check);
   sqlite3_finalize(stmt_check);

   if (rc != SQLITE_ROW) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }

   /* Unlock: set lockout_until to 0 and reset failed_attempts */
   const char *sql_unlock =
       "UPDATE users SET lockout_until = 0, failed_attempts = 0 WHERE username = ?";
   sqlite3_stmt *stmt_unlock = NULL;
   sqlite3_prepare_v2(s_db.db, sql_unlock, -1, &stmt_unlock, NULL);
   sqlite3_bind_text(stmt_unlock, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_unlock);
   sqlite3_finalize(stmt_unlock);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}
