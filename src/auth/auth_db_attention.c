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
 * auth_db accessor layer for SAGE: attention_rules CRUD + attention_log inserts.
 * enum fields (rule_type/direction/notify) are stored as INTEGER; thresholds are
 * concrete REALs (catalog defaults are resolved before insert).  Ad-hoc prepared
 * statements (cold path).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_attention.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/path_utils.h"
#include "logging.h"

static int64_t now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int auth_db_attention_rule_insert(const sage_watch_t *w, int64_t *out_id) {
   if (!w) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql =
       "INSERT INTO attention_rules (user_id, name, metric, rule_type, direction, threshold, "
       "hysteresis, slope_per_min, slope_window_sec, absence_after_sec, notify, ttl_min, enabled, "
       "named, muted_until, source, created_at, updated_at) "
       "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_rule_insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int64_t ts = now_ms();
   sqlite3_bind_int(stmt, 1, w->user_id);
   sqlite3_bind_text(stmt, 2, w->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, w->metric, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, (int)w->rule_type);
   sqlite3_bind_int(stmt, 5, (int)w->direction);
   sqlite3_bind_double(stmt, 6, w->threshold);
   sqlite3_bind_double(stmt, 7, w->hysteresis);
   sqlite3_bind_double(stmt, 8, w->slope_per_min);
   sqlite3_bind_int(stmt, 9, w->slope_window_sec);
   sqlite3_bind_int(stmt, 10, w->absence_after_sec);
   sqlite3_bind_int(stmt, 11, (int)w->notify);
   sqlite3_bind_int(stmt, 12, w->ttl_min);
   sqlite3_bind_int(stmt, 13, w->enabled ? 1 : 0);
   sqlite3_bind_int(stmt, 14, w->named ? 1 : 0);
   sqlite3_bind_int64(stmt, 15, (sqlite3_int64)w->muted_until);
   sqlite3_bind_text(stmt, 16, w->source_tag, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 17, (sqlite3_int64)ts);
   sqlite3_bind_int64(stmt, 18, (sqlite3_int64)ts);

   int rc = sqlite3_step(stmt);
   int64_t new_id = (rc == SQLITE_DONE) ? (int64_t)sqlite3_last_insert_rowid(s_db.db) : 0;
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: attention_rule_insert step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();
   if (out_id) {
      *out_id = new_id;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_attention_rule_update(int user_id, int64_t id, const sage_watch_t *w) {
   if (!w) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* rule_type is updatable so a metric's single watch can switch kind (e.g.
    * threshold -> slope) on edit; without it a re-add with a new rule_type would
    * silently keep the old kind and ignore the new trigger fields. */
   const char *sql =
       "UPDATE attention_rules SET name=?, rule_type=?, direction=?, threshold=?, hysteresis=?, "
       "slope_per_min=?, slope_window_sec=?, absence_after_sec=?, notify=?, ttl_min=?, named=?, "
       "updated_at=? WHERE id=? AND user_id=?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_rule_update failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt, 1, w->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, (int)w->rule_type);
   sqlite3_bind_int(stmt, 3, (int)w->direction);
   sqlite3_bind_double(stmt, 4, w->threshold);
   sqlite3_bind_double(stmt, 5, w->hysteresis);
   sqlite3_bind_double(stmt, 6, w->slope_per_min);
   sqlite3_bind_int(stmt, 7, w->slope_window_sec);
   sqlite3_bind_int(stmt, 8, w->absence_after_sec);
   sqlite3_bind_int(stmt, 9, (int)w->notify);
   sqlite3_bind_int(stmt, 10, w->ttl_min);
   sqlite3_bind_int(stmt, 11, w->named ? 1 : 0);
   sqlite3_bind_int64(stmt, 12, (sqlite3_int64)now_ms());
   sqlite3_bind_int64(stmt, 13, (sqlite3_int64)id);
   sqlite3_bind_int(stmt, 14, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: attention_rule_update step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int auth_db_attention_rule_set_enabled(int user_id, int64_t id, bool enabled) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE attention_rules SET enabled=?, muted_until=0, updated_at=? "
                          "WHERE id=? AND user_id=?",
                          -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_rule_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
   sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now_ms());
   sqlite3_bind_int64(stmt, 3, (sqlite3_int64)id);
   sqlite3_bind_int(stmt, 4, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: attention_rule_set_enabled step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int auth_db_attention_rule_delete(int user_id, int64_t id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM attention_rules WHERE id=? AND user_id=?", -1,
                          &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_rule_delete failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: attention_rule_delete step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int auth_db_attention_rule_list(int user_id, sage_watch_t *out, int max, int *out_count) {
   if (!out || max <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* user_id == 0 => all users (cache load); else one user.  SECURITY: 0 is a
    * trusted-caller-only sentinel — only attention_reload() (in-process, not
    * user-driven) passes it.  Every tool/WebUI path resolves the caller's real
    * user_id (>= 1 via tool_get_current_user_id), so an untrusted caller can
    * never reach the all-users branch.  Do not plumb an untrusted value here. */
   const char *sql =
       (user_id > 0)
           ? "SELECT id, user_id, name, metric, rule_type, direction, threshold, hysteresis, "
             "slope_per_min, slope_window_sec, absence_after_sec, notify, ttl_min, enabled, "
             "muted_until, source, named FROM attention_rules WHERE user_id=? ORDER BY id"
           : "SELECT id, user_id, name, metric, rule_type, direction, threshold, hysteresis, "
             "slope_per_min, slope_window_sec, absence_after_sec, notify, ttl_min, enabled, "
             "muted_until, source, named FROM attention_rules ORDER BY user_id, id";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_rule_list failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   if (user_id > 0) {
      sqlite3_bind_int(stmt, 1, user_id);
   }

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      sage_watch_t *w = &out[n];
      memset(w, 0, sizeof(*w));
      w->id = sqlite3_column_int64(stmt, 0);
      w->user_id = sqlite3_column_int(stmt, 1);
      const char *name = (const char *)sqlite3_column_text(stmt, 2);
      const char *metric = (const char *)sqlite3_column_text(stmt, 3);
      if (name) {
         safe_strncpy(w->name, name, sizeof(w->name));
      }
      if (metric) {
         safe_strncpy(w->metric, metric, sizeof(w->metric));
      }
      w->rule_type = (sage_rule_type_t)sqlite3_column_int(stmt, 4);
      w->direction = (sage_direction_t)sqlite3_column_int(stmt, 5);
      w->threshold = sqlite3_column_double(stmt, 6);
      w->hysteresis = sqlite3_column_double(stmt, 7);
      w->slope_per_min = sqlite3_column_double(stmt, 8);
      w->slope_window_sec = sqlite3_column_int(stmt, 9);
      w->absence_after_sec = sqlite3_column_int(stmt, 10);
      w->notify = (sage_notify_t)sqlite3_column_int(stmt, 11);
      w->ttl_min = sqlite3_column_int(stmt, 12);
      w->enabled = sqlite3_column_int(stmt, 13) != 0;
      w->muted_until = sqlite3_column_int64(stmt, 14);
      const char *src = (const char *)sqlite3_column_text(stmt, 15);
      if (src) {
         safe_strncpy(w->source_tag, src, sizeof(w->source_tag));
      }
      w->named = sqlite3_column_int(stmt, 16) != 0;
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (out_count) {
      *out_count = n;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_attention_rule_count(int user_id, int *out_count) {
   if (!out_count) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM attention_rules WHERE user_id=?", -1,
                          &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_rule_count failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *out_count = count;
   return AUTH_DB_SUCCESS;
}

int auth_db_attention_log_insert(int user_id,
                                 const char *event_key,
                                 const char *category,
                                 const char *source,
                                 const char *summary,
                                 const char *gate_rule,
                                 const char *privacy,
                                 const char *surface,
                                 int64_t delivered_at_ms) {
   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "INSERT INTO attention_log (user_id, event_key, category, source, summary, "
                     "gate_rule, privacy, surface, delivered_at, logged_at) "
                     "VALUES (?,?,?,?,?,?,?,?,?,?)";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare attention_log_insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, event_key ? event_key : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, category ? category : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, source ? source : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, summary ? summary : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, gate_rule ? gate_rule : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, privacy ? privacy : "public", -1, SQLITE_TRANSIENT);
   if (surface) {
      sqlite3_bind_text(stmt, 8, surface, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 8);
   }
   if (delivered_at_ms > 0) {
      sqlite3_bind_int64(stmt, 9, (sqlite3_int64)delivered_at_ms);
   } else {
      sqlite3_bind_null(stmt, 9);
   }
   sqlite3_bind_int64(stmt, 10, (sqlite3_int64)now_ms());

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: attention_log_insert step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}
