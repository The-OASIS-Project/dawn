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
 * Authentication Database Core Module
 *
 * Provides database lifecycle management (init/shutdown/is_ready), the
 * shared s_db state, file-permission helpers, and the custom SQLite
 * functions registered into every connection.  The schema/migration
 * ladder lives in auth_db_schema.c and the prepared-statement init lives
 * in auth_db_statements.c — both reached through auth_db_internal.h.
 *
 * SECURITY: All database operations use prepared statements.
 * NEVER use sqlite3_exec() or sqlite3_mprintf() with user input.
 * See: CWE-89, OWASP SQL Injection Prevention Cheat Sheet
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db_internal.h"
#include "core/path_utils.h"
#include "logging.h"

/* =============================================================================
 * Shared State Definition
 * ============================================================================= */

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
   .last_cleanup = 0,
   .last_vacuum = 0,
};

/* =============================================================================
 * File Permission Helpers
 * ============================================================================= */

int auth_db_internal_create_parent_dir(const char *path) {
   /* Set restrictive umask — auth databases need 0700 directories */
   mode_t old_umask = umask(0077);
   bool ok = path_ensure_parent_dir_mode(path, 0700);
   umask(old_umask);

   return ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_internal_verify_permissions(const char *path) {
   struct stat st;

   if (stat(path, &st) != 0) {
      /* File doesn't exist yet, that's OK */
      if (errno == ENOENT) {
         return AUTH_DB_SUCCESS;
      }
      OLOG_ERROR("auth_db: stat(%s) failed: %s", path, strerror(errno));
      return AUTH_DB_FAILURE;
   }

   /* Check for world/group readable or writable */
   if ((st.st_mode & 0077) != 0) {
      OLOG_WARNING("auth_db: SECURITY: %s has unsafe permissions %04o, fixing to 0600", path,
                   st.st_mode & 0777);

      if (chmod(path, 0600) != 0) {
         OLOG_ERROR("auth_db: failed to fix permissions on %s: %s", path, strerror(errno));
         return AUTH_DB_FAILURE;
      }
   }

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Custom SQLite Functions
 * ============================================================================= */

/**
 * @brief SQLite custom function: powf(base, exp)
 *
 * Enables atomic confidence decay in UPDATE statements without
 * a SELECT-compute-UPDATE loop. Used by memory decay system.
 */
static void sqlite_powf(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   double base = sqlite3_value_double(argv[0]);
   double exp = sqlite3_value_double(argv[1]);
   double result = pow(base, exp);
   /* Guard against NaN/Inf from edge cases (negative base, huge exponent) */
   if (isnan(result) || isinf(result)) {
      sqlite3_result_double(ctx, 0.0);
   } else {
      sqlite3_result_double(ctx, result);
   }
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int auth_db_init(const char *db_path) {
   pthread_mutex_lock(&s_db.mutex);

   if (s_db.initialized) {
      OLOG_WARNING("auth_db_init: already initialized");
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_SUCCESS;
   }

   const char *path = db_path ? db_path : AUTH_DB_DEFAULT_PATH;

   /* Create parent directory with secure permissions */
   if (auth_db_internal_create_parent_dir(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check existing file permissions */
   if (auth_db_internal_verify_permissions(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Set restrictive umask for database file creation */
   mode_t old_umask = umask(0077);

   /* Open database with FULLMUTEX for thread safety */
   int rc = sqlite3_open_v2(path, &s_db.db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            NULL);

   umask(old_umask);

   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db_init: failed to open %s: %s", path,
                 s_db.db ? sqlite3_errmsg(s_db.db) : "unknown");
      if (s_db.db) {
         sqlite3_close(s_db.db);
         s_db.db = NULL;
      }
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Verify permissions after creation */
   if (auth_db_internal_verify_permissions(path) != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Enable WAL mode for better concurrency */
   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: failed to enable WAL mode: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      /* Continue anyway - DELETE mode works too */
   }

   /* Set conservative cache size for embedded systems (64 pages × 4KB = 256KB) */
   sqlite3_exec(s_db.db, "PRAGMA cache_size=64", NULL, NULL, NULL);

   /* Enable foreign keys */
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

   /* Register custom SQL functions */
   sqlite3_create_function(s_db.db, "powf", 2, SQLITE_UTF8, NULL, sqlite_powf, NULL, NULL);

   /* Create schema if needed */
   if (auth_db_create_schema(path) != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* What records a user's removals for withdrawal (after the migrations
    * that make its table). */
   if (auth_db_withdraw_install(s_db.db) != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Prepare statements */
   if (auth_db_prepare_statements() != AUTH_DB_SUCCESS) {
      auth_db_finalize_statements();
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   s_db.initialized = true;
   s_db.last_cleanup = time(NULL);

   OLOG_INFO("auth_db_init: initialized at %s", path);

   pthread_mutex_unlock(&s_db.mutex);

   /* Ensure the local pseudo-satellite row exists. Runs outside the init
    * mutex because it calls other auth_db_* helpers that acquire the mutex
    * themselves. Safe: s_db.initialized is already true here. */
   if (satellite_db_ensure_local_pseudo() != AUTH_DB_SUCCESS) {
      OLOG_WARNING("auth_db_init: could not ensure local pseudo-satellite row");
   }

   return AUTH_DB_SUCCESS;
}

void auth_db_shutdown(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return;
   }

   /* Checkpoint WAL to main database */
   if (s_db.db) {
      sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
   }

   /* Finalize all statements */
   auth_db_finalize_statements();

   /* Close database */
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }

   s_db.initialized = false;

   OLOG_INFO("auth_db_shutdown: complete");

   pthread_mutex_unlock(&s_db.mutex);
}

bool auth_db_is_ready(void) {
   pthread_mutex_lock(&s_db.mutex);
   bool ready = s_db.initialized;
   pthread_mutex_unlock(&s_db.mutex);
   return ready;
}
