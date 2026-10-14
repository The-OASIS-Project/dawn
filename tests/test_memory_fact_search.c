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
 * Unit tests for src/memory/memory_fact_search.c — the extracted
 * pipeline shared by the legacy memoryCallback recall path and the
 * Phase 1c focus-source fact adapter.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dawn_error.h"
#include "memory/memory_fact_search.h"
#include "memory/memory_types.h"
#include "test_memory_focus_adapters_mocks.h"
#include "unity.h"

void setUp(void) {
   mock_reset();
}

void tearDown(void) {
}

static void seed_fact(int idx, int64_t id, int user_id, const char *text, float confidence) {
   memory_fact_t *f = &s_mock.facts[idx];
   memset(f, 0, sizeof(*f));
   f->id = id;
   f->user_id = user_id;
   strncpy(f->fact_text, text, sizeof(f->fact_text) - 1);
   f->confidence = confidence;
   f->created_at = 1700000000 + idx;
   f->access_count = 0;
}

/* -----------------------------------------------------------------------
 * Helper-pipeline tests (the spec calls for ≥7).
 * --------------------------------------------------------------------- */

static void test_single_token_keyword_path(void) {
   seed_fact(0, 100, 1, "alpha bravo", 0.8f);
   seed_fact(1, 101, 1, "charlie delta", 0.6f);
   s_mock.fact_count = 2;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(/*user_id*/ 1, /*query*/ "alpha", /*query_embedding*/ NULL,
                                      /*embed_dim*/ 0, /*since_ts*/ 0, out, scores, 10, &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, n); /* stub returns all user-1 facts on any keyword */
   TEST_ASSERT_EQUAL_INT64(100, out[0].id);
}

static void test_keyword_only_scores(void) {
   seed_fact(0, 100, 1, "alpha bravo", 0.8f);
   seed_fact(1, 101, 1, "charlie delta", 0.6f);
   s_mock.fact_count = 2;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(1, "alpha bravo charlie", NULL, 0, 0, out, scores, 10, &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   /* Without embeddings the keyword ranking is the output, its BM25 score
    * carried as hundredths (the stub scores every hit 1.0). */
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_FLOAT(100.0f, scores[0]);
}

static void test_hybrid_path_reorders(void) {
   seed_fact(0, 100, 1, "first", 0.9f);
   seed_fact(1, 101, 1, "second", 0.8f);
   seed_fact(2, 102, 1, "third", 0.7f);
   s_mock.fact_count = 3;

   const float dummy_embed[1] = { 0.0f };
   s_mock.embeddings_available = true;
   s_mock.hybrid_enabled = true;
   /* Hybrid re-ranks: 102 first, then 100, then 101 — opposite order
    * from the keyword path. */
   s_mock.hybrid_results[0] = (mock_hybrid_result_t){ 102, 0.95f, 0 };
   s_mock.hybrid_results[1] = (mock_hybrid_result_t){ 100, 0.80f, 0 };
   s_mock.hybrid_results[2] = (mock_hybrid_result_t){ 101, 0.65f, 0 };
   s_mock.hybrid_result_count = 3;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(1, "first", dummy_embed, 1, 0, out, scores, 10, &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_INT64(102, out[0].id);
   TEST_ASSERT_EQUAL_INT64(100, out[1].id);
   TEST_ASSERT_EQUAL_INT64(101, out[2].id);
   TEST_ASSERT_EQUAL_FLOAT(0.95f, scores[0]);
}

static void test_since_ts_filter(void) {
   seed_fact(0, 100, 1, "old", 0.9f);
   s_mock.facts[0].created_at = 100;
   seed_fact(1, 101, 1, "new", 0.9f);
   s_mock.facts[1].created_at = 9999;
   s_mock.fact_count = 2;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(1, "anything", NULL, 0, /*since_ts*/ 5000, out, scores, 10,
                                      &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(101, out[0].id);
}

static void test_user_id_scope(void) {
   seed_fact(0, 100, 1, "user1 fact", 0.9f);
   seed_fact(1, 101, 2, "user2 fact", 0.9f);
   s_mock.fact_count = 2;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(/*user_id*/ 1, "anything", NULL, 0, 0, out, scores, 10, &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(100, out[0].id);
}

static void test_null_query_embedding_keyword_only(void) {
   seed_fact(0, 100, 1, "alpha", 0.9f);
   s_mock.fact_count = 1;
   s_mock.embeddings_available = true;
   /* hybrid_enabled but query_embedding NULL → must NOT enter hybrid path. */
   s_mock.hybrid_enabled = true;
   s_mock.hybrid_results[0] = (mock_hybrid_result_t){ 999, 1.0f, 0 };
   s_mock.hybrid_result_count = 1;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(1, "alpha", /*query_embedding*/ NULL, 0, 0, out, scores, 10,
                                      &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   /* Keyword-only — original 100, NOT the hybrid 999. */
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(100, out[0].id);
}

/* The defense-in-depth fold-in: vector-only hit owned by a different
 * user MUST be rejected with an OLOG_ERROR. */
static void test_vector_only_cross_user_rejected(void) {
   seed_fact(0, 100, 1, "user1 fact", 0.9f);
   seed_fact(1, 999, 2, "user2 fact (foreign!)", 0.9f); /* user_id=2 — different */
   s_mock.fact_count = 2;

   const float dummy_embed[1] = { 0.0f };
   s_mock.embeddings_available = true;
   s_mock.hybrid_enabled = true;
   /* hybrid emits 100 (keyword-set hit) AND 999 (vector-only hit owned
    * by user 2) — the vector-only path must drop 999. */
   s_mock.hybrid_results[0] = (mock_hybrid_result_t){ 100, 0.8f, 0 };
   s_mock.hybrid_results[1] = (mock_hybrid_result_t){ 999, 0.9f, 0 };
   s_mock.hybrid_result_count = 2;

   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(/*user_id*/ 1, "anything", dummy_embed, 1, 0, out, scores, 10,
                                      &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(100, out[0].id);
}

static void test_empty_query_returns_zero(void) {
   seed_fact(0, 100, 1, "alpha", 0.9f);
   s_mock.fact_count = 1;
   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(1, "", NULL, 0, 0, out, scores, 10, &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_null_query_returns_zero(void) {
   memory_fact_t out[10];
   float scores[10];
   int n = -1;
   int rc = memory_fact_search_hybrid(1, NULL, NULL, 0, 0, out, scores, 10, &n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, n);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_single_token_keyword_path);
   RUN_TEST(test_keyword_only_scores);
   RUN_TEST(test_hybrid_path_reorders);
   RUN_TEST(test_since_ts_filter);
   RUN_TEST(test_user_id_scope);
   RUN_TEST(test_null_query_embedding_keyword_only);
   RUN_TEST(test_vector_only_cross_user_rejected);
   RUN_TEST(test_empty_query_returns_zero);
   RUN_TEST(test_null_query_returns_zero);
   return UNITY_END();
}
