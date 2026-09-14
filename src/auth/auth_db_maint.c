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
 * Authentication Database - Maintenance Module
 *
 * Handles database maintenance operations:
 * - Cleanup of expired data (sessions, attempts, logs, metrics)
 * - WAL checkpointing (full and passive)
 * - Database statistics
 * - VACUUM for space reclamation
 * - Secure backup with path validation
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* Vacuum rate limit: once per 24 hours */
#define VACUUM_COOLDOWN_SEC (24 * 60 * 60)

/* =============================================================================
 * Maintenance Operations
 * ============================================================================= */

int auth_db_run_cleanup(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);
   time_t session_cutoff = now - AUTH_SESSION_TIMEOUT_SEC;
   time_t attempt_cutoff = now - LOGIN_ATTEMPT_RETENTION_SEC;
   time_t log_cutoff = now - AUTH_LOG_RETENTION_SEC;

   sqlite3_reset(s_db.stmt_delete_expired_sessions);
   sqlite3_bind_int64(s_db.stmt_delete_expired_sessions, 1, (int64_t)session_cutoff);
   sqlite3_step(s_db.stmt_delete_expired_sessions);
   sqlite3_reset(s_db.stmt_delete_expired_sessions);

   sqlite3_reset(s_db.stmt_delete_old_attempts);
   sqlite3_bind_int64(s_db.stmt_delete_old_attempts, 1, (int64_t)attempt_cutoff);
   sqlite3_step(s_db.stmt_delete_old_attempts);
   sqlite3_reset(s_db.stmt_delete_old_attempts);

   sqlite3_reset(s_db.stmt_delete_old_logs);
   sqlite3_bind_int64(s_db.stmt_delete_old_logs, 1, (int64_t)log_cutoff);
   sqlite3_step(s_db.stmt_delete_old_logs);
   sqlite3_reset(s_db.stmt_delete_old_logs);

   /* Clean up old session metrics (90 day retention)
    * Cast to time_t before multiplication to prevent integer overflow */
   time_t metrics_cutoff = now - ((time_t)SESSION_METRICS_RETENTION_DAYS * 24 * 60 * 60);
   sqlite3_reset(s_db.stmt_metrics_delete_old);
   sqlite3_bind_int64(s_db.stmt_metrics_delete_old, 1, (int64_t)metrics_cutoff);
   sqlite3_step(s_db.stmt_metrics_delete_old);
   sqlite3_reset(s_db.stmt_metrics_delete_old);

   /* Messaging /link audit log (7-day retention).  Inline DELETE
    * rather than a cached prepared statement — the table sees low
    * write traffic (only on /link attempts, rate-limited to 5/sender/
    * 10min) so per-call prepare overhead is negligible.  See
    * docs/MESSAGING_CHANNELS_DESIGN.md §12. */
   time_t link_attempt_cutoff = now - ((time_t)7 * 24 * 60 * 60);
   sqlite3_stmt *stmt_link = NULL;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM messaging_link_attempts WHERE created_at < ?", -1,
                          &stmt_link, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt_link, 1, (int64_t)link_attempt_cutoff);
      sqlite3_step(stmt_link);
      sqlite3_finalize(stmt_link);
   }

   s_db.last_cleanup = now;

   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_checkpoint(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   int rc = sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_OK) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_checkpoint_passive(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* PASSIVE: Checkpoint as much as possible without waiting */
   int rc = sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_OK) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* =============================================================================
 * Statistics and Database Management
 * ============================================================================= */

int auth_db_get_stats(auth_db_stats_t *stats) {
   if (!stats) {
      return AUTH_DB_INVALID;
   }

   memset(stats, 0, sizeof(*stats));

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Combined query for all stats - reduces database round trips */
   const char *sql = "SELECT "
                     "(SELECT COUNT(*) FROM users), "
                     "(SELECT COUNT(*) FROM users WHERE is_admin = 1), "
                     "(SELECT COUNT(*) FROM users WHERE lockout_until > strftime('%s','now')), "
                     "(SELECT COUNT(*) FROM sessions), "
                     "(SELECT COUNT(*) FROM login_attempts "
                     " WHERE success = 0 AND timestamp > strftime('%s','now') - 86400), "
                     "(SELECT COUNT(*) FROM auth_log), "
                     "(SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size())";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      OLOG_ERROR("Failed to prepare stats query: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      stats->user_count = sqlite3_column_int(stmt, 0);
      stats->admin_count = sqlite3_column_int(stmt, 1);
      stats->locked_user_count = sqlite3_column_int(stmt, 2);
      stats->session_count = sqlite3_column_int(stmt, 3);
      stats->failed_attempts_24h = sqlite3_column_int(stmt, 4);
      stats->audit_log_count = sqlite3_column_int(stmt, 5);
      stats->db_size_bytes = sqlite3_column_int64(stmt, 6);
   }
   sqlite3_finalize(stmt);

   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_vacuum(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Rate limit: once per 24 hours */
   time_t now = time(NULL);
   if (s_db.last_vacuum > 0 && (now - s_db.last_vacuum) < VACUUM_COOLDOWN_SEC) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_RATE_LIMITED;
   }

   int rc = sqlite3_exec(s_db.db, "VACUUM", NULL, NULL, NULL);
   if (rc == SQLITE_OK) {
      s_db.last_vacuum = now;
   }

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_OK) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/**
 * @brief Check if a path is within one of the allowed backup directories.
 *
 * Resolves the parent directory of the path to its canonical form and checks
 * against a list of allowed directory prefixes.
 *
 * @param path Path to validate
 * @return 0 if path is allowed, non-zero otherwise
 */
static int validate_backup_path(const char *path) {
   /* Allowed backup directory prefixes */
   static const char *allowed_prefixes[] = { "/var/lib/dawn/", /* Main Dawn data directory */
                                             "/tmp/",          /* Temporary files */
                                             "/home/",         /* User home directories */
                                             NULL };

   if (!path || path[0] != '/') {
      /* Only absolute paths allowed */
      return AUTH_DB_FAILURE;
   }

   /* Check for ".." path traversal */
   if (strstr(path, "..") != NULL) {
      return AUTH_DB_FAILURE;
   }

   /* Get parent directory of the target path */
   char parent[PATH_MAX];
   safe_strscpy(parent, path);

   char *last_slash = strrchr(parent, '/');
   if (!last_slash || last_slash == parent) {
      /* Root directory or invalid path */
      return AUTH_DB_FAILURE;
   }
   *last_slash = '\0';

   /* Resolve to canonical path (follow symlinks) */
   char resolved[PATH_MAX];
   if (!realpath(parent, resolved)) {
      /* Parent directory doesn't exist or error resolving */
      return AUTH_DB_FAILURE;
   }

   /* Add trailing slash for prefix matching */
   size_t len = strlen(resolved);
   if (len < sizeof(resolved) - 1 && resolved[len - 1] != '/') {
      resolved[len] = '/';
      resolved[len + 1] = '\0';
   }

   /* Check against allowlist */
   for (const char **prefix = allowed_prefixes; *prefix != NULL; prefix++) {
      if (strncmp(resolved, *prefix, strlen(*prefix)) == 0) {
         return 0; /* Path is allowed */
      }
      /* Also check if resolved path equals the prefix without trailing slash */
      size_t prefix_len = strlen(*prefix);
      if (prefix_len > 0 && (*prefix)[prefix_len - 1] == '/') {
         if (strncmp(resolved, *prefix, prefix_len - 1) == 0 &&
             (resolved[prefix_len - 1] == '/' || resolved[prefix_len - 1] == '\0')) {
            return 0;
         }
      }
   }

   return AUTH_DB_FAILURE; /* Path not in allowlist */
}

int auth_db_backup(const char *dest_path) {
   if (!dest_path) {
      return AUTH_DB_INVALID;
   }

   /* Validate path against allowlist */
   if (validate_backup_path(dest_path) != 0) {
      OLOG_WARNING("Backup path not in allowed directories: %s", dest_path);
      return AUTH_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Create destination file with secure permissions.
    * O_NOFOLLOW prevents symlink attacks (TOCTOU race condition). */
   mode_t old_umask = umask(0077);
   int fd = open(dest_path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
   umask(old_umask);

   if (fd < 0) {
      OLOG_WARNING("Failed to create backup file: %s (%s)", dest_path, strerror(errno));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   close(fd);

   /* Open destination database */
   sqlite3 *dest_db = NULL;
   int rc = sqlite3_open(dest_path, &dest_db);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("Failed to open backup database: %s", sqlite3_errmsg(dest_db));
      unlink(dest_path);
      sqlite3_close(dest_db);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Start backup */
   sqlite3_backup *backup = sqlite3_backup_init(dest_db, "main", s_db.db, "main");
   if (!backup) {
      OLOG_WARNING("Failed to initialize backup: %s", sqlite3_errmsg(dest_db));
      sqlite3_close(dest_db);
      unlink(dest_path);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Step backup: 100 pages at a time, 10ms yield between steps */
   do {
      rc = sqlite3_backup_step(backup, 100);
      if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
         sqlite3_sleep(10);
      }
   } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

   sqlite3_backup_finish(backup);

   int result = AUTH_DB_SUCCESS;
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("Backup failed: %s", sqlite3_errmsg(dest_db));
      unlink(dest_path);
      result = AUTH_DB_FAILURE;
   }

   sqlite3_close(dest_db);

   /* Verify final permissions */
   struct stat st;
   if (result == AUTH_DB_SUCCESS && stat(dest_path, &st) == 0) {
      if ((st.st_mode & 0777) != 0600) {
         chmod(dest_path, 0600);
      }
   }

   pthread_mutex_unlock(&s_db.mutex);

   return result;
}
