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
 * Unit tests for memory embeddings math utilities and hybrid search logic.
 * Tests pure math functions without requiring ONNX model or database.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "memory/memory_embeddings.h"
#include "unity.h"

#define FLOAT_EQ(a, b) (fabsf((a) - (b)) < 1e-5f)

void setUp(void) {
}
void tearDown(void) {
}

/* ============================================================================
 * Test: L2 Norm
 * ============================================================================ */

static void test_l2_norm(void) {
   /* Unit vector */
   float unit[] = { 1.0f, 0.0f, 0.0f };
   float norm = memory_embeddings_l2_norm(unit, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, norm);

   /* Simple 3-4-5 triangle */
   float vec345[] = { 3.0f, 4.0f };
   norm = memory_embeddings_l2_norm(vec345, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, norm);

   /* Zero vector */
   float zero[] = { 0.0f, 0.0f, 0.0f };
   norm = memory_embeddings_l2_norm(zero, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, norm);

   /* NULL input */
   norm = memory_embeddings_l2_norm(NULL, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, norm);

   /* Zero dims */
   float any[] = { 1.0f };
   norm = memory_embeddings_l2_norm(any, 0);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, norm);
}

/* ============================================================================
 * Test: Cosine Similarity
 * ============================================================================ */

static void test_cosine_similarity(void) {
   /* Identical vectors */
   float a[] = { 1.0f, 2.0f, 3.0f };
   float b[] = { 1.0f, 2.0f, 3.0f };
   float sim = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sim);

   /* Orthogonal vectors */
   float x[] = { 1.0f, 0.0f };
   float y[] = { 0.0f, 1.0f };
   sim = memory_embeddings_cosine(x, y, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Opposite vectors — clamped to 0.0 */
   float pos[] = { 1.0f, 0.0f };
   float neg[] = { -1.0f, 0.0f };
   sim = memory_embeddings_cosine(pos, neg, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Zero vector */
   float zero[] = { 0.0f, 0.0f };
   sim = memory_embeddings_cosine(a, zero, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);
}

/* ============================================================================
 * Test: Cosine with Precomputed Norms
 * ============================================================================ */

static void test_cosine_with_norms(void) {
   float a[] = { 1.0f, 2.0f, 3.0f };
   float b[] = { 4.0f, 5.0f, 6.0f };
   float norm_a = memory_embeddings_l2_norm(a, 3);
   float norm_b = memory_embeddings_l2_norm(b, 3);

   float sim_precomputed = memory_embeddings_cosine_with_norms(a, b, 3, norm_a, norm_b);
   float sim_computed = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, sim_computed, sim_precomputed);

   /* Zero norm */
   float sim_zero = memory_embeddings_cosine_with_norms(a, b, 3, 0.0f, norm_b);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim_zero);
}

/* ============================================================================
 * Test: Cosine Clamping (negative to 0)
 * ============================================================================ */

static void test_cosine_clamping(void) {
   /* Anti-correlated vectors should clamp to 0 */
   float a[] = { 1.0f, 1.0f, 1.0f };
   float b[] = { -1.0f, -1.0f, -1.0f };
   float sim = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT_TRUE_MESSAGE(sim >= 0.0f, "negative cosine clamped to >= 0");
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Partially opposite */
   float c[] = { 1.0f, 0.0f };
   float d[] = { -0.5f, 0.1f };
   sim = memory_embeddings_cosine(c, d, 2);
   TEST_ASSERT_TRUE_MESSAGE(sim >= 0.0f, "partial opposite clamped to >= 0");
}

/* ============================================================================
 * Test: Hybrid Score Merge Logic
 * ============================================================================ */

static void test_hybrid_score_merge(void) {
   float kw_weight = 0.3f;
   float vec_weight = 0.7f;

   /* Fact with 2/3 keyword match tokens and 0.8 cosine */
   float kw_score = 2.0f / 3.0f;
   float cosine = 0.8f;
   float hybrid = kw_weight * kw_score + vec_weight * cosine;
   float expected = 0.3f * (2.0f / 3.0f) + 0.7f * 0.8f;
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, expected, hybrid);

   /* Un-embedded fact (keyword only, no vector penalty) */
   float kw_only = kw_weight * 1.0f;
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.3f, kw_only);

   /* Vector-only result (no keyword match) */
   float vec_only = vec_weight * 0.9f;
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.63f, vec_only);
}

/* ============================================================================
 * Test: Dimension Mismatch Skip
 * ============================================================================ */

static void test_dimension_validation(void) {
   /* Verify L2 norm handles different dimensions correctly */
   float vec4[] = { 1.0f, 1.0f, 1.0f, 1.0f };
   float norm4 = memory_embeddings_l2_norm(vec4, 4);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, norm4);

   float norm2 = memory_embeddings_l2_norm(vec4, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, sqrtf(2.0f), norm2);

   /* Cosine with different dim interpretations */
   float a[] = { 1.0f, 0.0f, 0.0f, 0.0f };
   float b[] = { 0.0f, 1.0f, 0.0f, 0.0f };
   float sim4 = memory_embeddings_cosine(a, b, 4);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim4);

   float sim2 = memory_embeddings_cosine(a, b, 2);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim2);
}

/* ============================================================================
 * Test: Edge Cases
 * ============================================================================ */

static void test_edge_cases(void) {
   /* Single dimension */
   float a1[] = { 5.0f };
   float b1[] = { 3.0f };
   float sim = memory_embeddings_cosine(a1, b1, 1);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sim);

   float c1[] = { -3.0f };
   sim = memory_embeddings_cosine(a1, c1, 1);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sim);

   /* Large dimension */
   float large_a[384];
   float large_b[384];
   for (int i = 0; i < 384; i++) {
      large_a[i] = (float)(i % 10) / 10.0f;
      large_b[i] = (float)(i % 10) / 10.0f;
   }
   sim = memory_embeddings_cosine(large_a, large_b, 384);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sim);

   /* Slightly different */
   large_b[0] += 0.01f;
   sim = memory_embeddings_cosine(large_a, large_b, 384);
   TEST_ASSERT_TRUE_MESSAGE(sim > 0.99f && sim <= 1.0f, "nearly identical 384D > 0.99");
}

/* ============================================================================
 * Test: memory_embeddings_rescore_against_query (Phase 2 Step 1 contract)
 * ============================================================================
 *
 * These tests cover the input-validation + sentinel-contract surface of the
 * rescore primitive.  The cache-hit scoring path requires a populated
 * s_cache (which depends on the embedding engine and auth_db) and is
 * exercised via the LoCoMo bench rather than these unit tests.  What we CAN
 * unit-test is the contract that prevents the prior -9pp regression:
 *
 *   1. NULL out_scores or fact_ids → FAILURE.
 *   2. fact_count == 0 → SUCCESS, no scores touched.
 *   3. NULL query_emb → every score gets the sentinel (caller drops).
 *   4. query_norm too small (< 1e-6f) → every score gets the sentinel.
 *   5. Sentinel is < 0.0f and distinct from any legitimate score
 *      (vec_weight in [0,1] × cosine in [0,1] + entity_bonus in [0,1] is
 *      always >= 0.0).
 *
 * The audit caught two bugs the contract design now prevents:
 *  - asymmetric entity_bonus on cache-miss facts (now: sentinel, not bonus)
 *  - silent fallback to bonus on NULL/missing embedding (now: explicit sentinel)
 */

static void test_rescore_null_out_scores_returns_failure(void) {
   int64_t fact_ids[] = { 1, 2, 3 };
   int rc = memory_embeddings_rescore_against_query(/*user_id*/ 1, /*query_emb*/ NULL,
                                                    /*query_norm*/ 0.0f, /*entity_bonus*/ 0.0f,
                                                    fact_ids, 3, /*out_scores*/ NULL);
   TEST_ASSERT_EQUAL_INT(1 /* FAILURE */, rc);
}

static void test_rescore_null_fact_ids_returns_failure(void) {
   float scores[3];
   int rc = memory_embeddings_rescore_against_query(1, NULL, 0.0f, 0.0f, /*fact_ids*/ NULL, 3,
                                                    scores);
   TEST_ASSERT_EQUAL_INT(1 /* FAILURE */, rc);
}

static void test_rescore_empty_count_is_noop_success(void) {
   float scores[1] = { 12.345f }; /* canary — must remain untouched */
   int64_t fact_ids[1] = { 99 };
   int rc = memory_embeddings_rescore_against_query(1, NULL, 0.0f, 0.0f, fact_ids, /*fact_count*/ 0,
                                                    scores);
   TEST_ASSERT_EQUAL_INT(0 /* SUCCESS */, rc);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, 12.345f, scores[0]);
}

static void test_rescore_null_query_emb_yields_all_sentinels(void) {
   float scores[3] = { 99.0f, 99.0f, 99.0f };
   int64_t fact_ids[] = { 10, 20, 30 };
   int rc = memory_embeddings_rescore_against_query(1, /*query_emb*/ NULL, /*query_norm*/ 1.0f,
                                                    /*entity_bonus*/ 0.1f, fact_ids, 3, scores);
   TEST_ASSERT_EQUAL_INT(0 /* SUCCESS */, rc);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, MEMORY_EMBEDDINGS_RESCORE_SENTINEL, scores[0]);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, MEMORY_EMBEDDINGS_RESCORE_SENTINEL, scores[1]);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, MEMORY_EMBEDDINGS_RESCORE_SENTINEL, scores[2]);
}

static void test_rescore_zero_query_norm_yields_all_sentinels(void) {
   float qemb[8] = { 0 }; /* zero vector */
   float scores[2] = { 99.0f, 99.0f };
   int64_t fact_ids[] = { 1, 2 };
   int rc = memory_embeddings_rescore_against_query(1, qemb, /*query_norm*/ 0.0f, 0.1f, fact_ids, 2,
                                                    scores);
   TEST_ASSERT_EQUAL_INT(0 /* SUCCESS */, rc);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, MEMORY_EMBEDDINGS_RESCORE_SENTINEL, scores[0]);
   TEST_ASSERT_FLOAT_WITHIN(1e-5f, MEMORY_EMBEDDINGS_RESCORE_SENTINEL, scores[1]);
}

static void test_rescore_sentinel_value_invariant(void) {
   /* Sentinel MUST be strictly negative so callers can use the
    * `score > MEMORY_EMBEDDINGS_RESCORE_SENTINEL` pattern to filter.
    * Legitimate scores fall in [0, vec_weight + entity_bonus] which is
    * non-negative for any in-range config. */
   TEST_ASSERT_TRUE(MEMORY_EMBEDDINGS_RESCORE_SENTINEL < 0.0f);
}

/* ============================================================================
 * Test: Duplicate-fact clustering (pure cluster_by_cosine)
 * ============================================================================ */

static void test_cluster_by_cosine(void) {
   /* 2D unit vectors so the supplied norm (1.0) is exact.  Two tight pairs
    * (~5deg apart, cos 0.9962) plus a zero-vector that must be skipped. */
   const int dims = 2;
   const int count = 5;
   int64_t ids[5] = { 100, 101, 102, 103, 104 };
   float embs[5 * 2] = {
      1.0f,      0.0f,      /* fact0 */
      0.996195f, 0.087156f, /* fact1: ~5deg from fact0 */
      0.0f,      1.0f,      /* fact2 */
      0.087156f, 0.996195f, /* fact3: ~5deg from fact2 */
      0.0f,      0.0f,      /* fact4: zero vector → norm 0 → skipped */
   };
   float norms[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };

   memory_dup_cluster_t clusters[MEMORY_DUP_MAX_CLUSTERS];
   int n = -1;

   /* threshold 0.85: {100,101} and {102,103} cluster; fact4 skipped. */
   int rc = memory_embeddings_cluster_by_cosine(ids, embs, norms, count, dims, 0.85f, clusters,
                                                MEMORY_DUP_MAX_CLUSTERS, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT(2, clusters[0].count);
   TEST_ASSERT_EQUAL_INT(2, clusters[1].count);
   TEST_ASSERT_EQUAL_INT64(100, clusters[0].ids[0]);
   TEST_ASSERT_EQUAL_INT64(101, clusters[0].ids[1]);
   TEST_ASSERT_EQUAL_INT64(102, clusters[1].ids[0]);
   TEST_ASSERT_EQUAL_INT64(103, clusters[1].ids[1]);
   /* min_similarity = the single seed-to-member cosine (~cos 5deg = 0.9962). */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9962f, clusters[0].min_similarity);

   /* threshold 0.999: the 0.9962 pairs fall below → no clusters. */
   n = -1;
   rc = memory_embeddings_cluster_by_cosine(ids, embs, norms, count, dims, 0.999f, clusters,
                                            MEMORY_DUP_MAX_CLUSTERS, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, n);

   /* max_clusters=1 → early-stop after the first cluster. */
   n = -1;
   rc = memory_embeddings_cluster_by_cosine(ids, embs, norms, count, dims, 0.85f, clusters, 1, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, n);

   /* NULL guard → FAILURE, count zeroed. */
   n = -1;
   rc = memory_embeddings_cluster_by_cosine(NULL, embs, norms, count, dims, 0.85f, clusters,
                                            MEMORY_DUP_MAX_CLUSTERS, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_band_neighbors_scan(void) {
   /* 2D unit vectors against query (1,0): cosine == the x-component exactly.
    * Arranged weakest-in-band first so the descending-score insertion sort has
    * to bubble later/stronger matches to the front. */
   const int dims = 2;
   const int count = 6;
   int64_t ids[6] = { 10, 11, 12, 13, 14, 15 };
   float embs[6 * 2] = {
      1.0f,  0.0f,     /* id10: cos 1.00 → >= high, excluded */
      0.82f, 0.57237f, /* id11: cos 0.82 → in band */
      0.85f, 0.52678f, /* id12: cos 0.85 → in band */
      0.90f, 0.43589f, /* id13: cos 0.90 → in band */
      0.70f, 0.71414f, /* id14: cos 0.70 → < low, excluded */
      0.0f,  0.0f,     /* id15: zero vector → norm 0 → skipped */
   };
   float norms[6] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
   float query[2] = { 1.0f, 0.0f };

   int64_t out_ids[MEMORY_BAND_NEIGHBORS_MAX];
   float out_scores[MEMORY_BAND_NEIGHBORS_MAX];
   int n = -1;

   /* band [0.80, 0.92): id13(0.90), id12(0.85), id11(0.82) — descending. */
   int rc = memory_embeddings_band_neighbors_scan(ids, embs, norms, count, dims, query, 1.0f, 0.80f,
                                                  0.92f, out_ids, out_scores,
                                                  MEMORY_BAND_NEIGHBORS_MAX, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_INT64(13, out_ids[0]);
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.90f, out_scores[0]);
   TEST_ASSERT_EQUAL_INT64(12, out_ids[1]);
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.85f, out_scores[1]);
   TEST_ASSERT_EQUAL_INT64(11, out_ids[2]);
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.82f, out_scores[2]);

   /* max=2 → top-2 by score; the weakest in-band (id11) is dropped. */
   n = -1;
   rc = memory_embeddings_band_neighbors_scan(ids, embs, norms, count, dims, query, 1.0f, 0.80f,
                                              0.92f, out_ids, out_scores, 2, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(13, out_ids[0]);
   TEST_ASSERT_EQUAL_INT64(12, out_ids[1]);

   /* Narrow band [0.84, 0.86): only id12(0.85). */
   n = -1;
   rc = memory_embeddings_band_neighbors_scan(ids, embs, norms, count, dims, query, 1.0f, 0.84f,
                                              0.86f, out_ids, out_scores, MEMORY_BAND_NEIGHBORS_MAX,
                                              &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(12, out_ids[0]);

   /* Zero query norm → no neighbors, but SUCCESS (not an error). */
   n = -1;
   rc = memory_embeddings_band_neighbors_scan(ids, embs, norms, count, dims, query, 0.0f, 0.80f,
                                              0.92f, out_ids, out_scores, MEMORY_BAND_NEIGHBORS_MAX,
                                              &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, n);

   /* Inverted band (low >= high): no element satisfies [low, high) → 0, SUCCESS. */
   n = -1;
   rc = memory_embeddings_band_neighbors_scan(ids, embs, norms, count, dims, query, 1.0f, 0.92f,
                                              0.80f, out_ids, out_scores, MEMORY_BAND_NEIGHBORS_MAX,
                                              &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, n);

   /* Empty cache (count == 0): valid args, nothing to scan → 0, SUCCESS. */
   n = -1;
   rc = memory_embeddings_band_neighbors_scan(ids, embs, norms, 0, dims, query, 1.0f, 0.80f, 0.92f,
                                              out_ids, out_scores, MEMORY_BAND_NEIGHBORS_MAX, &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, n);

   /* NULL guard → FAILURE, count zeroed. */
   n = -1;
   rc = memory_embeddings_band_neighbors_scan(NULL, embs, norms, count, dims, query, 1.0f, 0.80f,
                                              0.92f, out_ids, out_scores, MEMORY_BAND_NEIGHBORS_MAX,
                                              &n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* ============================================================================
 * Main
 * ============================================================================ */

/* ---- fact cache sizing ---- */

extern int g_stub_fact_total;
extern int g_stub_load_limit;
float stub_fact_angle(int64_t id);

/* Whether the cache holds fact @p id: the nearest fact to its exact vector. */
static bool cache_holds(int64_t id) {
   const float angle = stub_fact_angle(id);
   const float q[2] = { cosf(angle), sinf(angle) };
   int64_t match = 0;
   float score = 0.0f;
   memory_embeddings_nearest_fact(1, q, 2, 0.99999f, &match, &score);
   return match == id;
}

static void start_embedding_engine(void) {
   snprintf(g_config.memory.embedding_provider, sizeof(g_config.memory.embedding_provider), "onnx");
   TEST_ASSERT_EQUAL_INT(0, embedding_engine_init());
}

static void test_cache_holds_every_fact_past_the_old_cap(void) {
   start_embedding_engine();
   g_stub_fact_total = 9000; /* more than the 8,192 the cache used to stop at */
   memory_embeddings_invalidate_cache();
   TEST_ASSERT_TRUE(cache_holds(1));
   TEST_ASSERT_TRUE(cache_holds(9000));
}

static void test_cache_grows_as_facts_are_added(void) {
   start_embedding_engine();
   g_stub_fact_total = 10;
   memory_embeddings_invalidate_cache();
   TEST_ASSERT_TRUE(cache_holds(10));
   /* Appended past the room the load left (10 + 64). */
   for (int64_t id = 11; id <= 400; id++) {
      const float angle = stub_fact_angle(id);
      const float v[2] = { cosf(angle), sinf(angle) };
      TEST_ASSERT_EQUAL_INT(0, memory_embeddings_store_precomputed(1, id, v, 2));
   }
   /* Still the load's 10 in the stub store: only a cache that grew holds 400. */
   TEST_ASSERT_TRUE(cache_holds(400));
   TEST_ASSERT_TRUE(cache_holds(10));
}

static void test_cache_load_stops_at_the_ceiling(void) {
   /* [memory] fact_cache_mb bounds how many facts a load keeps, at the
    * embedding's size: 1 MB / 36 bytes (2 dims + 28) = 29,127. */
   start_embedding_engine();
   g_config.memory.fact_cache_mb = 1;
   g_stub_fact_total = 30000;
   memory_embeddings_invalidate_cache();
   TEST_ASSERT_TRUE(cache_holds(1));
   TEST_ASSERT_FALSE(cache_holds(30000)); /* past the ceiling */
   TEST_ASSERT_EQUAL_INT(29127, g_stub_load_limit);
   g_config.memory.fact_cache_mb = 0;
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_l2_norm);
   RUN_TEST(test_cosine_similarity);
   RUN_TEST(test_cosine_with_norms);
   RUN_TEST(test_cosine_clamping);
   RUN_TEST(test_hybrid_score_merge);
   RUN_TEST(test_dimension_validation);
   RUN_TEST(test_edge_cases);
   RUN_TEST(test_rescore_null_out_scores_returns_failure);
   RUN_TEST(test_rescore_null_fact_ids_returns_failure);
   RUN_TEST(test_rescore_empty_count_is_noop_success);
   RUN_TEST(test_rescore_null_query_emb_yields_all_sentinels);
   RUN_TEST(test_rescore_zero_query_norm_yields_all_sentinels);
   RUN_TEST(test_rescore_sentinel_value_invariant);
   RUN_TEST(test_cluster_by_cosine);
   RUN_TEST(test_band_neighbors_scan);
   RUN_TEST(test_cache_holds_every_fact_past_the_old_cap);
   RUN_TEST(test_cache_grows_as_facts_are_added);
   RUN_TEST(test_cache_load_stops_at_the_ceiling);
   return UNITY_END();
}
