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
 * Unit tests for src/memory/memory_focus_adapters.c — the four memory
 * adapters (fact, entity, relation, summary) wired into the
 * focus-source framework.  Cross-user isolation, ranker shape,
 * empty-result behavior, partial-failure cleanup, and round-robin
 * determinism for the relation adapter.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_source.h"
#include "core/focus/focus_source_internal.h"
#include "dawn_error.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_focus_adapters.h"
#include "test_memory_focus_adapters_mocks.h"
#include "unity.h"

/* The four adapters call into static query callbacks via their
 * adapter structs; we test them through the framework's public API. */

static void config_defaults_for_test(void) {
   /* Open ranker so the assertions on candidate counts aren't masked by
    * min_score / token-budget trimming.  Every adapter's source weight
    * is 1.0 so the ranker doesn't reorder by source. */
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.focus_injection.enabled = true;
   g_config.memory.focus_injection.focus_budget_bytes = 16384;
   g_config.memory.focus_injection.top_k = 32;
   g_config.memory.focus_injection.min_score = 0.0f;
   g_config.memory.focus_injection.weight_semantic = 1.0f;
   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
}

void setUp(void) {
   focus_unregister_all();
   mock_reset();
   /* The real entity cache is linked in: each test's mock entities are a new
    * pool, as an entity change would make them in production. */
   memory_embeddings_invalidate_entity_cache();
   config_defaults_for_test();
}

void tearDown(void) {
}

/* ----------------------------------------------------------------------- */
/* Fixture builders                                                        */
/* ----------------------------------------------------------------------- */

static void seed_fact(int idx, int64_t id, int user_id, const char *text, float confidence) {
   memory_fact_t *f = &s_mock.facts[idx];
   memset(f, 0, sizeof(*f));
   f->id = id;
   f->user_id = user_id;
   strncpy(f->fact_text, text, sizeof(f->fact_text) - 1);
   f->confidence = confidence;
   f->created_at = 1700000000 + idx;
}

static void seed_entity(int idx,
                        int64_t id,
                        int user_id,
                        const char *name,
                        const char *type,
                        bool keyword_match,
                        const float *embedding,
                        const char *photo_id) {
   memory_entity_t *e = &s_mock.entities[idx];
   memset(e, 0, sizeof(*e));
   e->id = id;
   e->user_id = user_id;
   strncpy(e->name, name, sizeof(e->name) - 1);
   strncpy(e->canonical_name, name, sizeof(e->canonical_name) - 1);
   strncpy(e->entity_type, type, sizeof(e->entity_type) - 1);
   e->mention_count = 1;
   e->first_seen = 1700000000;
   e->last_seen = 1700000100;
   s_mock.entity_keyword_match[idx] = keyword_match;
   s_mock.entity_embeddings[idx] = embedding;
   s_mock.entity_norms[idx] = 1.0f; /* unit-norm fixtures */
   s_mock.entity_photo_ids[idx] = photo_id;
}

static void seed_relation(int idx,
                          int64_t id,
                          int user_id,
                          int64_t subject_id,
                          const char *relation,
                          const char *object_name,
                          time_t valid_from,
                          time_t valid_to) {
   memory_relation_t *r = &s_mock.relations[idx];
   memset(r, 0, sizeof(*r));
   r->id = id;
   r->subject_entity_id = subject_id;
   strncpy(r->relation, relation, sizeof(r->relation) - 1);
   strncpy(r->object_name, object_name, sizeof(r->object_name) - 1);
   r->confidence = 0.9f;
   r->valid_from = valid_from;
   r->valid_to = valid_to;
   s_mock.relation_user_id[idx] = user_id;
}

static void seed_summary(int idx,
                         int64_t id,
                         int user_id,
                         const char *text,
                         time_t created_at,
                         bool consolidated) {
   memory_summary_t *s = &s_mock.summaries[idx];
   memset(s, 0, sizeof(*s));
   s->id = id;
   s->user_id = user_id;
   strncpy(s->summary, text, sizeof(s->summary) - 1);
   s->created_at = created_at;
   s->consolidated = consolidated;
}

/* Identity-pointed unit-norm embedding factory for entity tests.
 * Each fixture gets its own static float[4] = {1,0,0,0}/{0,1,0,0}... */
static const float embed_e1[MOCK_DIMS] = { 1.0f, 0.0f, 0.0f, 0.0f };
static const float embed_e2[MOCK_DIMS] = { 0.0f, 1.0f, 0.0f, 0.0f };
static const float embed_e3[MOCK_DIMS] = { 0.0f, 0.0f, 1.0f, 0.0f };
static const float embed_q_match_e1[MOCK_DIMS] = { 1.0f, 0.0f, 0.0f, 0.0f };
static const float embed_q_match_e2[MOCK_DIMS] = { 0.0f, 1.0f, 0.0f, 0.0f };

/* =====================================================================
 * Cross-user isolation — load-bearing security gates (one per adapter)
 * ===================================================================== */

static void test_fact_cross_user(void) {
   seed_fact(0, 100, /*user*/ 1, "user1 fact", 0.9f);
   seed_fact(1, 101, /*user*/ 2, "user2 fact", 0.9f);
   s_mock.fact_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());

   /* Query as user 2 — must NOT receive user 1's fact. */
   const float dummy[1] = { 0.0f };
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user_id*/ 2, false, "user1 fact", dummy, 1, 0, 5,
                                                &result));
   for (int i = 0; i < result.candidate_count; i++) {
      /* If any candidate came from facts, it MUST be the user-2 fact. */
      if (strcmp(result.candidates[i].source_id, "memory_fact") == 0) {
         TEST_ASSERT_EQUAL_STRING("user2 fact", result.candidates[i].text);
      }
   }
   focus_result_free(&result);
}

static void test_entity_cross_user(void) {
   seed_entity(0, 1, 1, "alpha_user1", "person", true, embed_e1, NULL);
   seed_entity(1, 2, 2, "alpha_user2", "person", true, embed_e2, NULL);
   s_mock.entity_count = 2;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user_id*/ 2, false, "alpha", embed_q_match_e2,
                                                MOCK_DIMS, 0, 5, &result));
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_entity") == 0) {
         TEST_ASSERT_TRUE(strstr(result.candidates[i].text, "alpha_user2") != NULL);
      }
   }
   focus_result_free(&result);
}

static void test_relation_cross_user(void) {
   /* Both users have an entity at id=1; relation user_id is parallel. */
   seed_entity(0, 1, 1, "user1_entity", "person", false, embed_e1, NULL);
   seed_entity(1, 2, 2, "user2_entity", "person", false, embed_e2, NULL);
   s_mock.entity_count = 2;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   seed_relation(0, 500, /*user*/ 1, /*subj*/ 1, "owns", "thing", 0, 0);
   seed_relation(1, 501, /*user*/ 2, /*subj*/ 2, "owns", "object", 0, 0);
   s_mock.relation_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user_id*/ 2, false, "anything", embed_q_match_e2,
                                                MOCK_DIMS, 0, 5, &result));
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_relation") == 0) {
         TEST_ASSERT_TRUE(strstr(result.candidates[i].text, "object") != NULL);
      }
   }
   focus_result_free(&result);
}

static void test_summary_cross_user(void) {
   seed_summary(0, 100, 1, "user1 summary", 1700000000, false);
   seed_summary(1, 101, 2, "user2 summary", 1700000000, false);
   s_mock.summary_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* `now` set far in the future so 30-day lookback admits both seeds. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user_id*/ 2, false, "summary", NULL, 0,
                                                1700100000, 5, &result));
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_summary") == 0) {
         TEST_ASSERT_EQUAL_STRING("user2 summary", result.candidates[i].text);
      }
   }
   focus_result_free(&result);
}

/* =====================================================================
 * Per-adapter happy paths
 * ===================================================================== */

static void test_fact_adapter_shape(void) {
   seed_fact(0, 100, 1, "Pepper's birthday is March 14", 0.85f);
   s_mock.facts[0].created_at = 1700000000;
   s_mock.fact_provenance[0] = (memory_provenance_t){ .conv_id = 42,
                                                      .msg_id_start = 304,
                                                      .msg_id_end = 304 };
   s_mock.fact_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   const float dummy[1] = { 0.0f };
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "Pepper", dummy, 1, 1700000200, 5, &result));
   /* Pull the memory_fact candidate from the merged result. */
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "memory_fact") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL(fc);
   TEST_ASSERT_EQUAL_STRING("Pepper's birthday is March 14", fc->text);
   TEST_ASSERT_EQUAL_STRING("fact:100", fc->item_id);
   TEST_ASSERT_TRUE(fc->importance_score >= 0.0f && fc->importance_score <= 1.0f);
   TEST_ASSERT_TRUE(fc->recency_score >= 0.0f && fc->recency_score <= 1.0f);
   TEST_ASSERT_EQUAL_INT64(42, fc->provenance.conv_id);
   TEST_ASSERT_EQUAL_INT64(304, fc->provenance.msg_id_start);
   focus_result_free(&result);
}

static void test_entity_adapter_photo_boost(void) {
   seed_entity(0, 1, 1, "Pepper", "person", false, embed_e1, "photo-abc"); /* has photo */
   seed_entity(1, 2, 1, "Rhodey", "person", false, embed_e2, NULL);
   s_mock.entity_count = 2;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "anything", embed_q_match_e1, MOCK_DIMS,
                                                1700000200, 5, &result));
   const focus_candidate_t *pepper = NULL, *rhodey = NULL;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_entity") != 0)
         continue;
      if (strstr(result.candidates[i].text, "Pepper"))
         pepper = &result.candidates[i];
      else if (strstr(result.candidates[i].text, "Rhodey"))
         rhodey = &result.candidates[i];
   }
   TEST_ASSERT_NOT_NULL(pepper);
   TEST_ASSERT_NOT_NULL(rhodey);
   /* Pepper has photo → importance boost; both share base.  Photo boost = 0.2. */
   TEST_ASSERT_TRUE(pepper->importance_score > rhodey->importance_score);
   TEST_ASSERT_TRUE(strstr(pepper->text, "(person)") != NULL);
   focus_result_free(&result);
}

static void test_entity_keyword_match_boosts_importance(void) {
   seed_entity(0, 1, 1, "Yamamoto", "person", /*keyword_match*/ true, embed_e1, NULL);
   seed_entity(1, 2, 1, "Watanabe", "person", /*keyword_match*/ false, embed_e2, NULL);
   s_mock.entity_count = 2;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "Yamamoto", embed_q_match_e1, MOCK_DIMS,
                                                1700000200, 5, &result));
   const focus_candidate_t *matched = NULL, *unmatched = NULL;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_entity") != 0)
         continue;
      if (strstr(result.candidates[i].text, "Yamamoto"))
         matched = &result.candidates[i];
      else if (strstr(result.candidates[i].text, "Watanabe"))
         unmatched = &result.candidates[i];
   }
   TEST_ASSERT_NOT_NULL(matched);
   TEST_ASSERT_NOT_NULL(unmatched);
   /* Yamamoto got the keyword-match boost (+0.1); Watanabe did not. */
   TEST_ASSERT_TRUE(matched->importance_score > unmatched->importance_score);
   focus_result_free(&result);
}

static void test_relation_filters_expired(void) {
   seed_entity(0, 1, 1, "Alice", "person", false, embed_e1, NULL);
   s_mock.entity_count = 1;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   /* Two relations on the same subject; one is expired, one is current. */
   seed_relation(0, 500, 1, /*subj*/ 1, "lived_in", "Atlanta", /*from*/ 100, /*to*/ 200);
   seed_relation(1, 501, 1, /*subj*/ 1, "lives_in", "Boston", /*from*/ 300, /*to*/ 0);
   s_mock.relation_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* now=400 → "lived_in" expired (valid_to=200 < 400), "lives_in" current. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "Alice", embed_q_match_e1, MOCK_DIMS,
                                                /*now*/ 400, 5, &result));
   bool saw_expired = false, saw_current = false;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_relation") != 0)
         continue;
      if (strstr(result.candidates[i].text, "Atlanta"))
         saw_expired = true;
      if (strstr(result.candidates[i].text, "Boston"))
         saw_current = true;
   }
   TEST_ASSERT_FALSE_MESSAGE(saw_expired, "expired relation must NOT surface");
   TEST_ASSERT_TRUE_MESSAGE(saw_current, "currently-valid relation must surface");
   focus_result_free(&result);
}

static void test_summary_lookback_filter(void) {
   /* now = 1700000000 (matches `now` we'll pass).  30-day lookback
    * cutoff = now - 30*86400 = 1697408000.  Old summary (1697000000)
    * is BEFORE cutoff → filtered.  Recent summary (1699999000) admits. */
   seed_summary(0, 100, 1, "ancient", 1697000000, false);
   seed_summary(1, 101, 1, "recent", 1699999000, false);
   seed_summary(2, 102, 1, "consolidated", 1699999500, true);
   s_mock.summary_count = 3;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "anything", NULL, 0,
                                                /*now*/ 1700000000, 10, &result));
   bool saw_ancient = false, saw_recent = false, saw_consolidated = false;
   const focus_candidate_t *consolidated = NULL, *recent = NULL;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_summary") != 0)
         continue;
      if (strstr(result.candidates[i].text, "ancient"))
         saw_ancient = true;
      if (strstr(result.candidates[i].text, "consolidated")) {
         saw_consolidated = true;
         consolidated = &result.candidates[i];
      } else if (strstr(result.candidates[i].text, "recent")) {
         saw_recent = true;
         recent = &result.candidates[i];
      }
   }
   TEST_ASSERT_FALSE(saw_ancient);
   TEST_ASSERT_TRUE(saw_recent);
   TEST_ASSERT_TRUE(saw_consolidated);
   TEST_ASSERT_NOT_NULL(consolidated);
   TEST_ASSERT_NOT_NULL(recent);
   /* importance: consolidated=1.0, normal=0.7 */
   TEST_ASSERT_TRUE(consolidated->importance_score > recent->importance_score);
   focus_result_free(&result);
}

/* =====================================================================
 * Empty-result behavior — all four adapters
 * ===================================================================== */

static void test_empty_db_zero_candidates(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   const float dummy[1] = { 0.0f };
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "anything", dummy, 1, 1700000000, 5, &result));
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(0, result.rejection_count);
   focus_result_free(&result);
}

static void test_unknown_user_zero_candidates(void) {
   seed_fact(0, 100, 1, "alpha", 0.9f);
   s_mock.fact_count = 1;
   seed_entity(0, 1, 1, "Alice", "person", true, embed_e1, NULL);
   s_mock.entity_count = 1;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   seed_summary(0, 100, 1, "summary", 1700000000, false);
   s_mock.summary_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* Nonexistent user_id=99999. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user*/ 99999, false, "alpha", embed_q_match_e1,
                                                MOCK_DIMS, 1700000000, 5, &result));
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   focus_result_free(&result);
}

/* =====================================================================
 * Cap honoring — caller's per_source_max_candidates is capped per-source
 * ===================================================================== */

static void test_cap_honoring_facts(void) {
   for (int i = 0; i < 8; i++) {
      char buf[32];
      snprintf(buf, sizeof(buf), "fact %d", i);
      seed_fact(i, 100 + i, 1, buf, 0.9f);
   }
   s_mock.fact_count = 8;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   const float dummy[1] = { 0.0f };
   focus_compose_result_t result = { 0 };
   /* per_source_max=3 — adapter must not return more than 3. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "fact", dummy, 1, 1700000000,
                                                /*per_source_max=*/3, &result));
   int fact_count = 0;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "memory_fact") == 0)
         fact_count++;
   TEST_ASSERT_EQUAL_INT(3, fact_count);
   focus_result_free(&result);
}

/* =====================================================================
 * Round-robin determinism — relation adapter
 * ===================================================================== */

/* At cosine 0.5 to embed_q_match_e1: the pool's typical level for that query. */
static const float embed_typical[MOCK_DIMS] = { 0.5f, 0.8660254f, 0.0f, 0.0f };

/* A pool of @p n entities at the typical level, then @p last (if any) at the
 * end: the least mentioned, so outside any most-mentioned subset. */
static void seed_entity_pool(int n) {
   for (int i = 0; i < n; i++) {
      char name[32];
      snprintf(name, sizeof(name), "Filler Thing %d", i);
      seed_entity(i, 1000 + i, 1, name, "thing", false, embed_typical, NULL);
      s_mock.entities[i].mention_count = 100;
   }
   s_mock.entity_count = n;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   g_config.memory.focus_injection.entity_min_relevance = 0.40f;
}

static bool has_entity(const focus_compose_result_t *r, const char *needle) {
   for (int i = 0; i < r->candidate_count; i++) {
      if (strcmp(r->candidates[i].source_id, "memory_entity") == 0 &&
          strstr(r->candidates[i].text, needle))
         return true;
   }
   return false;
}

static int count_source(const focus_compose_result_t *r, const char *source) {
   int n = 0;
   for (int i = 0; i < r->candidate_count; i++)
      if (strcmp(r->candidates[i].source_id, source) == 0)
         n++;
   return n;
}

/* The most similar entity is found wherever it sits in mention order, past
 * the cache's first read, and the typical ones are gated out. */
static void test_entity_found_past_the_most_mentioned(void) {
   seed_entity_pool(MOCK_MAX_ENTITIES - 1);
   seed_entity(MOCK_MAX_ENTITIES - 1, 9999, 1, "Marigold Garden Planner", "project", false,
               embed_e1, NULL);
   s_mock.entity_count = MOCK_MAX_ENTITIES;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "unrelated words", embed_q_match_e1,
                                                MOCK_DIMS, 1700000200, 8, &result));
   TEST_ASSERT_TRUE(has_entity(&result, "Marigold Garden Planner"));
   TEST_ASSERT_FALSE(has_entity(&result, "Filler Thing"));
   focus_result_free(&result);
}

/* An entity a long message names is found although the message's embedding
 * is diluted to the typical level; a two-word name needs both words. */
static void test_entity_named_in_long_message(void) {
   seed_entity_pool(40);
   seed_entity(40, 2001, 1, "Quillon", "organization", false, embed_typical, NULL);
   seed_entity(41, 2002, 1, "Nightly Backup Test", "event", false, embed_typical, NULL);
   s_mock.entity_count = 42;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(
       SUCCESS,
       focus_compose(1, false,
                     "good evening, what do you think about my decision to take my quillon "
                     "move? I'm also running a documents test",
                     embed_q_match_e1, MOCK_DIMS, 1700000200, 8, &result));
   TEST_ASSERT_TRUE(has_entity(&result, "Quillon"));
   TEST_ASSERT_FALSE(has_entity(&result, "Nightly Backup Test")); /* only "test" */
   TEST_ASSERT_FALSE(has_entity(&result, "Filler Thing"));
   focus_result_free(&result);
}

/* Relations come only from relevant subjects: none relevant, none at all. */
static void test_relation_needs_a_relevant_subject(void) {
   seed_entity_pool(40);
   seed_entity(0, 1000, 1, "Marigold Project", "project", false, embed_typical, NULL);
   seed_relation(0, 5000, 1, 1000, "owns", "something", 0, 0);
   s_mock.relation_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "the weather today", embed_q_match_e1,
                                                MOCK_DIMS, 1700000200, 8, &result));
   TEST_ASSERT_EQUAL_INT(0, count_source(&result, "memory_relation"));
   focus_result_free(&result);

   /* Named, the same subject brings its relation. */
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "tell me about the marigold project",
                                       embed_q_match_e1, MOCK_DIMS, 1700000200, 8, &result));
   TEST_ASSERT_EQUAL_INT(1, count_source(&result, "memory_relation"));
   focus_result_free(&result);
}

/* Without a query embedding, named entities are still found. */
static void test_entity_names_without_embedding(void) {
   seed_entity_pool(40);
   seed_entity(40, 2001, 1, "Quillon", "organization", false, embed_typical, NULL);
   s_mock.entity_count = 41;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what about quillon", NULL, 0, 1700000200,
                                                8, &result));
   TEST_ASSERT_TRUE(has_entity(&result, "Quillon"));
   TEST_ASSERT_EQUAL_INT(1, count_source(&result, "memory_entity"));
   focus_result_free(&result);
}

/* A pool too small to gate: context injection keeps the most similar, the
 * memory tool's search returns only the entities the query names. */
static void test_entity_small_pool(void) {
   seed_entity(0, 1, 1, "Quillon", "organization", false, embed_e1, NULL);
   seed_entity(1, 2, 1, "Watanabe", "person", false, embed_e2, NULL);
   s_mock.entity_count = 2;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   g_config.memory.focus_injection.entity_min_relevance = 0.40f;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "unrelated words", embed_q_match_e2,
                                                MOCK_DIMS, 1700000200, 8, &result));
   TEST_ASSERT_TRUE(has_entity(&result, "Watanabe"));
   focus_result_free(&result);

   int64_t ids[4];
   char names[4][MEMORY_ENTITY_NAME_MAX];
   TEST_ASSERT_EQUAL_INT(0, memory_embeddings_entity_search(1, "unrelated words", NULL, ids, names,
                                                            NULL, NULL, 4));
   TEST_ASSERT_EQUAL_INT(1, memory_embeddings_entity_search(1, "what do we know about quillon?",
                                                            NULL, ids, names, NULL, NULL, 4));
   TEST_ASSERT_EQUAL_STRING("Quillon", names[0]);

   /* The gate off (0) means any similar entity counts, on this path too. */
   g_config.memory.focus_injection.entity_min_relevance = 0.0f;
   memory_entity_match_t m[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         memory_embeddings_entity_matches(1, "unrelated words", embed_q_match_e2,
                                                          MOCK_DIMS, 0.0f, m, 4, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
}

/* A name near the end of a long message is still found. */
static void test_entity_named_late_in_long_message(void) {
   seed_entity_pool(40);
   seed_entity(40, 2001, 1, "Quillon", "organization", false, embed_typical, NULL);
   s_mock.entity_count = 41;
   char msg[1024];
   size_t off = 0;
   for (int i = 0; i < 60; i++) {
      off += (size_t)snprintf(msg + off, sizeof(msg) - off, "word%02d ", i);
   }
   snprintf(msg + off, sizeof(msg) - off, "and finally quillon");

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, msg, embed_q_match_e1, MOCK_DIMS,
                                                1700000200, 8, &result));
   TEST_ASSERT_TRUE(has_entity(&result, "Quillon"));
   focus_result_free(&result);
}

/* Longest match wins: the query's "ai" belongs to "Marigold AI Planner", so
 * "AI" isn't named; "Quillon" alone still is. */
static void test_entity_longest_match_wins(void) {
   seed_entity_pool(40);
   seed_entity(40, 3001, 1, "Marigold AI Planner", "project", false, embed_typical, NULL);
   seed_entity(41, 3002, 1, "AI", "topic", false, embed_typical, NULL);
   seed_entity(42, 3003, 1, "Quillon", "org", false, embed_typical, NULL);
   s_mock.entity_count = 43;
   memory_entity_match_t m[8];
   int n = 0;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_embeddings_entity_matches(1, "recall Marigold AI Planner",
                                                                   embed_q_match_e1, MOCK_DIMS,
                                                                   0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("Marigold AI Planner", m[0].name);

   /* Without the longer match, "ai" does name AI. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_embeddings_entity_matches(1, "what can ai do for us",
                                                                   embed_q_match_e1, MOCK_DIMS,
                                                                   0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("AI", m[0].name);

   /* A three-word title with one content word takes two matching words. */
   seed_entity(43, 3004, 1, "Why We Build", "work", false, embed_typical, NULL);
   s_mock.entity_count = 44;
   memory_embeddings_invalidate_entity_cache_for_user(1);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_embeddings_entity_matches(1, "who builds the displays",
                                                                   embed_q_match_e1, MOCK_DIMS,
                                                                   0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);

   /* Function words and repeats count toward a name: "the quillmen" names
    * The Quillmen, "borra borra" names Borra Borra.  A tie goes to the exact
    * name: "morning report" names Morning Report, not Morning Report Test. */
   seed_entity(44, 3005, 1, "The Quillmen", "band", false, embed_typical, NULL);
   seed_entity(45, 3006, 1, "Borra Borra", "place", false, embed_typical, NULL);
   seed_entity(46, 3007, 1, "Morning Report", "event", false, embed_typical, NULL);
   seed_entity(47, 3008, 1, "Morning Report Test", "event", false, embed_typical, NULL);
   s_mock.entity_count = 48;
   memory_embeddings_invalidate_entity_cache_for_user(1);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         memory_embeddings_entity_matches(1, "play the quillmen", embed_q_match_e1,
                                                          MOCK_DIMS, 0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("The Quillmen", m[0].name);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_embeddings_entity_matches(1, "trip to borra borra",
                                                                   embed_q_match_e1, MOCK_DIMS,
                                                                   0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("Borra Borra", m[0].name);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_embeddings_entity_matches(1, "time for the morning report",
                                                                   embed_q_match_e1, MOCK_DIMS,
                                                                   0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("Morning Report", m[0].name);

   /* A single letter is a word too: "X Corp" needs the phrase, not "corp". */
   seed_entity(48, 3009, 1, "X Corp", "org", false, embed_typical, NULL);
   s_mock.entity_count = 49;
   memory_embeddings_invalidate_entity_cache_for_user(1);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         memory_embeddings_entity_matches(1, "the corp picnic", embed_q_match_e1,
                                                          MOCK_DIMS, 0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_embeddings_entity_matches(1, "what does x corp sell",
                                                                   embed_q_match_e1, MOCK_DIMS,
                                                                   0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("X Corp", m[0].name);

   /* Unrelated names each stand on their own. */
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, memory_embeddings_entity_matches(1, "the marigold ai planner and quillon",
                                                 embed_q_match_e1, MOCK_DIMS, 0.40f, m, 8, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
}

/* Each user keeps a copy; changing one user's entities reloads only theirs. */
static void test_entity_cache_per_user(void) {
   seed_entity(0, 1, 1, "Quillon", "organization", false, embed_e1, NULL);
   seed_entity(1, 2, 2, "Watanabe", "person", false, embed_e2, NULL);
   s_mock.entity_count = 2;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   memory_entity_match_t m[4];
   int n = 0;

   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         memory_embeddings_entity_matches(1, "quillon", NULL, 0, 0.4f, m, 4, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         memory_embeddings_entity_matches(2, "watanabe", NULL, 0, 0.4f, m, 4, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("Watanabe", m[0].name);
   const int loads = s_mock.call_count_entity_embeddings;

   /* Alternating users: both copies stay. */
   memory_embeddings_entity_matches(1, "quillon", NULL, 0, 0.4f, m, 4, &n);
   memory_embeddings_entity_matches(2, "watanabe", NULL, 0, 0.4f, m, 4, &n);
   TEST_ASSERT_EQUAL_INT(loads, s_mock.call_count_entity_embeddings);

   /* User 2 changes: only user 2 reloads. */
   memory_embeddings_invalidate_entity_cache_for_user(2);
   memory_embeddings_entity_matches(1, "quillon", NULL, 0, 0.4f, m, 4, &n);
   TEST_ASSERT_EQUAL_INT(loads, s_mock.call_count_entity_embeddings);
   memory_embeddings_entity_matches(2, "watanabe", NULL, 0, 0.4f, m, 4, &n);
   TEST_ASSERT_EQUAL_INT(loads + 1, s_mock.call_count_entity_embeddings);
}

static void test_relation_round_robin(void) {
   /* 3 entities, all valid subjects.  Top-3 cosine seeds the round-robin. */
   seed_entity(0, 1, 1, "Subj1", "person", false, embed_e1, NULL);
   seed_entity(1, 2, 1, "Subj2", "person", false, embed_e2, NULL);
   seed_entity(2, 3, 1, "Subj3", "person", false, embed_e3, NULL);
   s_mock.entity_count = 3;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;

   /* Each subject has 5 relations available. */
   int idx = 0;
   for (int subj = 1; subj <= 3; subj++) {
      for (int j = 0; j < 5; j++) {
         char obj[32];
         snprintf(obj, sizeof(obj), "obj_%d_%d", subj, j);
         seed_relation(idx, 1000 + idx, 1, subj, "owns", obj, 0, 0);
         idx++;
      }
   }
   s_mock.relation_count = idx;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   /* per_source_max=4 across 3 subjects → 2/1/1 in similarity-desc order. */
   focus_compose_result_t result = { 0 };
   /* Query embedding aligns most with embed_e1 (subject 1). */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "anything", embed_q_match_e1, MOCK_DIMS,
                                                1700000000, /*per_source_max=*/4, &result));
   int subj_counts[4] = { 0 }; /* index by subj_id */
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_relation") != 0)
         continue;
      const char *p = strstr(result.candidates[i].text, "obj_");
      if (p && p[4] >= '1' && p[4] <= '3')
         subj_counts[p[4] - '0']++;
   }
   /* 4 = floor(4/3)*3 + remainder 1 → subjects[0]=2, subjects[1]=1, subjects[2]=1. */
   /* Subject 1 (top similarity) gets 2; subjects 2 & 3 get 1 each. */
   TEST_ASSERT_EQUAL_INT(2, subj_counts[1]);
   TEST_ASSERT_EQUAL_INT(1, subj_counts[2]);
   TEST_ASSERT_EQUAL_INT(1, subj_counts[3]);
   focus_result_free(&result);
}

/* =====================================================================
 * NULL / empty safety — summary adapter
 * ===================================================================== */

static void test_summary_null_query_text(void) {
   seed_summary(0, 100, 1, "summary", 1700000000, false);
   s_mock.summary_count = 1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, /*query_text*/ NULL, NULL, 0, 1700000000,
                                                5, &result));
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "memory_summary"));
   focus_result_free(&result);
}

static void test_summary_empty_query_text(void) {
   seed_summary(0, 100, 1, "summary", 1700000000, false);
   s_mock.summary_count = 1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "", NULL, 0, 1700000000, 5, &result));
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "memory_summary"));
   focus_result_free(&result);
}

/* =====================================================================
 * End-to-end via the framework — register_all + focus_compose
 * ===================================================================== */

static void test_register_all_succeeds(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   /* Re-register without focus_unregister_all() must FAIL on the first
    * duplicate (memory_fact) per the framework's no-overwrite rule. */
   TEST_ASSERT_EQUAL_INT(FAILURE, memory_focus_adapters_register_all());
}

static void test_compose_no_query_embedding_skips_vector_adapters(void) {
   seed_fact(0, 100, 1, "alpha", 0.9f);
   s_mock.fact_count = 1;
   seed_entity(0, 1, 1, "Alice", "person", false, embed_e1, NULL);
   s_mock.entity_count = 1;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   seed_summary(0, 100, 1, "summary text", 1700000000, false);
   s_mock.summary_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* No query_embedding → fact/entity/relation skipped (requires_embedding=true);
    * summary still runs. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "alpha", /*qembed*/ NULL, 0, 1700000000,
                                                5, &result));
   bool saw_summary = false;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_fact") == 0 ||
          strcmp(result.candidates[i].source_id, "memory_entity") == 0 ||
          strcmp(result.candidates[i].source_id, "memory_relation") == 0) {
         TEST_FAIL_MESSAGE("requires_embedding=true adapter should have been skipped");
      }
      if (strcmp(result.candidates[i].source_id, "memory_summary") == 0)
         saw_summary = true;
   }
   TEST_ASSERT_TRUE(saw_summary);
   focus_result_free(&result);
}

/* =====================================================================
 * Item-id format invariant — "<source_short>:<int64_id>"
 * ===================================================================== */

static void test_item_id_format(void) {
   seed_fact(0, 100, 1, "fact", 0.9f);
   s_mock.fact_count = 1;
   seed_entity(0, 7, 1, "Ent", "person", true, embed_e1, NULL);
   s_mock.entity_count = 1;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;
   seed_relation(0, 42, 1, /*subj*/ 7, "knows", "thing", 0, 0);
   s_mock.relation_count = 1;
   seed_summary(0, 13, 1, "summary text", 1700000000, false);
   s_mock.summary_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "Ent", embed_q_match_e1, MOCK_DIMS,
                                                1700000000, 5, &result));
   bool saw_fact = false, saw_entity = false, saw_relation = false, saw_summary = false;
   for (int i = 0; i < result.candidate_count; i++) {
      const char *sid = result.candidates[i].source_id;
      const char *id = result.candidates[i].item_id;
      if (strcmp(sid, "memory_fact") == 0) {
         TEST_ASSERT_EQUAL_STRING("fact:100", id);
         saw_fact = true;
      } else if (strcmp(sid, "memory_entity") == 0) {
         TEST_ASSERT_EQUAL_STRING("entity:7", id);
         saw_entity = true;
      } else if (strcmp(sid, "memory_relation") == 0) {
         TEST_ASSERT_EQUAL_STRING("relation:42", id);
         saw_relation = true;
      } else if (strcmp(sid, "memory_summary") == 0) {
         TEST_ASSERT_EQUAL_STRING("summary:13", id);
         saw_summary = true;
      }
   }
   TEST_ASSERT_TRUE(saw_fact && saw_entity && saw_relation && saw_summary);
   focus_result_free(&result);
}

/* =====================================================================
 * Provenance shape — entities are zeroed; facts/summaries/relations
 * carry the back-link.
 * ===================================================================== */

static void test_entity_provenance_zeroed(void) {
   seed_entity(0, 1, 1, "Ent", "person", true, embed_e1, NULL);
   s_mock.entity_count = 1;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "Ent", embed_q_match_e1, MOCK_DIMS,
                                                1700000000, 5, &result));
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "memory_entity") == 0) {
         TEST_ASSERT_EQUAL_INT64(0, result.candidates[i].provenance.conv_id);
         TEST_ASSERT_EQUAL_INT64(0, result.candidates[i].provenance.msg_id_start);
         TEST_ASSERT_EQUAL_INT64(0, result.candidates[i].provenance.msg_id_end);
      }
   }
   focus_result_free(&result);
}

/* ============================================================================
 * Phase 1g-i: parallel score_breakdowns array — populated alongside
 * candidates, freed alongside candidates, summed-equals-final_score.
 * Exercises the real focus_source.c rank pass (this test file links it).
 * ============================================================================ */

static void test_score_breakdowns_aligned(void) {
   seed_fact(0, 100, 1, "fact alpha", 0.9f);
   seed_fact(1, 101, 1, "fact beta", 0.5f);
   s_mock.fact_count = 2;
   seed_entity(0, 1, 1, "alpha", "person", true, embed_e1, NULL);
   s_mock.entity_count = 1;
   s_mock.entity_dim = MOCK_DIMS;
   s_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "alpha", embed_q_match_e1, MOCK_DIMS,
                                                1700000000, 5, &result));

   /* Both arrays must have the same shape: parallel, fully populated. */
   TEST_ASSERT_TRUE_MESSAGE(result.candidate_count > 0,
                            "test setup must produce at least one candidate");
   TEST_ASSERT_NOT_NULL_MESSAGE(result.score_breakdowns,
                                "score_breakdowns must be allocated when candidate_count > 0");

   for (int i = 0; i < result.candidate_count; i++) {
      const focus_score_breakdown_t *b = &result.score_breakdowns[i];
      /* Sum of the four contributions must equal final_score (within
       * float precision).  This is the parallel-array invariant — if a
       * future refactor diverges the two, the catch goes here. */
      const float sum = b->semantic_contribution + b->recency_contribution +
                        b->importance_contribution + b->source_contribution;
      char msg[128];
      snprintf(msg, sizeof(msg), "candidate %d: sum=%.6f vs final_score=%.6f", i, sum,
               b->final_score);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, b->final_score, sum, msg);
      /* applied_source_weight is the raw lookup, must be non-negative. */
      TEST_ASSERT_TRUE(b->applied_source_weight >= 0.0f);
   }

   /* Rank order: descending final_score. */
   for (int i = 1; i < result.candidate_count; i++) {
      TEST_ASSERT_TRUE_MESSAGE(result.score_breakdowns[i - 1].final_score >=
                                   result.score_breakdowns[i].final_score,
                               "score_breakdowns must inherit candidate's rank order");
   }

   focus_result_free(&result);
   /* After free both arrays must be NULL.  (focus_result_free contract.) */
   TEST_ASSERT_NULL(result.candidates);
   TEST_ASSERT_NULL(result.score_breakdowns);
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
}

int main(void) {
   UNITY_BEGIN();

   /* Cross-user isolation — load-bearing security gates */
   RUN_TEST(test_fact_cross_user);
   RUN_TEST(test_entity_cross_user);
   RUN_TEST(test_relation_cross_user);
   RUN_TEST(test_summary_cross_user);

   /* Per-adapter happy paths */
   RUN_TEST(test_fact_adapter_shape);
   RUN_TEST(test_entity_adapter_photo_boost);
   RUN_TEST(test_entity_keyword_match_boosts_importance);
   RUN_TEST(test_relation_filters_expired);
   RUN_TEST(test_summary_lookback_filter);

   /* Empty-result behavior */
   RUN_TEST(test_empty_db_zero_candidates);
   RUN_TEST(test_unknown_user_zero_candidates);

   /* Cap honoring */
   RUN_TEST(test_cap_honoring_facts);

   /* Round-robin determinism */
   RUN_TEST(test_relation_round_robin);
   RUN_TEST(test_entity_found_past_the_most_mentioned);
   RUN_TEST(test_entity_named_in_long_message);
   RUN_TEST(test_relation_needs_a_relevant_subject);
   RUN_TEST(test_entity_names_without_embedding);
   RUN_TEST(test_entity_small_pool);
   RUN_TEST(test_entity_named_late_in_long_message);
   RUN_TEST(test_entity_cache_per_user);
   RUN_TEST(test_entity_longest_match_wins);

   /* NULL safety */
   RUN_TEST(test_summary_null_query_text);
   RUN_TEST(test_summary_empty_query_text);

   /* End-to-end via framework */
   RUN_TEST(test_register_all_succeeds);
   RUN_TEST(test_compose_no_query_embedding_skips_vector_adapters);

   /* Shape invariants */
   RUN_TEST(test_item_id_format);
   RUN_TEST(test_entity_provenance_zeroed);

   /* Phase 1g-i: parallel score_breakdowns */
   RUN_TEST(test_score_breakdowns_aligned);

   return UNITY_END();
}
