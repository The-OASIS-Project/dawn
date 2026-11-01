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
 * The session-start memory block (memory_build_context) on a real schema: a
 * summary or preference of a batch that read outside text stays out of it
 * (the block sits in a conversation's frozen prefix); one stored before the
 * kind was kept (UNKNOWN) and a clean one go in.
 */

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "config/dawn_config.h"
#include "core/outside_kind.h"
#include "memory/memory_context.h"
#include "memory/memory_db.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_stem.h"
#include "unity.h"

/* Single g_config definition for the linked auth/memory chain. */
dawn_config_t g_config;

/* The summaries are stored without embeddings. */
int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   (void)user_id;
   (void)fact_id;
   (void)text;
   return 0;
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
   return 1.0f;
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
   return 0.9f;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.enabled = true;
   auth_db_init(":memory:");
   memory_stem_init();
   auth_db_create_user("jon", "hash", true); /* user id 1 */
}

void tearDown(void) {
   auth_db_shutdown();
}

static void store_summary(const char *text, int outside) {
   const memory_provenance_t prov = { .outside = outside };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_summary_create(1, "s", text, "", "neutral", 4, 60, &prov, NULL));
}

static void test_a_tainted_summary_is_left_out(void) {
   store_summary("Talked about the garden.", OUTSIDE_NONE);
   store_summary("Read an email from Dawn Smith.", OUTSIDE_EMAIL);
   store_summary("Planned a trip.", OUTSIDE_UNKNOWN);
   char *block = memory_build_context(1, NULL, 0);
   TEST_ASSERT_NOT_NULL(block);
   TEST_ASSERT_NOT_NULL(strstr(block, "garden"));
   TEST_ASSERT_NOT_NULL(strstr(block, "trip"));
   TEST_ASSERT_NULL(strstr(block, "Dawn Smith"));
   free(block);
}

/* Only tainted summaries: nothing to say. */
static void test_only_tainted_summaries_make_no_block(void) {
   store_summary("Read a web page.", OUTSIDE_WEB);
   TEST_ASSERT_NULL(memory_build_context(1, NULL, 0));
}

/* A value from a batch that read outside text is stored with that kind,
 * never over a value that came from the user, and stays out of the block. */
static void test_tainted_preferences(void) {
   const memory_provenance_t clean = { .outside = OUTSIDE_NONE };
   const memory_provenance_t web = { .outside = OUTSIDE_WEB };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_pref_upsert(1, "tea", "green", 0.8f, "explicit", &clean));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_pref_upsert(1, "tea", "black", 0.8f, "inferred", &web));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_pref_upsert(1, "airline", "Acme Air", 0.8f, "inferred", &web));
   memory_preference_t prefs[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_list(1, prefs, 4, 0, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   for (int i = 0; i < n; i++) {
      if (strcmp(prefs[i].category, "tea") == 0) {
         TEST_ASSERT_EQUAL_STRING("green", prefs[i].value);
         TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, prefs[i].outside);
      } else {
         TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, prefs[i].outside);
      }
   }
   char *block = memory_build_context(1, NULL, 0);
   TEST_ASSERT_NOT_NULL(block);
   TEST_ASSERT_NOT_NULL(strstr(block, "green"));
   TEST_ASSERT_NULL(strstr(block, "Acme Air"));
   free(block);
}

/* A cursor advance raises the conversation's floor to what the rows it
 * passes read (the first taint sticks); a continuation reads its parent's. */
static void test_floor_rises_with_the_cursor(void) {
   int64_t conv = 0, cont = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(1, "Chat", &conv));
   int floor = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_outside(conv, 1, &floor));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, floor);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_set_last_extracted(conv, 2, 5, OUTSIDE_OTHER));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_set_last_extracted(conv, 3, 6, OUTSIDE_WEB));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_set_last_extracted(conv, 4, 7, OUTSIDE_NONE));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_outside(conv, 1, &floor));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, floor);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create_continuation(1, conv, "summary", &cont));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_outside(cont, 1, &floor));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, floor);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_get_outside(cont, 2, &floor));
}

/* A summary found by meaning carries its kind like one listed by date. */
static void test_semantic_summary_keeps_outside(void) {
   const memory_provenance_t prov = { .outside = OUTSIDE_WEB };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_summary_create(1, "s", "Read a web page.", "",
                                                                     "neutral", 4, 60, &prov, &id));
   float v[4] = { 1, 0, 0, 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_summary_update_embedding(1, id, v, 4));
   memory_summary_t out[4];
   float sc[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_summary_search_semantic(1, v, 4, 0, 4, 64, out, sc, &n, NULL));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, out[0].outside);
}

/* A continuation keeps its parent's floor after the parent is deleted. */
static void test_deleted_parent_keeps_taint(void) {
   int64_t conv = 0, cont = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(1, "Chat", &conv));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_set_last_extracted(conv, 2, 5, OUTSIDE_WEB));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create_continuation(1, conv, "summary", &cont));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(conv, 1));
   int floor = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_outside(cont, 1, &floor));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_WEB, floor);
}

/* Forgetting a conversation moves its cursor past rows nothing checked; a
 * tool result among them raises the floor (its kind unknown, so OTHER). */
static void test_forget_past_a_tool_row_raises_the_floor(void) {
   int64_t conv = 0, row = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(1, "Chat", &conv));
   const conv_message_row_t user = { .role = "user", .content = "Look it up." };
   const conv_message_row_t tool = { .role = "tool", .content = "a result", .tool_call_id = "c1" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &user, &row));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &tool, &row));
   memory_conv_learned_t gone = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_conversations_forget(1, &conv, 1, &gone));
   int floor = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_outside(conv, 1, &floor));
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, floor);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_tainted_summary_is_left_out);
   RUN_TEST(test_only_tainted_summaries_make_no_block);
   RUN_TEST(test_tainted_preferences);
   RUN_TEST(test_floor_rises_with_the_cursor);
   RUN_TEST(test_semantic_summary_keeps_outside);
   RUN_TEST(test_deleted_parent_keeps_taint);
   RUN_TEST(test_forget_past_a_tool_row_raises_the_floor);
   return UNITY_END();
}
