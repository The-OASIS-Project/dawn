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
 * Unit tests for contacts_db.c — contact CRUD against memory entities.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "memory/contacts_db.h"
#include "unity.h"

/* ============================================================================
 * DDL — create memory_entities + contacts tables and prepare statements
 * ============================================================================ */

static const char *DDL =
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
    "  canonical_id INTEGER DEFAULT NULL,"
    "  UNIQUE(user_id, canonical_name)"
    ");"
    "CREATE TABLE IF NOT EXISTS contacts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  entity_id INTEGER NOT NULL,"
    "  field_type TEXT NOT NULL,"
    "  value TEXT NOT NULL,"
    "  label TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_contacts_entity ON contacts(entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_contacts_user_type ON contacts(user_id, field_type);";

/* Prepared statement SQL — must match auth_db_statements.c exactly (find
 * shares its macro). */

static const char *SQL_FIND = CONTACTS_FIND_SQL;

static const char *SQL_ADD =
    "INSERT INTO contacts (user_id, entity_id, field_type, value, label, created_at) "
    "SELECT ?, ?, ?, ?, ?, ? WHERE EXISTS "
    "(SELECT 1 FROM memory_entities WHERE id = ? AND user_id = ?)";

static const char *SQL_DELETE = "DELETE FROM contacts WHERE id = ? AND user_id = ?";

static const char *SQL_LIST =
    "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
    "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
    "WHERE c.user_id = ? AND (? IS NULL OR c.field_type = ?) "
    "ORDER BY e.name LIMIT ? OFFSET ?";

static const char *SQL_UPDATE =
    "UPDATE contacts SET field_type = ?, value = ?, label = ? WHERE id = ? AND user_id = ?";

static const char *SQL_COUNT = "SELECT COUNT(*) FROM contacts WHERE user_id = ?";

/* ============================================================================
 * Helpers
 * ============================================================================ */

/**
 * @brief Prepare all six contacts statements on s_db.
 */
static void prepare_stmts(void) {
   int rc;
   rc = sqlite3_prepare_v2(s_db.db, SQL_FIND, -1, &s_db.stmt_contacts_find, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare find failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   rc = sqlite3_prepare_v2(s_db.db, SQL_ADD, -1, &s_db.stmt_contacts_add, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare add failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   rc = sqlite3_prepare_v2(s_db.db, SQL_DELETE, -1, &s_db.stmt_contacts_delete, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare delete failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   rc = sqlite3_prepare_v2(s_db.db, SQL_LIST, -1, &s_db.stmt_contacts_list, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare list failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   rc = sqlite3_prepare_v2(s_db.db, SQL_UPDATE, -1, &s_db.stmt_contacts_update, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare update failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   rc = sqlite3_prepare_v2(s_db.db, SQL_COUNT, -1, &s_db.stmt_contacts_count, NULL);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "prepare count failed: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
}

static void finalize_stmts(void) {
   if (s_db.stmt_contacts_find)
      sqlite3_finalize(s_db.stmt_contacts_find);
   if (s_db.stmt_contacts_add)
      sqlite3_finalize(s_db.stmt_contacts_add);
   if (s_db.stmt_contacts_delete)
      sqlite3_finalize(s_db.stmt_contacts_delete);
   if (s_db.stmt_contacts_list)
      sqlite3_finalize(s_db.stmt_contacts_list);
   if (s_db.stmt_contacts_update)
      sqlite3_finalize(s_db.stmt_contacts_update);
   if (s_db.stmt_contacts_count)
      sqlite3_finalize(s_db.stmt_contacts_count);
}

/**
 * @brief Insert a memory entity directly via SQL (test helper).
 * @return The rowid of the inserted entity, or 0 on failure.
 */
static int64_t insert_entity(int user_id, const char *name, const char *canonical) {
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name) "
       "VALUES (?, ?, 'person', ?)",
       -1, &st, NULL);
   if (rc != SQLITE_OK)
      return 0;

   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, canonical, -1, SQLITE_TRANSIENT);

   rc = sqlite3_step(st);
   int64_t id = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(s_db.db) : 0;
   sqlite3_finalize(st);
   return id;
}

/* ============================================================================
 * Setup / Teardown
 * ============================================================================ */

static void setup_db(void) {
   memset(&s_db, 0, sizeof(s_db));
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   pthread_mutex_init(&s_db.mutex, NULL);

   /* Enable foreign keys for CASCADE behavior */
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   prepare_stmts();
   s_db.initialized = true;
}

static void teardown_db(void) {
   finalize_stmts();
   if (s_db.db) {
      sqlite3_close(s_db.db);
   }
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
 * Tests
 * ============================================================================ */

static void test_add_contact(void) {
   int64_t eid = insert_entity(1, "Alice Smith", "alice smith");
   TEST_ASSERT_TRUE(eid > 0);

   int rc = contacts_add(1, eid, "email", "alice@example.com", "work");
   TEST_ASSERT_EQUAL_INT(0, rc);
}

static void test_find_by_name(void) {
   int64_t eid = insert_entity(1, "Bob Jones", "bob jones");
   contacts_add(1, eid, "phone", "+15551234567", "mobile");

   contact_result_t results[4];
   int count = 0;
   int rc = contacts_find(1, "bob", NULL, results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("+15551234567", results[0].value);
   TEST_ASSERT_EQUAL_STRING("phone", results[0].field_type);
   TEST_ASSERT_EQUAL_STRING("mobile", results[0].label);
}

static void test_find_by_field_type_filter(void) {
   int64_t eid = insert_entity(1, "Carol Lee", "carol lee");
   contacts_add(1, eid, "email", "carol@example.com", "personal");
   contacts_add(1, eid, "phone", "+15559876543", "work");

   contact_result_t results[4];
   int count = 0;

   /* Filter to email only */
   int rc = contacts_find(1, "carol", "email", results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("carol@example.com", results[0].value);

   /* Filter to phone only */
   count = 0;
   rc = contacts_find(1, "carol", "phone", results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("+15559876543", results[0].value);
}

static void test_list_all(void) {
   int64_t e1 = insert_entity(1, "Dan Brown", "dan brown");
   int64_t e2 = insert_entity(1, "Eve White", "eve white");
   contacts_add(1, e1, "email", "dan@example.com", "work");
   contacts_add(1, e2, "phone", "+15550001111", "home");

   contact_result_t results[10];
   int count = 0;
   int rc = contacts_list(1, NULL, results, 10, 0, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
}

static void test_list_with_type_filter(void) {
   int64_t e1 = insert_entity(1, "Fay Green", "fay green");
   int64_t e2 = insert_entity(1, "Gus Black", "gus black");
   contacts_add(1, e1, "phone", "+15550002222", "mobile");
   contacts_add(1, e2, "email", "gus@example.com", "work");
   contacts_add(1, e2, "phone", "+15550003333", "home");

   contact_result_t results[10];
   int count = 0;
   int rc = contacts_list(1, "phone", results, 10, 0, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* Both phone entries returned, email excluded */
   for (int i = 0; i < count; i++) {
      TEST_ASSERT_EQUAL_STRING("phone", results[i].field_type);
   }
}

static void test_count(void) {
   int64_t e1 = insert_entity(1, "Hal Grey", "hal grey");
   int64_t e2 = insert_entity(1, "Ivy Tan", "ivy tan");
   contacts_add(1, e1, "email", "hal@example.com", "work");
   contacts_add(1, e1, "phone", "+15551110000", "mobile");
   contacts_add(1, e2, "phone", "+15552220000", "home");

   int count = 0;
   int rc = contacts_count(1, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(3, count);
}

static void test_delete(void) {
   int64_t eid = insert_entity(1, "Jay Fox", "jay fox");
   contacts_add(1, eid, "email", "jay@example.com", "personal");

   /* Retrieve the contact_id via list */
   contact_result_t results[4];
   int count = 0;
   contacts_list(1, NULL, results, 4, 0, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   int64_t cid = results[0].contact_id;

   int rc = contacts_delete(1, cid);
   TEST_ASSERT_EQUAL_INT(0, rc);

   int new_count = 0;
   contacts_count(1, &new_count);
   TEST_ASSERT_EQUAL_INT(0, new_count);
}

static void test_update(void) {
   int64_t eid = insert_entity(1, "Kay Lim", "kay lim");
   contacts_add(1, eid, "email", "kay_old@example.com", "personal");

   /* Get the contact_id */
   contact_result_t results[4];
   int count = 0;
   contacts_list(1, NULL, results, 4, 0, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   int64_t cid = results[0].contact_id;

   /* Update value and label */
   int rc = contacts_update(1, cid, "email", "kay_new@example.com", "work");
   TEST_ASSERT_EQUAL_INT(0, rc);

   /* Verify the update via find */
   count = 0;
   rc = contacts_find(1, "kay", NULL, results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("kay_new@example.com", results[0].value);
   TEST_ASSERT_EQUAL_STRING("work", results[0].label);
}

static void test_user_isolation(void) {
   int64_t e1 = insert_entity(1, "Leo Park", "leo park");
   int64_t e2 = insert_entity(2, "Leo Park", "leo park");
   contacts_add(1, e1, "email", "leo_user1@example.com", "work");
   contacts_add(2, e2, "email", "leo_user2@example.com", "work");

   contact_result_t results[4];
   int count = 0;

   /* User 1 sees only their contact */
   int rc = contacts_find(1, "leo", NULL, results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("leo_user1@example.com", results[0].value);

   /* User 2 sees only their contact */
   count = 0;
   rc = contacts_find(2, "leo", NULL, results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("leo_user2@example.com", results[0].value);
}

static void test_find_no_results(void) {
   contact_result_t results[4];
   int count = -1;
   int rc = contacts_find(1, "nonexistent", NULL, results, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_delete_wrong_user(void) {
   int64_t eid = insert_entity(1, "Mia Ross", "mia ross");
   contacts_add(1, eid, "phone", "+15553334444", "mobile");

   /* Get the contact_id */
   contact_result_t results[4];
   int count = 0;
   contacts_list(1, NULL, results, 4, 0, &count);
   TEST_ASSERT_EQUAL_INT(1, count);
   int64_t cid = results[0].contact_id;

   /* Delete as user 2 — should fail (returns FAILURE, 0 rows changed) */
   int rc = contacts_delete(2, cid);
   TEST_ASSERT_EQUAL_INT(1, rc);

   /* Original contact still exists */
   int remaining = 0;
   contacts_count(1, &remaining);
   TEST_ASSERT_EQUAL_INT(1, remaining);
}

static void test_pagination(void) {
   /* Create 5 contacts across two entities */
   int64_t e1 = insert_entity(1, "Ava Bell", "ava bell");
   int64_t e2 = insert_entity(1, "Ben Cruz", "ben cruz");
   contacts_add(1, e1, "email", "ava1@example.com", "work");
   contacts_add(1, e1, "phone", "+15550010001", "mobile");
   contacts_add(1, e2, "email", "ben1@example.com", "work");
   contacts_add(1, e2, "phone", "+15550020001", "mobile");
   contacts_add(1, e2, "address", "123 Main St", "home");

   /* Page 1: first 2 results */
   contact_result_t results[10];
   int count = 0;
   int rc = contacts_list(1, NULL, results, 2, 0, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(2, count);

   /* Page 2: next 2 results with offset=2 */
   count = 0;
   rc = contacts_list(1, NULL, results, 2, 2, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(2, count);

   /* Page 3: remaining 1 result with offset=4 */
   count = 0;
   rc = contacts_list(1, NULL, results, 2, 4, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(1, count);

   /* Past the end: offset=5 returns 0 */
   count = 0;
   rc = contacts_list(1, NULL, results, 2, 5, &count);
   TEST_ASSERT_EQUAL_INT(0, rc);
   TEST_ASSERT_EQUAL_INT(0, count);
}

/* ============================================================================
 * Main
 * ============================================================================ */

/* Best match first, whatever the alphabet says: the exact name, then whole
 * words, then partial ones; an exact name is never cut by the limit.  An
 * alias matches as whole words at most.  The name is normalized first. */
static void test_find_ranks_matches(void) {
   int64_t a = insert_entity(1, "Chris Adams", "chris adams");
   int64_t b = insert_entity(1, "Christine Abbott", "christine abbott");
   int64_t c = insert_entity(1, "Chris", "chris");
   contacts_add(1, a, "phone", "+15550000001", "");
   contacts_add(1, b, "phone", "+15550000002", "");
   contacts_add(1, c, "phone", "+15550000003", "");

   contact_result_t results[1];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(0, contacts_find(1, "  CHRIS ", "phone", results, 1, &count));
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("+15550000003", results[0].value);
   TEST_ASSERT_EQUAL_INT(CONTACT_MATCH_EXACT, results[0].match);

   contact_result_t all[4];
   TEST_ASSERT_EQUAL_INT(0, contacts_find(1, "chris", "phone", all, 4, &count));
   TEST_ASSERT_EQUAL_INT(3, count);
   TEST_ASSERT_EQUAL_INT(CONTACT_MATCH_EXACT, all[0].match);
   TEST_ASSERT_EQUAL_INT(CONTACT_MATCH_WORD, all[1].match);
   TEST_ASSERT_EQUAL_STRING("Chris Adams", all[1].entity_name);
   TEST_ASSERT_EQUAL_INT(CONTACT_MATCH_PARTIAL, all[2].match);

   /* "mom", an alias of Linda: a whole-word match, never exact. */
   int64_t linda = insert_entity(1, "Linda Kay", "linda kay");
   contacts_add(1, linda, "phone", "+15550000004", "");
   char sql[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name, "
            "canonical_id) VALUES (1, 'Mom', 'person', 'mom', %lld)",
            (long long)linda);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(0, contacts_find(1, "mom", "phone", all, 4, &count));
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_STRING("Linda Kay", all[0].entity_name);
   TEST_ASSERT_EQUAL_INT(CONTACT_MATCH_WORD, all[0].match);
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_add_contact);
   RUN_TEST(test_find_by_name);
   RUN_TEST(test_find_ranks_matches);
   RUN_TEST(test_find_by_field_type_filter);
   RUN_TEST(test_list_all);
   RUN_TEST(test_list_with_type_filter);
   RUN_TEST(test_count);
   RUN_TEST(test_delete);
   RUN_TEST(test_update);
   RUN_TEST(test_user_isolation);
   RUN_TEST(test_find_no_results);
   RUN_TEST(test_delete_wrong_user);
   RUN_TEST(test_pagination);

   return UNITY_END();
}
