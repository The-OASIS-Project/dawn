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
 * Schema migration v104: memory_facts.superseded_kind (a merged duplicate or a
 * replaced value: forgetting a fact takes its duplicates and brings back what
 * it replaced); merged_from_id / merged_from_fact_id (what a merge moved onto
 * the fact kept, and from where, so restoring the duplicate moves it back);
 * and an index on memory_relations.closed_by_fact_id, which every fact delete
 * needs (the foreign key's ON DELETE SET NULL looks it up).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* Merge links the legacy walk below follows: chains are tens of links, so
 * only a cycle (the code before superseding was guarded allowed them)
 * reaches it. */
#define V104_CHAIN_DEPTH_MAX_SQL "256"

/* Run @p sql, logging @p what on failure. */
static int v104_exec(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v104 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

/* Add @p column to @p table with @p decl unless it is there (a retried v104). */
static int v104_add_column(sqlite3 *db, const char *table, const char *column, const char *sql) {
   return auth_db_column_exists(db, table, column) ? AUTH_DB_SUCCESS : v104_exec(db, sql, column);
}

int auth_db_migrations_v104(sqlite3 *db) {
   if (v104_add_column(db, "memory_facts", "superseded_kind",
                       "ALTER TABLE memory_facts ADD COLUMN superseded_kind INTEGER NOT NULL "
                       "DEFAULT 0") != AUTH_DB_SUCCESS ||
       v104_add_column(db, "memory_facts", "merged_from_id",
                       "ALTER TABLE memory_facts ADD COLUMN merged_from_id INTEGER REFERENCES "
                       "memory_facts(id) ON DELETE SET NULL") != AUTH_DB_SUCCESS ||
       v104_add_column(db, "memory_relations", "merged_from_fact_id",
                       "ALTER TABLE memory_relations ADD COLUMN merged_from_fact_id INTEGER "
                       "REFERENCES memory_facts(id) ON DELETE SET NULL") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   /* Every fact delete looks these up (ON DELETE SET NULL). */
   if (v104_exec(db,
                 "CREATE INDEX IF NOT EXISTS idx_memory_relations_closed_by ON "
                 "memory_relations(closed_by_fact_id) WHERE closed_by_fact_id IS NOT NULL;"
                 "CREATE INDEX IF NOT EXISTS idx_memory_facts_merged_from ON "
                 "memory_facts(merged_from_id) WHERE merged_from_id IS NOT NULL;"
                 "CREATE INDEX IF NOT EXISTS idx_memory_relations_merged_from ON "
                 "memory_relations(merged_from_fact_id) WHERE merged_from_fact_id IS NOT NULL",
                 "indexes") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   /* A fact superseded by itself (the unguarded code allowed it) is current. */
   if (v104_exec(db,
                 "UPDATE memory_facts SET superseded_by = NULL, superseded_at = NULL WHERE "
                 "superseded_by = id",
                 "self-superseded facts") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   /* Rows superseded before this count as merged, except where the data says
    * otherwise: a fact whose relation its keeper closed was replaced (a merge
    * never closes anything; closed_by_fact_id exists from v103 on).
    * Idempotent, so a retried v104 runs it again. */
   if (v104_exec(db,
                 "UPDATE memory_facts SET superseded_kind = 1 WHERE superseded_by IS NOT NULL "
                 "AND EXISTS (SELECT 1 FROM memory_relations r WHERE r.fact_id = "
                 "memory_facts.id AND r.closed_by_fact_id = memory_facts.superseded_by)",
                 "superseded_kind backfill") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   /* A merge now hands the fact kept what the duplicate stated and the values
    * it replaced, marked with where they came from; one made before didn't.
    * Apply that to the rows it left: each merged fact's head (the first fact
    * up its merges that isn't itself merged) is found once, so a chain of any
    * length moves in one go (the depth bound only stops a cycle the old code
    * allowed).  What a merged fact closed stays on it: restore and reopen
    * look through merged facts. */
   return v104_exec(
       db,
       "DROP TABLE IF EXISTS temp.v104_head;"
       "CREATE TEMP TABLE v104_head AS WITH RECURSIVE h(id, at, d) AS ("
       "SELECT id, superseded_by, 0 FROM memory_facts WHERE superseded_by IS NOT NULL AND "
       "superseded_kind = 0 UNION ALL SELECT h.id, f.superseded_by, h.d + 1 FROM h JOIN "
       "memory_facts f ON f.id = h.at WHERE f.superseded_by IS NOT NULL AND "
       "f.superseded_kind = 0 AND h.d < " V104_CHAIN_DEPTH_MAX_SQL ") "
       "SELECT id, at AS head FROM (SELECT id, at, MAX(d) FROM h GROUP BY id);"
       "UPDATE memory_relations SET merged_from_fact_id = COALESCE(merged_from_fact_id, "
       "fact_id), fact_id = (SELECT head FROM v104_head WHERE v104_head.id = "
       "memory_relations.fact_id) WHERE fact_id IN (SELECT id FROM v104_head);"
       "UPDATE memory_facts SET merged_from_id = COALESCE(merged_from_id, superseded_by), "
       "superseded_by = (SELECT head FROM v104_head WHERE v104_head.id = "
       "memory_facts.superseded_by) WHERE superseded_kind = 1 AND superseded_by IN (SELECT id "
       "FROM v104_head);"
       "DROP TABLE v104_head;",
       "merge leftovers to the fact kept");
}
