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
 * Authentication Database - User Settings Module
 *
 * Handles per-user settings storage and retrieval:
 * - Persona description and mode (append/replace)
 * - Location and timezone
 * - Units preference (metric/imperial)
 * - UI theme
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * User Settings Operations
 * ============================================================================= */

int auth_db_get_user_settings(int user_id, auth_user_settings_t *settings_out) {
   if (!settings_out) {
      return AUTH_DB_INVALID;
   }

   /* Initialize with defaults */
   memset(settings_out, 0, sizeof(*settings_out));
   safe_strscpy(settings_out->persona_mode, "append");
   safe_strscpy(settings_out->timezone, "UTC");
   safe_strscpy(settings_out->units, "metric");
   safe_strscpy(settings_out->theme, "cyan");

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_get_user_settings);
   sqlite3_bind_int(s_db.stmt_get_user_settings, 1, user_id);

   int rc = sqlite3_step(s_db.stmt_get_user_settings);

   if (rc == SQLITE_ROW) {
      /* Copy non-null values from database */
      const char *persona = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 0);
      const char *persona_mode = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 1);
      const char *location = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 2);
      const char *timezone = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 3);
      const char *units = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 4);

      if (persona) {
         safe_strscpy(settings_out->persona_description, persona);
      }
      if (persona_mode) {
         safe_strscpy(settings_out->persona_mode, persona_mode);
      }
      if (location) {
         safe_strscpy(settings_out->location, location);
      }
      if (timezone) {
         safe_strscpy(settings_out->timezone, timezone);
      }
      if (units) {
         safe_strscpy(settings_out->units, units);
      }

      const char *theme = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 5);
      if (theme) {
         safe_strscpy(settings_out->theme, theme);
      }
   } else if (rc != SQLITE_DONE) {
      /* Unexpected error */
      OLOG_ERROR("auth_db: get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_reset(s_db.stmt_get_user_settings);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   /* SQLITE_DONE means no row found - return defaults */

   sqlite3_reset(s_db.stmt_get_user_settings);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int auth_db_set_user_settings(int user_id, const auth_user_settings_t *settings) {
   if (!settings) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_set_user_settings);
   sqlite3_bind_int(s_db.stmt_set_user_settings, 1, user_id);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 2, settings->persona_description, -1,
                     SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 3, settings->persona_mode, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 4, settings->location, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 5, settings->timezone, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 6, settings->units, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 7, settings->theme, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_set_user_settings, 8, (int64_t)time(NULL));

   int rc = sqlite3_step(s_db.stmt_set_user_settings);
   sqlite3_reset(s_db.stmt_set_user_settings);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: set_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

int auth_db_init_user_settings(int user_id) {
   auth_user_settings_t defaults;
   memset(&defaults, 0, sizeof(defaults));
   safe_strscpy(defaults.timezone, "UTC");
   safe_strscpy(defaults.units, "metric");
   return auth_db_set_user_settings(user_id, &defaults);
}

/* =============================================================================
 * User identity fields (v44) — stored on the users table.  Ad-hoc
 * sqlite3_prepare_v2 instead of cached prepared statements; called only on
 * settings GET/SET round-trips, not on every request, so the prepare cost
 * is negligible.
 * ============================================================================= */

int auth_db_get_user_identity(int user_id, auth_user_identity_t *out) {
   if (!out) {
      return AUTH_DB_INVALID;
   }
   memset(out, 0, sizeof(*out));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT real_name, preferred_address, identity_aliases "
                     "FROM users WHERE id = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      OLOG_ERROR("auth_db: get_user_identity step failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   const char *rn = (const char *)sqlite3_column_text(stmt, 0);
   const char *pa = (const char *)sqlite3_column_text(stmt, 1);
   const char *ia = (const char *)sqlite3_column_text(stmt, 2);
   if (rn) {
      safe_strscpy(out->real_name, rn);
   }
   if (pa) {
      safe_strscpy(out->preferred_address, pa);
   }
   if (ia) {
      safe_strscpy(out->identity_aliases, ia);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int auth_db_set_user_identity(int user_id, const auth_user_identity_t *id) {
   if (!id) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   /* Empty strings → NULL: keep the column unset rather than persisting "" so
    * downstream "field is set?" checks read NULL uniformly. */
   const char *sql = "UPDATE users SET "
                     "  real_name         = NULLIF(?, ''), "
                     "  preferred_address = NULLIF(?, ''), "
                     "  identity_aliases  = NULLIF(?, '') "
                     "WHERE id = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(stmt, 1, id->real_name, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, id->preferred_address, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, id->identity_aliases, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: set_user_identity step failed");
      return AUTH_DB_FAILURE;
   }
   if (changes == 0) {
      return AUTH_DB_NOT_FOUND;
   }
   return AUTH_DB_SUCCESS;
}
