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
 * Unit tests for relation-driven fact supersede.  Verifies that when an
 * exclusive relation is superseded, the old relation's linked fact_id is
 * returned so the caller can propagate the supersede to the fact layer.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "memory/memory_db.h"
#include "unity.h"

/* ============================================================================
 * Schema + Statement Setup
 * ============================================================================ */

static const char *DDL = "CREATE TABLE IF NOT EXISTS memory_facts ("
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
                         "  superseded_at INTEGER,"
                         "  normalized_hash INTEGER DEFAULT 0,"
                         "  embedding BLOB DEFAULT NULL,"
                         "  embedding_norm REAL DEFAULT NULL"
                         ");"
                         "CREATE TABLE IF NOT EXISTS memory_entities ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  name TEXT NOT NULL,"
                         "  entity_type TEXT NOT NULL,"
                         "  canonical_name TEXT NOT NULL,"
                         "  embedding BLOB DEFAULT NULL,"
                         "  embedding_norm REAL DEFAULT NULL,"
                         "  photo_id TEXT DEFAULT NULL,"
                         "  first_seen INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
                         "  last_seen INTEGER,"
                         "  mention_count INTEGER DEFAULT 1,"
                         "  UNIQUE(user_id, canonical_name)"
                         ");"
                         "CREATE TABLE IF NOT EXISTS memory_relations ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  subject_entity_id INTEGER NOT NULL,"
                         "  relation TEXT NOT NULL,"
                         "  object_entity_id INTEGER,"
                         "  object_value TEXT,"
                         "  fact_id INTEGER,"
                         "  confidence REAL DEFAULT 0.8,"
                         "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
                         "  valid_from INTEGER DEFAULT NULL,"
                         "  valid_to INTEGER DEFAULT NULL,"
                         /* v40 + v49 columns — mirror production so the upsert
                          * prepared statement's 12 bind slots all have targets
                          * and the partial UNIQUE index has rows to constrain. */
                         "  source_conversation_id INTEGER DEFAULT NULL,"
                         "  source_msg_id_start    INTEGER DEFAULT NULL,"
                         "  source_msg_id_end      INTEGER DEFAULT NULL,"
                         "  mention_count INTEGER NOT NULL DEFAULT 1"
                         ");"
                         /* v49 partial UNIQUE — MUST exist before stmt_memory_relation_create
                          * prepare, otherwise the ON CONFLICT clause has no matching
                          * uniqueness constraint to resolve against. */
                         "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_relations_unique_open "
                         "ON memory_relations(user_id, subject_entity_id, relation, "
                         "                    COALESCE(object_entity_id, 0), "
                         "                    COALESCE(object_value, '')) "
                         "WHERE valid_to IS NULL;";

static void setup_db(void) {
   memset(&s_db, 0, sizeof(s_db));
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   pthread_mutex_init(&s_db.mutex, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   /* Prepare statements used by memory_db_relation_supersede().  Production
    * v49 upsert: ON CONFLICT against idx_memory_relations_unique_open bumps
    * mention_count on a re-witnessed open relation.  Mirrors production SQL
    * in src/auth/auth_db_statements.c so the test exercises the same path. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
                           "object_entity_id, object_value, fact_id, confidence, created_at, "
                           "valid_from, valid_to, "
                           "source_conversation_id, source_msg_id_start, source_msg_id_end) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'), ?, ?, ?, ?, ?) "
                           "ON CONFLICT(user_id, subject_entity_id, relation, "
                           "            COALESCE(object_entity_id, 0), COALESCE(object_value, '')) "
                           "WHERE valid_to IS NULL DO UPDATE SET "
                           "  mention_count = mention_count + 1, "
                           "  confidence = MAX(confidence, excluded.confidence), "
                           "  source_conversation_id = excluded.source_conversation_id, "
                           "  source_msg_id_start = excluded.source_msg_id_start, "
                           "  source_msg_id_end = excluded.source_msg_id_end, "
                           "  fact_id = COALESCE(fact_id, excluded.fact_id) "
                           "RETURNING id, mention_count",
                           -1, &s_db.stmt_memory_relation_create, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare relation_create failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_relations SET valid_to = ? "
                           "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                           "  AND valid_to IS NULL "
                           "  AND (COALESCE(object_entity_id, 0) != COALESCE(?, 0) "
                           "    OR COALESCE(object_value, '') != COALESCE(?, '')) "
                           "RETURNING fact_id",
                           -1, &s_db.stmt_memory_relation_close_open, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare relation_close_open failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   /* Prepare statements used by memory_db_fact_supersede() — SQL mirrors
    * the CWE-639 defense-in-depth in production: (old_id, user_id) gates
    * the UPDATE and an EXISTS subquery enforces same-user ownership of
    * new_fact_id (cross-user pointer prevention). */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET superseded_by = ?, "
                           "superseded_at = CAST(strftime('%s', 'now') AS INTEGER) "
                           "WHERE id = ? AND user_id = ? "
                           "AND EXISTS (SELECT 1 FROM memory_facts WHERE id = ? AND user_id = ?)",
                           -1, &s_db.stmt_memory_fact_supersede, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare fact_supersede failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }

   s_db.initialized = true;
}

static void teardown_db(void) {
   if (s_db.stmt_memory_relation_create)
      sqlite3_finalize(s_db.stmt_memory_relation_create);
   if (s_db.stmt_memory_relation_close_open)
      sqlite3_finalize(s_db.stmt_memory_relation_close_open);
   if (s_db.stmt_memory_fact_supersede)
      sqlite3_finalize(s_db.stmt_memory_fact_supersede);
   s_db.stmt_memory_relation_create = NULL;
   s_db.stmt_memory_relation_close_open = NULL;
   s_db.stmt_memory_fact_supersede = NULL;
   if (s_db.db)
      sqlite3_close(s_db.db);
   pthread_mutex_destroy(&s_db.mutex);
   memset(&s_db, 0, sizeof(s_db));
}

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

/* ============================================================================
 * Helpers — direct SQL inserts for test setup
 * ============================================================================ */

static int64_t insert_fact(int user_id, const char *text) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "INSERT INTO memory_facts (user_id, fact_text) VALUES (?, ?)", -1,
                      &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, text, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static int64_t insert_entity(int user_id, const char *name, const char *type) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name) "
                      "VALUES (?, ?, ?, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, type, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, name, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

/* Count facts in the canonical recall set — mirrors the `superseded_by IS NULL`
 * filter every production retrieval query carries (e.g. memory_embeddings.c). */
static int count_active_facts(int user_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(
       s_db.db, "SELECT COUNT(*) FROM memory_facts WHERE user_id = ? AND superseded_by IS NULL", -1,
       &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

/* Raw row existence by id (harness-independent — proves the row survived a supersede). */
static int count_rows_with_id(int64_t fact_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM memory_facts WHERE id = ?", -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, fact_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

static int64_t get_fact_superseded_by(int64_t fact_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT superseded_by FROM memory_facts WHERE id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, fact_id);
   int64_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      result = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return result;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_exclusive_supersede_returns_old_fact_id(void) {
   int user_id = 1;
   int64_t fact_a = insert_fact(user_id, "Alice works at Google");
   int64_t alice = insert_entity(user_id, "alice", "person");
   int64_t google = insert_entity(user_id, "google", "org");
   int64_t microsoft = insert_entity(user_id, "microsoft", "org");

   memory_db_relation_supersede(user_id, alice, "works_at", google, NULL, fact_a, 0.9f, 0, 0, NULL,
                                NULL);

   int64_t fact_b = insert_fact(user_id, "Alice works at Microsoft");
   int64_t old_fact_id = 0;
   int rc = memory_db_relation_supersede(user_id, alice, "works_at", microsoft, NULL, fact_b, 0.9f,
                                         0, 0, NULL, &old_fact_id);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(fact_a, old_fact_id);

   memory_db_fact_supersede(old_fact_id, fact_b, user_id);
   int64_t superseded_by = get_fact_superseded_by(fact_a);
   TEST_ASSERT_EQUAL_INT64(fact_b, superseded_by);
}

static void test_no_fact_id_on_old_relation(void) {
   int user_id = 2;
   int64_t bob = insert_entity(user_id, "bob", "person");
   int64_t nyc = insert_entity(user_id, "nyc", "place");
   int64_t sf = insert_entity(user_id, "sf", "place");

   memory_db_relation_supersede(user_id, bob, "lives_in", nyc, NULL, 0, 0.8f, 0, 0, NULL, NULL);

   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, bob, "lives_in", sf, NULL, 0, 0.8f, 0, 0, NULL,
                                         &old_fact_id);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, old_fact_id);
}

static void test_non_exclusive_skips(void) {
   int user_id = 3;
   int64_t fact_a = insert_fact(user_id, "Carol likes cats");
   int64_t carol = insert_entity(user_id, "carol", "person");
   int64_t cats = insert_entity(user_id, "cats", "thing");
   int64_t dogs = insert_entity(user_id, "dogs", "thing");

   memory_db_relation_supersede(user_id, carol, "likes", cats, NULL, fact_a, 0.8f, 0, 0, NULL,
                                NULL);

   int64_t fact_b = insert_fact(user_id, "Carol likes dogs");
   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, carol, "likes", dogs, NULL, fact_b, 0.8f, 0, 0,
                                         NULL, &old_fact_id);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, old_fact_id);
}

static void test_null_out_param(void) {
   int user_id = 4;
   int64_t dave = insert_entity(user_id, "dave", "person");
   int64_t mit = insert_entity(user_id, "mit", "org");

   int rc = memory_db_relation_supersede(user_id, dave, "attends_school", mit, NULL, 0, 0.8f, 0, 0,
                                         NULL, NULL);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
}

static void test_same_object_idempotent(void) {
   int user_id = 5;
   int64_t fact_a = insert_fact(user_id, "Eve works at Apple");
   int64_t eve = insert_entity(user_id, "eve", "person");
   int64_t apple = insert_entity(user_id, "apple", "org");

   memory_db_relation_supersede(user_id, eve, "works_at", apple, NULL, fact_a, 0.9f, 0, 0, NULL,
                                NULL);

   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, eve, "works_at", apple, NULL, fact_a, 0.9f, 0, 0,
                                         NULL, &old_fact_id);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, old_fact_id);
}

static void test_contradictory_pair(void) {
   int user_id = 6;
   int64_t fact_a = insert_fact(user_id, "Frank likes spiders");
   int64_t frank = insert_entity(user_id, "frank", "person");
   int64_t spiders = insert_entity(user_id, "spiders", "thing");

   memory_db_relation_supersede(user_id, frank, "likes", spiders, NULL, fact_a, 0.8f, 0, 0, NULL,
                                NULL);

   int64_t fact_b = insert_fact(user_id, "Frank dislikes spiders");
   int64_t old_fact_id = 0;
   int rc = memory_db_relation_supersede(user_id, frank, "dislikes", spiders, NULL, fact_b, 0.9f, 0,
                                         0, NULL, &old_fact_id);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(fact_a, old_fact_id);
}

static void test_contradictory_pair_different_object(void) {
   int user_id = 7;
   int64_t fact_a = insert_fact(user_id, "Grace enjoys cooking");
   int64_t grace = insert_entity(user_id, "grace", "person");

   memory_db_relation_supersede(user_id, grace, "enjoys", 0, "cooking", fact_a, 0.8f, 0, 0, NULL,
                                NULL);

   int64_t fact_b = insert_fact(user_id, "Grace hates gardening");
   int64_t old_fact_id = -1;
   int rc = memory_db_relation_supersede(user_id, grace, "hates", 0, "gardening", fact_b, 0.9f, 0,
                                         0, NULL, &old_fact_id);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, old_fact_id);
}

/* ============================================================================
 * v49 dedup test helpers
 * ============================================================================ */

/* Look up the open row for (user, subject, relation, object_value) and
 * return its mention_count.  Returns -1 if no row matches. */
static int query_open_mention_count(int user_id,
                                    int64_t subj,
                                    const char *rel,
                                    int64_t obj_id,
                                    const char *obj_val) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT mention_count FROM memory_relations "
                      "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                      "  AND COALESCE(object_entity_id, 0) = COALESCE(?, 0) "
                      "  AND COALESCE(object_value, '') = COALESCE(?, '') "
                      "  AND valid_to IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subj);
   sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
   if (obj_id > 0)
      sqlite3_bind_int64(stmt, 4, obj_id);
   else
      sqlite3_bind_null(stmt, 4);
   if (obj_val)
      sqlite3_bind_text(stmt, 5, obj_val, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(stmt, 5);
   int result = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      result = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return result;
}

/* Count rows matching the open dedup group — useful for asserting the
 * partial UNIQUE invariant ("only one open row per group"). */
static int count_open_rows(int user_id,
                           int64_t subj,
                           const char *rel,
                           int64_t obj_id,
                           const char *obj_val) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_relations "
                      "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                      "  AND COALESCE(object_entity_id, 0) = COALESCE(?, 0) "
                      "  AND COALESCE(object_value, '') = COALESCE(?, '') "
                      "  AND valid_to IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subj);
   sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
   if (obj_id > 0)
      sqlite3_bind_int64(stmt, 4, obj_id);
   else
      sqlite3_bind_null(stmt, 4);
   if (obj_val)
      sqlite3_bind_text(stmt, 5, obj_val, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(stmt, 5);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return count;
}

/* Read the stored confidence, valid_from, source_conversation_id, and
 * fact_id of the open winner row in a group.  Pointers may be NULL to skip
 * the corresponding read. */
static void query_open_winner_fields(int user_id,
                                     int64_t subj,
                                     const char *rel,
                                     int64_t obj_id,
                                     const char *obj_val,
                                     double *conf_out,
                                     int64_t *valid_from_out,
                                     int64_t *src_conv_out,
                                     int64_t *fact_id_out) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(
       s_db.db,
       "SELECT confidence, COALESCE(valid_from, 0), COALESCE(source_conversation_id, 0), "
       "       COALESCE(fact_id, 0) FROM memory_relations "
       "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
       "  AND COALESCE(object_entity_id, 0) = COALESCE(?, 0) "
       "  AND COALESCE(object_value, '') = COALESCE(?, '') "
       "  AND valid_to IS NULL",
       -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subj);
   sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
   if (obj_id > 0)
      sqlite3_bind_int64(stmt, 4, obj_id);
   else
      sqlite3_bind_null(stmt, 4);
   if (obj_val)
      sqlite3_bind_text(stmt, 5, obj_val, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(stmt, 5);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      if (conf_out)
         *conf_out = sqlite3_column_double(stmt, 0);
      if (valid_from_out)
         *valid_from_out = sqlite3_column_int64(stmt, 1);
      if (src_conv_out)
         *src_conv_out = sqlite3_column_int64(stmt, 2);
      if (fact_id_out)
         *fact_id_out = sqlite3_column_int64(stmt, 3);
   }
   sqlite3_finalize(stmt);
}

/* ============================================================================
 * v49 dedup tests
 * ============================================================================ */

static void test_supersede_increments_mention_count(void) {
   int user_id = 10;
   int64_t dave = insert_entity(user_id, "dave", "person");
   int64_t hiking = insert_entity(user_id, "hiking", "thing");

   /* First insert: row inserted, mention_count starts at 1. */
   int rc = memory_db_relation_supersede(user_id, dave, "enjoys", hiking, NULL, 0, 0.8f, 0, 0, NULL,
                                         NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, query_open_mention_count(user_id, dave, "enjoys", hiking, NULL));
   TEST_ASSERT_EQUAL_INT(1, count_open_rows(user_id, dave, "enjoys", hiking, NULL));

   /* Re-witness same edge: upsert path, mention_count bumps to 2, still one row. */
   rc = memory_db_relation_supersede(user_id, dave, "enjoys", hiking, NULL, 0, 0.8f, 0, 0, NULL,
                                     NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, query_open_mention_count(user_id, dave, "enjoys", hiking, NULL));
   TEST_ASSERT_EQUAL_INT(1, count_open_rows(user_id, dave, "enjoys", hiking, NULL));

   /* Third witness — partial UNIQUE keeps row count at 1. */
   rc = memory_db_relation_supersede(user_id, dave, "enjoys", hiking, NULL, 0, 0.8f, 0, 0, NULL,
                                     NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, query_open_mention_count(user_id, dave, "enjoys", hiking, NULL));
   TEST_ASSERT_EQUAL_INT(1, count_open_rows(user_id, dave, "enjoys", hiking, NULL));
}

static void test_supersede_dedup_preserves_max_confidence(void) {
   int user_id = 11;
   int64_t eve = insert_entity(user_id, "eve", "person");
   int64_t painting = insert_entity(user_id, "painting", "thing");

   /* Lower-then-higher: stored confidence rises to the new high. */
   memory_db_relation_supersede(user_id, eve, "enjoys", painting, NULL, 0, 0.7f, 0, 0, NULL, NULL);
   memory_db_relation_supersede(user_id, eve, "enjoys", painting, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   double conf = 0;
   query_open_winner_fields(user_id, eve, "enjoys", painting, NULL, &conf, NULL, NULL, NULL);
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, (float)conf);

   /* Higher-then-lower: stored confidence stays at the high (no downgrade). */
   int user_id2 = 12;
   int64_t frank = insert_entity(user_id2, "frank", "person");
   int64_t cooking = insert_entity(user_id2, "cooking", "thing");
   memory_db_relation_supersede(user_id2, frank, "enjoys", cooking, NULL, 0, 0.9f, 0, 0, NULL,
                                NULL);
   memory_db_relation_supersede(user_id2, frank, "enjoys", cooking, NULL, 0, 0.7f, 0, 0, NULL,
                                NULL);
   conf = 0;
   query_open_winner_fields(user_id2, frank, "enjoys", cooking, NULL, &conf, NULL, NULL, NULL);
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, (float)conf);
}

static void test_supersede_dedup_keeps_existing_valid_from(void) {
   int user_id = 13;
   int64_t gina = insert_entity(user_id, "gina", "person");
   int64_t yoga = insert_entity(user_id, "yoga", "thing");

   /* First insert with valid_from=1000.  Re-witness with valid_from=2000
    * (latest-wins would overwrite; the upsert intentionally omits
    * valid_from from UPDATE-SET so the original start-of-validity bound
    * sticks). */
   memory_db_relation_supersede(user_id, gina, "enjoys", yoga, NULL, 0, 0.8f, 1000, 0, NULL, NULL);
   memory_db_relation_supersede(user_id, gina, "enjoys", yoga, NULL, 0, 0.8f, 2000, 0, NULL, NULL);

   int64_t vf = 0;
   query_open_winner_fields(user_id, gina, "enjoys", yoga, NULL, NULL, &vf, NULL, NULL);
   TEST_ASSERT_EQUAL_INT64(1000, vf);
}

static void test_supersede_dedup_takes_latest_provenance(void) {
   int user_id = 14;
   int64_t henry = insert_entity(user_id, "henry", "person");
   int64_t pottery = insert_entity(user_id, "pottery", "thing");

   memory_provenance_t prov_a = { .conv_id = 100, .msg_id_start = 1, .msg_id_end = 5 };
   memory_provenance_t prov_b = { .conv_id = 200, .msg_id_start = 7, .msg_id_end = 9 };

   memory_db_relation_supersede(user_id, henry, "enjoys", pottery, NULL, 0, 0.8f, 0, 0, &prov_a,
                                NULL);
   memory_db_relation_supersede(user_id, henry, "enjoys", pottery, NULL, 0, 0.8f, 0, 0, &prov_b,
                                NULL);

   int64_t src_conv = 0;
   query_open_winner_fields(user_id, henry, "enjoys", pottery, NULL, NULL, NULL, &src_conv, NULL);
   TEST_ASSERT_EQUAL_INT64(200, src_conv);
}

static void test_supersede_dedup_coalesces_fact_id(void) {
   int user_id = 15;
   int64_t iris = insert_entity(user_id, "iris", "person");
   int64_t sailing = insert_entity(user_id, "sailing", "thing");

   /* NULL → non-NULL: orphan adopts the new fact_id. */
   memory_db_relation_supersede(user_id, iris, "enjoys", sailing, NULL, 0, 0.8f, 0, 0, NULL, NULL);
   int64_t fact_a = insert_fact(user_id, "Iris loves sailing");
   memory_db_relation_supersede(user_id, iris, "enjoys", sailing, NULL, fact_a, 0.8f, 0, 0, NULL,
                                NULL);
   int64_t stored = 0;
   query_open_winner_fields(user_id, iris, "enjoys", sailing, NULL, NULL, NULL, NULL, &stored);
   TEST_ASSERT_EQUAL_INT64(fact_a, stored);

   /* Non-NULL → NULL: existing link preserved (COALESCE). */
   int user_id2 = 16;
   int64_t jake = insert_entity(user_id2, "jake", "person");
   int64_t kayaking = insert_entity(user_id2, "kayaking", "thing");
   int64_t fact_b = insert_fact(user_id2, "Jake enjoys kayaking");
   memory_db_relation_supersede(user_id2, jake, "enjoys", kayaking, NULL, fact_b, 0.8f, 0, 0, NULL,
                                NULL);
   memory_db_relation_supersede(user_id2, jake, "enjoys", kayaking, NULL, 0, 0.8f, 0, 0, NULL,
                                NULL);
   stored = 0;
   query_open_winner_fields(user_id2, jake, "enjoys", kayaking, NULL, NULL, NULL, NULL, &stored);
   TEST_ASSERT_EQUAL_INT64(fact_b, stored);
}

static void test_supersede_to_different_object_starts_fresh_mention_count(void) {
   int user_id = 17;
   int64_t kate = insert_entity(user_id, "kate", "person");
   int64_t boston = insert_entity(user_id, "boston", "place");
   int64_t seattle = insert_entity(user_id, "seattle", "place");

   /* Build up mention_count = 3 on (kate, lives_in, boston). */
   memory_db_relation_supersede(user_id, kate, "lives_in", boston, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   memory_db_relation_supersede(user_id, kate, "lives_in", boston, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   memory_db_relation_supersede(user_id, kate, "lives_in", boston, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(3, query_open_mention_count(user_id, kate, "lives_in", boston, NULL));

   /* Supersede to a different object — exclusive-relation close path closes
    * the boston row (valid_to set), then the upsert inserts a fresh open
    * row for seattle with mention_count = 1.  Confirms that supersede
    * semantically replaces — close-then-upsert is correct, not "increment
    * the new object's count to 4". */
   int64_t old_fact_id = 0;
   int rc = memory_db_relation_supersede(user_id, kate, "lives_in", seattle, NULL, 0, 0.9f, 0, 0,
                                         NULL, &old_fact_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* boston row is now closed (valid_to set) — not visible via open query. */
   TEST_ASSERT_EQUAL_INT(0, count_open_rows(user_id, kate, "lives_in", boston, NULL));
   /* seattle row is fresh and open with mention_count = 1. */
   TEST_ASSERT_EQUAL_INT(1, count_open_rows(user_id, kate, "lives_in", seattle, NULL));
   TEST_ASSERT_EQUAL_INT(1, query_open_mention_count(user_id, kate, "lives_in", seattle, NULL));
}

/* ============================================================================
 * Main
 * ============================================================================ */

/* C4: the dedup-merge path ('forget ... replaced_by: keeper') supersedes a fact into a
 * keeper.  Pins the invariants that make the merge safe: the merged fact drops out of the
 * canonical recall set, points at its survivor, and its row survives (recoverable). */
static void test_fact_supersede_hides_from_recall_but_keeps_row(void) {
   int user_id = 7;
   int64_t keep = insert_fact(user_id, "Jon prefers direct tool execution");
   int64_t dupe = insert_fact(user_id, "Jon prefers direct action execution");

   TEST_ASSERT_EQUAL_INT(2, count_active_facts(user_id)); /* both active to start */

   int rc = memory_db_fact_supersede(dupe, keep, user_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Hidden from recall (superseded_by IS NULL filter), keeper still active. */
   TEST_ASSERT_EQUAL_INT(1, count_active_facts(user_id));
   /* Points at the survivor. */
   TEST_ASSERT_EQUAL_INT64(keep, get_fact_superseded_by(dupe));

   /* Row survives -> recoverable: the merge soft-hides, never deletes. */
   TEST_ASSERT_EQUAL_INT(1, count_rows_with_id(dupe));
}

/* C4 / CWE-639: a user superseding their fact into ANOTHER user's fact must not create a
 * cross-user pointer.  The supersede SQL's EXISTS-ownership guard makes it a no-op (it still
 * returns SUCCESS — it checks step==DONE, not changes()); the real guarantee is that no
 * pointer is written and the fact stays in recall.  The handler also guards by fetching the
 * keeper up front, so this is the DB-layer backstop. */
static void test_fact_supersede_no_cross_user_pointer(void) {
   int64_t mine = insert_fact(1, "fact owned by user 1");
   insert_fact(2, "fact owned by user 2");
   int64_t theirs = sqlite3_last_insert_rowid(s_db.db);

   memory_db_fact_supersede(mine, theirs, 1);
   TEST_ASSERT_EQUAL_INT64(0, get_fact_superseded_by(mine)); /* no cross-user pointer */
   TEST_ASSERT_EQUAL_INT(1, count_active_facts(1));          /* still in recall */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_exclusive_supersede_returns_old_fact_id);
   RUN_TEST(test_no_fact_id_on_old_relation);
   RUN_TEST(test_non_exclusive_skips);
   RUN_TEST(test_null_out_param);
   RUN_TEST(test_same_object_idempotent);
   RUN_TEST(test_contradictory_pair);
   RUN_TEST(test_contradictory_pair_different_object);
   /* v49 dedup tests */
   RUN_TEST(test_supersede_increments_mention_count);
   RUN_TEST(test_supersede_dedup_preserves_max_confidence);
   RUN_TEST(test_supersede_dedup_keeps_existing_valid_from);
   RUN_TEST(test_supersede_dedup_takes_latest_provenance);
   RUN_TEST(test_supersede_dedup_coalesces_fact_id);
   RUN_TEST(test_supersede_to_different_object_starts_fresh_mention_count);
   RUN_TEST(test_fact_supersede_hides_from_recall_but_keeps_row);
   RUN_TEST(test_fact_supersede_no_cross_user_pointer);
   return UNITY_END();
}
