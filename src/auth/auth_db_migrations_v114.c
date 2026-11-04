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
 * Schema migration v114: documents.outside, the outside kind a note's text
 * carries (core/outside_kind.h), so a chunk recalled into a turn reads as the
 * user's own words only when its note is, and document_versions.outside, the
 * kind a version's document carried, restored with it.  Each table is filled
 * in the run that adds its column: a research run's report is RESEARCH; a
 * note whose gloss fact learned from outside text carries that fact's kind;
 * a version takes its document's kind (when the document is gone, RESEARCH
 * if a research run names it, else 0); the rest stay NONE.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/outside_kind.h"
#include "logging.h"

_Static_assert(OUTSIDE_RESEARCH == 3, "v114's SQL writes this outside kind");

/* clang-format off */
static const char k_fill_documents_sql[] =
    "UPDATE documents SET outside = 3 WHERE id IN (SELECT report_doc_id FROM research_runs "
    "WHERE report_doc_id IS NOT NULL);"
    "UPDATE documents SET outside = (SELECT f.outside FROM memory_facts f WHERE f.note_doc_id = "
    "documents.id AND f.outside NOT IN (" OUTSIDE_UNTAINTED_SQL ") ORDER BY f.id LIMIT 1) "
    "WHERE outside = 0 AND filetype = 'note' AND EXISTS (SELECT 1 FROM memory_facts f WHERE "
    "f.note_doc_id = documents.id AND f.outside NOT IN (" OUTSIDE_UNTAINTED_SQL "));";
static const char k_fill_versions_sql[] =
    "UPDATE document_versions SET outside = COALESCE((SELECT d.outside FROM documents d WHERE "
    "d.id = document_versions.document_id), CASE WHEN document_id IN (SELECT report_doc_id "
    "FROM research_runs WHERE report_doc_id IS NOT NULL) THEN 3 ELSE 0 END);";
/* clang-format on */

/* Add @p table's outside column and fill it with @p fill_sql, unless it is
 * there: a fresh database has it (the base schema), and a rerun (an earlier
 * attempt added it, a later step failed) leaves alone the rows new code has
 * tagged since. */
static bool v114_add_outside(sqlite3 *db, const char *table, const char *fill_sql) {
   if (auth_db_column_exists(db, table, "outside")) {
      return true;
   }
   char alter[96];
   snprintf(alter, sizeof(alter), "ALTER TABLE %s ADD COLUMN outside INTEGER NOT NULL DEFAULT 0",
            table);
   char *errmsg = NULL;
   if (sqlite3_exec(db, alter, NULL, NULL, &errmsg) != SQLITE_OK ||
       sqlite3_exec(db, fill_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v114 migration (%s outside kind) failed: %s", table,
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return false;
   }
   return true;
}

int auth_db_migrations_v114(sqlite3 *db) {
   /* One commit for both, all or nothing; documents first (versions copy it). */
   bool ok = sqlite3_exec(db, "SAVEPOINT v114", NULL, NULL, NULL) == SQLITE_OK;
   ok = ok && v114_add_outside(db, "documents", k_fill_documents_sql) &&
        v114_add_outside(db, "document_versions", k_fill_versions_sql);
   if (!ok) {
      sqlite3_exec(db, "ROLLBACK TO v114", NULL, NULL, NULL);
   }
   if (sqlite3_exec(db, "RELEASE v114", NULL, NULL, NULL) != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); /* never leave it open */
      ok = false;
   }
   return ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}
