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
 * Superseded facts: listing them, and restoring one (a merge undone, with what
 * the merge moved; or a value a later one replaced by mistake).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_embeddings_internal.h"
#include "utils/string_utils.h"

int memory_db_fact_list_superseded(int user_id,
                                   int64_t keeper_id,
                                   const char *text_filter,
                                   memory_superseded_fact_t *out,
                                   int max,
                                   int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   if (user_id <= 0 || !out || max <= 0 || !count_out) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT f.id, f.superseded_by, COALESCE(f.superseded_at, 0), "
       "f.fact_text, COALESCE(k.fact_text, ''), f.superseded_kind FROM memory_facts f "
       "LEFT JOIN memory_facts k ON k.id = f.superseded_by AND k.user_id = f.user_id "
       "WHERE +f.user_id = ?1 AND f.superseded_by IS NOT NULL "
       "AND (?2 = 0 OR f.superseded_by = ?2) "
       "AND (?3 IS NULL OR instr(lower(f.fact_text), lower(?3)) > 0) "
       "ORDER BY f.superseded_at DESC, f.id DESC LIMIT ?4",
       -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: list superseded prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_int64(st, 2, keeper_id);
   if (text_filter && text_filter[0]) {
      sqlite3_bind_text(st, 3, text_filter, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 3);
   }
   sqlite3_bind_int(st, 4, max);
   int n = 0;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
      memory_superseded_fact_t *f = &out[n++];
      memset(f, 0, sizeof(*f));
      f->id = sqlite3_column_int64(st, 0);
      f->keeper_id = sqlite3_column_int64(st, 1);
      f->superseded_at = (time_t)sqlite3_column_int64(st, 2);
      const char *t = (const char *)sqlite3_column_text(st, 3);
      const char *k = (const char *)sqlite3_column_text(st, 4);
      safe_strscpy(f->text, t ? t : "");
      safe_strscpy(f->keeper_text, k ? k : "");
      f->replaced = sqlite3_column_int(st, 5) == MEMORY_SUPERSEDED_REPLACED;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      return MEMORY_DB_FAILURE;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_internal_reopen_rows_locked(int user_id,
                                          sqlite3_stmt *sel,
                                          int *reopened_out,
                                          int *kept_closed) {
   sqlite3_stmt *open_any = NULL;
   sqlite3_stmt *reopen = NULL;
   int reopened = 0;
   bool ok = sel != NULL &&
             sqlite3_prepare_v2(s_db.db,
                                "SELECT EXISTS (SELECT 1 FROM memory_relations WHERE "
                                "user_id = ?1 AND subject_entity_id = ?2 AND valid_to IS NULL AND "
                                "((relation = ?3 AND (?6 OR (COALESCE(object_entity_id, 0) = ?4 "
                                "AND COALESCE(object_value, '') = ?5))) OR (relation = ?7 AND "
                                "COALESCE(object_entity_id, 0) = ?4 AND "
                                "COALESCE(object_value, '') = ?5)))",
                                -1, &open_any, NULL) == SQLITE_OK &&
             sqlite3_prepare_v2(s_db.db,
                                "UPDATE memory_relations SET valid_to = NULL, "
                                "closed_by_fact_id = NULL WHERE id = ?1 AND user_id = ?2",
                                -1, &reopen, NULL) == SQLITE_OK;
   int step = SQLITE_DONE;
   while (ok && (step = sqlite3_step(sel)) == SQLITE_ROW) {
      const char *relation = (const char *)sqlite3_column_text(sel, 2);
      sqlite3_reset(open_any);
      sqlite3_bind_int(open_any, 1, user_id);
      sqlite3_bind_int64(open_any, 2, sqlite3_column_int64(sel, 1));
      sqlite3_bind_text(open_any, 3, relation ? relation : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(open_any, 4, sqlite3_column_int64(sel, 3));
      sqlite3_bind_text(open_any, 5, (const char *)sqlite3_column_text(sel, 4), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_int(open_any, 6, memory_db_relation_is_one_at_a_time(relation) ? 1 : 0);
      /* Nor "likes X" while "dislikes X" is open. */
      const char *opposite = memory_db_relation_contradictory_opposite(relation);
      if (opposite) {
         sqlite3_bind_text(open_any, 7, opposite, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(open_any, 7);
      }
      ok = sqlite3_step(open_any) == SQLITE_ROW;
      if (!ok) {
         break;
      }
      if (sqlite3_column_int(open_any, 0)) {
         (*kept_closed)++;
         continue;
      }
      sqlite3_reset(reopen);
      sqlite3_bind_int64(reopen, 1, sqlite3_column_int64(sel, 0));
      sqlite3_bind_int(reopen, 2, user_id);
      ok = sqlite3_step(reopen) == SQLITE_DONE;
      reopened += ok ? sqlite3_changes(s_db.db) : 0;
   }
   ok = ok && step == SQLITE_DONE; /* the whole set, or nothing commits */
   sqlite3_finalize(sel);
   sqlite3_finalize(open_any);
   sqlite3_finalize(reopen);
   *reopened_out = ok ? reopened : 0;
   return ok ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* The relations @p keeper_id's value closed on @p fact_id (by it, or by a
 * fact since merged into it), reopened as reopen_rows_locked does. */
static int reopen_relations_locked(int user_id,
                                   int64_t fact_id,
                                   int64_t keeper_id,
                                   int *reopened,
                                   int *kept_closed) {
   sqlite3_stmt *sel = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "WITH RECURSIVE " MEMORY_MERGED_UNDER_CTE(
                              "kept", "?3", "?1") " " MEMORY_REOPEN_SELECT
                                                  "AND fact_id = ?2 AND closed_by_fact_id IN "
                                                  "(SELECT id FROM kept)",
                          -1, &sel, NULL) != SQLITE_OK) {
      sqlite3_finalize(sel);
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(sel, 1, user_id);
   sqlite3_bind_int64(sel, 2, fact_id);
   sqlite3_bind_int64(sel, 3, keeper_id);
   return memory_db_internal_reopen_rows_locked(user_id, sel, reopened, kept_closed);
}

/* What merges moved off @p fact_id and the facts merged into it (relations
 * it stated, values it replaced), back on it now it is current again. */
static bool restore_moved_back_locked(int user_id, int64_t fact_id) {
   return memory_db_internal_exec_locked(
              "WITH RECURSIVE " MEMORY_MERGED_UNDER_CTE(
                  "mine", "?2", "?1") " UPDATE memory_relations SET fact_id = ?2, "
                                      "merged_from_fact_id = NULLIF(merged_from_fact_id, ?2) "
                                      "WHERE +user_id = ?1 AND merged_from_fact_id IN (SELECT id "
                                      "FROM mine)",
              user_id, fact_id, 0) == SQLITE_DONE &&
          memory_db_internal_exec_locked(
              "WITH RECURSIVE " MEMORY_MERGED_UNDER_CTE(
                  "mine", "?2", "?1") " UPDATE memory_facts SET superseded_by = ?2, "
                                      "merged_from_id = NULLIF(merged_from_id, ?2) WHERE "
                                      "+user_id = ?1 AND id != ?2 AND superseded_by IS NOT NULL "
                                      "AND merged_from_id IN (SELECT id FROM mine)",
              user_id, fact_id, 0) == SQLITE_DONE;
}

int memory_db_fact_restore(int user_id, int64_t fact_id, memory_restore_result_t *out) {
   if (out) {
      memset(out, 0, sizeof(*out));
   }
   if (user_id <= 0 || fact_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   int64_t keeper_id = 0;
   /* Whether a newer value stays current beside it: some link up its chain
    * is a replace, and the chain reaches a current fact (a walk that runs out
    * of steps says no). */
   bool newer_current = false;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           s_db.db,
           "WITH RECURSIVE up(id, next, rep, depth) AS (SELECT id, superseded_by, superseded_kind, "
           "0 FROM memory_facts WHERE id = ?1 AND user_id = ?2 AND superseded_by IS NOT NULL "
           "UNION ALL SELECT p.id, p.superseded_by, " MEMORY_LINK_KIND(
               "p", "u.rep") ", "
                             "u.depth + 1 FROM up u JOIN memory_facts p ON p.id = u.next WHERE "
                             "u.depth < "
                             "" MEMORY_CHAIN_DEPTH_SQL ") "
                             "SELECT (SELECT next FROM up WHERE depth = 0), MAX(rep), MAX(next IS "
                             "NULL) FROM up",
           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, fact_id);
      sqlite3_bind_int(st, 2, user_id);
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
         keeper_id = sqlite3_column_int64(st, 0);
         newer_current = sqlite3_column_int(st, 1) == MEMORY_SUPERSEDED_REPLACED &&
                         sqlite3_column_int(st, 2) != 0;
      }
   }
   sqlite3_finalize(st);
   if (keeper_id <= 0) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND; /* not the user's, or not superseded */
   }

   bool ok = auth_db_txn_begin_locked("memory_db: restore") == AUTH_DB_SUCCESS;
   ok = ok && memory_db_internal_exec_locked("UPDATE memory_facts SET superseded_by = NULL, "
                                             "superseded_at = NULL, merged_from_id = NULL, "
                                             "superseded_kind = " MEMORY_MERGED_SQL
                                             " WHERE id = ?2 AND user_id = ?1",
                                             user_id, fact_id, 0) == SQLITE_DONE;
   ok = ok && restore_moved_back_locked(user_id, fact_id);
   int reopened = 0;
   int kept_closed = 0;
   ok = ok && reopen_relations_locked(user_id, fact_id, keeper_id, &reopened, &kept_closed) ==
                  MEMORY_DB_SUCCESS;
   const bool committed = auth_db_txn_end_locked(ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE,
                                                 "memory_db: restore") == AUTH_DB_SUCCESS;
   AUTH_DB_UNLOCK();
   if (!ok || !committed) {
      return MEMORY_DB_FAILURE;
   }
   /* The fact cache holds current facts only: this one is current again.
    * Off the DB lock (a reload reads the DB under the cache's lock). */
   memory_embeddings_invalidate_cache_for_user(user_id);
   OLOG_INFO("memory_db: user %d restored fact %lld (was superseded by %lld; %d relation(s) "
             "reopened)",
             user_id, (long long)fact_id, (long long)keeper_id, reopened);
   if (out) {
      out->reopened = reopened;
      out->kept_closed = kept_closed;
      out->replaced = newer_current;
   }
   return MEMORY_DB_SUCCESS;
}
