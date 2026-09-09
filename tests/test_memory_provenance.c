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
 * Unit tests for v40 memory provenance:
 *   - source columns on memory_facts, memory_preferences, memory_summaries
 *   - memory_db_fact_get_source (ownership, privacy, NULL sentinel)
 *   - NULL provenance → NOT_FOUND from get_source
 *   - pref upsert latest-source-wins
 *   - null bind produces SQL NULL not integer 0
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_types.h"
#include "unity.h"

/* Minimal DDL matching the v40 schema (conversations + messages needed for
 * conv_db_is_private + source range queries). */
/* clang-format off */
static const char *DDL =
   "CREATE TABLE IF NOT EXISTS users ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  username TEXT UNIQUE NOT NULL,"
   "  real_name TEXT DEFAULT NULL,"
   "  preferred_address TEXT DEFAULT NULL,"
   "  identity_aliases TEXT DEFAULT NULL"
   ");"
   "INSERT INTO users (id, username) VALUES (1, 'alice_test');"
   "INSERT INTO users (id, username) VALUES (2, 'bob_test');"

   "CREATE TABLE IF NOT EXISTS conversations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  title TEXT NOT NULL DEFAULT 'Test',"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  message_count INTEGER DEFAULT 0,"
   "  is_archived INTEGER DEFAULT 0,"
   "  last_extracted_msg_count INTEGER DEFAULT 0,"
   "  last_extracted_msg_id    INTEGER NOT NULL DEFAULT 0,"
   "  extraction_attempts INTEGER DEFAULT 0,"
   "  extraction_last_attempt_at INTEGER DEFAULT 0,"
   "  is_private INTEGER DEFAULT 0,"
   "  title_locked INTEGER DEFAULT 0,"
   "  origin TEXT DEFAULT 'webui',"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO conversations (id, user_id, is_private) VALUES (10, 1, 0);"
   "INSERT INTO conversations (id, user_id, is_private) VALUES (20, 1, 1);"  /* private */
   /* Extra conversations seeded for the provenance-extend test cases —
    * memory_facts.source_conversation_id has a FK that's enforced when
    * PRAGMA foreign_keys=ON, so the "newer conv replaces" branch needs a
    * pre-existing target. */
   "INSERT INTO conversations (id, user_id, is_private) VALUES (25, 1, 0);"
   "INSERT INTO conversations (id, user_id, is_private) VALUES (30, 1, 0);"

   "CREATE TABLE IF NOT EXISTS messages ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  conversation_id INTEGER NOT NULL,"
   "  role TEXT NOT NULL,"
   "  content TEXT NOT NULL,"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO messages (id, conversation_id, role, content) VALUES (100, 10, 'user', 'hello');"
   "INSERT INTO messages (id, conversation_id, role, content) VALUES (101, 10, 'assistant', 'hi');"
   "INSERT INTO messages (id, conversation_id, role, content) VALUES (200, 20, 'user', 'secret');"

   "CREATE TABLE IF NOT EXISTS memory_facts ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  fact_text TEXT NOT NULL,"
   "  confidence REAL DEFAULT 1.0,"
   "  source TEXT DEFAULT 'inferred',"
   "  category TEXT NOT NULL DEFAULT 'general',"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  last_accessed INTEGER,"
   "  access_count INTEGER DEFAULT 0,"
   "  superseded_by INTEGER,"
   "  normalized_hash INTEGER DEFAULT 0,"
   "  embedding BLOB DEFAULT NULL,"
   "  embedding_norm REAL DEFAULT NULL,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  expires_at             INTEGER DEFAULT NULL,"  /* v58 */
   "  note_doc_id            INTEGER DEFAULT NULL,"  /* v61 (memory→note bridge) */
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");"

   "CREATE TABLE IF NOT EXISTS memory_preferences ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  category TEXT NOT NULL,"
   "  value TEXT NOT NULL,"
   "  confidence REAL DEFAULT 0.5,"
   "  source TEXT DEFAULT 'inferred',"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  reinforcement_count INTEGER DEFAULT 1,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL,"
   "  UNIQUE(user_id, category)"
   ");"

   "CREATE TABLE IF NOT EXISTS memory_summaries ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  session_id TEXT NOT NULL,"
   "  summary TEXT NOT NULL,"
   "  topics TEXT,"
   "  sentiment TEXT,"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  message_count INTEGER,"
   "  duration_seconds INTEGER,"
   "  consolidated INTEGER DEFAULT 0,"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");"

   /* Phase B: relations table (subset — entities/links not needed for source tests) */
   "CREATE TABLE IF NOT EXISTS memory_relations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  subject_entity_id INTEGER NOT NULL DEFAULT 0,"
   "  relation TEXT NOT NULL,"
   "  object_entity_id INTEGER DEFAULT 0,"
   "  object_name TEXT,"
   "  confidence REAL DEFAULT 1.0,"
   "  fact_id INTEGER DEFAULT 0,"
   "  valid_from INTEGER DEFAULT NULL,"
   "  valid_to INTEGER DEFAULT NULL,"
   "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
   ");";
/* clang-format on */

static int prepare_statements(void) {
   int rc;

   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_facts (user_id, fact_text, confidence, source, "
                           "category, created_at, normalized_hash, "
                           "source_conversation_id, source_msg_id_start, source_msg_id_end) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_memory_fact_create, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   /* memory_db_fact_get is used by the new provenance-extend + cleanup-meta-
    * facts tests to verify row-level state.  Production DDL keeps these
    * columns; mirror that here.  SQL filters on (id, user_id) to mirror
    * the CWE-639 defense-in-depth in production. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category, expires_at FROM memory_facts "
       "WHERE id = ? AND user_id = ?",
       -1, &s_db.stmt_memory_fact_get, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_preferences (user_id, category, value, confidence, source, created_at, "
       "updated_at, source_conversation_id, source_msg_id_start, source_msg_id_end) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id, category) DO UPDATE SET "
       "value=excluded.value, confidence=excluded.confidence, updated_at=excluded.updated_at, "
       "source_conversation_id=excluded.source_conversation_id, "
       "source_msg_id_start=excluded.source_msg_id_start, "
       "source_msg_id_end=excluded.source_msg_id_end, "
       "reinforcement_count=reinforcement_count+1",
       -1, &s_db.stmt_memory_pref_upsert, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_summaries (user_id, session_id, summary, topics, sentiment, "
       "created_at, message_count, duration_seconds, "
       "source_conversation_id, source_msg_id_start, source_msg_id_end) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_memory_summary_create, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   /* conv_set_last_extracted — not used in these tests but must be valid */
   rc = sqlite3_prepare_v2(s_db.db, "SELECT 1", -1, &s_db.stmt_conv_set_last_extracted, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   /* v58 expiry-guard tests: mirror the production fact_list (with the
    * expires_at retrieval guard, numbered params) + prune_expired statements
    * so memory_db_fact_list / memory_db_fact_prune_expired exercise their real
    * bind logic against this DB.  Keep in sync with auth_db_statements.c. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL "
       "  AND (expires_at IS NULL OR expires_at >= ?4) "
       "ORDER BY confidence DESC LIMIT ?2 OFFSET ?3",
       -1, &s_db.stmt_memory_fact_list, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   /* WebUI sort variants — mirror auth_db_statements.c so memory_db_fact_list_sorted
    * exercises its real statement selection + bind logic here. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL "
       "  AND (expires_at IS NULL OR expires_at >= ?4) "
       "ORDER BY created_at DESC, id DESC LIMIT ?2 OFFSET ?3",
       -1, &s_db.stmt_memory_fact_list_created_desc, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL "
       "  AND (expires_at IS NULL OR expires_at >= ?4) "
       "ORDER BY created_at ASC, id ASC LIMIT ?2 OFFSET ?3",
       -1, &s_db.stmt_memory_fact_list_created_asc, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_facts WHERE user_id = ? "
                           "AND expires_at IS NOT NULL AND expires_at < ?",
                           -1, &s_db.stmt_memory_fact_prune_expired, NULL);
   if (rc != SQLITE_OK)
      return FAILURE;

   return SUCCESS;
}

static void open_db(void) {
   if (sqlite3_open(":memory:", &s_db.db) != SQLITE_OK) {
      fprintf(stderr, "open failed\n");
      exit(1);
   }
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   char *err = NULL;
   if (sqlite3_exec(s_db.db, DDL, NULL, NULL, &err) != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", err ? err : "?");
      sqlite3_free(err);
      exit(1);
   }
   if (prepare_statements() != 0) {
      fprintf(stderr, "prepare failed\n");
      exit(1);
   }
   s_db.initialized = true;
}

static void close_db(void) {
   s_db.initialized = false;
   if (s_db.stmt_memory_fact_create)
      sqlite3_finalize(s_db.stmt_memory_fact_create);
   if (s_db.stmt_memory_fact_get)
      sqlite3_finalize(s_db.stmt_memory_fact_get);
   if (s_db.stmt_memory_pref_upsert)
      sqlite3_finalize(s_db.stmt_memory_pref_upsert);
   if (s_db.stmt_memory_summary_create)
      sqlite3_finalize(s_db.stmt_memory_summary_create);
   if (s_db.stmt_conv_set_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_set_last_extracted);
   if (s_db.stmt_memory_fact_list)
      sqlite3_finalize(s_db.stmt_memory_fact_list);
   if (s_db.stmt_memory_fact_list_created_desc)
      sqlite3_finalize(s_db.stmt_memory_fact_list_created_desc);
   if (s_db.stmt_memory_fact_list_created_asc)
      sqlite3_finalize(s_db.stmt_memory_fact_list_created_asc);
   if (s_db.stmt_memory_fact_prune_expired)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_expired);
   s_db.stmt_memory_fact_create = NULL;
   s_db.stmt_memory_fact_get = NULL;
   s_db.stmt_memory_pref_upsert = NULL;
   s_db.stmt_memory_summary_create = NULL;
   s_db.stmt_conv_set_last_extracted = NULL;
   s_db.stmt_memory_fact_list = NULL;
   s_db.stmt_memory_fact_list_created_desc = NULL;
   s_db.stmt_memory_fact_list_created_asc = NULL;
   s_db.stmt_memory_fact_prune_expired = NULL;
   sqlite3_close(s_db.db);
   s_db.db = NULL;
}

void setUp(void) {
   open_db();
}
void tearDown(void) {
   close_db();
}

/* ================================================================
 * Tests
 * ================================================================ */

void test_fact_create_with_source_roundtrip(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t id = 0;
   int rc = memory_db_fact_create(1, "User likes coffee", 0.9f, "inferred", "interests", &prov,
                                  &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, id);

   int64_t conv_id = 0, start = 0, end = 0;
   rc = memory_db_fact_get_source(id, 1, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv_id);
   TEST_ASSERT_EQUAL(100, start);
   TEST_ASSERT_EQUAL(101, end);
}

void test_fact_create_null_provenance_returns_not_found(void) {
   int64_t id = 0;
   int rc = memory_db_fact_create(1, "No prov fact", 0.8f, "inferred", "general", NULL, &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, id);

   int64_t conv_id = 0, start = 0, end = 0;
   rc = memory_db_fact_get_source(id, 1, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_null_bind_produces_sql_null_not_zero(void) {
   int64_t id = 0;
   memory_db_fact_create(1, "Null bind test", 0.7f, "inferred", "general", NULL, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* Verify column is SQL NULL, not integer 0 */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT source_conversation_id FROM memory_facts WHERE id = ?", -1,
                      &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, id);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 0));
   sqlite3_finalize(stmt);
}

void test_get_source_wrong_user_returns_not_found(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t id = 0;
   memory_db_fact_create(1, "Alice's fact", 0.9f, "inferred", "personal", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* User 2 should not see user 1's source */
   int64_t conv_id = 0, start = 0, end = 0;
   int rc = memory_db_fact_get_source(id, 2, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_get_source_private_conv_returns_not_found(void) {
   /* Conv 20 is marked is_private=1 in DDL */
   memory_provenance_t prov = { .conv_id = 20, .msg_id_start = 200, .msg_id_end = 200 };
   int64_t id = 0;
   memory_db_fact_create(1, "Private source fact", 0.9f, "inferred", "personal", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   int64_t conv_id = 0, start = 0, end = 0;
   int rc = memory_db_fact_get_source(id, 1, &conv_id, &start, &end);
   /* conv is private → NOT_FOUND */
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_pref_upsert_with_source_roundtrip(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int rc = memory_db_pref_upsert(1, "theme", "dark", 0.8f, "inferred", &prov);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   /* Verify columns via direct SQL */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id, source_msg_id_start, source_msg_id_end "
                      "FROM memory_preferences WHERE user_id=1 AND category='theme'",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(10, sqlite3_column_int64(stmt, 0));
   TEST_ASSERT_EQUAL(100, sqlite3_column_int64(stmt, 1));
   TEST_ASSERT_EQUAL(101, sqlite3_column_int64(stmt, 2));
   sqlite3_finalize(stmt);
}

void test_pref_upsert_latest_source_wins(void) {
   memory_provenance_t prov1 = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 100 };
   memory_provenance_t prov2 = { .conv_id = 10, .msg_id_start = 101, .msg_id_end = 101 };

   memory_db_pref_upsert(1, "language", "english", 0.7f, "inferred", &prov1);
   memory_db_pref_upsert(1, "language", "french", 0.9f, "inferred", &prov2);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_msg_id_start FROM memory_preferences "
                      "WHERE user_id=1 AND category='language'",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(101, sqlite3_column_int64(stmt, 0)); /* prov2 wins */
   sqlite3_finalize(stmt);
}

void test_summary_create_with_source(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t id = 0;
   int rc = memory_db_summary_create(1, "sess1", "Test summary", "topics", "neutral", 2, 60, &prov,
                                     &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, id);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id, source_msg_id_start, source_msg_id_end "
                      "FROM memory_summaries WHERE id=?",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, id);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(10, sqlite3_column_int64(stmt, 0));
   TEST_ASSERT_EQUAL(100, sqlite3_column_int64(stmt, 1));
   TEST_ASSERT_EQUAL(101, sqlite3_column_int64(stmt, 2));
   sqlite3_finalize(stmt);
}

void test_summary_create_null_provenance(void) {
   int64_t id = 0;
   int rc = memory_db_summary_create(1, "sess2", "No prov summary", "t", "neutral", 1, 30, NULL,
                                     &id);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT source_conversation_id FROM memory_summaries WHERE id=?", -1,
                      &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, id);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 0));
   sqlite3_finalize(stmt);
}

void test_pref_upsert_null_provenance_sql_null(void) {
   memory_db_pref_upsert(1, "units", "metric", 0.8f, "inferred", NULL);

   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id FROM memory_preferences "
                      "WHERE user_id=1 AND category='units'",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 0));
   sqlite3_finalize(stmt);
}

void test_get_source_nonexistent_fact(void) {
   int64_t conv_id = 0, start = 0, end = 0;
   int rc = memory_db_fact_get_source(99999, 1, &conv_id, &start, &end);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

void test_conversations_have_last_extracted_msg_id_column(void) {
   /* Verify the column exists and defaults to 0 */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT last_extracted_msg_id FROM conversations WHERE id=10", -1,
                               &stmt, NULL);
   TEST_ASSERT_EQUAL(SQLITE_OK, rc);
   TEST_ASSERT_EQUAL(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL(0, sqlite3_column_int64(stmt, 0));
   sqlite3_finalize(stmt);
}

/* =============================================================================
 * Phase B: batch source-reader tests for relations / summaries / preferences.
 *
 * Each record type gets four assertions:
 *   - roundtrip: stored provenance comes back through the batch API.
 *   - NULL provenance: row stored with NULL source columns → out_conv = 0.
 *   - private-conv filter: row stored pointing at a private conversation →
 *     out_conv = 0 (filtered in SQL via JOIN on is_private = 0).
 *   - N=64 chunked succeeds: post-fold-in (HIGH-1/HIGH-2 from Phase B
 *     architecture review), the public APIs accept any positive N and chunk
 *     internally in MAX_PROVENANCE_BATCH-sized passes.  64 IDs land in 2
 *     chunks; non-existent IDs yield out=0.  This test pins the new contract
 *     and replaces the original "n>cap fails closed" test — fail-closed
 *     semantics are still enforced at the inner SQL builder, just not at the
 *     public surface.
 * ============================================================================= */

/* Helper: insert a memory_relations row with explicit provenance (or NULL). */
static int64_t insert_relation(int user_id, int64_t conv_id, int64_t start, int64_t end) {
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       (conv_id > 0)
           ? "INSERT INTO memory_relations (user_id, subject_entity_id, relation, object_name, "
             "source_conversation_id, source_msg_id_start, source_msg_id_end) "
             "VALUES (?, 1, 'is_a', 'literal', ?, ?, ?)"
           : "INSERT INTO memory_relations (user_id, subject_entity_id, relation, object_name) "
             "VALUES (?, 1, 'is_a', 'literal')";
   sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   if (conv_id > 0) {
      sqlite3_bind_int64(stmt, 2, conv_id);
      sqlite3_bind_int64(stmt, 3, start);
      sqlite3_bind_int64(stmt, 4, end);
   }
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

/* ------------------------------ relations ------------------------------ */

void test_relations_get_sources_roundtrip(void) {
   int64_t rid = insert_relation(1, 10, 100, 101);
   int64_t conv[1] = { 0 }, start[1] = { 0 }, end[1] = { 0 };
   int rc = memory_db_relations_get_sources(1, &rid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[0]);
   TEST_ASSERT_EQUAL(100, start[0]);
   TEST_ASSERT_EQUAL(101, end[0]);
}

void test_relations_get_sources_null_prov_returns_zero(void) {
   int64_t rid = insert_relation(1, 0, 0, 0);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_relations_get_sources(1, &rid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   /* No provenance recorded — output should be zeroed by the API. */
   TEST_ASSERT_EQUAL(0, conv[0]);
   TEST_ASSERT_EQUAL(0, start[0]);
   TEST_ASSERT_EQUAL(0, end[0]);
}

void test_relations_get_sources_private_conv_filter(void) {
   int64_t rid = insert_relation(1, 20, 200, 200); /* conv 20 is is_private=1 */
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_relations_get_sources(1, &rid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   /* Privacy JOIN should suppress the row → output zeroed. */
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_relations_get_sources_n_chunked_succeeds(void) {
   /* Insert one relation with provenance; query with N=64 (2× the cap).
    * Public API chunks internally — IDs without a row return out=0; the
    * known ID is found regardless of which chunk it falls into. */
   int64_t rid = insert_relation(1, 10, 100, 101);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? rid : (i + 1000); /* place the real ID in chunk 2 */
   int rc = memory_db_relations_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
   /* Spot-check that other slots are 0. */
   TEST_ASSERT_EQUAL(0, conv[0]);
   TEST_ASSERT_EQUAL(0, conv[63]);
}

/* ------------------------------ summaries ------------------------------ */

static int64_t insert_summary_with_prov(int user_id, int64_t conv_id, int64_t start, int64_t end) {
   memory_provenance_t prov = { .conv_id = conv_id, .msg_id_start = start, .msg_id_end = end };
   int64_t id = 0;
   memory_db_summary_create(user_id, "sess", "summary text", "topics", "neutral", 1, 1,
                            (conv_id > 0) ? &prov : NULL, &id);
   return id;
}

void test_summaries_get_sources_roundtrip(void) {
   int64_t sid = insert_summary_with_prov(1, 10, 100, 101);
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t conv[1] = { 0 }, start[1] = { 0 }, end[1] = { 0 };
   int rc = memory_db_summaries_get_sources(1, &sid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[0]);
   TEST_ASSERT_EQUAL(100, start[0]);
   TEST_ASSERT_EQUAL(101, end[0]);
}

void test_summaries_get_sources_null_prov_returns_zero(void) {
   int64_t sid = insert_summary_with_prov(1, 0, 0, 0);
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_summaries_get_sources(1, &sid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_summaries_get_sources_private_conv_filter(void) {
   int64_t sid = insert_summary_with_prov(1, 20, 200, 200); /* conv 20 private */
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_summaries_get_sources(1, &sid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_summaries_get_sources_n_chunked_succeeds(void) {
   int64_t sid = insert_summary_with_prov(1, 10, 100, 101);
   TEST_ASSERT_GREATER_THAN(0, sid);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? sid : (i + 1000);
   int rc = memory_db_summaries_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
}

/* ------------------------------ preferences ------------------------------ */

/* memory_db_pref_upsert returns MEMORY_DB_SUCCESS but does not surface the row
 * id; the test queries it back directly. */
static int64_t pref_id_for_category(int user_id, const char *cat) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT id FROM memory_preferences WHERE user_id=? AND category=?",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, cat, -1, SQLITE_TRANSIENT);
   int64_t id = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      id = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return id;
}

void test_prefs_get_sources_roundtrip(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   memory_db_pref_upsert(1, "theme", "dark", 0.8f, "inferred", &prov);
   int64_t pid = pref_id_for_category(1, "theme");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t conv[1] = { 0 }, start[1] = { 0 }, end[1] = { 0 };
   int rc = memory_db_prefs_get_sources(1, &pid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[0]);
   TEST_ASSERT_EQUAL(100, start[0]);
   TEST_ASSERT_EQUAL(101, end[0]);
}

void test_prefs_get_sources_null_prov_returns_zero(void) {
   memory_db_pref_upsert(1, "units", "metric", 0.8f, "inferred", NULL);
   int64_t pid = pref_id_for_category(1, "units");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_prefs_get_sources(1, &pid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_prefs_get_sources_private_conv_filter(void) {
   memory_provenance_t prov = { .conv_id = 20, .msg_id_start = 200, .msg_id_end = 200 };
   memory_db_pref_upsert(1, "secret_pref", "x", 0.8f, "inferred", &prov);
   int64_t pid = pref_id_for_category(1, "secret_pref");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t conv[1] = { 99 }, start[1] = { 99 }, end[1] = { 99 };
   int rc = memory_db_prefs_get_sources(1, &pid, 1, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, conv[0]);
}

void test_prefs_get_sources_n_chunked_succeeds(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   memory_db_pref_upsert(1, "chunked_test", "x", 0.8f, "inferred", &prov);
   int64_t pid = pref_id_for_category(1, "chunked_test");
   TEST_ASSERT_GREATER_THAN(0, pid);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? pid : (i + 1000);
   int rc = memory_db_prefs_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
}

/* Pin the existing facts batch reader against the same chunked-success
 * contract.  (Pre-Phase-B this silently truncated past ~32 IDs; B.1
 * introduced fail-closed; this fold-in re-expanded the public surface to any N
 * via internal chunking.) */
void test_facts_get_sources_n_chunked_succeeds(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 101 };
   int64_t fid = 0;
   memory_db_fact_create(1, "chunked fact", 0.9f, "inferred", "general", &prov, &fid);
   TEST_ASSERT_GREATER_THAN(0, fid);
   int64_t ids[64];
   int64_t conv[64] = { 0 }, start[64] = { 0 }, end[64] = { 0 };
   for (int i = 0; i < 64; i++)
      ids[i] = (i == 50) ? fid : (i + 1000);
   int rc = memory_db_facts_get_sources(1, ids, 64, conv, start, end);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(10, conv[50]);
   TEST_ASSERT_EQUAL(100, start[50]);
   TEST_ASSERT_EQUAL(101, end[50]);
}

/* =============================================================================
 * memory_db_fact_provenance_extend (paraphrase-dedup write path).
 *
 * Four branches matter:
 *   1. Fact has no prior provenance — adopt the new triple wholesale.
 *   2. Same conv as existing — widen msg range (start = min, end = max).
 *   3. Newer conv than existing — replace.
 *   4. Older conv than existing — no-op (most-recent-wins semantics).
 * Plus: ownership check, missing-fact check.
 * ============================================================================= */

void test_provenance_extend_adopts_when_no_prior(void) {
   /* No-prov fact (NULL prov on create). */
   int64_t id = 0;
   memory_db_fact_create(1, "User likes hiking", 0.8f, "inferred", "interests", NULL, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   int rc = memory_db_fact_provenance_extend(id, 1, /* new_conv */ 10, /* start */ 100,
                                             /* end */ 105);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   int64_t conv = 0, s = 0, e = 0;
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get_source(id, 1, &conv, &s, &e));
   TEST_ASSERT_EQUAL(10, conv);
   TEST_ASSERT_EQUAL(100, s);
   TEST_ASSERT_EQUAL(105, e);
}

void test_provenance_extend_same_conv_widens_range(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 105 };
   int64_t id = 0;
   memory_db_fact_create(1, "User has 3 children", 0.9f, "explicit", "personal", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* Newer mention in same conv at messages 200-210 — widen to 100..210. */
   int rc = memory_db_fact_provenance_extend(id, 1, 10, 200, 210);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   int64_t conv = 0, s = 0, e = 0;
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get_source(id, 1, &conv, &s, &e));
   TEST_ASSERT_EQUAL(10, conv);
   TEST_ASSERT_EQUAL(100, s); /* start preserved (min) */
   TEST_ASSERT_EQUAL(210, e); /* end advanced (max) */

   /* Earlier mention in the same conv at messages 50-99 — start should
    * shrink to 50, end stays at 210. */
   rc = memory_db_fact_provenance_extend(id, 1, 10, 50, 99);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get_source(id, 1, &conv, &s, &e));
   TEST_ASSERT_EQUAL(50, s);
   TEST_ASSERT_EQUAL(210, e);
}

void test_provenance_extend_newer_conv_replaces(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 105 };
   int64_t id = 0;
   memory_db_fact_create(1, "User lives in Atlanta", 0.9f, "explicit", "personal", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* New mention in conv 25 (later) — replace the whole triple. */
   int rc = memory_db_fact_provenance_extend(id, 1, 25, 500, 502);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   int64_t conv = 0, s = 0, e = 0;
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get_source(id, 1, &conv, &s, &e));
   TEST_ASSERT_EQUAL(25, conv);
   TEST_ASSERT_EQUAL(500, s);
   TEST_ASSERT_EQUAL(502, e);
}

void test_provenance_extend_older_conv_is_noop(void) {
   memory_provenance_t prov = { .conv_id = 25, .msg_id_start = 500, .msg_id_end = 502 };
   int64_t id = 0;
   memory_db_fact_create(1, "User works at Netsurion", 0.9f, "explicit", "professional", &prov,
                         &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* Older conv 10 — should NOT overwrite the more-recent conv 25. */
   int rc = memory_db_fact_provenance_extend(id, 1, 10, 100, 105);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);

   int64_t conv = 0, s = 0, e = 0;
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get_source(id, 1, &conv, &s, &e));
   TEST_ASSERT_EQUAL(25, conv); /* unchanged */
   TEST_ASSERT_EQUAL(500, s);
   TEST_ASSERT_EQUAL(502, e);
}

void test_provenance_extend_wrong_user_returns_not_found(void) {
   memory_provenance_t prov = { .conv_id = 10, .msg_id_start = 100, .msg_id_end = 105 };
   int64_t id = 0;
   memory_db_fact_create(1, "User likes coffee", 0.9f, "explicit", "preferences", &prov, &id);
   TEST_ASSERT_GREATER_THAN(0, id);

   /* Wrong user must fail without modifying the row. */
   int rc = memory_db_fact_provenance_extend(id, /* wrong user */ 99, 25, 500, 502);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);

   int64_t conv = 0, s = 0, e = 0;
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get_source(id, 1, &conv, &s, &e));
   TEST_ASSERT_EQUAL(10, conv); /* unchanged */
}

void test_provenance_extend_missing_fact_returns_not_found(void) {
   int rc = memory_db_fact_provenance_extend(999999, 1, 10, 100, 105);
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, rc);
}

/* =============================================================================
 * memory_db_facts_delete_by_patterns (cleanup-meta-facts admin command).
 *
 * Verifies pattern-based bulk delete with dry-run, multi-pattern, user
 * scoping, and parameterized LIKE escape behavior.
 * ============================================================================= */

void test_cleanup_meta_facts_dry_run_counts_without_delete(void) {
   int64_t id1 = 0, id2 = 0, id3 = 0;
   memory_db_fact_create(1, "User asked about gold prices", 0.7f, "inferred", "general", NULL,
                         &id1);
   memory_db_fact_create(1, "User has 3 children", 0.9f, "explicit", "personal", NULL, &id2);
   memory_db_fact_create(1, "User inquired about lights", 0.7f, "inferred", "general", NULL, &id3);

   const char *patterns[] = { "User asked%", "User inquired%" };
   int matched = -1;
   int rc = memory_db_facts_delete_by_patterns(1, patterns, 2, /* dry_run */ true, &matched);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(2, matched);

   /* All three rows should still be present. */
   memory_fact_t got = { 0 };
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(id1, 1, &got));
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(id2, 1, &got));
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(id3, 1, &got));
}

void test_cleanup_meta_facts_commit_deletes_matches_only(void) {
   int64_t id_meta1 = 0, id_meta2 = 0, id_keep = 0, id_other_user = 0;
   memory_db_fact_create(1, "User asked about weather", 0.7f, "inferred", "general", NULL,
                         &id_meta1);
   memory_db_fact_create(1, "User requested weather info", 0.7f, "inferred", "general", NULL,
                         &id_meta2);
   memory_db_fact_create(1, "User lives in Atlanta", 0.9f, "explicit", "personal", NULL, &id_keep);
   /* Scope check — user 2's matching row must NOT be touched. */
   memory_db_fact_create(2, "User asked about something", 0.7f, "inferred", "general", NULL,
                         &id_other_user);

   const char *patterns[] = { "User asked%", "User requested%" };
   int deleted = -1;
   int rc = memory_db_facts_delete_by_patterns(1, patterns, 2, /* dry_run */ false, &deleted);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(2, deleted);

   memory_fact_t got = { 0 };
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, memory_db_fact_get(id_meta1, 1, &got));
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, memory_db_fact_get(id_meta2, 1, &got));
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(id_keep, 1, &got));
   /* Other user's row preserved (still present under user 2's scope). */
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(id_other_user, 2, &got));
   /* And user 1 cannot see user 2's row (CWE-639 defense-in-depth). */
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, memory_db_fact_get(id_other_user, 1, &got));
}

void test_cleanup_meta_facts_no_match_returns_zero(void) {
   int64_t id = 0;
   memory_db_fact_create(1, "User has 3 children", 0.9f, "explicit", "personal", NULL, &id);

   const char *patterns[] = { "Nonexistent prefix%" };
   int matched = -1;
   int rc = memory_db_facts_delete_by_patterns(1, patterns, 1, /* dry_run */ false, &matched);
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL(0, matched);

   memory_fact_t got = { 0 };
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(id, 1, &got));
}

/* =============================================================================
 * v58 (C3) — fact-expiry retrieval guard + prune
 * ============================================================================= */

/* The expiry retrieval guard hides a past-expiry fact only when enabled, leaves
 * the by-id fetch un-guarded (carrying expires_at), and restores instantly on
 * disable with no row mutation.  Pins the numbered-param guard binds. */
void test_expiry_guard_hides_and_restores(void) {
   g_config.memory.expire_enabled = false; /* known starting state */

   int64_t durable_id = 0, transient_id = 0;
   memory_db_fact_create(1, "Jon lives in Atlanta", 0.9f, "explicit", "personal", NULL,
                         &durable_id);
   memory_db_fact_create(1, "Forecast for 2026-04-12: clear", 0.9f, "explicit", "general", NULL,
                         &transient_id);
   TEST_ASSERT_TRUE(durable_id > 0 && transient_id > 0);

   int64_t past = (int64_t)time(NULL) - 86400; /* expired one day ago */
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_set_expires_at(transient_id, 1, past));

   memory_fact_t facts[8];
   int n = 0;

   /* Disabled → guard admits everything. */
   memory_db_fact_list(1, facts, 8, 0, &n);
   TEST_ASSERT_EQUAL(2, n);

   /* Enabled → expired fact hidden, durable remains. */
   g_config.memory.expire_enabled = true;
   memory_db_fact_list(1, facts, 8, 0, &n);
   TEST_ASSERT_EQUAL(1, n);
   TEST_ASSERT_EQUAL(durable_id, facts[0].id);

   /* By-id fetch stays un-guarded and carries expires_at (col 10). */
   memory_fact_t got = { 0 };
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(transient_id, 1, &got));
   TEST_ASSERT_EQUAL_INT64(past, got.expires_at);

   /* Consumer-side predicate mirrors the SQL guard. */
   TEST_ASSERT_TRUE(memory_db_fact_expiry_hidden(past));
   TEST_ASSERT_FALSE(memory_db_fact_expiry_hidden((int64_t)time(NULL) + 86400)); /* future */
   TEST_ASSERT_FALSE(memory_db_fact_expiry_hidden(0));                           /* durable */

   /* Disable → instant restore, no row mutation. */
   g_config.memory.expire_enabled = false;
   TEST_ASSERT_FALSE(memory_db_fact_expiry_hidden(past));
   memory_db_fact_list(1, facts, 8, 0, &n);
   TEST_ASSERT_EQUAL(2, n);
}

/* memory_db_fact_list_sorted honors the sort order: DEFAULT keeps confidence DESC,
 * CREATED_DESC/ASC order by created_at.  Guards the WebUI sort statements + the
 * enum->statement selection. */
void test_fact_list_sort_orders(void) {
   g_config.memory.expire_enabled = false;

   int64_t now = (int64_t)time(NULL);
   int64_t id_old = 0, id_mid = 0, id_new = 0;
   /* confidence order (0.9 > 0.7 > 0.5) is deliberately different from age order
    * so DEFAULT (confidence) and CREATED_* produce distinct sequences. */
   memory_db_fact_create_at(1, "oldest fact", 0.5f, "explicit", "general", NULL, now - 300,
                            &id_old);
   memory_db_fact_create_at(1, "middle fact", 0.9f, "explicit", "general", NULL, now - 200,
                            &id_mid);
   memory_db_fact_create_at(1, "newest fact", 0.7f, "explicit", "general", NULL, now - 100,
                            &id_new);
   TEST_ASSERT_TRUE(id_old > 0 && id_mid > 0 && id_new > 0);

   memory_fact_t facts[8];
   int n = 0;

   /* DEFAULT == confidence DESC: mid(0.9), new(0.7), old(0.5). */
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS,
                     memory_db_fact_list_sorted(1, MEMORY_SORT_DEFAULT, facts, 8, 0, &n));
   TEST_ASSERT_EQUAL(3, n);
   TEST_ASSERT_EQUAL_INT64(id_mid, facts[0].id);
   TEST_ASSERT_EQUAL_INT64(id_new, facts[1].id);
   TEST_ASSERT_EQUAL_INT64(id_old, facts[2].id);

   /* CREATED_DESC == newest first: new, mid, old. */
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS,
                     memory_db_fact_list_sorted(1, MEMORY_SORT_CREATED_DESC, facts, 8, 0, &n));
   TEST_ASSERT_EQUAL(3, n);
   TEST_ASSERT_EQUAL_INT64(id_new, facts[0].id);
   TEST_ASSERT_EQUAL_INT64(id_mid, facts[1].id);
   TEST_ASSERT_EQUAL_INT64(id_old, facts[2].id);

   /* CREATED_ASC == oldest first: old, mid, new. */
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS,
                     memory_db_fact_list_sorted(1, MEMORY_SORT_CREATED_ASC, facts, 8, 0, &n));
   TEST_ASSERT_EQUAL(3, n);
   TEST_ASSERT_EQUAL_INT64(id_old, facts[0].id);
   TEST_ASSERT_EQUAL_INT64(id_mid, facts[1].id);
   TEST_ASSERT_EQUAL_INT64(id_new, facts[2].id);

   /* The plain list stays the DEFAULT (confidence) order. */
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_list(1, facts, 8, 0, &n));
   TEST_ASSERT_EQUAL_INT64(id_mid, facts[0].id);
}

/* prune_expired hard-deletes facts past their reference date (retention 0) and
 * leaves durable + future-expiry facts intact. */
void test_prune_expired_removes_past_only(void) {
   int64_t durable_id = 0, transient_id = 0, future_id = 0;
   memory_db_fact_create(1, "Jon works at Acme", 0.9f, "explicit", "professional", NULL,
                         &durable_id);
   memory_db_fact_create(1, "Event on 2026-01-01", 0.9f, "explicit", "general", NULL,
                         &transient_id);
   memory_db_fact_create(1, "Launch on 2030-01-01", 0.9f, "explicit", "general", NULL, &future_id);

   memory_db_fact_set_expires_at(transient_id, 1, (int64_t)time(NULL) - 86400);
   memory_db_fact_set_expires_at(future_id, 1, (int64_t)time(NULL) + 30 * 86400);

   int deleted = 0;
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_prune_expired(1, 0, &deleted));
   TEST_ASSERT_EQUAL(1, deleted); /* only the past-expiry fact */

   memory_fact_t got = { 0 };
   TEST_ASSERT_EQUAL(MEMORY_DB_NOT_FOUND, memory_db_fact_get(transient_id, 1, &got));
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(durable_id, 1, &got));
   TEST_ASSERT_EQUAL(MEMORY_DB_SUCCESS, memory_db_fact_get(future_id, 1, &got));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_fact_create_with_source_roundtrip);
   RUN_TEST(test_fact_create_null_provenance_returns_not_found);
   RUN_TEST(test_null_bind_produces_sql_null_not_zero);
   RUN_TEST(test_get_source_wrong_user_returns_not_found);
   RUN_TEST(test_get_source_private_conv_returns_not_found);
   RUN_TEST(test_pref_upsert_with_source_roundtrip);
   RUN_TEST(test_pref_upsert_latest_source_wins);
   RUN_TEST(test_summary_create_with_source);
   RUN_TEST(test_summary_create_null_provenance);
   RUN_TEST(test_pref_upsert_null_provenance_sql_null);
   RUN_TEST(test_get_source_nonexistent_fact);
   RUN_TEST(test_conversations_have_last_extracted_msg_id_column);

   /* Phase B batch readers (12 new tests) */
   RUN_TEST(test_relations_get_sources_roundtrip);
   RUN_TEST(test_relations_get_sources_null_prov_returns_zero);
   RUN_TEST(test_relations_get_sources_private_conv_filter);
   RUN_TEST(test_relations_get_sources_n_chunked_succeeds);
   RUN_TEST(test_summaries_get_sources_roundtrip);
   RUN_TEST(test_summaries_get_sources_null_prov_returns_zero);
   RUN_TEST(test_summaries_get_sources_private_conv_filter);
   RUN_TEST(test_summaries_get_sources_n_chunked_succeeds);
   RUN_TEST(test_prefs_get_sources_roundtrip);
   RUN_TEST(test_prefs_get_sources_null_prov_returns_zero);
   RUN_TEST(test_prefs_get_sources_private_conv_filter);
   RUN_TEST(test_prefs_get_sources_n_chunked_succeeds);

   /* Pin the facts batch reader against the same chunked-success contract. */
   RUN_TEST(test_facts_get_sources_n_chunked_succeeds);

   /* Provenance extend (paraphrase-dedup write path) — 6 cases. */
   RUN_TEST(test_provenance_extend_adopts_when_no_prior);
   RUN_TEST(test_provenance_extend_same_conv_widens_range);
   RUN_TEST(test_provenance_extend_newer_conv_replaces);
   RUN_TEST(test_provenance_extend_older_conv_is_noop);
   RUN_TEST(test_provenance_extend_wrong_user_returns_not_found);
   RUN_TEST(test_provenance_extend_missing_fact_returns_not_found);

   /* cleanup-meta-facts pattern-based delete — 3 cases. */
   RUN_TEST(test_cleanup_meta_facts_dry_run_counts_without_delete);
   RUN_TEST(test_cleanup_meta_facts_commit_deletes_matches_only);
   RUN_TEST(test_cleanup_meta_facts_no_match_returns_zero);

   /* v58 fact-expiry guard + prune — 2 cases. */
   RUN_TEST(test_expiry_guard_hides_and_restores);
   RUN_TEST(test_fact_list_sort_orders);
   RUN_TEST(test_prune_expired_removes_past_only);

   return UNITY_END();
}
