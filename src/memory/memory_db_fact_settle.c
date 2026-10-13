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
 * What a user's forget does with the facts it leaves: a merged fact is the
 * fact it went into, a replaced one is that fact's history.  Facts a going one
 * superseded either pass to the nearest fact that stays, or, when the whole
 * chain above them goes, are current again (and the relations the going facts
 * closed reopen).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"

#define IN_GOING "(SELECT id FROM fact_going)"

/* For every fact that stays but points at a going one, and every going fact
 * that closed a relation: the nearest fact up its superseded_by chain that
 * stays (landing; NULL when the chain's head goes too) and whether any link on
 * the way was a replace (rep).  Columns: start, at, rep, depth; the deepest row
 * per start is its answer. */
#define SETTLE_WALK_CTE                                                                            \
   "WITH RECURSIVE walk(start, at, rep, depth) AS ("                                               \
   "SELECT f.id, f.superseded_by, " MEMORY_LINK_KIND(                                              \
       "f", "0") ", 0 FROM memory_facts f "                                                        \
                 "WHERE +f.user_id = ?1 AND f.superseded_by IN " IN_GOING                          \
                 " AND f.id NOT IN " IN_GOING                                                      \
                 " UNION SELECT f.id, f.superseded_by, " MEMORY_LINK_KIND(                         \
                     "f", "0") ", 0 FROM memory_facts f "                                          \
                               "WHERE f.id IN " IN_GOING                                           \
                               " AND +f.user_id = ?1 AND EXISTS (SELECT 1 FROM "                   \
                               "memory_relations r WHERE r.closed_by_fact_id = f.id) "             \
                               "UNION ALL SELECT w.start, p.superseded_by, " MEMORY_LINK_KIND(     \
                                   "p", "w.rep") ", w.depth + 1 "                                  \
                                                 "FROM walk w JOIN memory_facts p ON p.id = w.at " \
                                                 "WHERE w.at IN " IN_GOING                         \
                                                 " AND w.depth < " MEMORY_CHAIN_DEPTH_SQL ") "

/* SETTLE_WALK_CTE reduced to one row per start: start, landing, rep. */
#define SETTLE_ANSWERS                                                                           \
   "SELECT start, CASE WHEN at IS NULL OR at IN " IN_GOING " THEN NULL ELSE at END AS landing, " \
   "rep FROM (SELECT start, at, rep, MAX(depth) FROM walk GROUP BY start)"

int memory_db_internal_going_reset_locked(void) {
   return sqlite3_exec(s_db.db,
                       "CREATE TEMP TABLE IF NOT EXISTS fact_going(id INTEGER PRIMARY KEY);"
                       "DELETE FROM fact_going;",
                       NULL, NULL, NULL) == SQLITE_OK
              ? MEMORY_DB_SUCCESS
              : MEMORY_DB_FAILURE;
}

int memory_db_internal_settle_going_locked(int user_id) {
   bool ok = sqlite3_exec(s_db.db,
                          "CREATE TEMP TABLE IF NOT EXISTS fact_settle(id INTEGER PRIMARY KEY, "
                          "landing INTEGER, rep INTEGER);"
                          "CREATE TEMP TABLE IF NOT EXISTS fact_settle_reopen(id INTEGER PRIMARY "
                          "KEY);"
                          "DELETE FROM fact_settle; DELETE FROM fact_settle_reopen;",
                          NULL, NULL, NULL) == SQLITE_OK;
   ok = ok &&
        memory_db_internal_exec_locked(SETTLE_WALK_CTE "INSERT INTO fact_settle " SETTLE_ANSWERS,
                                       user_id, 0, 0) == SQLITE_DONE;
   /* A fact that stays under a going one passes to the nearest that stays:
    * still that fact's history if any link on the way was a replace. */
   ok = ok && memory_db_internal_exec_locked(
                  "UPDATE memory_facts SET superseded_by = (SELECT s.landing FROM fact_settle s "
                  "WHERE s.id = memory_facts.id), superseded_kind = (SELECT s.rep FROM "
                  "fact_settle s WHERE s.id = memory_facts.id) WHERE user_id = ?1 AND id IN "
                  "(SELECT id FROM fact_settle WHERE landing IS NOT NULL) AND id NOT IN " IN_GOING,
                  user_id, 0, 0) == SQLITE_DONE;
   /* What a going fact closed is closed by the fact it passes to. */
   ok = ok && memory_db_internal_exec_locked(
                  "UPDATE memory_relations SET closed_by_fact_id = (SELECT s.landing FROM "
                  "fact_settle s WHERE s.id = memory_relations.closed_by_fact_id) WHERE "
                  "+user_id = ?1 AND closed_by_fact_id IN (SELECT id FROM fact_settle WHERE "
                  "landing IS NOT NULL AND id IN " IN_GOING ")",
                  user_id, 0, 0) == SQLITE_DONE;
   /* With nothing above it staying, what it closed on a fact that stays may
    * open again (memory_db_internal_restore_settled_locked).  Never a relation whose
    * fact goes: nothing would be behind it. */
   ok = ok && memory_db_internal_exec_locked(
                  "INSERT INTO fact_settle_reopen SELECT r.id FROM memory_relations r WHERE "
                  "+r.user_id = ?1 AND r.valid_to IS NOT NULL AND r.fact_id IS NOT NULL AND "
                  "r.fact_id NOT IN " IN_GOING " AND r.closed_by_fact_id IN (SELECT id FROM "
                  "fact_settle WHERE landing IS NULL AND id IN " IN_GOING ")",
                  user_id, 0, 0) == SQLITE_DONE;
   if (!ok) {
      OLOG_WARNING("memory_db: settling a forget for user %d failed: %s", user_id,
                   sqlite3_errmsg(s_db.db));
   }
   return ok ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_internal_restore_settled_locked(int user_id, int64_t *ids, int max, int *count) {
   *count = 0;
   /* The replaced values now current (their keeper chain went), reported;
    * a duplicate left current is the same fact, not news. */
   sqlite3_stmt *st = NULL;
   bool ok = sqlite3_prepare_v2(s_db.db,
                                "SELECT f.id FROM fact_settle s JOIN memory_facts f ON f.id = s.id "
                                "WHERE s.landing IS NULL AND s.rep = " MEMORY_REPLACED_SQL
                                " AND f.user_id = ?1 AND f.superseded_by IS NULL ORDER BY f.id",
                                -1, &st, NULL) == SQLITE_OK;
   if (ok) {
      sqlite3_bind_int(st, 1, user_id);
      int rc;
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         if (*count < max && ids) {
            ids[*count] = sqlite3_column_int64(st, 0);
         }
         (*count)++;
      }
      ok = rc == SQLITE_DONE;
   }
   sqlite3_finalize(st);
   ok =
       ok &&
       memory_db_internal_exec_locked(
           "UPDATE memory_facts SET superseded_at = NULL, merged_from_id = NULL, superseded_kind = "
           "" MEMORY_MERGED_SQL " WHERE user_id = ?1 AND superseded_by IS NULL AND id IN "
           "(SELECT id FROM fact_settle WHERE landing IS NULL)",
           user_id, 0, 0) == SQLITE_DONE;
   /* Reopened only once its closer is gone and its fact is current. */
   sqlite3_stmt *sel = NULL;
   if (ok) {
      ok = sqlite3_prepare_v2(s_db.db,
                              MEMORY_REOPEN_SELECT
                              "AND closed_by_fact_id IS NULL AND id IN (SELECT "
                              "id FROM fact_settle_reopen) AND EXISTS (SELECT 1 FROM "
                              "memory_facts f WHERE f.id = memory_relations.fact_id AND "
                              "f.superseded_by IS NULL)",
                              -1, &sel, NULL) == SQLITE_OK;
      if (ok) {
         sqlite3_bind_int(sel, 1, user_id);
      }
   }
   int reopened = 0;
   int kept_closed = 0;
   ok = ok && memory_db_internal_reopen_rows_locked(user_id, sel, &reopened, &kept_closed) ==
                  MEMORY_DB_SUCCESS;
   (void)sqlite3_exec(s_db.db, "DELETE FROM fact_settle; DELETE FROM fact_settle_reopen;", NULL,
                      NULL, NULL);
   if (!ok) {
      *count = 0;
      OLOG_WARNING("memory_db: bringing back what a forget left for user %d failed: %s", user_id,
                   sqlite3_errmsg(s_db.db));
   }
   return ok ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_internal_settle_count_locked(int user_id, int *restored, int64_t *first_id) {
   *restored = 0;
   if (first_id) {
      *first_id = 0;
   }
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               SETTLE_WALK_CTE "SELECT COUNT(*), MIN(a.start) FROM (" SETTLE_ANSWERS
                                               ") a WHERE a.landing IS NULL AND a.rep = "
                                               "" MEMORY_REPLACED_SQL
                                               " AND a.start NOT IN " IN_GOING,
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      rc = sqlite3_step(st);
      if (rc == SQLITE_ROW) {
         *restored = sqlite3_column_int(st, 0);
         if (first_id) {
            *first_id = sqlite3_column_int64(st, 1);
         }
      }
   }
   sqlite3_finalize(st);
   return rc == SQLITE_ROW ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}
