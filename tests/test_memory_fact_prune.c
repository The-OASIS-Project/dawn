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
 * Merged (superseded) facts are pruned a retention window after the merge,
 * never after their creation, against the real schema via auth_db_init: an
 * old fact merged today stays recoverable, and the v95 migration gives facts
 * merged before it a fresh window.
 */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#define AUTH_DB_INTERNAL_ALLOWED /* test reads and backdates rows via the shared s_db handle */
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_stem.h"
#include "memory/memory_types.h"
#include "unity.h"

/* Single g_config definition for the linked auth/memory chain. */
dawn_config_t g_config;

/* Embedding-engine stubs — the bridge calls embed_and_store; memory_db_facts
 * calls invalidate_cache.  No-ops keep the lifecycle tests DB-only. */
int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   (void)user_id;
   (void)fact_id;
   (void)text;
   return SUCCESS;
}
void memory_embeddings_invalidate_cache(void) {
}
void memory_embeddings_invalidate_all(void) {
}
void memory_embeddings_invalidate_entity_cache(void) {
}
void memory_embeddings_invalidate_entity_cache_for_user(int user_id) {
   (void)user_id;
}
void memory_embeddings_invalidate_cache_for_user(int user_id) {
   (void)user_id;
}
float memory_embeddings_l2_norm(const float *vec, int dims) {
   (void)vec;
   (void)dims;
   return 0.0f;
}
float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float na,
                                          float nb) {
   (void)a;
   (void)b;
   (void)dims;
   (void)na;
   (void)nb;
   return 0.0f;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.enabled = true;
   auth_db_init(":memory:");
   memory_stem_init();
   auth_db_create_user("tester", "hash", true); /* user id 1 */
}

void tearDown(void) {
   auth_db_shutdown();
}

#define DAY (24 * 60 * 60)

static int64_t fact_at(const char *text, int64_t created_at) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_fact_create_at(1, text, 0.9f, "explicit", "general", NULL,
                                                  created_at, &id));
   return id;
}

static int64_t column_of(int64_t id, const char *column) {
   char sql[128];
   snprintf(sql, sizeof(sql), "SELECT COALESCE(%s, 0) FROM memory_facts WHERE id = %lld", column,
            (long long)id);
   sqlite3_stmt *st = NULL;
   int64_t v = -1;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) == SQLITE_OK &&
       sqlite3_step(st) == SQLITE_ROW) {
      v = sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   return v; /* -1: no such row */
}

static void set_column(int64_t id, const char *column, const char *value) {
   char sql[160];
   snprintf(sql, sizeof(sql), "UPDATE memory_facts SET %s = %s WHERE id = %lld", column, value,
            (long long)id);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
}

/* An old fact merged today survives the prune; a window after the merge it
 * goes. */
static void test_prune_counts_from_the_merge(void) {
   const int64_t now = (int64_t)time(NULL);
   const int64_t keep = fact_at("Jon prefers direct feedback", now);
   const int64_t old = fact_at("Jon wants to be told when he is wrong", now - 100 * DAY);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_supersede(old, keep, 1));
   TEST_ASSERT_INT64_WITHIN(5, now, column_of(old, "superseded_at"));

   int pruned = -1;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_prune_superseded(1, 30, &pruned));
   TEST_ASSERT_EQUAL_INT(0, pruned);
   TEST_ASSERT_EQUAL_INT64(keep, column_of(old, "superseded_by"));

   char when[32];
   snprintf(when, sizeof(when), "%lld", (long long)(now - 31 * DAY));
   set_column(old, "superseded_at", when);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_prune_superseded(1, 30, &pruned));
   TEST_ASSERT_EQUAL_INT(1, pruned);
   TEST_ASSERT_EQUAL_INT64(-1, column_of(old, "superseded_by"));
   TEST_ASSERT_EQUAL_INT64(0, column_of(keep, "superseded_by")); /* the keeper stays */
}

/* A merged fact with no merge time is never pruned; the v95 migration stamps
 * it now, which starts its window. */
static void test_migration_gives_earlier_merges_a_window(void) {
   const int64_t now = (int64_t)time(NULL);
   const int64_t keep = fact_at("Jon lives in Georgia", now);
   const int64_t old = fact_at("Jon is based in Georgia", now - 200 * DAY);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_supersede(old, keep, 1));
   set_column(old, "superseded_at", "NULL"); /* merged before v95 */

   int pruned = -1;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_prune_superseded(1, 30, &pruned));
   TEST_ASSERT_EQUAL_INT(0, pruned);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v95(s_db.db));
   TEST_ASSERT_INT64_WITHIN(5, now, column_of(old, "superseded_at"));
   TEST_ASSERT_EQUAL_INT64(0, column_of(keep, "superseded_at")); /* never merged: untouched */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v95(s_db.db)); /* idempotent */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_prune_counts_from_the_merge);
   RUN_TEST(test_migration_gives_earlier_merges_a_window);
   return UNITY_END();
}
