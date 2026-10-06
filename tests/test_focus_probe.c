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
 * Phase 1i.B unit tests — focus-injection probe (Components 4 + 5).
 *
 * Probe-style coverage for the dynamic-context-injection ranker, dedup,
 * and budget-truncation pipeline.  Each test case mirrors the synthetic-
 * case shape that the multi-model fix-rate probe (1i.C) consumes via
 * `benchmarks/focus_probe_cases.json`, but as deterministic fast C
 * assertions — no LLM, runs in ctest in milliseconds.
 *
 * Coverage (5 documented properties + budget force-keep + combo):
 *   1. recency        — newer item ranks above older when other dims tied
 *   2. importance     — higher importance ranks first when other dims tied
 *   3. source-weight  — per-source weights reorder items via the ranker
 *   4. shown-once     — an item a conversation shows, unchanged, is named, not re-sent
 *   5. changed        — the same item with new text is sent again, same handle
 *   6. budget (C5)    — over-budget pool: force-keep FIRST only (case 4)
 *   7. combo          — recency + importance simultaneously
 *
 * Architecture: the framework's static helpers cannot be extracted for
 * testing (`compute_score_breakdown`, `lookup_source_weight`, etc.) per
 * pre-dispatch invariant M3.  We exercise the full pipeline through the
 * public surface instead: a fake adapter (`fake_query`) emits a
 * programmable synthetic pool, and tests assert on the surfaced result.
 *
 * Properties 4 and 5 go through `build_focus_block()` and then the seam's
 * own choice (focus_incremental.h) against a history holding the earlier
 * turn's context: the real implementation + a real session_t fixture +
 * stubs for the embedding engine and the WebSocket broadcast — same link
 * shape test_prompt_builder uses, just with REAL focus_compose instead of
 * stubs so the ranker is exercised end-to-end.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/build_focus_block.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_incremental.h"
#include "core/focus/focus_source.h"
#include "core/focus/focus_source_internal.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_history_kind.h"
#include "unity.h"

/* The session a turn runs on: the prompt builder's argument (build_focus_block
 * reads the conversation's item handles from it). */
static session_t *s_dispatch;
static void set_dispatch(session_t *session) {
   s_dispatch = session;
}

/* =============================================================================
 * Fake adapter — emits a programmable synthetic pool.  Replaces real
 * memory / document / calendar adapters for deterministic probe-style
 * tests.  Shared across C4 (ranker/dedup) and C5 (budget) test cases per
 * pre-dispatch invariant M5.
 * ============================================================================= */

typedef struct {
   const char *source_id; /* "memory_fact", "calendar_event", etc. */
   focus_source_type_t source_type;
   const char *text;
   const char *item_id;
   time_t item_timestamp;
   float semantic_score;
   float recency_score;
   float importance_score;
} fake_seed_t;

#define FAKE_SEED_CAP 8

static struct {
   fake_seed_t seeds[FAKE_SEED_CAP];
   int seed_count;
   const char *adapter_source_id;
   focus_source_type_t adapter_source_type;
   bool requires_embedding;
   int call_count;
} s_fake;

static int fake_query(int user_id,
                      bool include_private,
                      const char *query_text,
                      const float *query_embedding,
                      size_t embed_dim,
                      time_t now,
                      int max_candidates,
                      focus_candidate_t **out_candidates,
                      int *out_count) {
   (void)user_id;
   (void)include_private;
   (void)query_text;
   (void)query_embedding;
   (void)embed_dim;
   (void)now;

   s_fake.call_count++;
   *out_candidates = NULL;
   *out_count = 0;

   /* Filter the seeds whose source_id matches THIS adapter — when two
    * adapters are registered they share the seed pool but differ on
    * source_id.  Uses pointer-equality on the static strings; tests
    * always pass the same const char* for both seed and adapter. */
   int match = 0;
   for (int i = 0; i < s_fake.seed_count; i++) {
      if (s_fake.seeds[i].source_id == s_fake.adapter_source_id) {
         match++;
      }
   }
   if (match == 0)
      return SUCCESS;

   const int emit = (match < max_candidates) ? match : max_candidates;
   focus_candidate_t *arr = calloc((size_t)emit, sizeof(*arr));
   if (arr == NULL)
      return FAILURE;

   bool warned = false;
   int j = 0;
   for (int i = 0; i < s_fake.seed_count && j < emit; i++) {
      const fake_seed_t *seed = &s_fake.seeds[i];
      if (seed->source_id != s_fake.adapter_source_id)
         continue;
      if (focus_candidate_init(&arr[j], seed->source_id, seed->source_type, seed->text,
                               seed->item_id, seed->item_timestamp, seed->semantic_score,
                               seed->recency_score, seed->importance_score, &warned) != SUCCESS) {
         focus_adapter_failure_cleanup(arr, j, out_candidates, out_count);
         return FAILURE;
      }
      j++;
   }

   *out_candidates = arr;
   *out_count = j;
   return SUCCESS;
}

static focus_source_adapter_t s_fake_adapter;

static void fake_register(const char *source_id,
                          focus_source_type_t source_type,
                          bool requires_embedding) {
   s_fake.adapter_source_id = source_id;
   s_fake.adapter_source_type = source_type;
   s_fake.requires_embedding = requires_embedding;
   memset(&s_fake_adapter, 0, sizeof(s_fake_adapter));
   s_fake_adapter.source_id = source_id;
   s_fake_adapter.source_type = source_type;
   s_fake_adapter.requires_embedding = requires_embedding;
   s_fake_adapter.query = fake_query;
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_register_source(&s_fake_adapter));
}

/* Second adapter slot — used by the source-weight test which needs two
 * registered adapters with different per-source weights. */
static focus_source_adapter_t s_fake_adapter_2;
static const char *s_fake_adapter_2_source_id = NULL;

static int fake_query_2(int user_id,
                        bool include_private,
                        const char *query_text,
                        const float *query_embedding,
                        size_t embed_dim,
                        time_t now,
                        int max_candidates,
                        focus_candidate_t **out_candidates,
                        int *out_count) {
   (void)user_id;
   (void)include_private;
   (void)query_text;
   (void)query_embedding;
   (void)embed_dim;
   (void)now;

   *out_candidates = NULL;
   *out_count = 0;

   int match = 0;
   for (int i = 0; i < s_fake.seed_count; i++) {
      if (s_fake.seeds[i].source_id == s_fake_adapter_2_source_id) {
         match++;
      }
   }
   if (match == 0)
      return SUCCESS;

   const int emit = (match < max_candidates) ? match : max_candidates;
   focus_candidate_t *arr = calloc((size_t)emit, sizeof(*arr));
   if (arr == NULL)
      return FAILURE;

   bool warned = false;
   int j = 0;
   for (int i = 0; i < s_fake.seed_count && j < emit; i++) {
      const fake_seed_t *seed = &s_fake.seeds[i];
      if (seed->source_id != s_fake_adapter_2_source_id)
         continue;
      if (focus_candidate_init(&arr[j], seed->source_id, seed->source_type, seed->text,
                               seed->item_id, seed->item_timestamp, seed->semantic_score,
                               seed->recency_score, seed->importance_score, &warned) != SUCCESS) {
         focus_adapter_failure_cleanup(arr, j, out_candidates, out_count);
         return FAILURE;
      }
      j++;
   }

   *out_candidates = arr;
   *out_count = j;
   return SUCCESS;
}

static void fake_register_second(const char *source_id, focus_source_type_t source_type) {
   s_fake_adapter_2_source_id = source_id;
   memset(&s_fake_adapter_2, 0, sizeof(s_fake_adapter_2));
   s_fake_adapter_2.source_id = source_id;
   s_fake_adapter_2.source_type = source_type;
   s_fake_adapter_2.requires_embedding = false;
   s_fake_adapter_2.query = fake_query_2;
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_register_source(&s_fake_adapter_2));
}

/* =============================================================================
 * Test fixture
 * ============================================================================= */

void setUp(void) {
   focus_unregister_all();
   memset(&s_fake, 0, sizeof(s_fake));
   memset(&g_config, 0, sizeof(g_config));

   /* Open ranker by default — individual tests narrow weights as needed.
    * weight_semantic = 1, others = 0 means recency/importance/source
    * differences are invisible unless the test sets them.  This isolates
    * each property to one test. */
   g_config.memory.focus_injection.enabled = true;
   g_config.memory.focus_injection.focus_budget_bytes = 16384;
   g_config.memory.focus_injection.top_k = 8;
   g_config.memory.focus_injection.min_score = 0.0f;
   g_config.memory.focus_injection.weight_semantic = 1.0f;
   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
   /* Source weights all 1.0 by default.  Source-weight test overrides. */
   g_config.memory.focus_injection.source_weights.memory_fact = 1.0f;
   g_config.memory.focus_injection.source_weights.memory_entity = 1.0f;
   g_config.memory.focus_injection.source_weights.memory_relation = 1.0f;
   g_config.memory.focus_injection.source_weights.memory_summary = 1.0f;
   g_config.memory.focus_injection.source_weights.document_chunk = 1.0f;
   g_config.memory.focus_injection.source_weights.calendar_event = 1.0f;
   g_config.memory.focus_injection.source_weights.recent_email = 1.0f;
   g_config.memory.focus_injection.source_weights.dawn_background = 1.0f;

   /* Properties 4 and 5 publish a session here; ranker tests leave it NULL. */
   set_dispatch(NULL);
}

void tearDown(void) {
   focus_unregister_all();
   set_dispatch(NULL);
}

static fake_seed_t *seed_add(const char *source_id,
                             focus_source_type_t source_type,
                             const char *text,
                             const char *item_id,
                             time_t ts,
                             float semantic,
                             float recency,
                             float importance) {
   TEST_ASSERT_LESS_THAN_INT_MESSAGE(FAKE_SEED_CAP, s_fake.seed_count,
                                     "seed pool overflow — bump FAKE_SEED_CAP");
   fake_seed_t *s = &s_fake.seeds[s_fake.seed_count++];
   s->source_id = source_id;
   s->source_type = source_type;
   s->text = text;
   s->item_id = item_id;
   s->item_timestamp = ts;
   s->semantic_score = semantic;
   s->recency_score = recency;
   s->importance_score = importance;
   return s;
}

/* =============================================================================
 * Property 1 — recency: newer item ranks above older when other dims tied
 * ============================================================================= */

static void test_recency_promotes_newer(void) {
   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, /*requires_embedding*/ false);
   /* Two candidates, identical except recency_score (0.9 vs 0.1). */
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "older fact", "fact:1", 1700000000,
            /*semantic*/ 0.5f, /*recency*/ 0.1f, /*importance*/ 0.5f);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "newer fact", "fact:2", 1700000200,
            /*semantic*/ 0.5f, /*recency*/ 0.9f, /*importance*/ 0.5f);

   /* Lift the recency weight so the recency_score difference dominates. */
   g_config.memory.focus_injection.weight_recency = 1.0f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user_id*/ 1, false, "anything", NULL, 0,
                                                /*now*/ 1700000300, /*per_source_max*/ 8, &result));
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("newer fact", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("older fact", result.candidates[1].text);
   focus_result_free(&result);
}

/* =============================================================================
 * Property 2 — importance: higher importance ranks first when other dims tied
 * ============================================================================= */

static void test_importance_promotes_higher(void) {
   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "low importance", "fact:lo", 1700000000, 0.5f,
            0.5f, /*importance*/ 0.1f);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "high importance", "fact:hi", 1700000000, 0.5f,
            0.5f, /*importance*/ 0.9f);

   g_config.memory.focus_injection.weight_importance = 1.0f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", NULL, 0, 1700000100, 8, &result));
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("high importance", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("low importance", result.candidates[1].text);
   focus_result_free(&result);
}

/* =============================================================================
 * Property 3 — source-weight: per-source weights reorder items via ranker
 * ============================================================================= */

static void test_source_weight_reorders(void) {
   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   fake_register_second("calendar_event", FOCUS_SOURCE_EXTERNAL);
   /* Both candidates score identically on every other dimension; only
    * source_weight should change the order. */
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "from fact", "fact:1", 1700000000, 0.5f, 0.5f,
            0.5f);
   seed_add("calendar_event", FOCUS_SOURCE_EXTERNAL, "from calendar", "cal:1", 1700000000, 0.5f,
            0.5f, 0.5f);

   /* Calendar's source weight beats memory_fact's. */
   g_config.memory.focus_injection.source_weights.memory_fact = 0.5f;
   g_config.memory.focus_injection.source_weights.calendar_event = 1.0f;
   g_config.memory.focus_injection.weight_source = 1.0f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", NULL, 0, 1700000100, 8, &result));
   TEST_ASSERT_EQUAL_INT(2, result.candidate_count);
   TEST_ASSERT_EQUAL_STRING("from calendar", result.candidates[0].text);
   TEST_ASSERT_EQUAL_STRING("from fact", result.candidates[1].text);
   focus_result_free(&result);
}

/* =============================================================================
 * Properties 4 + 5 — an item is sent once; a changed one again
 *
 * Each turn's items are built (build_focus_block), then chosen against a
 * history holding the earlier turns' contexts, as the seam does
 * (focus_incremental_scan / _select / _render).
 * ============================================================================= */

/* One turn: build its items, choose them against @p hist, append its framed
 * context there; returns the items part sent (heap, or NULL) and @p states[0]. */
static char *probe_turn(struct json_object *hist, int64_t turn_id, focus_item_state_t *state0) {
   composed_prompt_t cp = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 7, turn_id, "turn", &cp));
   TEST_ASSERT_TRUE(cp.n_focus_items > 0);
   focus_scan_t scan;
   TEST_ASSERT_EQUAL_INT(0, focus_incremental_scan(hist, NULL, NULL, 0, &scan));
   focus_selection_t sel;
   TEST_ASSERT_EQUAL_INT(0, focus_incremental_select(cp.focus_items, cp.n_focus_items, NULL, &scan,
                                                     &sel));
   char *items = focus_incremental_render(cp.focus_items, &sel, false);
   *state0 = sel.states[0];
   char frame[8192];
   snprintf(frame, sizeof(frame),
            "--- TURN CONTEXT ---\n[system_time] now\n%s--- END TURN CONTEXT ---\n",
            items ? items : "");
   struct json_object *parts = json_object_new_array();
   json_object_array_add(parts, llm_history_context_part(frame, MESSAGE_KIND_TURN_CONTEXT));
   struct json_object *q = json_object_new_object();
   json_object_object_add(q, "role", json_object_new_string("user"));
   json_object_array_add(parts, llm_history_context_part("question", MESSAGE_KIND_NONE));
   json_object_object_add(q, "content", parts);
   json_object_array_add(hist, q);
   focus_selection_free(&sel);
   focus_scan_free(&scan);
   composed_prompt_free(&cp);
   return items;
}

static void test_shown_item_is_not_sent_again(void) {
   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "stable text", "fact:42", 1700000000, 0.5f, 0.5f,
            0.5f);
   g_config.memory.focus_injection.weight_semantic = 1.0f;

   session_t s;
   memset(&s, 0, sizeof(s));
   s.session_id = 42;
   pthread_mutex_init(&s.history_mutex, NULL);
   set_dispatch(&s);
   struct json_object *hist = json_object_new_array();

   focus_item_state_t state;
   char *t1 = probe_turn(hist, 100, &state);
   TEST_ASSERT_EQUAL_INT(FOCUS_ITEM_NEW, state);
   TEST_ASSERT_NOT_NULL(strstr(t1, "stable text"));
   free(t1);

   /* Every later turn: still relevant, already shown, so named only. */
   for (int turn = 0; turn < 12; turn++) {
      char *t = probe_turn(hist, 101 + turn, &state);
      TEST_ASSERT_EQUAL_INT(FOCUS_ITEM_REFERENCED, state);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("[still relevant: M1]\n", t, "named, not sent again");
      free(t);
   }
   json_object_put(hist);
   set_dispatch(NULL);
   pthread_mutex_destroy(&s.history_mutex);
}

static void test_changed_item_is_sent_again(void) {
   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "the old text", "fact:99", 1700000000, 0.4f, 0.5f,
            0.5f);
   g_config.memory.focus_injection.weight_semantic = 1.0f;

   session_t s;
   memset(&s, 0, sizeof(s));
   s.session_id = 43;
   pthread_mutex_init(&s.history_mutex, NULL);
   set_dispatch(&s);
   struct json_object *hist = json_object_new_array();

   focus_item_state_t state;
   free(probe_turn(hist, 200, &state));
   TEST_ASSERT_EQUAL_INT(FOCUS_ITEM_NEW, state);

   /* The item's text changes (the fake pool is shared state). */
   s_fake.seeds[0].text = "the new text";
   char *t2 = probe_turn(hist, 201, &state);
   TEST_ASSERT_EQUAL_INT(FOCUS_ITEM_CHANGED, state);
   TEST_ASSERT_NOT_NULL(strstr(t2, "[M1 memory_fact"));
   TEST_ASSERT_NOT_NULL(strstr(t2, "the new text"));
   free(t2);
   json_object_put(hist);
   set_dispatch(NULL);
   pthread_mutex_destroy(&s.history_mutex);
}

/* =============================================================================
 * C5 — budget: force-keep first only (case 4 per pre-dispatch invariant H3)
 *
 * Build an over-budget pool with TWO max-sized candidates.  Assert ONLY
 * the first survives and the second drops cleanly (no partial budget
 * consumption, no broken state).
 * ============================================================================= */

static void test_budget_force_keep_first_only(void) {
   /* FOCUS_TEXT_MAX_BYTES = 4608, so the byte cost of a full-sized
    * candidate is ~4608.  Set focus_budget_bytes = 400 so even one
    * candidate is well over budget — force-keep triggers on the first,
    * the second drops cleanly. */
   g_config.memory.focus_injection.focus_budget_bytes = 400;
   g_config.memory.focus_injection.weight_semantic = 1.0f;

   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, false);

   /* Two ~max-sized texts.  Static buffer with distinct tail bytes so we
    * can identify which survived. */
   static char big1[FOCUS_TEXT_MAX_BYTES + 1];
   static char big2[FOCUS_TEXT_MAX_BYTES + 1];
   memset(big1, 'A', FOCUS_TEXT_MAX_BYTES);
   memset(big2, 'B', FOCUS_TEXT_MAX_BYTES);
   big1[FOCUS_TEXT_MAX_BYTES] = '\0';
   big2[FOCUS_TEXT_MAX_BYTES] = '\0';

   /* big1 has higher semantic_score → ranks first → force-keep target. */
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, big1, "fact:big1", 1700000000,
            /*semantic*/ 0.9f, 0.5f, 0.5f);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, big2, "fact:big2", 1700000000,
            /*semantic*/ 0.8f, 0.5f, 0.5f);

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", NULL, 0, 1700000100, 8, &result));
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, result.candidate_count,
                                 "C5: force-keep must surface ONLY the first over-budget item");
   /* The surviving candidate must be the higher-ranked one (big1). */
   TEST_ASSERT_NOT_NULL(result.candidates[0].text);
   TEST_ASSERT_EQUAL_INT_MESSAGE('A', result.candidates[0].text[0],
                                 "force-kept candidate must be the top-ranked (big1, all 'A')");
   focus_result_free(&result);
}

/* =============================================================================
 * Combo — recency + importance simultaneously (one optional case per M1)
 *
 * Three candidates: (a) older + low-importance, (b) older + high-
 * importance, (c) newer + low-importance.  With both weights nonzero,
 * (b) should beat (a) on importance and (c) should beat (a) on recency,
 * but with our weight choice (b) wins overall.
 * ============================================================================= */

static void test_combo_recency_plus_importance(void) {
   fake_register("memory_fact", FOCUS_SOURCE_INTERNAL, false);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "a:older,low-imp", "fact:a", 1700000000, 0.5f,
            0.1f, 0.1f);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "b:older,high-imp", "fact:b", 1700000000, 0.5f,
            0.1f, 0.9f);
   seed_add("memory_fact", FOCUS_SOURCE_INTERNAL, "c:newer,low-imp", "fact:c", 1700000200, 0.5f,
            0.9f, 0.1f);

   /* weight_importance dominates so (b) wins overall; weight_recency
    * still nonzero so (c) > (a). */
   g_config.memory.focus_injection.weight_importance = 1.0f;
   g_config.memory.focus_injection.weight_recency = 0.5f;

   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", NULL, 0, 1700000300, 8, &result));
   TEST_ASSERT_EQUAL_INT(3, result.candidate_count);
   /* (b) ranks first (importance 0.9 dominates).  (c) > (a) on recency.  */
   TEST_ASSERT_TRUE_MESSAGE(strstr(result.candidates[0].text, "b:") != NULL,
                            "high-importance item must rank first overall");
   TEST_ASSERT_TRUE_MESSAGE(strstr(result.candidates[1].text, "c:") != NULL,
                            "newer-but-low-importance must outrank older-low-importance");
   TEST_ASSERT_TRUE_MESSAGE(strstr(result.candidates[2].text, "a:") != NULL,
                            "older + low-importance ranks last");
   focus_result_free(&result);
}

/* =============================================================================
 * Runner
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_recency_promotes_newer);
   RUN_TEST(test_importance_promotes_higher);
   RUN_TEST(test_source_weight_reorders);
   RUN_TEST(test_shown_item_is_not_sent_again);
   RUN_TEST(test_changed_item_is_sent_again);
   RUN_TEST(test_budget_force_keep_first_only);
   RUN_TEST(test_combo_recency_plus_importance);
   return UNITY_END();
}
