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
 * The v114 migration on a v113 database: a note's outside kind filled in from
 * the research run that filed it and from its gloss fact, any other
 * document's as DOCUMENT, a note's from the conversations that saved it, a
 * conversation holding a framing tool's stored result raised to OTHER, a
 * version's from its document, a rerun leaving rows tagged since alone,
 * documents.own added at 0, and migrated documents and document_versions
 * tables matching new ones.  Also a database a later step stamped past 114
 * without these columns: still migrated, and its version kept.
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

static int outside_in(const char *table, int id) {
   char sql[96];
   snprintf(sql, sizeof(sql), "SELECT outside FROM %s WHERE id = ?", table);
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   sqlite3_bind_int(st, 1, id);
   const int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

/* Notes and documents as a v113 install stored them.
 *   1 a research run's report filed as a note; 5 one too large for a note.
 *   2 a note whose gloss learned from an email; 3 one with a clean gloss;
 *   4 one whose gloss predates the kind (UNKNOWN); 6 an uploaded file.
 * Versions: 1 of the report, 2 of the clean note, 3 of a deleted item (99),
 * 4 of a deleted research report (98, its run still naming it), 5 of the
 * uploaded file, 6 of a deleted upload whose id a later note (7) reuses.
 * Conversations: 13 read an email and saved note 8 by name; 14 holds a
 * calendar result and edited note 10 by id; 15 a weather result; 16 (UNKNOWN)
 * a document_read result; 17 (clean) saved note 11.  Note 9 was typed in
 * the WebUI. */
static void seed_v113(void) {
   exec_ok("PRAGMA foreign_keys=OFF;"
           "ALTER TABLE documents DROP COLUMN own;"
           "ALTER TABLE documents DROP COLUMN outside;"
           "ALTER TABLE document_versions DROP COLUMN outside;"
           "DELETE FROM schema_version;"
           "INSERT INTO schema_version (version) VALUES (113);"
           "INSERT INTO users (id, username, password_hash, created_at) VALUES (1, 'jon', 'x', 1);"
           "INSERT INTO conversations (id, user_id, created_at, updated_at) VALUES "
           "(10, 1, 1, 1), (11, 1, 1, 1), (12, 1, 1, 1);"
           "INSERT INTO documents (id, user_id, filename, filepath, filetype, file_hash, "
           "num_chunks, created_at) VALUES (1, 1, 'Research #1', '', 'note', 'h1', 1, 1),"
           "(2, 1, 'Tea', '', 'note', 'h2', 1, 1), (3, 1, 'Groceries', '', 'note', 'h3', 1, 1),"
           "(4, 1, 'Old', '', 'note', 'h4', 1, 1), (5, 1, 'Research #2', '', 'text', 'h5', 3, 1),"
           "(6, 1, 'a.pdf', 'a.pdf', 'pdf', 'h6', 2, 1), (7, 1, 'Later', '', 'note', 'h7', 1, 10),"
           "(8, 1, 'Inbox Note', '', 'note', 'h8', 1, 1), (9, 1, 'Typed', '', 'note', 'h9', 1, 1),"
           "(10, 1, 'Plans', '', 'note', 'h10', 1, 1), (11, 1, 'Clean', '', 'note', 'h11', 1, 1);"
           "INSERT INTO conversations (id, user_id, created_at, updated_at, outside) VALUES "
           "(13, 1, 1, 1, 2), (14, 1, 1, 1, 0), (15, 1, 1, 1, 0), (16, 1, 1, 1, 8), "
           "(17, 1, 1, 1, 0);"
           "INSERT INTO messages (id, conversation_id, role, content, tool_calls, tool_call_id, "
           "created_at) VALUES (1, 13, 'assistant', '', '[{\"id\":\"c1\",\"type\":"
           "\"function\",\"function\":{\"name\":\"document_manage\",\"arguments\":"
           "\"{\\\"action\\\":\\\"save_note\\\",\\\"label\\\":\\\"inbox "
           "note\\\"}\"}}]', NULL, 1), (2, 13, 'tool', 'Saved.', NULL, 'c1', 1),"
           "(3, 14, 'assistant', '', '[{\"id\":\"c2\",\"function\":{\"name\":"
           "\"calendar\",\"arguments\":\"{}\"}},{\"id\":\"c3\",\"function\":{"
           "\"name\":\"document_manage\",\"arguments\":\"{\\\"action\\\":"
           "\\\"edit\\\",\\\"id\\\":10}\"}}]', NULL, 1),"
           "(4, 14, 'tool', 'Lunch.', NULL, 'c2', 1),"
           "(5, 15, 'assistant', '', '[{\"id\":\"c4\",\"function\":{\"name\":"
           "\"weather\",\"arguments\":\"{}\"}}]', NULL, 1),"
           "(6, 15, 'tool', 'Sunny.', NULL, 'c4', 1),"
           "(7, 16, 'assistant', '', '[{\"id\":\"c5\",\"function\":{\"name\":"
           "\"document_read\",\"arguments\":\"{}\"}}]', NULL, 1),"
           "(8, 16, 'tool', 'Text.', NULL, 'c5', 1),"
           "(9, 17, 'assistant', '', '[{\"id\":\"c6\",\"function\":{\"name\":"
           "\"document_manage\",\"arguments\":\"{\\\"action\\\":\\\"save_note\\\","
           "\\\"label\\\":\\\"Clean\\\"}\"}}]', NULL, 1),"
           "(10, 17, 'tool', 'Saved.', NULL, 'c6', 1);"
           "INSERT INTO research_runs (conversation_id, user_id, brief, status, report_doc_id, "
           "created_at) VALUES (10, 1, 'tea', 'done', 1, 1), (11, 1, 'more', 'done', 5, 1), (12, "
           "1, 'old', 'done', 98, 1);"
           "INSERT INTO memory_facts (user_id, fact_text, source, note_doc_id, outside) VALUES "
           "(1, 'g2', 'explicit', 2, 2), (1, 'g3', 'explicit', 3, 0), (1, 'g4', 'explicit', 4, 8),"
           "(1, 'other', 'explicit', NULL, 1);"
           "INSERT INTO document_versions (id, document_id, user_id, filename, text, "
           "archived_at) VALUES (1, 1, 1, 'Research #1', 'old', 1), (2, 3, 1, 'Groceries', "
           "'eggs', 1), (3, 99, 1, 'Gone', 'x', 1), (4, 98, 1, 'Research #3', 'y', 1), "
           "(5, 6, 1, 'a.pdf', 'z', 1), (6, 7, 1, 'b.pdf', 'w', 5);"
           "PRAGMA foreign_keys=ON;");
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

/* Pre-upgrade backups of the test database taken from @p version. */
static int backups_from(int version) {
   char copy[TEST_TMP_PATH_MAX], copy2[TEST_TMP_PATH_MAX], dir[TEST_TMP_PATH_MAX + 16];
   char prefix[TEST_TMP_PATH_MAX + 16];
   snprintf(copy, sizeof(copy), "%s", s_path);
   snprintf(copy2, sizeof(copy2), "%s", s_path);
   snprintf(dir, sizeof(dir), "%s/backups", dirname(copy));
   snprintf(prefix, sizeof(prefix), "%s.v%d-", basename(copy2), version);
   int n = 0;
   DIR *d = opendir(dir);
   for (struct dirent *e = d ? readdir(d) : NULL; e; e = readdir(d)) {
      n += strncmp(e->d_name, prefix, strlen(prefix)) == 0;
   }
   if (d) {
      closedir(d);
   }
   return n;
}

/* A v113 database as seed_v113() leaves it, then stamped @p version; a
 * stamp past 114 comes from a later step that added
 * message_unprompted.replayed and none of v114's columns.  Then reopened,
 * which runs the migrations. */
static void open_seeded(int version) {
   auth_db_shutdown();
   remove_db_files(s_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path));
   seed_v113();
   if (version > 114) {
      char sql[128];
      snprintf(sql, sizeof(sql),
               "ALTER TABLE message_unprompted ADD COLUMN replayed INTEGER NOT NULL DEFAULT 0;"
               "UPDATE schema_version SET version = %d;",
               version);
      exec_ok(sql);
   }
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path));
}

void setUp(void) {
   char name[64];
   snprintf(name, sizeof(name), "dawn_test_mig_v114_%d.db", ++s_test_no);
   test_tmp_path(s_path, sizeof(s_path), name);
   open_seeded(113); /* runs v114 */
}

void tearDown(void) {
   auth_db_shutdown();
   remove_db_files(s_path);
}

static void test_notes_outside_filled_in(void) {
   TEST_ASSERT_EQUAL_INT64(AUTH_DB_SCHEMA_VERSION,
                           auth_db_query_int64(s_db.db, "SELECT version FROM schema_version"));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside_in("documents", 1));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_in("documents", 2));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 3));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 4));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside_in("documents", 5));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside_in("documents", 6)); /* not a note */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside_in("document_versions", 1));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("document_versions", 2));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside_in("document_versions", 3));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside_in("document_versions", 4));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside_in("document_versions", 5));
   /* No document starts vouched for. */
   TEST_ASSERT_EQUAL_INT64(0, auth_db_query_int64(s_db.db, "SELECT COUNT(*) FROM documents "
                                                           "WHERE own != 0"));
}

/* A version older than the document now holding its id was another's: it
 * doesn't take the later note's kind. */
static void test_reused_id_not_inherited(void) {
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 7));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside_in("document_versions", 6));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("document_versions", 2)); /* its own: kept */
}

/* A rerun (the version held back by a later step) leaves alone what new code
 * has tagged since. */
static void test_rerun_keeps_new_tags(void) {
   exec_ok("UPDATE documents SET outside = 1 WHERE id = 3;"
           "UPDATE documents SET outside = 0 WHERE id = 1;");
   exec_ok("UPDATE document_versions SET outside = 2 WHERE id = 3;");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v114(s_db.db));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_in("document_versions", 3));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, outside_in("documents", 3));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 1));
}

/* A stored result of a tool that frames only others' text raises its
 * conversation to OTHER; a note takes the floor of the conversation its save,
 * edit or append ran in; a typed one stays the user's own. */
static void test_framed_tool_results_and_note_calls(void) {
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_in("conversations", 13)); /* tainted: kept */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, outside_in("conversations", 14));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("conversations", 15));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, outside_in("conversations", 16));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_in("documents", 8));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 9));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, outside_in("documents", 10));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("conversations", 17)); /* a save: own */
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 11));
}

/* A database stamped 115 by a later step before v114 reached it: v114 still
 * runs (columns and fills), after a backup, and the version stays 115. */
static void test_later_stamp_still_migrated(void) {
   open_seeded(115);
   TEST_ASSERT_EQUAL_INT64(115, auth_db_query_int64(s_db.db, "SELECT version FROM schema_version"));
   TEST_ASSERT_FALSE(auth_db_migrations_v114_pending(s_db.db));
   TEST_ASSERT_TRUE(auth_db_column_exists(s_db.db, "message_unprompted", "replayed"));
   TEST_ASSERT_EQUAL_INT(1, backups_from(115));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside_in("documents", 1));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_in("documents", 2));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside_in("documents", 6));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, outside_in("documents", 10));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, outside_in("conversations", 14));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside_in("document_versions", 4));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside_in("document_versions", 6));
   TEST_ASSERT_EQUAL_INT64(0, auth_db_query_int64(s_db.db, "SELECT COUNT(*) FROM documents "
                                                           "WHERE own != 0"));
}

/* Reopening a complete database, at 114 or stamped 115, changes nothing and
 * takes no backup. */
static void test_reopen_complete_changes_nothing(void) {
   for (int version = 114; version <= 115; version++) {
      if (version == 115) {
         open_seeded(115);
      }
      exec_ok("UPDATE documents SET outside = 0 WHERE id = 1;"
              "UPDATE documents SET outside = 1, own = 1 WHERE id = 3;"
              "UPDATE document_versions SET outside = 2 WHERE id = 3;"
              "UPDATE conversations SET outside = 0 WHERE id = 14;");
      const int before = backups_from(version);
      auth_db_shutdown();
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path));
      TEST_ASSERT_EQUAL_INT64(version,
                              auth_db_query_int64(s_db.db, "SELECT version FROM schema_version"));
      TEST_ASSERT_EQUAL_INT(before, backups_from(version));
      TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("documents", 1));
      TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, outside_in("documents", 3));
      TEST_ASSERT_EQUAL_INT64(1, auth_db_query_int64(s_db.db, "SELECT own FROM documents "
                                                              "WHERE id = 3"));
      TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_in("document_versions", 3));
      TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, outside_in("conversations", 14));
   }
}

/* The documents and document_versions tables' columns, in a stable order. */
static char *schema_of(sqlite3 *db) {
   static const char sql[] =
       "SELECT group_concat(line, char(10)) FROM (SELECT t.name || ' ' || c.cid || ' ' || c.name "
       "|| ' ' || c.type || ' ' || c.\"notnull\" || ' ' || COALESCE(c.dflt_value, '-') AS line "
       "FROM sqlite_master t, pragma_table_info(t.name) c WHERE t.type = 'table' AND t.name IN "
       "('documents', 'document_versions') ORDER BY t.name, c.cid)";
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   char *out = strdup((const char *)sqlite3_column_text(st, 0));
   sqlite3_finalize(st);
   return out;
}

/* A migrated database and a new one have the same two tables. */
static void test_migrated_schema_matches_new(void) {
   char *migrated = schema_of(s_db.db);
   auth_db_shutdown();
   remove_db_files(s_fresh_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_fresh_path));
   char *fresh = schema_of(s_db.db);
   TEST_ASSERT_NOT_NULL(strstr(fresh, "documents 10 outside INTEGER 1 0"));
   TEST_ASSERT_NOT_NULL(strstr(fresh, "documents 11 own INTEGER 1 0"));
   TEST_ASSERT_NOT_NULL(strstr(fresh, "document_versions 6 outside INTEGER 1 0"));
   TEST_ASSERT_EQUAL_STRING(fresh, migrated);
   free(migrated);
   free(fresh);
   auth_db_shutdown();
   remove_db_files(s_fresh_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_path)); /* for tearDown */
}

int main(void) {
   test_tmp_path(s_fresh_path, sizeof(s_fresh_path), "dawn_test_mig_v114_fresh.db");
   UNITY_BEGIN();
   RUN_TEST(test_notes_outside_filled_in);
   RUN_TEST(test_reused_id_not_inherited);
   RUN_TEST(test_rerun_keeps_new_tags);
   RUN_TEST(test_framed_tool_results_and_note_calls);
   RUN_TEST(test_later_stamp_still_migrated);
   RUN_TEST(test_reopen_complete_changes_nothing);
   RUN_TEST(test_migrated_schema_matches_new);
   return UNITY_END();
}
