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
 * Unit tests for src/core/focus/focus_source.c — adapter registry, filter-on-
 * retrieval pipeline, three-factor ranker, top_k / min_score / token-budget
 * trim, memory ownership.
 *
 * Strategy: fake adapters synthesize candidates inline.  No production DB,
 * no embedding engine, no document_search.  memory_filter is REAL — tests
 * intentionally exercise real injection-pattern detection.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_source.h"
#include "core/focus/focus_source_internal.h"
#include "dawn_error.h"
#include "unity.h"

/* =============================================================================
 * Per-test programmable fake-adapter state
 * ============================================================================= */

typedef struct {
   const char *source_id;
   focus_source_type_t source_type;
   bool requires_embedding;
   /* Programmable knobs */
   int candidate_count;   /* How many to emit on a call */
   const char **texts;    /* Borrowed; copied into heap on emit */
   const char **item_ids; /* Borrowed; copied into heap on emit */
   float *semantic_scores;
   float *recency_scores;
   float *importance_scores;
   time_t *timestamps;
   int target_user_id;         /* Only emit when invoked with this user_id */
   bool return_failure;        /* If true, adapter returns FAILURE */
   bool partial_alloc_failure; /* Allocate the array, populate 2, then FAILURE */
   int over_cap_count;         /* Returns this many regardless of max_candidates hint */
   /* Capture telemetry */
   int call_count;
   int last_user_id;
   bool last_include_private;
} fake_adapter_state_t;

static fake_adapter_state_t s_fake[MAX_FOCUS_SOURCES];
static int s_fake_count = 0;
static focus_source_adapter_t s_fake_adapters[MAX_FOCUS_SOURCES];

static char *xstrdup(const char *s) {
   if (s == NULL)
      return NULL;
   size_t n = strlen(s);
   char *p = malloc(n + 1);
   if (p == NULL) {
      fprintf(stderr, "test_focus_source: OOM in xstrdup\n");
      abort();
   }
   memcpy(p, s, n + 1);
   return p;
}

/* Per-fake trampoline.  We pre-generate distinct functions via a macro
 * so each adapter has its own static query function pointer that
 * unambiguously identifies its state slot. */
#define DEFINE_FAKE(N)                                                                          \
   static int fake_query_##N(int user_id, bool include_private, const char *query_text,         \
                             const float *query_embedding, size_t embed_dim, time_t now,        \
                             int max_candidates, focus_candidate_t **out_candidates,            \
                             int *out_count) {                                                  \
      (void)query_text;                                                                         \
      (void)query_embedding;                                                                    \
      (void)embed_dim;                                                                          \
      (void)now;                                                                                \
      (void)max_candidates;                                                                     \
      fake_adapter_state_t *st = &s_fake[N];                                                    \
      st->call_count++;                                                                         \
      st->last_user_id = user_id;                                                               \
      st->last_include_private = include_private;                                               \
      *out_candidates = NULL;                                                                   \
      *out_count = 0;                                                                           \
      if (st->target_user_id != 0 && user_id != st->target_user_id)                             \
         return SUCCESS;                                                                        \
      if (st->return_failure) {                                                                 \
         *out_candidates = NULL;                                                                \
         *out_count = 0;                                                                        \
         return FAILURE;                                                                        \
      }                                                                                         \
      int n = st->over_cap_count > 0 ? st->over_cap_count : st->candidate_count;                \
      if (n <= 0)                                                                               \
         return SUCCESS;                                                                        \
      focus_candidate_t *out = calloc((size_t)n, sizeof(*out));                                 \
      if (out == NULL) {                                                                        \
         *out_candidates = NULL;                                                                \
         *out_count = 0;                                                                        \
         return FAILURE;                                                                        \
      }                                                                                         \
      int populated = 0;                                                                        \
      for (int i = 0; i < n; i++) {                                                             \
         out[i].source_id = st->source_id;                                                      \
         out[i].source_type = st->source_type;                                                  \
         out[i].semantic_score = st->semantic_scores ? st->semantic_scores[i] : FOCUS_SCORE_NA; \
         out[i].recency_score = st->recency_scores ? st->recency_scores[i] : FOCUS_SCORE_NA;    \
         out[i].importance_score = st->importance_scores ? st->importance_scores[i] : 0.5f;     \
         out[i].item_timestamp = st->timestamps ? st->timestamps[i] : 0;                        \
         out[i].text = xstrdup(st->texts ? st->texts[i] : "default text");                      \
         out[i].item_id = xstrdup(st->item_ids ? st->item_ids[i] : "default-id");               \
         memset(&out[i].provenance, 0, sizeof(out[i].provenance));                              \
         populated = i + 1;                                                                     \
         if (st->partial_alloc_failure && populated >= 2) {                                     \
            /* Free the partial allocation and fail.  Adapter contract                          \
             * mandates this — framework will not clean up on FAILURE. */                     \
            for (int j = 0; j < populated; j++) {                                               \
               free(out[j].text);                                                               \
               free(out[j].item_id);                                                            \
            }                                                                                   \
            free(out);                                                                          \
            *out_candidates = NULL;                                                             \
            *out_count = 0;                                                                     \
            return FAILURE;                                                                     \
         }                                                                                      \
      }                                                                                         \
      *out_candidates = out;                                                                    \
      *out_count = populated;                                                                   \
      return SUCCESS;                                                                           \
   }

DEFINE_FAKE(0)
DEFINE_FAKE(1)
DEFINE_FAKE(2)
DEFINE_FAKE(3)
DEFINE_FAKE(4)
DEFINE_FAKE(5)
DEFINE_FAKE(6)
DEFINE_FAKE(7)
DEFINE_FAKE(8)
DEFINE_FAKE(9)
DEFINE_FAKE(10)
DEFINE_FAKE(11)
DEFINE_FAKE(12)
DEFINE_FAKE(13)
DEFINE_FAKE(14)
DEFINE_FAKE(15)
#undef DEFINE_FAKE

static const focus_source_query_fn FAKE_FUNCS[MAX_FOCUS_SOURCES] = {
   fake_query_0,  fake_query_1,  fake_query_2,  fake_query_3,  fake_query_4,  fake_query_5,
   fake_query_6,  fake_query_7,  fake_query_8,  fake_query_9,  fake_query_10, fake_query_11,
   fake_query_12, fake_query_13, fake_query_14, fake_query_15,
};

static int register_fake(const char *source_id, focus_source_type_t type, bool requires_embedding) {
   if (s_fake_count >= MAX_FOCUS_SOURCES)
      return FAILURE;
   int slot = s_fake_count++;
   memset(&s_fake[slot], 0, sizeof(s_fake[slot]));
   s_fake[slot].source_id = source_id;
   s_fake[slot].source_type = type;
   s_fake[slot].requires_embedding = requires_embedding;

   s_fake_adapters[slot].source_id = source_id;
   s_fake_adapters[slot].source_type = type;
   s_fake_adapters[slot].requires_embedding = requires_embedding;
   s_fake_adapters[slot].query = FAKE_FUNCS[slot];

   return focus_register_source(&s_fake_adapters[slot]);
}

/* =============================================================================
 * Test scaffolding
 * ============================================================================= */

dawn_config_t g_config;

static void reset_config_defaults(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.focus_injection.enabled = true;
   g_config.memory.focus_injection.focus_budget_bytes = 16384;
   g_config.memory.focus_injection.top_k = 16;
   g_config.memory.focus_injection.min_score = 0.0f;
   g_config.memory.focus_injection.weight_semantic = 1.0f;
   g_config.memory.focus_injection.weight_recency = 0.3f;
   g_config.memory.focus_injection.weight_importance = 0.2f;
   g_config.memory.focus_injection.weight_source = 1.0f;
   g_config.memory.focus_injection.source_weights.memory_fact = 1.0f;
   g_config.memory.focus_injection.source_weights.memory_entity = 0.9f;
   g_config.memory.focus_injection.source_weights.memory_relation = 0.85f;
   g_config.memory.focus_injection.source_weights.memory_summary = 0.7f;
   g_config.memory.focus_injection.source_weights.document_chunk = 0.7f;
   g_config.memory.focus_injection.source_weights.calendar_event = 0.6f;
   g_config.memory.focus_injection.source_weights.recent_email = 0.5f;
   g_config.memory.focus_injection.source_weights.dawn_background = 0.8f;
}

void setUp(void) {
   focus_unregister_all();
   memset(s_fake, 0, sizeof(s_fake));
   memset(s_fake_adapters, 0, sizeof(s_fake_adapters));
   s_fake_count = 0;
   reset_config_defaults();
}

void tearDown(void) {
}

/* =============================================================================
 * Test cases
 * ============================================================================= */

/* 1. register / iterate — register 3 fakes; assert focus_compose visits all 3. */
static void test_register_and_iterate_all_adapters(void) {
   const char *texts0[] = { "alpha" };
   const char *texts1[] = { "beta" };
   const char *texts2[] = { "gamma" };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   register_fake("memory_entity", FOCUS_SOURCE_INTERNAL, false);
   register_fake("document_chunk", FOCUS_SOURCE_EXTERNAL, false);

   s_fake[0].candidate_count = 1;
   s_fake[0].texts = texts0;
   s_fake[1].candidate_count = 1;
   s_fake[1].texts = texts1;
   s_fake[2].candidate_count = 1;
   s_fake[2].texts = texts2;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(/*user_id*/ 1, /*include_private*/ false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(1, s_fake[0].call_count);
   TEST_ASSERT_EQUAL_INT(1, s_fake[1].call_count);
   TEST_ASSERT_EQUAL_INT(1, s_fake[2].call_count);
   focus_result_free(&result);
}

/* 2. cross-user isolation — fake only emits for user_id=42; ask for user_id=99. */
static void test_cross_user_isolation(void) {
   const char *texts[] = { "private to user 42" };
   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 1;
   s_fake[0].texts = texts;
   s_fake[0].target_user_id = 42;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(99, false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(0, result.rejection_count);
   TEST_ASSERT_EQUAL_INT(99, s_fake[0].last_user_id);
   focus_result_free(&result);
}

/* 3. ranker math — hand-computed weights; assert ordering matches. */
static void test_ranker_math(void) {
   const char *texts[] = { "low", "high", "mid" };
   float sem[] = { 0.1f, 0.9f, 0.5f };
   float rec[] = { 0.0f, 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f, 0.0f };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 3;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   /* w_sem=1, w_rec=0, w_imp=0, w_src=0 — score == semantic_score */
   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, result.candidate_count);
   /* Sorted desc by score: high → mid → low */
   TEST_ASSERT_EQUAL_STRING("high", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("mid", result.candidates[1].text);
   TEST_ASSERT_EQUAL_STRING("low", result.candidates[2].text);
   focus_result_free(&result);
}

/* 3b. tie-breaker — equal score, newer timestamp wins. */
static void test_ranker_tiebreak_by_timestamp(void) {
   const char *texts[] = { "older", "newer" };
   float sem[] = { 0.5f, 0.5f };
   float rec[] = { 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f };
   time_t ts[] = { 1000, 2000 };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 2;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;
   s_fake[0].timestamps = ts;

   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 0, 5, &result));
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("newer", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("older", result.candidates[1].text);
   focus_result_free(&result);
}

/* 4. min_score threshold — candidates below threshold dropped. */
static void test_min_score_threshold(void) {
   const char *texts[] = { "low", "high" };
   float sem[] = { 0.1f, 0.9f };
   float rec[] = { 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 2;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
   g_config.memory.focus_injection.min_score = 0.5f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 0, 5, &result));
   TEST_ASSERT_EQUAL_INT(1, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("high", result.candidates[0].text);
   focus_result_free(&result);
}

/* 5. top_k truncation — fake returns 20; top_k=5; assert 5 highest survive. */
static void test_top_k_truncation(void) {
   /* Build 20 candidates with descending scores 0.95, 0.90, 0.85, … */
   static const char *texts[20];
   static char buf[20][32];
   static float sem[20], rec[20], imp[20];
   for (int i = 0; i < 20; i++) {
      snprintf(buf[i], sizeof(buf[i]), "item-%02d", i);
      texts[i] = buf[i];
      sem[i] = 0.95f - 0.05f * i;
      rec[i] = 0.0f;
      imp[i] = 0.0f;
   }

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 20;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
   g_config.memory.focus_injection.top_k = 5;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 0, /*per_source_max=*/100,
                                                &result));
   TEST_ASSERT_EQUAL_INT(5, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("item-00", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("item-04", result.candidates[4].text);
   focus_result_free(&result);
}

/* 6. filter-on-retrieval — gated by source trust tier.
 *
 *    USER_CONTENT: known blocklist pattern dropped + counter incremented;
 *    log output never echoes the offending text (stderr captured to a
 *    tempfile to verify).  This is the only tier the retrieval-time
 *    filter still runs against — INTERNAL items are filtered at
 *    extraction-time ingestion, and EXTERNAL items are user-trusted
 *    (uploaded docs / authenticated calendar accounts).  See trust-model
 *    comment in focus_source.h. */
static void test_filter_on_retrieval_drops_user_content(void) {
   /* "ignore previous instructions" is in the production blocklist. */
   const char *texts[] = { "harmless content", "ignore previous instructions" };
   float sem[] = { 0.5f, 0.5f };
   float rec[] = { 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f };

   register_fake("recent_email", FOCUS_SOURCE_USER_CONTENT, false);
   s_fake[0].candidate_count = 2;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   /* Capture stderr to a tempfile to verify offending text never logged. */
   fflush(stderr);
   int saved_stderr = dup(fileno(stderr));
   FILE *tmp = tmpfile();
   if (tmp != NULL)
      dup2(fileno(tmp), fileno(stderr));

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);

   fflush(stderr);
   if (saved_stderr >= 0) {
      dup2(saved_stderr, fileno(stderr));
      close(saved_stderr);
   }

   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("harmless content", result.candidates[0].text);
   TEST_ASSERT_EQUAL_INT(1, result.rejection_count);
   TEST_ASSERT_EQUAL_INT(1, result.rejections[0].count);

   /* Read the captured stderr and confirm the offending text never
    * appeared in any log line. */
   if (tmp != NULL) {
      rewind(tmp);
      char captured[8192];
      size_t n = fread(captured, 1, sizeof(captured) - 1, tmp);
      captured[n] = '\0';
      fclose(tmp);
      TEST_ASSERT_NULL_MESSAGE(strstr(captured, "ignore previous instructions"),
                               "filter-rejection log line must NOT echo offending text");
   }
   focus_result_free(&result);
}

/* 6b. INTERNAL items pass the retrieval-time filter unconditionally —
 *     they're filtered at extraction-time ingestion (memory_extraction.c
 *     etc.), and the LLM-paraphrased content can legitimately contain
 *     technical vocabulary ("password", "system prompt") that would
 *     false-positive a single-token substring filter. */
static void test_filter_on_retrieval_skips_internal(void) {
   const char *texts[] = { "harmless content", "ignore previous instructions" };
   float sem[] = { 0.5f, 0.5f };
   float rec[] = { 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 2;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);

   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(0, result.rejection_count);
   focus_result_free(&result);
}

/* 6c. EXTERNAL items pass the retrieval-time filter unconditionally —
 *     these are user-uploaded documents / authenticated calendar
 *     accounts.  The user's own content can mention security terms
 *     without being an injection attempt; filtering at retrieval was
 *     causing 4-6 false rejections per turn on real uploads. */
static void test_filter_on_retrieval_skips_external(void) {
   const char *texts[] = { "harmless content", "ignore previous instructions" };
   float sem[] = { 0.5f, 0.5f };
   float rec[] = { 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f };

   register_fake("document_chunk", FOCUS_SOURCE_EXTERNAL, false);
   s_fake[0].candidate_count = 2;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);

   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(0, result.rejection_count);
   focus_result_free(&result);
}

/* 7. requires_embedding=true with NULL query_embedding — adapter NOT called;
 *    no contribution; no false rejection counted. */
static void test_requires_embedding_skipped_without_query(void) {
   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, /*requires_embedding=*/true);
   s_fake[0].candidate_count = 1;
   const char *texts[] = { "should not appear" };
   s_fake[0].texts = texts;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, /*query_embedding=*/NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(0, result.rejection_count);
   TEST_ASSERT_EQUAL_INT(0, s_fake[0].call_count);
   focus_result_free(&result);
}

/* 8. byte-budget truncation — focus_budget_bytes caps result, last
 *    over-budget candidate dropped. */
static void test_byte_budget_truncation(void) {
   /* Each text is exactly 40 bytes.  Budget=60 admits one candidate (40),
    * the second pushes the total to 80 > 60 and is dropped. */
   static char body[2][41];
   const char *texts[] = { body[0], body[1] };
   memset(body[0], 'a', 40);
   body[0][40] = '\0';
   memset(body[1], 'b', 40);
   body[1][40] = '\0';

   float sem[] = { 0.9f, 0.8f };
   float rec[] = { 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 2;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
   g_config.memory.focus_injection.focus_budget_bytes = 60;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 0, 5, &result));
   TEST_ASSERT_EQUAL_INT(1, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING(body[0], result.candidates[0].text);
   focus_result_free(&result);
}

/* A caller's shown size for a candidate: its text, capped at 20 bytes. */
static int shown_bytes_capped_at_20(const focus_candidate_t *c) {
   const int len = (int)strlen(c->text);
   return len > 20 ? 20 : len;
}

/* 8a. A candidate that doesn't fit is skipped, and smaller ones ranked below
 *     it still fill the budget. */
static void test_byte_budget_skips_and_packs(void) {
   static char body[3][41];
   const char *texts[] = { body[0], body[1], body[2] };
   memset(body[0], 'a', 40);
   body[0][40] = '\0';
   memset(body[1], 'b', 40);
   body[1][40] = '\0';
   memset(body[2], 'c', 10);
   body[2][10] = '\0';

   float sem[] = { 0.9f, 0.8f, 0.7f };
   float rec[] = { 0.0f, 0.0f, 0.0f };
   float imp[] = { 0.0f, 0.0f, 0.0f };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 3;
   s_fake[0].texts = texts;
   s_fake[0].semantic_scores = sem;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;

   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
   g_config.memory.focus_injection.focus_budget_bytes = 60;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 0, 5, &result));
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count); /* 40 + 10; the second 40 skipped */
   TEST_ASSERT_EQUAL_STRING(body[0], result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING(body[2], result.candidates[1].text);
   focus_result_free(&result);

   /* A caller that shows capped lines charges what it shows: all three fit. */
   const focus_limits_t limits = { .top_k = 0,
                                   .min_score = -1.0f,
                                   .budget_bytes = 60,
                                   .item_bytes = shown_bytes_capped_at_20 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose_ex(1, false, NULL, NULL, 0, 0, 5, &limits, &result));
   TEST_ASSERT_EQUAL_INT(3, result.candidate_count);
   focus_result_free(&result);
}

/* 8b. UTF-8-safe truncation — focus_utf8_safe_cap must never cut inside a
 *     multi-byte character, or the truncated text is invalid UTF-8 and breaks
 *     the context_injection WebSocket text frame (browser drops the socket). */
static void test_utf8_safe_cap(void) {
   /* "AAA" + em-dash U+2014 (bytes E2 80 94) + "XYZ" — em-dash at offsets 3-5. */
   const char *t = "AAA\xE2\x80\x94XYZ";
   TEST_ASSERT_EQUAL_INT(3, (int)focus_utf8_safe_cap(t, 3));          /* boundary before em-dash */
   TEST_ASSERT_EQUAL_INT(3, (int)focus_utf8_safe_cap(t, 4));          /* mid em-dash -> back to 3 */
   TEST_ASSERT_EQUAL_INT(3, (int)focus_utf8_safe_cap(t, 5));          /* mid em-dash -> back to 3 */
   TEST_ASSERT_EQUAL_INT(6, (int)focus_utf8_safe_cap(t, 6));          /* full em-dash kept */
   TEST_ASSERT_EQUAL_INT(9, (int)focus_utf8_safe_cap(t, 99));         /* whole string fits */
   TEST_ASSERT_EQUAL_INT(5, (int)focus_utf8_safe_cap("abcdefgh", 5)); /* ASCII: exact cut ok */
   TEST_ASSERT_EQUAL_INT(0, (int)focus_utf8_safe_cap(NULL, 10));      /* NULL guard */
}

/* 9. memory ownership — register/compose/free cycle 1000× and confirm the
 *    counter-based proxy returns to zero each iteration.  Smoke-test;
 *    valgrind locally is the real audit. */
static void test_memory_ownership_cycle(void) {
   const char *texts[] = { "cycle text" };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 1;
   s_fake[0].texts = texts;

   for (int i = 0; i < 1000; i++) {
      focus_compose_result_t result = { 0 };
      int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);
      TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
      TEST_ASSERT_EQUAL_INT(1, result.candidate_count);
      focus_result_free(&result);
      TEST_ASSERT_NULL(result.candidates);
      TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   }
}

/* 10. double-register same source_id — second register returns FAILURE,
 *     no overwrite. */
static void test_double_register_same_source_id_fails(void) {
   int rc1 = register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc1);

   /* Build a second adapter with the same source_id but distinct fn. */
   focus_source_adapter_t dup = { 0 };
   dup.source_id = "memory_fact";
   dup.source_type = FOCUS_SOURCE_INTERNAL;
   dup.requires_embedding = false;
   dup.query = FAKE_FUNCS[1];
   int rc2 = focus_register_source(&dup);
   TEST_ASSERT_EQUAL_INT(FAILURE, rc2);
}

/* 11. NA-score handling — candidate with FOCUS_SCORE_NA should contribute 0.0
 *     to that ranker term, NOT -1.0 * weight. */
static void test_na_score_contributes_zero(void) {
   const char *texts_a[] = { "with sem" };
   const char *texts_b[] = { "no sem" };
   float sem_a[] = { 1.0f };
   float sem_b[] = { FOCUS_SCORE_NA };
   float rec[] = { 0.0f };
   float imp[] = { 0.0f };

   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   register_fake("memory_entity", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 1;
   s_fake[0].texts = texts_a;
   s_fake[0].semantic_scores = sem_a;
   s_fake[0].recency_scores = rec;
   s_fake[0].importance_scores = imp;
   s_fake[1].candidate_count = 1;
   s_fake[1].texts = texts_b;
   s_fake[1].semantic_scores = sem_b;
   s_fake[1].recency_scores = rec;
   s_fake[1].importance_scores = imp;

   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 0, 5, &result));
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   /* "with sem" scores 1.0; "no sem" scores 0.0 (NA contributes zero, not
    * negative).  Order: with → no. */
   TEST_ASSERT_EQUAL_STRING("with sem", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("no sem", result.candidates[1].text);
   focus_result_free(&result);
}

/* 12. focus_unregister_all — subsequent compose yields zero with SUCCESS;
 *     subsequent register works. */
static void test_unregister_all_resets_registry(void) {
   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   focus_unregister_all();

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   focus_result_free(&result);

   /* Re-register after reset works. */
   focus_source_adapter_t a = { 0 };
   a.source_id = "memory_entity";
   a.query = FAKE_FUNCS[0];
   int rc2 = focus_register_source(&a);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc2);
}

/* 13. defensive cap — adapter ignores per_source_max_candidates and returns
 *     50; framework caps at the requested 10. */
static void test_framework_caps_over_eager_adapter(void) {
   static const char *texts[50];
   static char buf[50][16];
   for (int i = 0; i < 50; i++) {
      snprintf(buf[i], sizeof(buf[i]), "item%d", i);
      texts[i] = buf[i];
   }
   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].over_cap_count = 50;
   s_fake[0].texts = texts;

   g_config.memory.focus_injection.top_k = 64;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, /*per_source_max=*/10, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(10, result.candidate_count);
   focus_result_free(&result);
}

/* 14. registry-cap exhaustion — register MAX_FOCUS_SOURCES then one more. */
static void test_registry_cap_exhaustion(void) {
   for (int i = 0; i < MAX_FOCUS_SOURCES; i++) {
      static char source_ids[MAX_FOCUS_SOURCES][32];
      snprintf(source_ids[i], sizeof(source_ids[i]), "src_%d", i);
      int rc = register_fake(source_ids[i], FOCUS_SOURCE_INTERNAL, false);
      TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   }
   /* 17th must fail — cap is MAX_FOCUS_SOURCES = 16. */
   focus_source_adapter_t extra = { 0 };
   extra.source_id = "overflow";
   extra.query = FAKE_FUNCS[0];
   int rc = focus_register_source(&extra);
   TEST_ASSERT_EQUAL_INT(FAILURE, rc);

   /* Existing 16 still queryable — issue a compose and verify all are visited. */
   focus_compose_result_t result = { 0 };
   int rc2 = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc2);
   focus_result_free(&result);
   for (int i = 0; i < MAX_FOCUS_SOURCES; i++) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_fake[i].call_count,
                                    "every registered fake should be called");
   }
}

/* 15. empty-registry SUCCESS — no adapters registered. */
static void test_empty_registry_returns_success(void) {
   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   TEST_ASSERT_EQUAL_INT(0, result.rejection_count);
   TEST_ASSERT_NULL(result.candidates);
   focus_result_free(&result);
}

/* 16. adapter partial-failure cleanup — adapter mallocs array, populates
 *     2 entries, frees its own allocation, then returns FAILURE.  The
 *     framework MUST NOT attempt to free; framework cleanup happens only
 *     on SUCCESS.  We assert SUCCESS overall (the rest of the registry
 *     still runs) and zero candidates from this slot. */
static void test_adapter_partial_failure_cleans_up(void) {
   const char *texts[] = { "a", "b", "c" };
   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   s_fake[0].candidate_count = 3;
   s_fake[0].texts = texts;
   s_fake[0].partial_alloc_failure = true;

   /* Add a second healthy adapter so we can verify the rest of the
    * registry still runs after the failing adapter. */
   const char *texts2[] = { "ok" };
   register_fake("memory_entity", FOCUS_SOURCE_INTERNAL, false);
   s_fake[1].candidate_count = 1;
   s_fake[1].texts = texts2;

   focus_compose_result_t result = { 0 };
   int rc = focus_compose(1, false, NULL, NULL, 0, 0, 5, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("ok", result.candidates[0].text);
   focus_result_free(&result);
}

/* 17. include_private flag is propagated to adapters. */
static void test_include_private_flag_propagated(void) {
   register_fake("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, /*include_private=*/true, NULL, NULL, 0, 0, 5, &result));
   TEST_ASSERT_EQUAL_INT(1, s_fake[0].call_count);
   TEST_ASSERT_TRUE(s_fake[0].last_include_private);
   focus_result_free(&result);

   focus_compose_result_t result2 = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, /*include_private=*/false, NULL, NULL, 0, 0, 5,
                                                &result2));
   TEST_ASSERT_EQUAL_INT(2, s_fake[0].call_count);
   TEST_ASSERT_FALSE(s_fake[0].last_include_private);
   focus_result_free(&result2);
}

/* =============================================================================
 * Runner
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_register_and_iterate_all_adapters);
   RUN_TEST(test_cross_user_isolation);
   RUN_TEST(test_ranker_math);
   RUN_TEST(test_ranker_tiebreak_by_timestamp);
   RUN_TEST(test_min_score_threshold);
   RUN_TEST(test_top_k_truncation);
   RUN_TEST(test_filter_on_retrieval_drops_user_content);
   RUN_TEST(test_filter_on_retrieval_skips_internal);
   RUN_TEST(test_filter_on_retrieval_skips_external);
   RUN_TEST(test_requires_embedding_skipped_without_query);
   RUN_TEST(test_byte_budget_truncation);
   RUN_TEST(test_byte_budget_skips_and_packs);
   RUN_TEST(test_utf8_safe_cap);
   RUN_TEST(test_memory_ownership_cycle);
   RUN_TEST(test_double_register_same_source_id_fails);
   RUN_TEST(test_na_score_contributes_zero);
   RUN_TEST(test_unregister_all_resets_registry);
   RUN_TEST(test_framework_caps_over_eager_adapter);
   RUN_TEST(test_registry_cap_exhaustion);
   RUN_TEST(test_empty_registry_returns_success);
   RUN_TEST(test_adapter_partial_failure_cleans_up);
   RUN_TEST(test_include_private_flag_propagated);
   return UNITY_END();
}
