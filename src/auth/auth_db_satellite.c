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
 * Satellite Mapping Database Module
 *
 * Provides persistent satellite-to-user mappings for DAP2 satellites.
 * Enables user assignment, location/HA area configuration, and enable/disable.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Helper: Read satellite_mapping_t from prepared statement result row
 * ============================================================================= */

static void read_mapping_row(sqlite3_stmt *stmt, satellite_mapping_t *out) {
   memset(out, 0, sizeof(*out));

   const char *uuid = (const char *)sqlite3_column_text(stmt, 0);
   if (uuid) {
      strncpy(out->uuid, uuid, sizeof(out->uuid) - 1);
   }

   const char *name = (const char *)sqlite3_column_text(stmt, 1);
   if (name) {
      strncpy(out->name, name, sizeof(out->name) - 1);
   }

   const char *location = (const char *)sqlite3_column_text(stmt, 2);
   if (location) {
      strncpy(out->location, location, sizeof(out->location) - 1);
   }

   const char *ha_area = (const char *)sqlite3_column_text(stmt, 3);
   if (ha_area) {
      strncpy(out->ha_area, ha_area, sizeof(out->ha_area) - 1);
   }

   out->user_id = sqlite3_column_int(stmt, 4);
   out->tier = sqlite3_column_int(stmt, 5);
   out->last_seen = (time_t)sqlite3_column_int64(stmt, 6);
   out->created_at = (time_t)sqlite3_column_int64(stmt, 7);
   out->enabled = sqlite3_column_int(stmt, 8) != 0;
}

/* =============================================================================
 * Satellite Mapping Operations
 * ============================================================================= */

int satellite_db_upsert(const satellite_mapping_t *mapping) {
   if (!mapping || mapping->uuid[0] == '\0') {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_upsert);
   sqlite3_bind_text(s_db.stmt_satellite_upsert, 1, mapping->uuid, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_satellite_upsert, 2, mapping->name, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_satellite_upsert, 3, mapping->location, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_satellite_upsert, 4, mapping->ha_area, -1, SQLITE_STATIC);
   if (mapping->user_id > 0)
      sqlite3_bind_int(s_db.stmt_satellite_upsert, 5, mapping->user_id);
   else
      sqlite3_bind_null(s_db.stmt_satellite_upsert, 5);
   sqlite3_bind_int(s_db.stmt_satellite_upsert, 6, mapping->tier);
   sqlite3_bind_int64(s_db.stmt_satellite_upsert, 7, (int64_t)mapping->last_seen);
   sqlite3_bind_int64(s_db.stmt_satellite_upsert, 8, (int64_t)mapping->created_at);
   sqlite3_bind_int(s_db.stmt_satellite_upsert, 9, mapping->enabled ? 1 : 0);

   int rc = sqlite3_step(s_db.stmt_satellite_upsert);
   sqlite3_reset(s_db.stmt_satellite_upsert);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("satellite_db_upsert: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

int satellite_db_get(const char *uuid, satellite_mapping_t *out) {
   if (!uuid || !out) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_get);
   sqlite3_bind_text(s_db.stmt_satellite_get, 1, uuid, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_satellite_get);

   if (rc == SQLITE_ROW) {
      read_mapping_row(s_db.stmt_satellite_get, out);
      sqlite3_reset(s_db.stmt_satellite_get);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_satellite_get);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
}

int satellite_db_delete(const char *uuid) {
   if (!uuid) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_delete);
   sqlite3_bind_text(s_db.stmt_satellite_delete, 1, uuid, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_satellite_delete);
   sqlite3_reset(s_db.stmt_satellite_delete);
   int changes = sqlite3_changes(s_db.db);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("satellite_db_delete: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int satellite_db_update_user(const char *uuid, int user_id) {
   if (!uuid) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_update_user);
   if (user_id > 0)
      sqlite3_bind_int(s_db.stmt_satellite_update_user, 1, user_id);
   else
      sqlite3_bind_null(s_db.stmt_satellite_update_user, 1);
   sqlite3_bind_text(s_db.stmt_satellite_update_user, 2, uuid, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_satellite_update_user);
   sqlite3_reset(s_db.stmt_satellite_update_user);
   int changes = sqlite3_changes(s_db.db);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("satellite_db_update_user: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int satellite_db_update_location(const char *uuid, const char *location, const char *ha_area) {
   if (!uuid) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_update_location);
   sqlite3_bind_text(s_db.stmt_satellite_update_location, 1, location ? location : "", -1,
                     SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_satellite_update_location, 2, ha_area ? ha_area : "", -1,
                     SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_satellite_update_location, 3, uuid, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_satellite_update_location);
   sqlite3_reset(s_db.stmt_satellite_update_location);
   int changes = sqlite3_changes(s_db.db);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("satellite_db_update_location: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int satellite_db_set_enabled(const char *uuid, bool enabled) {
   if (!uuid) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_set_enabled);
   sqlite3_bind_int(s_db.stmt_satellite_set_enabled, 1, enabled ? 1 : 0);
   sqlite3_bind_text(s_db.stmt_satellite_set_enabled, 2, uuid, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_satellite_set_enabled);
   sqlite3_reset(s_db.stmt_satellite_set_enabled);
   int changes = sqlite3_changes(s_db.db);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("satellite_db_set_enabled: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int satellite_db_update_last_seen(const char *uuid) {
   if (!uuid) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_update_last_seen);
   sqlite3_bind_int64(s_db.stmt_satellite_update_last_seen, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_satellite_update_last_seen, 2, uuid, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_satellite_update_last_seen);
   sqlite3_reset(s_db.stmt_satellite_update_last_seen);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("satellite_db_update_last_seen: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

int satellite_db_list(int (*callback)(const satellite_mapping_t *, void *), void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_satellite_list);

   int result = AUTH_DB_SUCCESS;
   while (sqlite3_step(s_db.stmt_satellite_list) == SQLITE_ROW) {
      satellite_mapping_t mapping;
      read_mapping_row(s_db.stmt_satellite_list, &mapping);

      if (callback(&mapping, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_satellite_list);
   AUTH_DB_UNLOCK();

   return result;
}

int satellite_db_ensure_local_pseudo(void) {
   /* Only create the row if it's missing — upsert preserves user_id / enabled /
    * ha_area on conflict, but calling upsert unconditionally would also bump
    * last_seen on every restart which adds noise. So check first. */
   satellite_mapping_t existing;
   int rc = satellite_db_get(LOCAL_PSEUDO_SATELLITE_UUID, &existing);
   if (rc == AUTH_DB_SUCCESS) {
      return AUTH_DB_SUCCESS;
   }

   satellite_mapping_t m;
   memset(&m, 0, sizeof(m));
   strncpy(m.uuid, LOCAL_PSEUDO_SATELLITE_UUID, sizeof(m.uuid) - 1);
   strncpy(m.name, "Local Device", sizeof(m.name) - 1);
   m.location[0] = '\0';
   m.ha_area[0] = '\0';
   m.user_id = 0; /* unassigned → backward-compat default (plays for all) */
   m.tier = LOCAL_PSEUDO_SATELLITE_TIER;
   m.enabled = true;
   m.last_seen = time(NULL);
   m.created_at = time(NULL);

   rc = satellite_db_upsert(&m);
   if (rc == AUTH_DB_SUCCESS) {
      OLOG_INFO("satellite: provisioned local pseudo-satellite (unassigned)");
   }
   return rc;
}

bool satellite_local_speaker_is_assigned_to(int user_id) {
   if (user_id <= 0) {
      return false;
   }
   satellite_mapping_t m;
   if (satellite_db_get(LOCAL_PSEUDO_SATELLITE_UUID, &m) != AUTH_DB_SUCCESS) {
      /* No mapping / DB error: treat as NOT explicitly assigned.  Unlike
       * plays_for_user (which fails OPEN so a needed alert is still heard), this
       * gates PRIVATE content — failing closed keeps it off a speaker we cannot
       * confirm is bound to this user. */
      return false;
   }
   /* Enabled AND bound to exactly this user.  The unassigned default (m.user_id
    * <= 0, "plays for all") is deliberately NOT a match. */
   return m.enabled && m.user_id > 0 && m.user_id == user_id;
}

bool satellite_local_speaker_plays_for_user(int event_user_id) {
   satellite_mapping_t m;
   int rc = satellite_db_get(LOCAL_PSEUDO_SATELLITE_UUID, &m);
   if (rc != AUTH_DB_SUCCESS) {
      /* Mapping missing or DB error — fail open rather than go silent on
       * notifications the user might need to hear. */
      return true;
   }

   /* Admin disable wins over everything, including system events. Otherwise
    * a user who explicitly silenced the daemon speaker would still get woken
    * up by system alerts. */
   if (!m.enabled) {
      return false;
   }

   /* System events (no user_id) play on any enabled speaker. */
   if (event_user_id <= 0) {
      return true;
   }

   if (m.user_id <= 0) {
      /* Unassigned → backward-compatible behavior: play for everyone. */
      return true;
   }
   return m.user_id == event_user_id;
}
