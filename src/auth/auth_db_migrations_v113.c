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
 * Schema migration v113: memory_facts.origin (how a fact came to be stored);
 * an outside kind (core/outside_kind.h) on facts, relations, summaries and
 * preferences, and a sticky one on conversations (the floor a later batch
 * starts from), all filled in for what is already stored; the consolidation
 * record tables (tables only; nothing writes them); and the fact-category index
 * dropped (nothing reads by it).
 *
 * Each table's fill runs only in the run that adds its column, in one
 * transaction with it: a rerun (an earlier attempt added the columns, then a
 * later step failed) leaves alone the rows new code has tagged since.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/automated_event.h"
#include "core/outside_kind.h"
#include "llm/llm_context_text.h"
#include "logging.h"
#include "memory/memory_types.h"
#include "tools/tool_own_results.h"
#include "tools/tool_registry.h"

/* The values the SQL below writes. */
_Static_assert(OUTSIDE_WEB == 1 && OUTSIDE_EMAIL == 2 && OUTSIDE_RESEARCH == 3 &&
                   OUTSIDE_MESSAGING == 4 && OUTSIDE_DOCUMENT == 5 && OUTSIDE_OTHER == 7 &&
                   OUTSIDE_UNKNOWN == 8,
               "v113's SQL writes these outside kinds");
_Static_assert(MEMORY_ORIGIN_IMPORT == 2 && MEMORY_ORIGIN_NOTE == 3,
               "v113's SQL writes these origins");

/* Run @p sql, logging @p what on failure. */
static int v113_exec(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v113 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

/* Add the integer @p column to @p table unless it is there; *@p added says
 * whether it was. */
static int v113_add_column(sqlite3 *db, const char *table, const char *column, bool *added) {
   *added = false;
   if (auth_db_column_exists(db, table, column)) {
      return AUTH_DB_SUCCESS;
   }
   char sql[160];
   snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s INTEGER NOT NULL DEFAULT 0", table,
            column);
   if (v113_exec(db, sql, column) != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   *added = true;
   return AUTH_DB_SUCCESS;
}

/* A result DAWN framed as someone else's text (llm_third_party_frame). */
#define V113_EMAIL_FRAMED CONV_FRAMED_SQL("messages", TOOL_FRAME_EMAIL)
#define V113_WEB_FRAMED CONV_FRAMED_SQL("messages", TOOL_FRAME_WEB)

/* Which stored conversations read outside text, and from which message.
 * Outside text is a framed result, the result of a tool whose results aren't
 * DAWN's own (by its call's name, v113_own: tools/tool_own_results.h's
 * ALWAYS list; or whose call isn't found), what CONV_ROW_OUTSIDE_KIND_SQL finds (a post, a
 * turn context's outside item, an attached document, a job's result, a
 * device's data), or anything in an SMS conversation (anyone can send one).
 * Only a framed result proves framing ran, so a conversation's cleanness is
 * judged only when it began after the first framed result this database
 * holds (a result read early in it shaped what came after); a taint found in
 * any conversation counts.  v113_conv: every conversation, whether
 * it is judged, and its first message carrying outside text with that kind
 * (NULL when none). */
/* clang-format off */
static const char k_judge_sql[] =
    "DROP TABLE IF EXISTS temp.v113_mark;"
    "DROP TABLE IF EXISTS temp.v113_calls;"
    "DROP TABLE IF EXISTS temp.v113_conv;"
    "CREATE TEMP TABLE v113_mark (id INTEGER PRIMARY KEY, conv INTEGER NOT NULL, "
    "created_at INTEGER NOT NULL, k INTEGER NOT NULL);"
    "INSERT INTO v113_mark SELECT id, conversation_id, created_at, k FROM ("
    "SELECT id, conversation_id, created_at, CASE "
    "WHEN kind IS NULL AND role = 'tool' AND " V113_EMAIL_FRAMED " THEN 2 "
    "WHEN kind IS NULL AND role = 'tool' AND " V113_WEB_FRAMED " THEN 1 "
    "ELSE " CONV_ROW_OUTSIDE_KIND_SQL("messages") " END AS k FROM messages) WHERE k IS NOT NULL;"
    /* Every conversation's tool calls, by call id. */
    "CREATE TEMP TABLE v113_calls (conv INTEGER NOT NULL, call_id TEXT NOT NULL, "
    "name TEXT, action TEXT);"
    "INSERT INTO v113_calls SELECT a.conversation_id, json_extract(j.value, '$.id'), "
    "json_extract(j.value, '$.function.name'), CASE WHEN json_valid(json_extract(j.value, "
    "'$.function.arguments')) THEN json_extract(json_extract(j.value, '$.function.arguments'), "
    "'$.action') END FROM messages a, json_each(a.tool_calls) j WHERE a.role = 'assistant' "
    "AND a.tool_calls IS NOT NULL AND json_valid(a.tool_calls) AND json_type(a.tool_calls) = "
    "'array' AND json_extract(j.value, '$.id') IS NOT NULL;"
    "CREATE INDEX temp.v113_calls_id ON v113_calls(conv, call_id);"
    "INSERT OR IGNORE INTO v113_mark SELECT t.id, t.conversation_id, t.created_at, 7 FROM "
    "messages t WHERE t.role = 'tool' AND t.kind IS NULL AND NOT EXISTS (SELECT 1 FROM "
    "v113_calls c WHERE c.conv = t.conversation_id AND c.call_id = t.tool_call_id AND c.name IN "
    "(SELECT name FROM v113_own) AND NOT (c.name = '" TOOL_OWN_RESULTS_EXCEPT_TOOL "' AND "
    "c.action IS '" TOOL_OWN_RESULTS_EXCEPT_ACTION "'));"
    /* An SMS conversation: from its first message on. */
    "INSERT OR IGNORE INTO v113_mark SELECT MIN(m.id), m.conversation_id, MIN(m.created_at), 4 "
    "FROM messages m JOIN conversations c ON c.id = m.conversation_id WHERE c.origin = "
    "'messaging:sms' GROUP BY m.conversation_id;"
    "CREATE INDEX temp.v113_mark_conv ON v113_mark(conv, id);"
    "CREATE TEMP TABLE v113_conv (conv INTEGER PRIMARY KEY, judged INTEGER NOT NULL, "
    "first_id INTEGER, k INTEGER);"
    "INSERT INTO v113_conv (conv, judged) SELECT id, COALESCE(created_at >= (SELECT "
    "MIN(created_at) FROM v113_mark WHERE k IN (1, 2)), 0) FROM conversations;"
    "UPDATE v113_conv SET first_id = (SELECT MIN(m.id) FROM v113_mark m WHERE m.conv = "
    "v113_conv.conv);"
    "UPDATE v113_conv SET k = (SELECT m.k FROM v113_mark m WHERE m.id = v113_conv.first_id);"
    /* A continuation read what the conversations it continues read, before
     * any message of its own (first_id 0, the closest one's first kind, from
     * v113_mark, which this leaves alone), and can't be judged when one of
     * them can't. */
    "WITH RECURSIVE chain(id, anc, d) AS (SELECT id, continued_from, 1 FROM conversations WHERE "
    "continued_from IS NOT NULL UNION ALL SELECT chain.id, c.continued_from, chain.d + 1 FROM "
    "chain JOIN conversations c ON c.id = chain.anc WHERE c.continued_from IS NOT NULL AND "
    "chain.d < " CONV_CONTINUATION_DEPTH_MAX_SQL ") "
    "UPDATE v113_conv SET k = COALESCE((SELECT m.k FROM chain JOIN v113_mark m ON m.conv = "
    "chain.anc WHERE chain.id = v113_conv.conv ORDER BY chain.d, m.id LIMIT 1), k), "
    "first_id = CASE WHEN EXISTS (SELECT 1 FROM chain JOIN v113_mark m ON m.conv = chain.anc "
    "WHERE chain.id = v113_conv.conv) THEN 0 ELSE first_id END, judged = "
    "CASE WHEN EXISTS (SELECT 1 FROM chain WHERE chain.id = v113_conv.conv AND chain.anc NOT IN "
    "(SELECT conv FROM v113_conv WHERE judged = 1)) THEN 0 ELSE judged END;";
/* clang-format on */

/* A sourced row's kind.  A taint any source shows wins (its own batch's
 * conversation up to the end of its range first, else the first another
 * source conversation read; its range there isn't kept); with none, UNKNOWN
 * when a source can't be judged or the conversation it came from was deleted
 * since, else clean.  @p T is the table, @p S its sources table and key. */
#define V113_SOURCED_KIND(T, S, KEY)                                                            \
   "COALESCE((SELECT v.k FROM v113_conv v WHERE v.conv = " T ".source_conversation_id AND "     \
   "v.first_id <= COALESCE(" T ".source_msg_id_end, v.first_id)), (SELECT v.k FROM " S " s "    \
   "JOIN v113_conv v ON v.conv = s.conversation_id WHERE s." KEY " = " T ".id AND "             \
   "s.conversation_id IS NOT " T ".source_conversation_id AND v.first_id IS NOT NULL ORDER BY " \
   "v.first_id LIMIT 1), CASE WHEN EXISTS (SELECT 1 FROM " S " s WHERE s." KEY " = " T ".id "   \
   "AND s.conversation_id NOT IN (SELECT conv FROM v113_conv WHERE judged = 1)) OR (" T         \
   ".source_conversation_id IS NOT NULL AND " T ".source_conversation_id NOT IN (SELECT conv "  \
   "FROM v113_conv WHERE judged = 1)) OR (" T ".source_conversation_id IS NULL AND " T          \
   ".origin_unsourced = 0) THEN 8 ELSE 0 END)"

/* A row with no source at all is clean only when its origin says the user
 * gave it outside any conversation (an import, a note's gloss, for facts);
 * anything else (a remember, a voice session never saved, a conversation
 * deleted since) can't be judged. */
#define V113_UNSOURCED(T, S, KEY)                                                                 \
   T ".source_conversation_id IS NULL AND NOT EXISTS (SELECT 1 FROM " S " s WHERE s." KEY " = " T \
     ".id)"

/* SQL assembled from the macros above, one clause a line. */
/* clang-format off */
static const char k_facts_sql[] =
    "UPDATE memory_facts SET outside = CASE "
    "WHEN " V113_UNSOURCED("memory_facts", "memory_fact_sources", "fact_id") " "
    "THEN (CASE WHEN origin IN (2, 3) THEN 0 ELSE 8 END) "
    "ELSE " V113_SOURCED_KIND("memory_facts", "memory_fact_sources", "fact_id") " END;";
/* Relations have no origin: one with no source can't be judged. */
static const char k_relations_sql[] =
    "UPDATE memory_relations SET outside = CASE "
    "WHEN " V113_UNSOURCED("memory_relations", "memory_relation_sources", "relation_id") " "
    "THEN 8 "
    "ELSE " V113_SOURCED_KIND("memory_relations", "memory_relation_sources", "relation_id")
    " END;";
/* clang-format on */

/* A summary keeps no sources table: its own conversation, or UNKNOWN. */
static const char k_summaries_sql[] =
    "UPDATE memory_summaries SET outside = COALESCE((SELECT CASE WHEN v.first_id <= "
    "COALESCE(memory_summaries.source_msg_id_end, v.first_id) THEN v.k WHEN v.judged = 1 THEN 0 "
    "END FROM v113_conv v WHERE v.conv = memory_summaries.source_conversation_id), 8);";

/* A preference's value carries no range to judge. */
static const char k_preferences_sql[] = "UPDATE memory_preferences SET outside = 8;";

/* Each conversation's floor: the first outside text it or a conversation it
 * continues read, else UNKNOWN when one of them can't be judged, else clean. */
static const char k_conversations_sql[] =
    "UPDATE conversations SET outside = (SELECT COALESCE(v.k, CASE WHEN v.judged = 0 THEN 8 ELSE 0 "
    "END) FROM v113_conv v WHERE v.conv = conversations.id);";

/* Imported facts are the user's own word (they now decay as told ones do);
 * a note's gloss says so in origin too. */
static const char k_origin_sql[] =
    "UPDATE memory_facts SET origin = 2, source = 'explicit' WHERE source = 'import';"
    "UPDATE memory_facts SET origin = 3 WHERE note_doc_id IS NOT NULL;";

/* The column each fill writes, and the fill: run only in the run that adds
 * the column. */
static const struct {
   const char *table;
   const char *sql;
} k_fills[] = {
   { "memory_facts", k_facts_sql },          { "memory_relations", k_relations_sql },
   { "memory_summaries", k_summaries_sql },  { "memory_preferences", k_preferences_sql },
   { "conversations", k_conversations_sql },
};
#define V113_FILLS (sizeof(k_fills) / sizeof(k_fills[0]))

/* v113_own: the tools whose results are DAWN's own. */
static int v113_own_table(sqlite3 *db) {
   /* Not the framing tools: a result of theirs stored before they framed
    * someone else's text can hold it unframed. */
   static const char *const own[] = { TOOL_OWN_RESULTS_ALWAYS_NAMES };
   if (v113_exec(db,
                 "DROP TABLE IF EXISTS temp.v113_own;"
                 "CREATE TEMP TABLE v113_own (name TEXT PRIMARY KEY);",
                 "own tools") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "INSERT INTO temp.v113_own (name) VALUES (?)", -1, &st, NULL) !=
       SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   int rc = SQLITE_DONE;
   for (size_t i = 0; i < sizeof(own) / sizeof(own[0]) && rc == SQLITE_DONE; i++) {
      sqlite3_reset(st);
      sqlite3_bind_text(st, 1, own[i], -1, SQLITE_STATIC);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* The steps, inside the savepoint. */
static int v113_steps(sqlite3 *db) {
   bool origin_added = false, added[V113_FILLS], any = false;
   if (v113_add_column(db, "memory_facts", "origin", &origin_added) != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   for (size_t i = 0; i < V113_FILLS; i++) {
      if (v113_add_column(db, k_fills[i].table, "outside", &added[i]) != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
      any = any || added[i];
   }
   if (v113_exec(db, MEMORY_CONSOLIDATION_TABLES_SQL, "consolidation record tables") !=
           AUTH_DB_SUCCESS ||
       v113_exec(db, "DROP INDEX IF EXISTS idx_memory_facts_user_category", "category index") !=
           AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   if (origin_added && v113_exec(db, k_origin_sql, "origin fill") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   if (!any) {
      return AUTH_DB_SUCCESS;
   }
   if (v113_own_table(db) != AUTH_DB_SUCCESS ||
       v113_exec(db, k_judge_sql, "outside scan") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   for (size_t i = 0; i < V113_FILLS; i++) {
      if (added[i] && v113_exec(db, k_fills[i].sql, k_fills[i].table) != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
   }
   return v113_exec(db,
                    "DROP TABLE temp.v113_conv; DROP TABLE temp.v113_calls; "
                    "DROP TABLE temp.v113_mark; DROP TABLE temp.v113_own;",
                    "scan tables");
}

int auth_db_migrations_v113(sqlite3 *db) {
   if (v113_exec(db, "SAVEPOINT v113", "savepoint") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   if (v113_steps(db) != AUTH_DB_SUCCESS) {
      sqlite3_exec(db, "ROLLBACK TO v113; RELEASE v113;", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   return v113_exec(db, "RELEASE v113", "commit");
}
