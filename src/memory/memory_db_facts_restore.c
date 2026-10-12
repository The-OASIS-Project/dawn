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
 * Superseded facts: listing them, restoring one (a merge undone, or a value
 * a later one replaced by mistake), and forgetting a fact together with the
 * facts merged into it.
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
#include "memory/memory_embeddings_internal.h"
#include "utils/string_utils.h"

/* Facts one forget takes with it: the fact and what was merged into it. */
#define FORGET_TREE_MAX 512

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
       "f.fact_text, COALESCE(k.fact_text, '') FROM memory_facts f "
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
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      return MEMORY_DB_FAILURE;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* Reopen the relations @p keeper_id's value closed on @p fact_id, unless a
 * value is open again already (that one, any for a one-at-a-time relation, or
 * its contradiction on the same object; counted in @p kept_closed).  Caller
 * holds the lock inside the transaction.  Reopened count, or -1. */
static int reopen_relations_locked(int user_id,
                                   int64_t fact_id,
                                   int64_t keeper_id,
                                   int *kept_closed) {
   sqlite3_stmt *sel = NULL;
   sqlite3_stmt *open_any = NULL;
   sqlite3_stmt *reopen = NULL;
   int reopened = 0;
   bool ok = sqlite3_prepare_v2(s_db.db,
                                "SELECT id, subject_entity_id, relation, "
                                "COALESCE(object_entity_id, 0), COALESCE(object_value, '') "
                                "FROM memory_relations WHERE user_id = ?1 AND fact_id = ?2 AND "
                                "closed_by_fact_id = ?3 AND valid_to IS NOT NULL",
                                -1, &sel, NULL) == SQLITE_OK &&
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
   if (ok) {
      sqlite3_bind_int(sel, 1, user_id);
      sqlite3_bind_int64(sel, 2, fact_id);
      sqlite3_bind_int64(sel, 3, keeper_id);
   }
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
   return ok ? reopened : -1;
}

int memory_db_fact_restore(int user_id, int64_t fact_id, int *reopened_out, int *kept_closed_out) {
   if (reopened_out) {
      *reopened_out = 0;
   }
   if (kept_closed_out) {
      *kept_closed_out = 0;
   }
   if (user_id <= 0 || fact_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   int64_t keeper_id = 0;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT superseded_by FROM memory_facts WHERE id = ?1 AND user_id = ?2 "
                          "AND superseded_by IS NOT NULL",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, fact_id);
      sqlite3_bind_int(st, 2, user_id);
      if (sqlite3_step(st) == SQLITE_ROW) {
         keeper_id = sqlite3_column_int64(st, 0);
      }
   }
   sqlite3_finalize(st);
   if (keeper_id <= 0) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND; /* not the user's, or not superseded */
   }

   bool ok = auth_db_txn_begin_locked("memory_db: restore") == AUTH_DB_SUCCESS;
   st = NULL;
   ok = ok && sqlite3_prepare_v2(s_db.db,
                                 "UPDATE memory_facts SET superseded_by = NULL, "
                                 "superseded_at = NULL WHERE id = ?1 AND user_id = ?2",
                                 -1, &st, NULL) == SQLITE_OK;
   if (ok) {
      sqlite3_bind_int64(st, 1, fact_id);
      sqlite3_bind_int(st, 2, user_id);
      ok = sqlite3_step(st) == SQLITE_DONE;
   }
   sqlite3_finalize(st);
   int kept_closed = 0;
   const int reopened = ok ? reopen_relations_locked(user_id, fact_id, keeper_id, &kept_closed)
                           : -1;
   ok = ok && reopened >= 0;
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
   if (reopened_out) {
      *reopened_out = reopened;
   }
   if (kept_closed_out) {
      *kept_closed_out = kept_closed;
   }
   return MEMORY_DB_SUCCESS;
}

/* @p fact_id and every fact merged into it, at any depth, deepest first and
 * @p fact_id last (a merge cycle can't put it earlier), into @p ids.  -1 on
 * error or when it holds more than @p max. */
static int forget_tree_locked(int user_id, int64_t fact_id, int64_t *ids, int max) {
   sqlite3_stmt *st = NULL;
   int n = 0;
   int rc = SQLITE_ERROR;
   if (sqlite3_prepare_v2(
           s_db.db,
           "WITH RECURSIVE tree(id, depth) AS ("
           "  SELECT id, 0 FROM memory_facts WHERE id = ?1 AND user_id = ?2 "
           "  UNION "
           "  SELECT f.id, t.depth + 1 FROM memory_facts f JOIN tree t ON "
           "  f.superseded_by = t.id WHERE +f.user_id = ?2 AND t.depth < ?3) "
           "SELECT id FROM tree GROUP BY id ORDER BY id = ?1, MAX(depth) DESC LIMIT ?3",
           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, fact_id);
      sqlite3_bind_int(st, 2, user_id);
      sqlite3_bind_int(st, 3, max + 1);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         if (n == max) {
            n = -1; /* more than a forget takes at once */
            break;
         }
         ids[n++] = sqlite3_column_int64(st, 0);
      }
   }
   sqlite3_finalize(st);
   return (n < 0 || (rc != SQLITE_DONE && rc != SQLITE_ROW)) ? -1 : n;
}

/* Merges into a tree being forgotten (an extraction finishing meanwhile) are
 * caught by collecting it again; past this many rounds the fact stays. */
#define FORGET_ROUNDS_MAX 3

int memory_db_fact_forget(int64_t fact_id, int user_id, int *merged_out) {
   if (merged_out) {
      *merged_out = 0;
   }
   if (user_id <= 0 || fact_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   int64_t *ids = malloc(FORGET_TREE_MAX * sizeof(*ids));
   if (!ids) {
      return MEMORY_DB_FAILURE;
   }
   /* What was merged into it goes first, deepest first, and the fact only once
    * nothing is merged into it any more: a merged fact left pointing at a
    * deleted one would be current again (superseded_by is ON DELETE SET NULL),
    * and a failure leaves the fact, with nothing pointing at a deleted one. */
   int rc = MEMORY_DB_FAILURE;
   for (int round = 0; round < FORGET_ROUNDS_MAX; round++) {
      AUTH_DB_LOCK_OR_RETURN((free(ids), MEMORY_DB_FAILURE));
      const int n = forget_tree_locked(user_id, fact_id, ids, FORGET_TREE_MAX);
      AUTH_DB_UNLOCK();
      if (n < 0) {
         OLOG_WARNING("memory_db: forget of fact %lld for user %d: merge tree unreadable or "
                      "over %d facts; fact kept",
                      (long long)fact_id, user_id, FORGET_TREE_MAX);
         rc = MEMORY_DB_FAILURE;
         break;
      }
      if (n == 0) {
         rc = round == 0 ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
         break;
      }
      if (n == 1) { /* nothing merged into it: the fact itself */
         rc = memory_db_fact_delete(fact_id, user_id);
         break;
      }
      rc = MEMORY_DB_SUCCESS;
      for (int i = 0; i < n - 1 && rc == MEMORY_DB_SUCCESS; i++) {
         rc = memory_db_fact_delete(ids[i], user_id);
         if (rc == MEMORY_DB_SUCCESS && merged_out) {
            (*merged_out)++;
         }
      }
      if (rc != MEMORY_DB_SUCCESS) {
         break;
      }
      rc = MEMORY_DB_FAILURE; /* rounds ran out with merges still arriving: kept */
   }
   free(ids);
   return rc;
}
