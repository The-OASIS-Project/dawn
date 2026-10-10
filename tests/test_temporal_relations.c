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
 * Unit tests for v33 temporal relations:
 *   - memory_db_relation_supersede auto-closes a one-at-a-time relation's prior value
 *   - memory_db_relation_supersede leaves other relations open
 *   - memory_db_relation_supersede skips close when same target re-mentioned
 *   - memory_db_relation_list_by_subject_at returns the right slice
 *   - default ("currently true") path filters out closed rows
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_types.h"
#include "unity.h"

/* DDL mirroring auth_db_core.c (relevant subset).  Covers v19 base tables
 * (memory_entities, memory_facts) plus v33 additions (memory_relations with
 * valid_from/valid_to) and v34 additions (memory_facts.category) — these
 * tests exercise both temporal relations and the canonical category list. */
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
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  UNIQUE(user_id, canonical_name)"
   ");"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (10, 1, 'Alice', 'person', 'alice');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (20, 1, 'Google', 'org', 'google');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (21, 1, 'Microsoft', 'org', 'microsoft');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (22, 1, 'Apple', 'org', 'apple');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (50, 1, 'Bob', 'person', 'bob');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (60, 1, 'CityA', 'place', 'citya');"
   "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name)"
   "  VALUES (61, 1, 'CityB', 'place', 'cityb');"
   "CREATE TABLE IF NOT EXISTS memory_facts ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  fact_text TEXT,"
   "  category TEXT NOT NULL DEFAULT 'general'"
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
   "  source_conversation_id INTEGER DEFAULT NULL,"
   "  source_msg_id_start    INTEGER DEFAULT NULL,"
   "  source_msg_id_end      INTEGER DEFAULT NULL,"
   "  mention_count INTEGER NOT NULL DEFAULT 1"
   ");"
   /* v49 partial UNIQUE — required for the upsert ON CONFLICT clause to parse. */
   "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_relations_unique_open "
   "ON memory_relations(user_id, subject_entity_id, relation, "
   "                    COALESCE(object_entity_id, 0), "
   "                    COALESCE(object_value, '')) "
   "WHERE valid_to IS NULL;";
/* clang-format on */

static int prepare_statements(void) {
   int rc;

   /* v49 upsert prep — mirrors production SQL in src/auth/auth_db_statements.c.
    * Note: this test file's relation inserts use distinct object values (lines
    * 245-247 create three lives_in rows with seattle/portland/denver as objects),
    * so the partial UNIQUE never trips — pre-existing temporal-test behavior
    * preserved.  Same-object re-witness would now bump mention_count instead
    * of creating a second row; not tested here. */
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
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db, MEMORY_RELATION_CLOSE_OPEN_SQL, -1,
                           &s_db.stmt_memory_relation_close_open, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject, NULL);
   if (rc != SQLITE_OK)
      return -1;

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0) "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? "
                           "  AND (r.valid_from IS NULL OR r.valid_from <= ?) "
                           "  AND (r.valid_to IS NULL OR r.valid_to > ?) "
                           "LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject_at, NULL);
   if (rc != SQLITE_OK)
      return -1;

   return 0;
}

static void setup_db(void) {
   if (sqlite3_open(":memory:", &s_db.db) != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB\n");
      exit(1);
   }

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

static void teardown_db(void) {
   if (s_db.stmt_memory_relation_create)
      sqlite3_finalize(s_db.stmt_memory_relation_create);
   if (s_db.stmt_memory_relation_close_open)
      sqlite3_finalize(s_db.stmt_memory_relation_close_open);
   if (s_db.stmt_memory_relation_list_by_subject)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_subject);
   if (s_db.stmt_memory_relation_list_by_subject_at)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_subject_at);
   s_db.stmt_memory_relation_create = NULL;
   s_db.stmt_memory_relation_close_open = NULL;
   s_db.stmt_memory_relation_list_by_subject = NULL;
   s_db.stmt_memory_relation_list_by_subject_at = NULL;
   if (s_db.db)
      sqlite3_close(s_db.db);
   s_db.db = NULL;
   s_db.initialized = false;
}

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

static int count_open_relations(int subject_id, const char *relation) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_relations "
                      "WHERE subject_entity_id = ? AND relation = ? AND valid_to IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, subject_id);
   sqlite3_bind_text(stmt, 2, relation, -1, SQLITE_TRANSIENT);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

/* A one-at-a-time relation (married_to): a new value closes the old. */
static void test_supersede_exclusive(void) {
   /* Insert: Alice married_to entity 20 */
   int rc = memory_db_relation_supersede(1, 10, "married_to", 20, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count_open_relations(10, "married_to"));

   /* Now: Alice married_to entity 21 — must auto-close 20 */
   rc = memory_db_relation_supersede(1, 10, "married_to", 21, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count_open_relations(10, "married_to"));
}

static void test_supersede_idempotent_same_object(void) {
   /* Insert: Alice works_at Microsoft */
   memory_db_relation_supersede(1, 10, "works_at", 21, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   int prior = count_open_relations(10, "works_at");
   TEST_ASSERT_EQUAL_INT(1, prior);

   /* Re-insert same target.  Pre-v49 behavior: the close_open Phase 1
    * skipped same-object, then the plain INSERT created a duplicate row
    * (open relation count went 1 → 2).  v49 behavior: the upsert collapses
    * both into the same row via idx_memory_relations_unique_open, bumping
    * mention_count from 1 to 2.  This is the genuinely idempotent
    * semantics the test name promised — pre-v49 was misleadingly named. */
   int rc = memory_db_relation_supersede(1, 10, "works_at", 21, NULL, 0, 0.9f, 0, 0, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(prior, count_open_relations(10, "works_at"));

   /* Confirm mention_count was bumped instead of a new row appearing. */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT mention_count FROM memory_relations "
                      "WHERE subject_entity_id = 10 AND relation = 'works_at' "
                      "  AND object_entity_id = 21 AND valid_to IS NULL",
                      -1, &stmt, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL_INT(2, sqlite3_column_int(stmt, 0));
   sqlite3_finalize(stmt);
}

static void test_non_exclusive_does_not_close(void) {
   memory_db_relation_supersede(1, 10, "likes", 20, NULL, 0, 0.7f, 0, 0, NULL, NULL);
   memory_db_relation_supersede(1, 10, "likes", 21, NULL, 0, 0.7f, 0, 0, NULL, NULL);
   memory_db_relation_supersede(1, 10, "likes", 22, NULL, 0, 0.7f, 0, 0, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(3, count_open_relations(10, "likes"));
}

static void test_list_by_subject_at(void) {
   memory_db_relation_create(1, 50, "lives_in", 60, NULL, 0, 0.9f, 1000, 2000, NULL);
   memory_db_relation_create(1, 50, "lives_in", 61, NULL, 0, 0.9f, 2000, 0, NULL);

   memory_relation_t out[10];

   /* As-of 1500: only CityA row matches */
   int n = 0;
   int rc = memory_db_relation_list_by_subject_at(1, 50, 1500, out, 10, &n);
   TEST_ASSERT_EQUAL_INT(0, rc);
   bool found_a = false, found_b = false;
   for (int i = 0; i < n; i++) {
      if (out[i].object_entity_id == 60)
         found_a = true;
      if (out[i].object_entity_id == 61)
         found_b = true;
   }
   TEST_ASSERT_TRUE_MESSAGE(found_a, "as-of 1500 includes CityA row");
   TEST_ASSERT_FALSE_MESSAGE(found_b, "as-of 1500 excludes CityB row");

   /* As-of 2500: only CityB row matches */
   rc = memory_db_relation_list_by_subject_at(1, 50, 2500, out, 10, &n);
   TEST_ASSERT_EQUAL_INT(0, rc);
   found_a = false;
   found_b = false;
   for (int i = 0; i < n; i++) {
      if (out[i].object_entity_id == 60)
         found_a = true;
      if (out[i].object_entity_id == 61)
         found_b = true;
   }
   TEST_ASSERT_FALSE_MESSAGE(found_a, "as-of 2500 excludes CityA");
   TEST_ASSERT_TRUE_MESSAGE(found_b, "as-of 2500 includes CityB row");
}

static void test_supersede_closes_at_new_valid_from(void) {
   int64_t t2018 = 1514764800;
   int64_t t2020 = 1577836800;

   memory_db_relation_supersede(1, 10, "married_to", 20, NULL, 0, 0.9f, t2018, 0, NULL, NULL);
   memory_db_relation_supersede(1, 10, "married_to", 21, NULL, 0, 0.9f, t2020, 0, NULL, NULL);

   /* As-of 2019: only A should be valid */
   memory_relation_t out[10];
   int n = 0;
   int rc = memory_db_relation_list_by_subject_at(1, 10, 1546300800, out, 10, &n);
   TEST_ASSERT_EQUAL_INT(0, rc);
   int married_to_count = 0;
   for (int i = 0; i < n; i++) {
      if (strcmp(out[i].relation, "married_to") == 0)
         married_to_count++;
   }
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, married_to_count,
                                 "as-of 2019: exactly one married_to — no overlap");
}

static void test_supersede_skips_close_for_historical_insert(void) {
   /* Establish current reality: Alice married_to entity 21, ongoing */
   memory_db_relation_supersede(1, 10, "married_to", 21, NULL, 0, 0.9f, 0, 0, NULL, NULL);

   /* Ingest HISTORICAL: Alice was married to A 2018-2020 */
   int64_t t2018 = 1514764800;
   int64_t t2020 = 1577836800;
   memory_db_relation_supersede(1, 10, "married_to", 20, NULL, 0, 0.9f, t2018, t2020, NULL, NULL);

   /* B row should still be open */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_relations "
                      "WHERE subject_entity_id = 10 AND relation = 'married_to' "
                      "  AND object_entity_id = 21 AND valid_to IS NULL",
                      -1, &stmt, NULL);
   int open_microsoft = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      open_microsoft = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, open_microsoft,
                                 "historical insert does not close current entity-21 row");

   /* As-of 2019 returns entity 20 */
   memory_relation_t out[10];
   int n = 0;
   int rc = memory_db_relation_list_by_subject_at(1, 10, 1546300800, out, 10, &n);
   TEST_ASSERT_EQUAL_INT(0, rc);
   bool found_google_2019 = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(out[i].relation, "married_to") == 0 && out[i].object_entity_id == 20)
         found_google_2019 = true;
   }
   TEST_ASSERT_TRUE_MESSAGE(found_google_2019, "as-of 2019: entity-20 row is valid");
}

static void test_canonical_categories(void) {
   TEST_ASSERT_EQUAL_INT(8, MEMORY_FACT_CATEGORY_COUNT);
   TEST_ASSERT_EQUAL_STRING("personal", MEMORY_FACT_CATEGORIES[0]);
   TEST_ASSERT_EQUAL_STRING("general", MEMORY_FACT_CATEGORIES[7]);
   TEST_ASSERT_NULL(MEMORY_FACT_CATEGORIES[8]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_supersede_exclusive);
   RUN_TEST(test_supersede_idempotent_same_object);
   RUN_TEST(test_non_exclusive_does_not_close);
   RUN_TEST(test_list_by_subject_at);
   RUN_TEST(test_supersede_closes_at_new_valid_from);
   RUN_TEST(test_supersede_skips_close_for_historical_insert);
   RUN_TEST(test_canonical_categories);
   return UNITY_END();
}
