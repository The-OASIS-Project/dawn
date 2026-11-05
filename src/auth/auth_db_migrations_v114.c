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
 * Schema migration v114: documents.outside, the outside kind a document's
 * text carries (core/outside_kind.h), so a chunk recalled into a turn reads as
 * the user's own words only when its document is (a note, or one the user
 * marks as their own), and document_versions.outside, the kind a version's
 * document carried, restored with it.  Each table is filled in the run that
 * adds its column: a research run's report is RESEARCH; a note whose gloss
 * fact learned from outside text carries that fact's kind; else a note the
 * model saved, edited or appended to (document_manage, naming it by its name
 * or id) carries the floor of the conversations those calls ran in, the
 * earliest tainted call first, else UNKNOWN if one is, else NONE; any other
 * document other than a note is DOCUMENT; the other notes (typed in the
 * WebUI) stay NONE.  First, in the same run, a conversation holding a stored
 * result of a tool that frames only others' text (tools/tool_own_results.h's
 * FRAMED list) is raised to OTHER: such a result stored before the tool
 * framed outside text can hold it unframed, and the live registry now reads
 * it as DAWN's own wherever a stored history is scanned.  Of document_manage
 * only a list counts (others' shared documents' names); its other results
 * name or quote the user's own notes, and counting them would raise every
 * conversation that saved a note, and so the note.  A version takes
 * its document's kind, at least DOCUMENT for a document other than a note.
 * A version names its document by id alone (document_versions has only
 * document_id, user_id, filename, text and archived_at), and a v113 database
 * can reuse an id (the highest-id document deleted, a note created), so the
 * kind is taken only from a document of the same user created no later than
 * the version was archived.  Otherwise (the document gone, or the id reused)
 * it is RESEARCH if a research run names the id, else DOCUMENT: what it was
 * can't be told, and a deleted upload recovered as a note must not read as
 * the user's words.  documents.own, the user's vouch for a document's text
 * (kept apart from where it came from), starts at 0 everywhere: a note is
 * the user's own through its kind already.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/outside_kind.h"
#include "logging.h"
#include "tools/tool_own_results.h"

_Static_assert(OUTSIDE_RESEARCH == 3 && OUTSIDE_DOCUMENT == 5 && OUTSIDE_OTHER == 7,
               "v114's SQL writes these kinds");

/* The FRAMED list as SQL ('a', 'b', ...); a name added or removed there
 * fails to compile here until this matches. */
#define V114_SQL_LIST5_(a, b, c, d, e) "'" a "', '" b "', '" c "', '" d "', '" e "'"
#define V114_SQL_LIST5(...) V114_SQL_LIST5_(__VA_ARGS__)
#define V114_FRAMED_SQL V114_SQL_LIST5(TOOL_OWN_RESULTS_FRAMED_NAMES)

/* clang-format off */
static const char k_fill_documents_sql[] =
    "DROP TABLE IF EXISTS temp.v114_calls;"
    /* Every stored call of a FRAMED tool, by conversation and call id. */
    "CREATE TEMP TABLE v114_calls (conv INTEGER NOT NULL, mid INTEGER NOT NULL, call_id TEXT, "
    "name TEXT NOT NULL, args TEXT);"
    "INSERT INTO v114_calls SELECT a.conversation_id, a.id, json_extract(j.value, '$.id'), "
    "json_extract(j.value, '$.function.name'), CASE WHEN json_valid(json_extract(j.value, "
    "'$.function.arguments')) THEN json_extract(j.value, '$.function.arguments') END FROM "
    "messages a, json_each(a.tool_calls) j WHERE a.role = 'assistant' AND a.tool_calls IS NOT "
    "NULL AND json_valid(a.tool_calls) AND json_type(a.tool_calls) = 'array' AND "
    "json_extract(j.value, '$.function.name') IN (" V114_FRAMED_SQL ");"
    "CREATE INDEX temp.v114_calls_id ON v114_calls(conv, call_id);"
    "UPDATE conversations SET outside = 7 WHERE outside IN (" OUTSIDE_UNTAINTED_SQL ") AND "
    "EXISTS (SELECT 1 FROM messages t JOIN v114_calls k ON k.conv = t.conversation_id AND "
    "k.call_id = t.tool_call_id WHERE t.conversation_id = conversations.id AND t.role = 'tool' "
    "AND (k.name != 'document_manage' OR json_extract(k.args, '$.action') = 'list'));"
    "UPDATE documents SET outside = 3 WHERE id IN (SELECT report_doc_id FROM research_runs "
    "WHERE report_doc_id IS NOT NULL);"
    "UPDATE documents SET outside = (SELECT f.outside FROM memory_facts f WHERE f.note_doc_id = "
    "documents.id AND f.outside NOT IN (" OUTSIDE_UNTAINTED_SQL ") ORDER BY f.id LIMIT 1) "
    "WHERE outside = 0 AND filetype = 'note' AND EXISTS (SELECT 1 FROM memory_facts f WHERE "
    "f.note_doc_id = documents.id AND f.outside NOT IN (" OUTSIDE_UNTAINTED_SQL "));"
    "UPDATE documents SET outside = COALESCE((SELECT v.outside FROM v114_calls k JOIN "
    "conversations v ON v.id = k.conv WHERE v.user_id = documents.user_id AND k.name = "
    "'document_manage' AND json_extract(k.args, '$.action') IN ('save_note', 'edit', 'append') "
    "AND (json_extract(k.args, '$.label') = documents.filename COLLATE NOCASE OR "
    "CAST(json_extract(k.args, '$.id') AS INTEGER) = documents.id) ORDER BY v.outside IN "
    "(" OUTSIDE_UNTAINTED_SQL "), v.outside = 0, k.mid LIMIT 1), 0) "
    "WHERE outside = 0 AND filetype = 'note';"
    "UPDATE documents SET outside = 5 WHERE outside = 0 AND filetype != 'note';"
    "DROP TABLE temp.v114_calls;";
static const char k_fill_versions_sql[] =
    "UPDATE document_versions SET outside = COALESCE((SELECT CASE WHEN d.filetype != 'note' AND "
    "d.outside IN (" OUTSIDE_UNTAINTED_SQL ") THEN 5 ELSE d.outside END FROM documents d WHERE "
    "d.id = document_versions.document_id AND d.user_id = document_versions.user_id AND "
    "d.created_at <= document_versions.archived_at), CASE WHEN document_id IN (SELECT report_doc_id "
    "FROM research_runs WHERE report_doc_id IS NOT NULL) THEN 3 ELSE 5 END);";
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

/* Add documents.own (0 for every row), unless it is there. */
static bool v114_add_own(sqlite3 *db) {
   if (auth_db_column_exists(db, "documents", "own")) {
      return true;
   }
   char *errmsg = NULL;
   if (sqlite3_exec(db, "ALTER TABLE documents ADD COLUMN own INTEGER NOT NULL DEFAULT 0", NULL,
                    NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v114 migration (documents own) failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return false;
   }
   return true;
}

int auth_db_migrations_v114(sqlite3 *db) {
   /* One commit for all, all or nothing; documents first (versions copy it). */
   bool ok = sqlite3_exec(db, "SAVEPOINT v114", NULL, NULL, NULL) == SQLITE_OK;
   ok = ok && v114_add_outside(db, "documents", k_fill_documents_sql) && v114_add_own(db) &&
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
