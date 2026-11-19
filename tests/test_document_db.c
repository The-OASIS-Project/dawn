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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for document_db.c — CRUD for documents and chunks.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "tools/document_db.h"
#include "unity.h"

/* ============================================================================
 * Test Harness
 * ============================================================================ */


/* ============================================================================
 * Setup / Teardown
 * ============================================================================ */

/* clang-format off */
static const char *DDL =
   "CREATE TABLE IF NOT EXISTS users ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  username TEXT UNIQUE NOT NULL,"
   "  real_name TEXT DEFAULT NULL,"
   "  preferred_address TEXT DEFAULT NULL,"
   "  identity_aliases TEXT DEFAULT NULL"
   ");"
   "INSERT INTO users (id, username) VALUES (1, 'alice');"
   "INSERT INTO users (id, username) VALUES (2, 'bob');"
   "CREATE TABLE IF NOT EXISTS documents ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER,"
   "  filename TEXT NOT NULL,"
   "  filepath TEXT NOT NULL,"
   "  filetype TEXT NOT NULL,"
   "  file_hash TEXT NOT NULL,"
   "  num_chunks INTEGER NOT NULL,"
   "  is_global INTEGER DEFAULT 0,"
   "  created_at INTEGER NOT NULL,"
   "  original_blob_id TEXT DEFAULT NULL,"
   "  outside INTEGER NOT NULL DEFAULT 0,"
   "  own INTEGER NOT NULL DEFAULT 0,"
   "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "CREATE TABLE IF NOT EXISTS document_chunks ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  document_id INTEGER NOT NULL,"
   "  chunk_index INTEGER NOT NULL,"
   "  text TEXT NOT NULL,"
   "  embedding BLOB NOT NULL,"
   "  embedding_norm REAL NOT NULL,"
   "  created_at INTEGER NOT NULL DEFAULT 0,"
   "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
   ");";
/* clang-format on */

/* SQL must match auth_db_core.c exactly */
static int prepare_statements(void) {
   int rc;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO documents (user_id, filename, filepath, filetype, file_hash, "
       "num_chunks, is_global, created_at, original_blob_id, outside) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CASE WHEN ?4 = 'note' THEN 0 ELSE 5 END)",
       -1, &s_db.stmt_doc_create, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, filename, filepath, filetype, file_hash, "
       "num_chunks, is_global, created_at, outside, own FROM documents WHERE id = ?",
       -1, &s_db.stmt_doc_get, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id FROM documents WHERE file_hash = ? "
                           "AND (user_id = ? OR is_global = 1)",
                           -1, &s_db.stmt_doc_get_by_hash, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at, outside, own FROM documents "
                           "WHERE user_id = ? OR is_global = 1 ORDER BY created_at DESC "
                           "LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT d.id, d.user_id, d.filename, d.filepath, d.filetype, "
       "d.file_hash, d.num_chunks, d.is_global, d.created_at, d.outside, d.own, "
       "COALESCE(u.username, '') FROM documents d "
       "LEFT JOIN users u ON d.user_id = u.id "
       "ORDER BY d.created_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_doc_list_all, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE documents SET is_global = ? WHERE id = ?", -1,
                           &s_db.stmt_doc_update_global, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM documents WHERE id = ?", -1, &s_db.stmt_doc_delete,
                           NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM documents WHERE user_id = ?", -1,
                           &s_db.stmt_doc_count_user, NULL);
   if (rc != SQLITE_OK)
      return -1;

   /* Mirrors the v35 shape in auth_db_core.c — tests were silently passing
    * before because out-of-range bind/column accesses fail without propagating
    * an error code, masking drift between this harness and production SQL. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO document_chunks (document_id, chunk_index, text, embedding, "
       "embedding_norm, created_at) VALUES (?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_chunk_create, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at, outside, own "
                           "FROM documents "
                           "WHERE (user_id = ? OR is_global = 1) "
                           "AND filename LIKE ? ESCAPE '\\' COLLATE NOCASE "
                           "ORDER BY CASE WHEN LOWER(filename) = LOWER(?) "
                           "THEN 0 ELSE 1 END, created_at DESC LIMIT 1",
                           -1, &s_db.stmt_doc_find_by_name, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT chunk_index, text FROM document_chunks "
                           "WHERE document_id = ? ORDER BY chunk_index LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_chunk_read, NULL);
   if (rc != SQLITE_OK)
      return -1;

   /* Mirror the production statements (auth_db_statements.c) so the new
    * range/grep primitives are exercised under the test harness. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT chunk_index, text FROM document_chunks "
                           "WHERE document_id = ? AND chunk_index BETWEEN ? AND ? "
                           "ORDER BY chunk_index",
                           -1, &s_db.stmt_doc_chunk_read_range, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.chunk_index, c.document_id, d.filename, d.num_chunks, "
       "d.user_id = ?1 AND " OUTSIDE_OWN_SQL(
           "d.own", "d.outside") " "
                                 "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                                 "WHERE (d.user_id = ?1 OR d.is_global = 1) "
                                 "AND c.text LIKE ?2 ESCAPE '\\' "
                                 "ORDER BY c.document_id, c.chunk_index LIMIT ?3 OFFSET ?4",
       -1, &s_db.stmt_doc_chunk_grep_ci, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.chunk_index, c.document_id, d.filename, d.num_chunks, "
       "d.user_id = ?1 AND " OUTSIDE_OWN_SQL(
           "d.own", "d.outside") " "
                                 "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                                 "WHERE (d.user_id = ?1 OR d.is_global = 1) "
                                 "AND instr(c.text, ?2) > 0 "
                                 "ORDER BY c.document_id, c.chunk_index LIMIT ?3 OFFSET ?4",
       -1, &s_db.stmt_doc_chunk_grep_cs, NULL);
   if (rc != SQLITE_OK)
      return -1;

   return 0;
}

static void setup_db(void) {
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   /* Enable foreign keys */
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   if (prepare_statements() != 0) {
      fprintf(stderr, "Prepare statements failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   s_db.initialized = true;
}

static void teardown_db(void) {
   s_db.initialized = false;

   /* Finalize all document statements */
   if (s_db.stmt_doc_create)
      sqlite3_finalize(s_db.stmt_doc_create);
   if (s_db.stmt_doc_get)
      sqlite3_finalize(s_db.stmt_doc_get);
   if (s_db.stmt_doc_get_by_hash)
      sqlite3_finalize(s_db.stmt_doc_get_by_hash);
   if (s_db.stmt_doc_list)
      sqlite3_finalize(s_db.stmt_doc_list);
   if (s_db.stmt_doc_list_all)
      sqlite3_finalize(s_db.stmt_doc_list_all);
   if (s_db.stmt_doc_update_global)
      sqlite3_finalize(s_db.stmt_doc_update_global);
   if (s_db.stmt_doc_delete)
      sqlite3_finalize(s_db.stmt_doc_delete);
   if (s_db.stmt_doc_count_user)
      sqlite3_finalize(s_db.stmt_doc_count_user);
   if (s_db.stmt_doc_chunk_create)
      sqlite3_finalize(s_db.stmt_doc_chunk_create);
   if (s_db.stmt_doc_find_by_name)
      sqlite3_finalize(s_db.stmt_doc_find_by_name);
   if (s_db.stmt_doc_chunk_read)
      sqlite3_finalize(s_db.stmt_doc_chunk_read);
   if (s_db.stmt_doc_chunk_read_range)
      sqlite3_finalize(s_db.stmt_doc_chunk_read_range);
   if (s_db.stmt_doc_chunk_grep_ci)
      sqlite3_finalize(s_db.stmt_doc_chunk_grep_ci);
   if (s_db.stmt_doc_chunk_grep_cs)
      sqlite3_finalize(s_db.stmt_doc_chunk_grep_cs);

   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }
}

/* ============================================================================
 * Test: Create and Get
 * ============================================================================ */

static void test_create_and_get(void) {
   printf("\n--- test_create_and_get ---\n");

   int64_t id = 0;
   int create_rc = document_db_create(1, "report.pdf", "/docs/report.pdf", "pdf",
                                      "aabbccdd11223344aabbccdd11223344"
                                      "aabbccdd11223344aabbccdd11223344",
                                      5, false, &id);
   TEST_ASSERT_TRUE_MESSAGE(create_rc == SUCCESS && id > 0,
                            "create returns SUCCESS with positive ID");

   document_t doc;
   int rc = document_db_get(id, &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "get returns success");
   TEST_ASSERT_TRUE_MESSAGE(doc.id == id, "ID matches");
   TEST_ASSERT_TRUE_MESSAGE(doc.user_id == 1, "user_id matches");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(doc.filename, "report.pdf") == 0, "filename matches");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(doc.filetype, "pdf") == 0, "filetype matches");
   TEST_ASSERT_TRUE_MESSAGE(doc.num_chunks == 5, "num_chunks matches");
   TEST_ASSERT_TRUE_MESSAGE(doc.is_global == false, "is_global is false");
   TEST_ASSERT_TRUE_MESSAGE(doc.created_at > 0, "created_at is set");
}

/* ============================================================================
 * Test: Get Non-existent
 * ============================================================================ */

static void test_get_nonexistent(void) {
   printf("\n--- test_get_nonexistent ---\n");

   document_t doc;
   int rc = document_db_get(99999, &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == FAILURE, "get non-existent returns FAILURE");
}

/* ============================================================================
 * Test: Delete
 * ============================================================================ */

static void test_delete(void) {
   printf("\n--- test_delete ---\n");

   int64_t id = 0;
   document_db_create(1, "temp.txt", "/docs/temp.txt", "txt",
                      "1111111111111111111111111111111111111111111111111111111111111111", 1, false,
                      &id);
   TEST_ASSERT_TRUE_MESSAGE(id > 0, "create for delete test");

   int rc = document_db_delete(id);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "delete returns success");

   document_t doc;
   rc = document_db_get(id, &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == FAILURE, "get after delete returns FAILURE");
}

/* ============================================================================
 * Test: Count User
 * ============================================================================ */

static void test_count_user(void) {
   printf("\n--- test_count_user ---\n");

   /* User 2 starts with 0 docs */
   int count = 0;
   document_db_count_user(2, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "user 2 starts with 0 docs");

   /* Create two docs for user 2 */
   int64_t tmp_id = 0;
   document_db_create(2, "a.txt", "a.txt", "txt",
                      "2222222222222222222222222222222222222222222222222222222222222222", 1, false,
                      &tmp_id);
   document_db_create(2, "b.txt", "b.txt", "txt",
                      "3333333333333333333333333333333333333333333333333333333333333333", 2, false,
                      &tmp_id);

   document_db_count_user(2, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 2, "user 2 has 2 docs after creating two");
}

/* ============================================================================
 * Test: Find by Hash
 * ============================================================================ */

static void test_find_by_hash(void) {
   printf("\n--- test_find_by_hash ---\n");

   const char *hash = "4444444444444444444444444444444444444444444444444444444444444444";
   int64_t id = 0;
   document_db_create(1, "hashed.md", "hashed.md", "md", hash, 3, false, &id);
   TEST_ASSERT_TRUE_MESSAGE(id > 0, "create doc with known hash");

   int64_t found = 0;
   document_db_find_by_hash(hash, 1, &found);
   TEST_ASSERT_TRUE_MESSAGE(found == id, "find_by_hash returns correct ID");

   int64_t not_found = 0;
   document_db_find_by_hash("0000000000000000000000000000000000000000000"
                            "000000000000000000000000",
                            1, &not_found);
   TEST_ASSERT_TRUE_MESSAGE(not_found == 0, "find_by_hash returns 0 for unknown hash");
}

/* ============================================================================
 * Test: List (user-scoped)
 * ============================================================================ */

static void test_list(void) {
   printf("\n--- test_list ---\n");

   document_t docs[10];
   int count = 0;
   int list_rc = document_db_list(1, docs, 10, 0, &count);
   TEST_ASSERT_TRUE_MESSAGE(list_rc == SUCCESS && count >= 0,
                            "list returns SUCCESS with non-negative count");

   /* All returned docs should belong to user 1 or be global */
   int all_accessible = 1;
   for (int i = 0; i < count; i++) {
      if (docs[i].user_id != 1 && !docs[i].is_global)
         all_accessible = 0;
   }
   TEST_ASSERT_TRUE_MESSAGE(all_accessible, "list only returns user's own or global docs");
}

/* ============================================================================
 * Test: List All (admin view)
 * ============================================================================ */

static void test_list_all(void) {
   printf("\n--- test_list_all ---\n");

   int64_t id1 = 0, id2 = 0;
   document_db_create(1, "u1.txt", "u1.txt", "txt", "hash_u1", 1, false, &id1);
   document_db_create(2, "u2.txt", "u2.txt", "txt", "hash_u2", 1, false, &id2);

   document_t docs[20];
   int count = 0;
   int list_rc = document_db_list_all(docs, 20, 0, &count);
   TEST_ASSERT_TRUE_MESSAGE(list_rc == SUCCESS && count >= 0,
                            "list_all returns SUCCESS with non-negative count");

   int has_user1 = 0, has_user2 = 0;
   for (int i = 0; i < count; i++) {
      if (docs[i].user_id == 1)
         has_user1 = 1;
      if (docs[i].user_id == 2)
         has_user2 = 1;
   }
   TEST_ASSERT_TRUE_MESSAGE(has_user1, "list_all includes user 1 docs");
   TEST_ASSERT_TRUE_MESSAGE(has_user2, "list_all includes user 2 docs");

   int has_owner_name = 0;
   for (int i = 0; i < count; i++) {
      if (strlen(docs[i].owner_name) > 0)
         has_owner_name = 1;
   }
   TEST_ASSERT_TRUE_MESSAGE(has_owner_name, "list_all populates owner_name from users JOIN");
}

/* ============================================================================
 * Test: List Pagination
 * ============================================================================ */

static void test_list_pagination(void) {
   printf("\n--- test_list_pagination ---\n");

   document_t docs[5];

   /* Get first page (limit 2) */
   int page1 = 0;
   document_db_list_all(docs, 2, 0, &page1);
   TEST_ASSERT_TRUE_MESSAGE(page1 >= 0, "page 1 returns non-negative count");

   /* Get second page */
   int page2 = 0;
   document_db_list_all(docs, 2, 2, &page2);
   TEST_ASSERT_TRUE_MESSAGE(page2 >= 0, "page 2 returns non-negative count");

   /* Total should equal list_all with high limit */
   int total = 0;
   document_db_list_all(docs, 20, 0, &total);
   TEST_ASSERT_TRUE_MESSAGE(page1 + page2 <= total, "pages don't exceed total");
}

/* ============================================================================
 * Test: Update Global
 * ============================================================================ */

static void test_update_global(void) {
   printf("\n--- test_update_global ---\n");

   int64_t id = 0;
   document_db_create(1, "private.txt", "private.txt", "txt",
                      "5555555555555555555555555555555555555555555555555555555555555555", 1, false,
                      &id);
   TEST_ASSERT_TRUE_MESSAGE(id > 0, "create private doc");

   document_t doc;
   document_db_get(id, &doc);
   TEST_ASSERT_TRUE_MESSAGE(doc.is_global == false, "starts as private");

   int rc = document_db_update_global(id, true);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "update_global returns success");

   document_db_get(id, &doc);
   TEST_ASSERT_TRUE_MESSAGE(doc.is_global == true, "now global after update");

   rc = document_db_update_global(id, false);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "update_global back to private");

   document_db_get(id, &doc);
   TEST_ASSERT_TRUE_MESSAGE(doc.is_global == false, "private again after second update");
}

/* ============================================================================
 * Test: Global Doc Visible to Other Users
 * ============================================================================ */

static void test_global_visibility(void) {
   printf("\n--- test_global_visibility ---\n");

   int64_t id = 0;
   document_db_create(1, "shared.pdf", "shared.pdf", "pdf",
                      "6666666666666666666666666666666666666666666666666666666666666666", 2, true,
                      &id);
   TEST_ASSERT_TRUE_MESSAGE(id > 0, "create global doc owned by user 1");

   /* User 2 should see it in their list */
   document_t docs[10];
   int count = 0;
   document_db_list(2, docs, 10, 0, &count);
   int found = 0;
   for (int i = 0; i < count; i++) {
      if (docs[i].id == id)
         found = 1;
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "global doc visible to user 2 via list");
}

/* ============================================================================
 * Test: Find by Name
 * ============================================================================ */

static void test_find_by_name(void) {
   printf("\n--- test_find_by_name ---\n");

   int64_t id = 0;
   document_db_create(1, "MyReport2024.pdf", "MyReport2024.pdf", "pdf",
                      "7777777777777777777777777777777777777777777777777777777777777777", 4, false,
                      &id);
   TEST_ASSERT_TRUE_MESSAGE(id > 0, "create doc for name search");

   document_t doc;
   int rc = document_db_find_by_name(1, "MyReport2024.pdf", &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "exact name match found");
   TEST_ASSERT_TRUE_MESSAGE(doc.id == id, "exact match returns correct doc");

   rc = document_db_find_by_name(1, "Report2024", &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "partial name match found");
   TEST_ASSERT_TRUE_MESSAGE(doc.id == id, "partial match returns correct doc");

   rc = document_db_find_by_name(1, "nonexistent_document", &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == FAILURE, "non-existent name returns FAILURE");

   /* User 2 should NOT find user 1's private doc */
   rc = document_db_find_by_name(2, "MyReport2024.pdf", &doc);
   TEST_ASSERT_TRUE_MESSAGE(rc == FAILURE, "other user cannot find private doc by name");
}

/* ============================================================================
 * Test: Chunk Create and Read
 * ============================================================================ */

static void test_chunk_create_and_read(void) {
   printf("\n--- test_chunk_create_and_read ---\n");

   int64_t doc_id = 0;
   document_db_create(1, "chunked.txt", "chunked.txt", "txt",
                      "888888888888888888888888888888888888888888888888888888"
                      "8888888888",
                      3, false, &doc_id);
   TEST_ASSERT_TRUE_MESSAGE(doc_id > 0, "create doc for chunk test");

   /* Create 3 chunks with dummy embeddings */
   float emb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
   int64_t c0 = 0, c1 = 0, c2 = 0;
   document_db_chunk_create(doc_id, 0, "First chunk text.", emb, 4, 1.0f, 0, &c0);
   document_db_chunk_create(doc_id, 1, "Second chunk text.", emb, 4, 1.0f, 0, &c1);
   document_db_chunk_create(doc_id, 2, "Third chunk text.", emb, 4, 1.0f, 0, &c2);
   TEST_ASSERT_TRUE_MESSAGE(c0 > 0, "chunk 0 created");
   TEST_ASSERT_TRUE_MESSAGE(c1 > 0, "chunk 1 created");
   TEST_ASSERT_TRUE_MESSAGE(c2 > 0, "chunk 2 created");

   /* Read all chunks */
   document_chunk_t chunks[5];
   int count = 0;
   document_db_chunk_read(doc_id, chunks, 5, 0, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 3, "read returns 3 chunks");
   TEST_ASSERT_TRUE_MESSAGE(chunks[0].chunk_index == 0, "chunk 0 index correct");
   TEST_ASSERT_TRUE_MESSAGE(chunks[1].chunk_index == 1, "chunk 1 index correct");
   TEST_ASSERT_TRUE_MESSAGE(chunks[2].chunk_index == 2, "chunk 2 index correct");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(chunks[0].text, "First chunk text.") == 0,
                            "chunk 0 text matches");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(chunks[2].text, "Third chunk text.") == 0,
                            "chunk 2 text matches");

   /* Paginated read: start at chunk 1, limit 2 */
   document_db_chunk_read(doc_id, chunks, 2, 1, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 2, "paginated read returns 2 chunks");
   TEST_ASSERT_TRUE_MESSAGE(chunks[0].chunk_index == 1, "first paginated chunk is index 1");
   TEST_ASSERT_TRUE_MESSAGE(chunks[1].chunk_index == 2, "second paginated chunk is index 2");
}

/* A note's chunk is the user's own words unless its text carries outside text
 * (a research report, a note written in a turn that read a page); one stored
 * before the kind was recorded counts as own.  A document's isn't until the
 * user marks it as their own. */
static void test_chunks_by_ids_own(void) {
   float emb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
   int64_t note = 0, report = 0, web = 0, old = 0, doc = 0, ids[5] = { 0 };
   document_db_create(1, "Groceries", "", "note", "h-note", 1, false, &note);
   document_db_create(1, "Research #1: Tea", "", "note", "h-report", 1, false, &report);
   document_db_create(1, "Tea notes", "", "note", "h-web", 1, false, &web);
   document_db_create(1, "Old note", "", "note", "h-old", 1, false, &old);
   document_db_create(1, "a.pdf", "a.pdf", "pdf", "h-doc", 1, false, &doc);
   document_db_chunk_create(note, 0, "Eggs", emb, 4, 1.0f, 0, &ids[0]);
   document_db_chunk_create(report, 0, "Tea grows", emb, 4, 1.0f, 0, &ids[1]);
   document_db_chunk_create(web, 0, "Tea is", emb, 4, 1.0f, 0, &ids[2]);
   document_db_chunk_create(old, 0, "Milk", emb, 4, 1.0f, 0, &ids[3]);
   document_db_chunk_create(doc, 0, "A page", emb, 4, 1.0f, 0, &ids[4]);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, report, OUTSIDE_RESEARCH, false, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, web, OUTSIDE_WEB, false, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, old, OUTSIDE_UNKNOWN, false, OUTSIDE_NONE));

   document_chunk_t out[5];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_chunks_get_by_ids(1, ids, 5, out, &n));
   TEST_ASSERT_EQUAL_INT(5, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL(out[i].id == ids[0] || out[i].id == ids[3], out[i].own);
   }
}

/* A note another user published globally reads as outside text, never as the
 * reader's own words; its owner still reads it as own. */
static void test_chunks_by_ids_global_note_not_own(void) {
   float emb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
   int64_t note = 0, id = 0;
   document_db_create(1, "Shared", "", "note", "h-shared", 1, true, &note);
   document_db_chunk_create(note, 0, "Shared words", emb, 4, 1.0f, 0, &id);

   document_chunk_t out[1];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_chunks_get_by_ids(2, &id, 1, out, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_FALSE(out[0].own);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_chunks_get_by_ids(1, &id, 1, out, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_TRUE(out[0].own);
}

static int doc_outside(int64_t id) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT outside FROM documents WHERE id = ?", -1, &st, NULL);
   sqlite3_bind_int64(st, 1, id);
   const int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

/* A save replaces a note's kind; an edit keeps a tainted one (part of the old
 * text stays) and takes the turn's when that one is; another user's note is
 * never touched. */
static void test_set_outside_combine(void) {
   int64_t note = 0;
   document_db_create(1, "Tea", "", "note", "h-tea", 1, false, &note);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, note, OUTSIDE_NONE, true, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, doc_outside(note));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, note, OUTSIDE_WEB, true, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, doc_outside(note));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, note, OUTSIDE_NONE, true, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, doc_outside(note));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, note, OUTSIDE_EMAIL, true, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, doc_outside(note));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, note, OUTSIDE_NONE, false, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, doc_outside(note));
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         document_db_set_outside(2, note, OUTSIDE_WEB, false, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, doc_outside(note));
}

static int doc_own_flag(int64_t id) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT own FROM documents WHERE id = ?", -1, &st, NULL);
   sqlite3_bind_int64(st, 1, id);
   const int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

/* Whether chunk @p chunk reads as user @p user's own words. */
static bool chunk_own(int user, int64_t chunk) {
   document_chunk_t out[1];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_chunks_get_by_ids(user, &chunk, 1, out, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   return out[0].own;
}

/* The user's vouch is kept apart from where the text came from: on makes a
 * document's chunk the user's words and keeps its kind; off clears the vouch
 * and turns only an unrecorded kind (a note's NONE) into DOCUMENT; only the
 * owner can do either (a global one included). */
static void test_set_own(void) {
   float emb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
   int64_t pdf = 0, note = 0, report = 0, chunk = 0, rchunk = 0, nchunk = 0;
   document_db_create(1, "own.pdf", "own.pdf", "pdf", "h-own", 1, true, &pdf);
   document_db_create(1, "Own note", "", "note", "h-own-note", 1, false, &note);
   document_db_create(1, "Research #1", "", "note", "h-own-report", 1, false, &report);
   document_db_chunk_create(pdf, 0, "My words", emb, 4, 1.0f, 0, &chunk);
   document_db_chunk_create(report, 0, "Tea grows", emb, 4, 1.0f, 0, &rchunk);
   document_db_chunk_create(note, 0, "Eggs", emb, 4, 1.0f, 0, &nchunk);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, report, OUTSIDE_RESEARCH, false, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, doc_outside(pdf));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, doc_outside(note));
   TEST_ASSERT_FALSE(chunk_own(1, chunk));

   /* On: the vouch, its provenance kept. */
   int outside = -1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, pdf, true, &outside));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside);
   document_t d;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(pdf, &d));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, d.outside);
   TEST_ASSERT_TRUE(d.own);
   TEST_ASSERT_TRUE(document_is_own(&d, 1));
   TEST_ASSERT_TRUE(chunk_own(1, chunk));
   /* Another user can't change it, and never reads it as their own. */
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_set_own(2, pdf, false, NULL));
   TEST_ASSERT_EQUAL_INT(1, doc_own_flag(pdf));
   TEST_ASSERT_FALSE(chunk_own(2, chunk));

   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, pdf, false, &outside));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside);
   TEST_ASSERT_EQUAL_INT(0, doc_own_flag(pdf));
   TEST_ASSERT_FALSE(chunk_own(1, chunk));

   /* A research report on, then off: still a research report, not own. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, report, true, &outside));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside);
   TEST_ASSERT_TRUE(chunk_own(1, rchunk));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, report, false, &outside));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, outside);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, doc_outside(report));
   TEST_ASSERT_FALSE(chunk_own(1, rchunk));

   /* A note off: no recorded kind, so it becomes a document. */
   TEST_ASSERT_TRUE(chunk_own(1, nchunk));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, note, false, &outside));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, outside);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, doc_outside(note));
   TEST_ASSERT_FALSE(chunk_own(1, nchunk));
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_set_own(1, 999999, true, NULL));
}

/* A write in a turn that read outside text drops the user's vouch (it was for
 * the old text); a clean turn's, or one no turn made (a panel restore),
 * leaves it. */
static void test_tainted_write_clears_own(void) {
   int64_t pdf = 0;
   document_db_create(1, "plan.pdf", "plan.pdf", "pdf", "h-vouch", 1, false, &pdf);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, pdf, true, NULL));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, pdf, OUTSIDE_NONE, true, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(1, doc_own_flag(pdf));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_set_outside(1, pdf, OUTSIDE_EMAIL, true, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(1, doc_own_flag(pdf)); /* a restore of a tainted version */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_outside(1, pdf, OUTSIDE_WEB, true, OUTSIDE_WEB));
   TEST_ASSERT_EQUAL_INT(0, doc_own_flag(pdf));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, doc_outside(pdf)); /* the first kind kept */
}

/* What the document tools frame: a grep hit and a document read as the
 * reader's own only for their own document of their own words; a document
 * not marked own, or another user's shared one, isn't. */
static void test_document_tools_own(void) {
   float emb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
   int64_t note = 0, pdf = 0, cid = 0;
   document_db_create(1, "Own note", "", "note", "h-tool-note", 1, true, &note);
   document_db_create(1, "upload.pdf", "upload.pdf", "pdf", "h-tool-pdf", 1, false, &pdf);
   document_db_chunk_create(note, 0, "Owl_Marker_7 in my words", emb, 4, 1.0f, 0, &cid);
   document_db_chunk_create(pdf, 0, "Owl_Marker_7 in a page", emb, 4, 1.0f, 0, &cid);

   doc_grep_hit_t hits[4];
   int n = 0;
   bool more = false;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_chunk_grep(1, "Owl_Marker_7", false, 0, hits, 4, &n, &more));
   TEST_ASSERT_EQUAL_INT(2, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL(hits[i].document_id == note, hits[i].own);
   }
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_chunk_grep(2, "Owl_Marker_7", true, 0, hits, 4, &n, &more));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_FALSE(hits[0].own); /* the shared note isn't user 2's */

   document_t d;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(note, &d));
   TEST_ASSERT_TRUE(document_is_own(&d, 1));
   TEST_ASSERT_FALSE(document_is_own(&d, 2));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(pdf, &d));
   TEST_ASSERT_FALSE(document_is_own(&d, 1));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_own(1, pdf, true, NULL));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(pdf, &d));
   TEST_ASSERT_TRUE(document_is_own(&d, 1));
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_chunk_grep(1, "Owl_Marker_7", true, 0, hits, 4, &n, &more));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_TRUE(hits[0].own && hits[1].own);
}

/* ============================================================================
 * Test: chunk_read_range — window by chunk_index VALUE, gap-safe
 * ============================================================================ */

static void test_chunk_read_range(void) {
   printf("\n--- test_chunk_read_range ---\n");

   int64_t doc_id = 0;
   document_db_create(1, "ranged.txt", "ranged.txt", "txt", "range_hash_000000000000000000000000",
                      5, false, &doc_id);
   TEST_ASSERT_TRUE_MESSAGE(doc_id > 0, "create doc for range test");

   /* Create chunks at indices 0,1,3,4 — SKIP index 2 to simulate an embed
    * failure gap. This is exactly the case where OFFSET != chunk_index. */
   float emb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
   int64_t cid = 0;
   document_db_chunk_create(doc_id, 0, "idx0", emb, 4, 1.0f, 0, &cid);
   document_db_chunk_create(doc_id, 1, "idx1", emb, 4, 1.0f, 0, &cid);
   document_db_chunk_create(doc_id, 3, "idx3", emb, 4, 1.0f, 0, &cid);
   document_db_chunk_create(doc_id, 4, "idx4", emb, 4, 1.0f, 0, &cid);

   document_chunk_t chunks[8];
   int count = -1;

   /* Range [1,3] over indices {0,1,3,4} must return exactly indices 1 and 3. */
   TEST_ASSERT_TRUE_MESSAGE(
       document_db_chunk_read_range(doc_id, 1, 3, chunks, 8, &count) == SUCCESS, "range read ok");
   TEST_ASSERT_TRUE_MESSAGE(count == 2, "range [1,3] returns 2 chunks (gap at 2 skipped)");
   TEST_ASSERT_TRUE_MESSAGE(chunks[0].chunk_index == 1, "first is index 1");
   TEST_ASSERT_TRUE_MESSAGE(chunks[1].chunk_index == 3, "second is index 3 (NOT offset-based 2)");

   /* Range [2,3] must return ONLY index 3 (index 2 doesn't exist). */
   document_db_chunk_read_range(doc_id, 2, 3, chunks, 8, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "range [2,3] returns only the existing index 3");
   TEST_ASSERT_TRUE_MESSAGE(chunks[0].chunk_index == 3, "the one chunk is index 3");

   /* Negative lo is clamped to 0; hi past the end is fine. */
   document_db_chunk_read_range(doc_id, -2, 100, chunks, 8, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 4, "full clamped range returns all 4 chunks");

   /* Empty range (hi < lo) is success with 0 results, not an error. */
   TEST_ASSERT_TRUE_MESSAGE(document_db_chunk_read_range(doc_id, 3, 2, chunks, 8, &count) ==
                                SUCCESS,
                            "inverted range is not an error");
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "inverted range returns 0 chunks");
}

/* ============================================================================
 * Test: chunk_grep — literal match, permission scoping, case, pagination
 * ============================================================================ */

static void test_chunk_grep(void) {
   printf("\n--- test_chunk_grep ---\n");

   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   int64_t cid = 0;

   /* User 1 private doc with a distinctive literal string. */
   int64_t priv = 0;
   document_db_create(1, "priv.txt", "priv.txt", "txt", "grep_priv_hash_0000000000000000000", 1,
                      false, &priv);
   document_db_chunk_create(priv, 0, "the Zebra_Quux_42 marker is here", emb, 4, 1.0f, 0, &cid);

   /* Global doc visible to everyone. */
   int64_t glob = 0;
   document_db_create(1, "glob.txt", "glob.txt", "txt", "grep_glob_hash_0000000000000000000", 1,
                      true, &glob);
   document_db_chunk_create(glob, 0, "a Public_Token_99 lives here", emb, 4, 1.0f, 0, &cid);

   doc_grep_hit_t hits[8];
   int n = -1;
   bool more = true;

   /* Owner finds the private literal; coordinates populated. */
   TEST_ASSERT_TRUE_MESSAGE(document_db_chunk_grep(1, "Zebra_Quux_42", false, 0, hits, 8, &n,
                                                   &more) == SUCCESS,
                            "grep ok");
   TEST_ASSERT_TRUE_MESSAGE(n == 1, "owner finds 1 match for private literal");
   TEST_ASSERT_TRUE_MESSAGE(hits[0].document_id == priv, "hit carries the document_id");
   TEST_ASSERT_TRUE_MESSAGE(hits[0].chunk_index == 0, "hit carries chunk_index");
   TEST_ASSERT_TRUE_MESSAGE(hits[0].num_chunks == 1, "hit carries num_chunks");
   TEST_ASSERT_TRUE_MESSAGE(more == false, "no more pages for a single match");

   /* Permission: a different user must NOT see the private doc's content. */
   document_db_chunk_grep(2, "Zebra_Quux_42", false, 0, hits, 8, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 0, "other user cannot grep a private doc (permission scoped)");

   /* Global content is visible to a different user. */
   document_db_chunk_grep(2, "Public_Token_99", false, 0, hits, 8, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 1, "other user CAN grep a global doc");
   TEST_ASSERT_TRUE_MESSAGE(hits[0].document_id == glob, "global hit document_id");

   /* Case sensitivity. */
   document_db_chunk_grep(1, "zebra_quux_42", false, 0, hits, 8, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 1, "case-insensitive (default) matches different case");
   document_db_chunk_grep(1, "zebra_quux_42", true, 0, hits, 8, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 0, "case-sensitive does not match different case");
   document_db_chunk_grep(1, "Zebra_Quux_42", true, 0, hits, 8, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 1, "case-sensitive matches exact case");

   /* LIKE wildcards in the needle are treated literally (escaped). */
   document_db_chunk_grep(1, "100%_real", false, 0, hits, 8, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 0, "wildcard chars are literal, not patterns");

   /* Pagination: a common literal across many chunks, small page. */
   int64_t many = 0;
   document_db_chunk_create(priv, 1, "Public_Token_99 again", emb, 4, 1.0f, 0, &many);
   document_db_chunk_create(priv, 2, "Public_Token_99 thrice", emb, 4, 1.0f, 0, &many);
   /* user 1 sees its own priv (2 hits) + the global (1 hit) = 3 total. */
   document_db_chunk_grep(1, "Public_Token_99", false, 0, hits, 2, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 2, "page size 2 returns 2 hits");
   TEST_ASSERT_TRUE_MESSAGE(more == true, "more=true when matches exceed the page");
   document_db_chunk_grep(1, "Public_Token_99", false, 2, hits, 2, &n, &more);
   TEST_ASSERT_TRUE_MESSAGE(n == 1, "offset=2 returns the remaining 1 hit");
   TEST_ASSERT_TRUE_MESSAGE(more == false, "no more pages after the last");
}

/* ============================================================================
 * Test: NULL / Invalid Parameters
 * ============================================================================ */

static void test_invalid_params(void) {
   printf("\n--- test_invalid_params ---\n");

   /* NULL output params */
   int dummy_count = 0;
   TEST_ASSERT_TRUE_MESSAGE(document_db_list(1, NULL, 10, 0, &dummy_count) == FAILURE,
                            "list with NULL out returns FAILURE");
   TEST_ASSERT_TRUE_MESSAGE(document_db_list_all(NULL, 10, 0, &dummy_count) == FAILURE,
                            "list_all with NULL out returns FAILURE");

   /* Zero/negative limit */
   document_t docs[5];
   TEST_ASSERT_TRUE_MESSAGE(document_db_list(1, docs, 0, 0, &dummy_count) == FAILURE,
                            "list with limit 0 returns FAILURE");
   TEST_ASSERT_TRUE_MESSAGE(document_db_list(1, docs, -1, 0, &dummy_count) == FAILURE,
                            "list with negative limit returns FAILURE");

   /* Delete non-existent */
   TEST_ASSERT_TRUE_MESSAGE(document_db_delete(99999) == FAILURE,
                            "delete non-existent returns FAILURE");

   /* Update global non-existent */
   TEST_ASSERT_TRUE_MESSAGE(document_db_update_global(99999, true) == FAILURE,
                            "update_global non-existent returns FAILURE");
}

/* ============================================================================
 * Test: Cascade Delete (chunks removed with document)
 * ============================================================================ */

static void test_cascade_delete(void) {
   printf("\n--- test_cascade_delete ---\n");

   int64_t doc_id = 0;
   document_db_create(1, "cascade.txt", "cascade.txt", "txt",
                      "999999999999999999999999999999999999999999999999999999"
                      "9999999999",
                      2, false, &doc_id);
   TEST_ASSERT_TRUE_MESSAGE(doc_id > 0, "create doc for cascade test");

   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   int64_t tmp_chunk = 0;
   document_db_chunk_create(doc_id, 0, "Chunk A", emb, 4, 1.0f, 0, &tmp_chunk);
   document_db_chunk_create(doc_id, 1, "Chunk B", emb, 4, 1.0f, 0, &tmp_chunk);

   /* Verify chunks exist */
   document_chunk_t chunks[5];
   int count = 0;
   document_db_chunk_read(doc_id, chunks, 5, 0, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 2, "chunks exist before delete");

   /* Delete document — chunks should cascade */
   int rc = document_db_delete(doc_id);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "delete returns success");

   document_db_chunk_read(doc_id, chunks, 5, 0, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 0, "chunks removed after cascade delete");
}

/* ============================================================================
 * Main
 * ============================================================================ */

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_create_and_get);
   RUN_TEST(test_get_nonexistent);
   RUN_TEST(test_delete);
   RUN_TEST(test_count_user);
   RUN_TEST(test_find_by_hash);
   RUN_TEST(test_list);
   RUN_TEST(test_list_all);
   RUN_TEST(test_list_pagination);
   RUN_TEST(test_update_global);
   RUN_TEST(test_global_visibility);
   RUN_TEST(test_find_by_name);
   RUN_TEST(test_chunk_create_and_read);
   RUN_TEST(test_chunks_by_ids_own);
   RUN_TEST(test_chunks_by_ids_global_note_not_own);
   RUN_TEST(test_set_outside_combine);
   RUN_TEST(test_set_own);
   RUN_TEST(test_tainted_write_clears_own);
   RUN_TEST(test_document_tools_own);
   RUN_TEST(test_chunk_read_range);
   RUN_TEST(test_chunk_grep);
   RUN_TEST(test_invalid_params);
   RUN_TEST(test_cascade_delete);
   return UNITY_END();
}
