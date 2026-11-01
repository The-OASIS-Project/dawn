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
 * The v113 migration on a v112 database: a fact's origin from its old source
 * (import) and note link, each memory row's outside kind judged from the
 * framed results its conversations hold (or UNKNOWN), the consolidation record
 * tables, and the category index gone for good.  A migrated database has the
 * schema a new one has, and a rerun leaves rows tagged since alone.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <dirent.h>
#include <libgen.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/outside_kind.h"
#include "memory/memory_types.h"
#include "test_tmp.h"
#include "unity.h"

static char s_path[TEST_TMP_PATH_MAX];
static char s_fresh_path[TEST_TMP_PATH_MAX];

static void exec_ok(const char *sql) {
   char *err = NULL;
   if (sqlite3_exec(s_db.db, sql, NULL, NULL, &err) != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : sql);
   }
   sqlite3_free(err);
}

static int64_t q_int(const char *sql) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   int64_t v = -1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      v = sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   return v;
}

static int fact_outside(const char *text) {
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT outside FROM memory_facts WHERE fact_text = '%s'", text);
   return (int)q_int(sql);
}

static int fact_origin(const char *text) {
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT origin FROM memory_facts WHERE fact_text = '%s'", text);
   return (int)q_int(sql);
}

static bool index_exists(const char *name) {
   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = '%s'", name);
   return q_int(sql) > 0;
}

/* A v112 database: v113's columns, tables and index removal undone. */
static void downgrade_to_v112(void) {
   exec_ok("PRAGMA foreign_keys=OFF;"
           "DROP TABLE memory_consolidation_ops;"
           "DROP TABLE memory_consolidation_batches;"
           "ALTER TABLE memory_facts DROP COLUMN origin;"
           "ALTER TABLE memory_facts DROP COLUMN outside;"
           "ALTER TABLE memory_relations DROP COLUMN outside;"
           "ALTER TABLE memory_summaries DROP COLUMN outside;"
           "ALTER TABLE memory_preferences DROP COLUMN outside;"
           "ALTER TABLE conversations DROP COLUMN outside;"
           "CREATE INDEX idx_memory_facts_user_category ON memory_facts(user_id, category);"
           "DELETE FROM schema_version;"
           "INSERT INTO schema_version (version) VALUES (112);"
           "PRAGMA foreign_keys=ON;");
}

/* Conversations and memory as a v112 install stored them.
 *   10 begun at 1000; its framed web result (at 1001) is the database's
 *      first, so judging starts at 1001 and 10 can't be judged.
 *   20 begun at 2000: a weather result (DAWN's own, unframed), then a
 *      framed email (205).
 *   30 begun at 3000: nothing from outside.
 *   40 begun at 4000: a document attached to the user's first message.
 *   50 begun at 5000 with a framed email; deleted before the upgrade.
 *   60 begun at 6000: an unframed search result (not DAWN's own).
 *   70 begun at 7000: a background job's result.
 *   80 begun at 8000, continuing 20: nothing from outside itself.
 *   90 begun at 900, continued by 95 at 9500: neither can be judged.
 *   85 begun at 850 (not judged), with an unframed search result.
 *   75 begun at 7500: a turn context naming an outside document's chunk.
 *   76 begun at 7600: a device's relayed data.
 *   77 begun at 7700: one of DAWN's posts, then the user's reply.
 *   99 an SMS conversation begun at 9900.
 * 30 also holds a turn context whose items aren't from outside. */
static void seed_v112(void) {
   exec_ok(
       "PRAGMA foreign_keys=OFF;"
       "INSERT INTO users (id, username, password_hash, created_at) VALUES (1, 'jon', 'x', 1);"
       "INSERT INTO conversations (id, user_id, created_at, updated_at, continued_from) VALUES "
       "(10, 1, 1000, 1000, NULL), (20, 1, 2000, 2000, NULL), (30, 1, 3000, 3000, NULL), "
       "(40, 1, 4000, 4000, NULL), (50, 1, 5000, 5000, NULL), (60, 1, 6000, 6000, NULL), "
       "(70, 1, 7000, 7000, NULL), (80, 1, 8000, 8000, 20), (90, 1, 900, 900, NULL), "
       "(95, 1, 9500, 9500, 90), (85, 1, 850, 850, NULL), (99, 1, 9900, 9900, NULL), "
       "(75, 1, 7500, 7500, NULL), (76, 1, 7600, 7600, NULL), (77, 1, 7700, 7700, NULL);"
       "UPDATE conversations SET origin = 'messaging:sms' WHERE id = 99;"
       "INSERT INTO messages (id, conversation_id, role, content, tool_calls, tool_call_id, "
       "kind, created_at) VALUES "
       "(101, 10, 'user', 'find a page', NULL, NULL, NULL, 1000),"
       "(102, 10, 'tool', '--- WEB CONTENT (dawn-ctx-0a1b2c3d) ---\nA page.\n"
       "--- END WEB CONTENT (dawn-ctx-0a1b2c3d) ---', NULL, 'w1', NULL, 1001),"
       "(201, 20, 'user', 'what is the weather', NULL, NULL, NULL, 2000),"
       "(202, 20, 'assistant', '', '[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
       "{\"name\":\"weather\",\"arguments\":\"{}\"}}]', NULL, NULL, 2001),"
       "(203, 20, 'tool', 'Sunny, 20 C', NULL, 'c1', NULL, 2002),"
       "(204, 20, 'user', 'read my mail', NULL, NULL, NULL, 2003),"
       "(205, 20, 'tool', 'Result:\n--- EMAIL CONTENT ---\nFrom: Dawn Smith\n"
       "--- END EMAIL CONTENT ---', NULL, 'c2', NULL, 2004),"
       "(206, 20, 'assistant', 'Dawn wrote.', NULL, NULL, NULL, 2005),"
       "(301, 30, 'user', 'my dog is Ash', NULL, NULL, NULL, 3000),"
       "(302, 30, 'assistant', 'Noted.', NULL, NULL, NULL, 3001),"
       "(303, 30, 'user', '[M2 memory_fact told 2026-01-02] Jon sits outside often', NULL, "
       "NULL, 'turn_context', 3002),"
       "(751, 75, 'user', '[M1 document_chunk outside 2026-01-02] The plan', NULL, NULL, "
       "'turn_context', 7500),"
       "(752, 75, 'user', 'what does it say', NULL, NULL, NULL, 7501),"
       "(761, 76, 'user', '[DEVICE DATA] Speak this information naturally to the user: x', "
       "NULL, NULL, NULL, 7600),"
       "(771, 77, 'assistant', 'Your briefing: the news', NULL, NULL, NULL, 7700),"
       "(772, 77, 'user', 'thanks', NULL, NULL, NULL, 7701),"
       "(401, 40, 'user', 'see this [ATTACHED DOCUMENT: plan.pdf]\nText', NULL, NULL, NULL, "
       "4000),"
       "(402, 40, 'assistant', 'Read it.', NULL, NULL, NULL, 4001),"
       "(601, 60, 'user', 'look it up', NULL, NULL, NULL, 6000),"
       "(602, 60, 'assistant', '', '[{\"id\":\"s1\",\"type\":\"function\",\"function\":"
       "{\"name\":\"search\",\"arguments\":\"{}\"}}]', NULL, NULL, 6001),"
       "(603, 60, 'tool', 'Some result', NULL, 's1', NULL, 6002),"
       "(701, 70, 'user', '[automated background-job update] done', NULL, NULL, 'envelope', "
       "7000),"
       "(702, 70, 'assistant', 'The job is done.', NULL, NULL, NULL, 7001),"
       "(801, 80, 'user', 'go on', NULL, NULL, NULL, 8000),"
       "(901, 90, 'user', 'old', NULL, NULL, NULL, 900),"
       "(951, 95, 'user', 'new', NULL, NULL, NULL, 9500),"
       "(851, 85, 'user', 'look up', NULL, NULL, NULL, 850),"
       "(852, 85, 'assistant', '', '[{\"id\":\"q1\",\"type\":\"function\",\"function\":"
       "{\"name\":\"search\",\"arguments\":\"{}\"}}]', NULL, NULL, 851),"
       "(853, 85, 'tool', 'A result', NULL, 'q1', NULL, 852),"
       "(854, 85, 'user', 'thanks', NULL, NULL, NULL, 853),"
       "(991, 99, 'user', 'hi from a phone', NULL, NULL, NULL, 9900);"
       "INSERT INTO documents (id, user_id, filename, filepath, filetype, file_hash, num_chunks, "
       "created_at) VALUES (7, 1, 'n', 'n', 'note', 'h', 1, 1);"
       /* text, source, conversation, range, unsourced, note */
       "INSERT INTO memory_facts (user_id, fact_text, source, source_conversation_id, "
       "source_msg_id_start, source_msg_id_end, origin_unsourced, note_doc_id) VALUES "
       "(1, 'imported', 'import', NULL, NULL, NULL, 1, NULL),"
       "(1, 'gloss', 'explicit', NULL, NULL, NULL, 1, 7),"
       "(1, 'voice only', 'explicit', NULL, NULL, NULL, 1, NULL),"
       "(1, 'before judging', 'inferred', 10, 101, 102, 0, NULL),"
       "(1, 'before the email', 'inferred', 20, 201, 203, 0, NULL),"
       "(1, 'after the email', 'inferred', 20, 204, 206, 0, NULL),"
       "(1, 'clean', 'explicit', 30, 301, 302, 0, NULL),"
       "(1, 'remembered', 'explicit', 30, 0, 0, 0, NULL),"
       "(1, 'document', 'inferred', 40, 401, 402, 0, NULL),"
       "(1, 'also from 90', 'inferred', 30, 301, 302, 0, NULL),"
       "(1, 'also from 20', 'inferred', 30, 301, 302, 0, NULL),"
       "(1, 'email and old', 'inferred', 20, 204, 206, 0, NULL),"
       "(1, 'from deleted 50', 'inferred', 50, 501, 502, 0, NULL),"
       "(1, 'searched', 'inferred', 60, 601, 603, 0, NULL),"
       "(1, 'job said', 'inferred', 70, 701, 702, 0, NULL),"
       "(1, 'unjudged', 'inferred', 90, 901, 901, 0, NULL),"
       "(1, 'after the search', 'inferred', 85, 854, 854, 0, NULL),"
       "(1, 'by text', 'inferred', 99, 991, 991, 0, NULL),"
       "(1, 'from context', 'inferred', 75, 751, 752, 0, NULL),"
       "(1, 'device said', 'inferred', 76, 761, 761, 0, NULL),"
       "(1, 'after the post', 'inferred', 77, 771, 772, 0, NULL),"
       "(1, 'continued mail', 'inferred', 80, 801, 801, 0, NULL),"
       "(1, 'continued old', 'inferred', 95, 951, 951, 0, NULL);"
       "INSERT INTO message_unprompted (message_id) VALUES (771);"
       "INSERT INTO memory_fact_sources (fact_id, conversation_id) SELECT id, 10 FROM "
       "memory_facts WHERE fact_text = 'email and old';"
       "INSERT INTO memory_fact_sources (fact_id, conversation_id) SELECT id, 90 FROM "
       "memory_facts WHERE fact_text IN ('also from 90', 'email and old');"
       "INSERT INTO memory_fact_sources (fact_id, conversation_id) SELECT id, 20 FROM "
       "memory_facts WHERE fact_text IN ('also from 20', 'email and old');"
       "INSERT INTO memory_fact_sources (fact_id, conversation_id) SELECT id, 50 FROM "
       "memory_facts WHERE fact_text = 'from deleted 50';"
       "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name) VALUES "
       "(1, 1, 'Jon', 'person', 'jon');"
       "INSERT INTO memory_relations (id, user_id, subject_entity_id, relation, object_value, "
       "source_conversation_id, source_msg_id_start, source_msg_id_end) VALUES "
       "(1, 1, 1, 'reads', 'mail', 20, 204, 206),"
       "(2, 1, 1, 'searched', 'web', 10, 101, 102),"
       "(3, 1, 1, 'has_pet', 'Ash', 30, 301, 302),"
       "(4, 1, 1, 'said', 'hi', NULL, NULL, NULL);"
       "INSERT INTO memory_summaries (id, user_id, session_id, summary, source_conversation_id, "
       "source_msg_id_start, source_msg_id_end) VALUES "
       "(1, 1, 's20', 'mail', 20, 201, 206),"
       "(2, 1, 's30', 'dog', 30, 301, 302),"
       "(3, 1, 'voice', 'unsaved', NULL, NULL, NULL),"
       "(4, 1, 's80', 'went on', 80, 801, 801);"
       "INSERT INTO memory_preferences (user_id, category, value) VALUES (1, 'tea', 'green');"
       "PRAGMA foreign_keys=ON;");
   /* Conversation 50 deleted the way the daemon does it (its facts' sources
    * cleared first: clear_memory_provenance). */
   exec_ok("UPDATE memory_facts SET source_conversation_id = NULL, source_msg_id_start = NULL, "
           "source_msg_id_end = NULL, origin_unsourced = 1 WHERE source_conversation_id = 50;"
           "DELETE FROM memory_fact_sources WHERE conversation_id = 50;"
           "DELETE FROM messages WHERE conversation_id = 50;"
           "DELETE FROM conversations WHERE id = 50;");
}

/* Each test's database has its own name: the upgrade backs it up under its
 * name and the second, and two tests can start within one second. */
static int s_test_no;

/* The database files and pre-upgrade backups a test made. */
static void remove_db_files(const char *path) {
   char side[TEST_TMP_PATH_MAX + 8];
   unlink(path);
   snprintf(side, sizeof(side), "%s-wal", path);
   unlink(side);
   snprintf(side, sizeof(side), "%s-shm", path);
   unlink(side);
   char copy[TEST_TMP_PATH_MAX], copy2[TEST_TMP_PATH_MAX], dir[TEST_TMP_PATH_MAX + 16];
   snprintf(copy, sizeof(copy), "%s", path);
   snprintf(copy2, sizeof(copy2), "%s", path);
   snprintf(dir, sizeof(dir), "%s/backups", dirname(copy));
   const char *base = basename(copy2);
   DIR *d = opendir(dir);
   for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
      if (strncmp(e->d_name, base, strlen(base)) == 0 && e->d_name[strlen(base)] == '.') {
         char full[TEST_TMP_PATH_MAX + 300];
         snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
         unlink(full);
      }
   }
   if (d) {
      closedir(d);
   }
}

void setUp(void) {
   char name[64];
   snprintf(name, sizeof(name), "dawn_test_mig_v113_%d.db", ++s_test_no);
   test_tmp_path(s_path, sizeof(s_path), name);
   remove_db_files(s_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path));
   downgrade_to_v112();
   seed_v112();
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path)); /* runs v113 */
}

void tearDown(void) {
   auth_db_shutdown();
   remove_db_files(s_path);
}

static void test_origin_from_import_and_note(void) {
   TEST_ASSERT_EQUAL_INT64(AUTH_DB_SCHEMA_VERSION, q_int("SELECT version FROM schema_version"));
   TEST_ASSERT_EQUAL_INT(MEMORY_ORIGIN_IMPORT, fact_origin("imported"));
   TEST_ASSERT_EQUAL_INT64(
       1, q_int("SELECT COUNT(*) FROM memory_facts WHERE fact_text = 'imported' AND "
                "source = 'explicit'"));
   TEST_ASSERT_EQUAL_INT64(0, q_int("SELECT COUNT(*) FROM memory_facts WHERE source = 'import'"));
   TEST_ASSERT_EQUAL_INT(MEMORY_ORIGIN_NOTE, fact_origin("gloss"));
   TEST_ASSERT_EQUAL_INT(MEMORY_ORIGIN_CONVERSATION, fact_origin("voice only"));
   TEST_ASSERT_EQUAL_INT(MEMORY_ORIGIN_CONVERSATION, fact_origin("clean"));
}

static void test_facts_outside_kind(void) {
   /* No source: clean only when the user gave it outside any conversation
    * (an import, a note's gloss); otherwise (a voice session never saved, a
    * conversation deleted since) it can't be judged. */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, fact_outside("imported"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, fact_outside("gloss"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_UNKNOWN, fact_outside("voice only"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_UNKNOWN, fact_outside("from deleted 50"));
   /* Begun before the first framed result: not judged clean, but a taint found
    * in it counts. */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_UNKNOWN, fact_outside("unjudged"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, fact_outside("before judging"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, fact_outside("after the search"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_MESSAGING, fact_outside("by text"));
   /* Judged by what its conversation read up to the end of its batch; DAWN's
    * own weather result is no outside text. */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, fact_outside("before the email"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, fact_outside("after the email"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, fact_outside("clean"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, fact_outside("remembered"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, fact_outside("document"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, fact_outside("searched"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, fact_outside("job said"));
   /* Another conversation it was learned from: unjudged, or tainted; a taint
    * a source shows wins over an unjudged one. */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_UNKNOWN, fact_outside("also from 90"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, fact_outside("also from 20"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, fact_outside("email and old"));
   /* DAWN's own input read like the live scan reads it. */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, fact_outside("from context"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, fact_outside("device said"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, fact_outside("after the post"));
   /* A continuation: what the conversations it continues read, or UNKNOWN
    * when one of them can't be judged. */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, fact_outside("continued mail"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_UNKNOWN, fact_outside("continued old"));
}

static void test_relations_summaries_preferences_outside_kind(void) {
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL,
                           q_int("SELECT outside FROM memory_relations WHERE id = 1"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q_int("SELECT outside FROM memory_relations WHERE id = 2"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_NONE,
                           q_int("SELECT outside FROM memory_relations WHERE id = 3"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_UNKNOWN,
                           q_int("SELECT outside FROM memory_relations WHERE id = 4"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL,
                           q_int("SELECT outside FROM memory_summaries WHERE id = 1"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_NONE,
                           q_int("SELECT outside FROM memory_summaries WHERE id = 2"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_UNKNOWN,
                           q_int("SELECT outside FROM memory_summaries WHERE id = 3"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL,
                           q_int("SELECT outside FROM memory_summaries WHERE id = 4"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_UNKNOWN, q_int("SELECT outside FROM memory_preferences"));
}

/* Each conversation's floor: its own first outside text (judged or not),
 * else one a conversation it continues read, else UNKNOWN when one can't be
 * judged. */
static void test_conversation_floors(void) {
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q_int("SELECT outside FROM conversations WHERE id = 10"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_OTHER, q_int("SELECT outside FROM conversations WHERE id = 85"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_MESSAGING,
                           q_int("SELECT outside FROM conversations WHERE id = 99"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL, q_int("SELECT outside FROM conversations WHERE id = 20"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_NONE, q_int("SELECT outside FROM conversations WHERE id = 30"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_OTHER, q_int("SELECT outside FROM conversations WHERE id = 60"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL, q_int("SELECT outside FROM conversations WHERE id = 80"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_UNKNOWN,
                           q_int("SELECT outside FROM conversations WHERE id = 95"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_DOCUMENT,
                           q_int("SELECT outside FROM conversations WHERE id = 75"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_OTHER, q_int("SELECT outside FROM conversations WHERE id = 76"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_OTHER, q_int("SELECT outside FROM conversations WHERE id = 77"));
}

/* The category index goes, and the next start doesn't make it again. */
static void test_category_index_gone_for_good(void) {
   TEST_ASSERT_FALSE(index_exists("idx_memory_facts_user_category"));
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path));
   TEST_ASSERT_FALSE(index_exists("idx_memory_facts_user_category"));
   TEST_ASSERT_TRUE(index_exists("idx_mco_deferred"));
}

/* A rerun (the version held back by a later step) leaves alone what new code
 * has tagged since, and fills nothing twice. */
static void test_rerun_keeps_new_tags(void) {
   exec_ok(
       "UPDATE memory_facts SET outside = 1 WHERE fact_text = 'clean';"
       "UPDATE memory_relations SET outside = 1 WHERE id = 3;"
       "UPDATE memory_summaries SET outside = 1 WHERE id = 2;"
       "UPDATE memory_preferences SET outside = 1;"
       "UPDATE conversations SET outside = 1 WHERE id = 30;"
       "INSERT INTO memory_facts (user_id, fact_text, source) VALUES (1, 'old file', 'import');");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v113(s_db.db));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, fact_outside("clean"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q_int("SELECT outside FROM memory_relations WHERE id = 3"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q_int("SELECT outside FROM memory_summaries WHERE id = 2"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q_int("SELECT outside FROM memory_preferences"));
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_WEB, q_int("SELECT outside FROM conversations WHERE id = 30"));
   TEST_ASSERT_EQUAL_INT(MEMORY_ORIGIN_CONVERSATION, fact_origin("old file"));
}

/* Each table's columns and the indexes on them, in a stable order. */
static char *schema_of(sqlite3 *db) {
   static const char sql[] =
       "SELECT group_concat(line, char(10)) FROM (SELECT t.name || '.' || c.name || ' ' || "
       "c.type || ' ' || c.\"notnull\" || ' ' || COALESCE(c.dflt_value, '-') || ' ' || c.pk AS "
       "line "
       "FROM sqlite_master t, pragma_table_info(t.name) c WHERE t.type = 'table' AND t.name IN "
       "('memory_facts', 'memory_relations', 'memory_summaries', 'memory_preferences', "
       "'memory_consolidation_batches', 'memory_consolidation_ops') "
       "UNION ALL SELECT 'index ' || name || ' ' || COALESCE(sql, '') FROM sqlite_master "
       "WHERE type = 'index' AND tbl_name IN ('memory_facts', 'memory_relations', "
       "'memory_summaries', 'memory_preferences', 'memory_consolidation_batches', "
       "'memory_consolidation_ops') UNION ALL SELECT 'conversations.' || name || ' ' || type || "
       "' ' || \"notnull\" || ' ' || COALESCE(dflt_value, '-') FROM pragma_table_info("
       "'conversations') WHERE name = 'outside' "
       "ORDER BY 1)";
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   char *out = strdup((const char *)sqlite3_column_text(st, 0));
   sqlite3_finalize(st);
   return out;
}

/* A migrated database and a new one have the same memory schema. */
static void test_migrated_schema_matches_new(void) {
   char *migrated = schema_of(s_db.db);
   auth_db_shutdown();
   remove_db_files(s_fresh_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_fresh_path));
   char *fresh = schema_of(s_db.db);
   TEST_ASSERT_NOT_NULL(strstr(fresh, "memory_facts.outside INTEGER 1 0 0"));
   TEST_ASSERT_EQUAL_STRING(fresh, migrated);
   free(migrated);
   free(fresh);
   auth_db_shutdown();
   remove_db_files(s_fresh_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path)); /* for tearDown */
}

int main(void) {
   test_tmp_path(s_fresh_path, sizeof(s_fresh_path), "dawn_test_mig_v113_fresh.db");
   UNITY_BEGIN();
   RUN_TEST(test_origin_from_import_and_note);
   RUN_TEST(test_facts_outside_kind);
   RUN_TEST(test_relations_summaries_preferences_outside_kind);
   RUN_TEST(test_conversation_floors);
   RUN_TEST(test_category_index_gone_for_good);
   RUN_TEST(test_rerun_keeps_new_tags);
   RUN_TEST(test_migrated_schema_matches_new);
   return UNITY_END();
}
