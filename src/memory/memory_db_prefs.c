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
 * Memory Database — Preference CRUD operations.
 *
 * Phase 6 split from memory_db.c — preference-side upsert / get / list /
 * search / delete.  Preferences are a separate noun from facts: stable
 * key→value pairs with reinforcement counters rather than free-text
 * statements.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Helper: Populate preference from statement row
 * ============================================================================= */

static void populate_pref_from_row(sqlite3_stmt *stmt, memory_preference_t *pref) {
   pref->id = sqlite3_column_int64(stmt, 0);
   pref->user_id = sqlite3_column_int(stmt, 1);

   const char *cat = (const char *)sqlite3_column_text(stmt, 2);
   if (cat) {
      safe_strscpy(pref->category, cat);
   }

   const char *val = (const char *)sqlite3_column_text(stmt, 3);
   if (val) {
      safe_strscpy(pref->value, val);
   }

   pref->confidence = (float)sqlite3_column_double(stmt, 4);

   const char *source = (const char *)sqlite3_column_text(stmt, 5);
   if (source) {
      safe_strscpy(pref->source, source);
   }

   pref->created_at = (time_t)sqlite3_column_int64(stmt, 6);
   pref->updated_at = (time_t)sqlite3_column_int64(stmt, 7);
   pref->reinforcement_count = sqlite3_column_int(stmt, 8);
}

/* =============================================================================
 * Preference Operations
 * ============================================================================= */

int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source,
                          const memory_provenance_t *prov) {
   if (!category || !value || !source) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Which preference row this is and whether its value changes: a new value
    * comes only from the conversation setting it, so its recorded sources reset;
    * the same value reinforced gains a source. */
   int64_t pref_id = 0;
   bool value_changed = false;
   /* The row and its sources change together (a savepoint nests inside a
    * caller's transaction). */
   const bool sp = sqlite3_exec(s_db.db, "SAVEPOINT pref_upsert", NULL, NULL, NULL) == SQLITE_OK;
   {
      sqlite3_stmt *cur = s_db.stmt_memory_pref_current;
      sqlite3_reset(cur);
      sqlite3_bind_int(cur, 1, user_id);
      sqlite3_bind_text(cur, 2, category, -1, SQLITE_STATIC);
      if (sqlite3_step(cur) == SQLITE_ROW) {
         pref_id = sqlite3_column_int64(cur, 0);
         const char *old = (const char *)sqlite3_column_text(cur, 1);
         value_changed = !old || strcmp(old, value) != 0;
      }
      sqlite3_reset(cur);
   }

   time_t now = time(NULL);
   sqlite3_stmt *stmt = s_db.stmt_memory_pref_upsert;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 4, confidence);
   sqlite3_bind_text(stmt, 5, source, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 6, (int64_t)now);
   sqlite3_bind_int64(stmt, 7, (int64_t)now);
   memory_db_internal_bind_provenance(stmt, 8, prov);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc == SQLITE_DONE) {
      if (pref_id == 0) {
         pref_id = sqlite3_last_insert_rowid(s_db.db); /* inserted */
      } else if (value_changed && s_db.stmt_memory_pref_sources_clear) {
         sqlite3_stmt *clr = s_db.stmt_memory_pref_sources_clear;
         sqlite3_reset(clr);
         sqlite3_bind_int64(clr, 1, pref_id);
         (void)sqlite3_step(clr);
         sqlite3_reset(clr);
      }
      if (prov) {
         memory_db_internal_source_add_locked(MEMORY_SOURCE_PREFERENCE, pref_id, prov->conv_id);
      }
   }
   if (sp) {
      if (rc != SQLITE_DONE) {
         sqlite3_exec(s_db.db, "ROLLBACK TO pref_upsert", NULL, NULL, NULL);
      }
      sqlite3_exec(s_db.db, "RELEASE pref_upsert", NULL, NULL, NULL);
   }

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: pref_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   OLOG_INFO("memory_db: upserted preference %s=%s for user %d", category, value, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_get(int user_id, const char *category, memory_preference_t *out_pref) {
   if (!category || !out_pref) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);

   int result = MEMORY_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, out_pref);
      result = MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_pref_list(int user_id,
                        memory_preference_t *out_prefs,
                        int max_prefs,
                        int offset,
                        int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_prefs || max_prefs <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_prefs);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < max_prefs && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, &out_prefs[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_search(int user_id,
                          const char *keywords,
                          memory_preference_t *out_prefs,
                          int max_prefs,
                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_prefs || max_prefs <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[256];
   memory_db_internal_build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_prefs);

   int count = 0;
   while (count < max_prefs && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, &out_prefs[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_delete(int user_id, const char *category) {
   if (!category) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}
