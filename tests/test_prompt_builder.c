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
 * Unit tests for build_focus_block (the turn's retrieved items) and its
 * focus dedup state.
 *
 * Coverage scope:
 *   - build_focus_block — drives focus_compose + memory_embeddings
 *     via programmable stubs in test_prompt_builder_stub.c.
 *
 * Out of scope (covered by the manual smoke gate, not unit tests):
 *   - session_dispatch_user_turn end-to-end (needs full session_manager
 *     runtime: auth_db, conv_db, satellite_db, ws lifecycle).
 *   - Cross-user / cross-turn integration through dawn_build_prompt
 *     (needs webui_server.c link → effectively the full daemon).
 *   - Performance benchmark (needs real DB and embedding model).
 *
 * The skipped tests are noted in the report; the manual smoke gate
 * step 11 + the existing 48 CI tests' continued passage are the
 * complementary gates.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/prompt_parts.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "unity.h"
#include "webui/build_focus_block.h"

/* The session a turn runs on: the prompt builder's argument (build_focus_block
 * reads its focus dedup state and item handles from it). */
static session_t *s_dispatch;
static void set_dispatch(session_t *session) {
   s_dispatch = session;
}

/* Stub helpers (defined in test_prompt_builder_stub.c). */
typedef struct {
   const char *source_id;
   const char *text;
   const char *item_id;
} pb_seed_candidate_t;

typedef struct {
   bool fail;
   pb_seed_candidate_t seeded[8];
   int seeded_count;
   int rejection_count;
   int call_count;
   int last_user_id;
   bool last_had_query_embedding;
   size_t last_embed_dim;
   char last_query_text[256];
} pb_focus_compose_mock_t;

typedef struct {
   bool available;
   int dims;
   bool fail_embed;
   int embed_call_count;
} pb_embed_mock_t;

void pb_focus_reset(void);
pb_focus_compose_mock_t *pb_focus_state(void);
void pb_embed_reset(void);
pb_embed_mock_t *pb_embed_state(void);
void pb_focus_set_seed_score(int idx, float score);
void pb_focus_set_seed_final_score(int idx, float final_score);
void pb_focus_clear_seed_scores(void);
void pb_session_init(session_t *s, uint32_t session_id);
void pb_session_destroy(session_t *s);

/* Phase 1g-i broadcast stub state (defined in test_prompt_builder_stub.c). */
typedef struct {
   int call_count;
   int last_user_id;
   int64_t last_conv_id;
   int64_t last_turn_id;
   int last_candidate_count;
   float last_first_final_score;
   bool last_had_breakdowns;
} pb_broadcast_mock_t;

void pb_broadcast_reset(void);
pb_broadcast_mock_t *pb_broadcast_state(void);

void setUp(void) {
   pb_focus_reset();
   pb_embed_reset();
   pb_broadcast_reset();
   /* Default config: feature ON.  Individual tests flip enabled=false
    * where needed to verify the gate.
    *
    * 1f knobs: recent_window_turns and score_uplift_factor get the
    * production defaults so the dedup tests don't drift if defaults
    * ever change in config_defaults.c.  min_score=0.0 so the
    * window_passed branch isn't blocked by a default-zero min_score
    * leftover from the earlier memset(0). */
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.focus_injection.enabled = true;
   g_config.memory.focus_injection.top_k = 8;
   g_config.memory.focus_injection.min_score = 0.0f;
   g_config.memory.focus_injection.dedup.recent_window_turns = 8;
   g_config.memory.focus_injection.dedup.score_uplift_factor = 1.5f;
   /* Make sure no leftover dispatch session leaks across tests; pre-1f
    * tests assume dedup is off. */
   set_dispatch(NULL);
}

void tearDown(void) {
}

/* ============================================================================
 * Composer tests — two-segment shape (stable_prefix + volatile_block)
 * ============================================================================ */


/* ============================================================================
 * Drift-detection hash
 * ============================================================================ */


/* ============================================================================
 * build_focus_block — feature gate (8-10)
 * ============================================================================ */

static void test_focus_disabled_short_circuits(void) {
   g_config.memory.focus_injection.enabled = false;
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "should not appear",
                                                        "fact:1" };

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "anything", &block));
   TEST_ASSERT_NULL(block);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, pb_focus_state()->call_count,
                                 "disabled feature must NOT invoke focus_compose");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, pb_embed_state()->embed_call_count,
                                 "disabled feature must NOT compute embeddings");
}

static void test_focus_unauthenticated_short_circuits(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "any", "fact:1" };

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 0, 0, 0, "x", &block));
   TEST_ASSERT_NULL(block);
   TEST_ASSERT_EQUAL_INT(0, pb_focus_state()->call_count);
   TEST_ASSERT_EQUAL_INT(0, pb_embed_state()->embed_call_count);
}

static void test_focus_empty_turn_text_short_circuits(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "", &block));
   TEST_ASSERT_NULL(block);
   TEST_ASSERT_EQUAL_INT(0, pb_focus_state()->call_count);

   block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, NULL, &block));
   TEST_ASSERT_NULL(block);
   TEST_ASSERT_EQUAL_INT(0, pb_focus_state()->call_count);
}

/* ============================================================================
 * build_focus_block — happy paths (11-15)
 * ============================================================================ */

static void test_focus_renders_candidate_lines(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 2;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "Pepper birthday March 14",
                                                        "fact:42" };
   pb_focus_state()->seeded[1] = (pb_seed_candidate_t){ "calendar_event",
                                                        "[2026-05-09 14:00] standup",
                                                        "calendar_occ:7" };

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 99, 0, 0, "what's coming up?", &block));
   TEST_ASSERT_NOT_NULL(block);
   TEST_ASSERT_NOT_NULL(strstr(block, "[memory_fact] Pepper birthday March 14"));
   TEST_ASSERT_NOT_NULL(strstr(block, "[calendar_event] [2026-05-09 14:00] standup"));
   /* Block is bare (no framing — composer adds those). */
   TEST_ASSERT_NULL_MESSAGE(strstr(block, "TURN CONTEXT"),
                            "build_focus_block must NOT include framing markers");
   free(block);
}

static void test_focus_passes_user_id_and_text(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 123, 0, 0, "hello world", &block));
   TEST_ASSERT_EQUAL_INT(123, pb_focus_state()->last_user_id);
   TEST_ASSERT_EQUAL_STRING("hello world", pb_focus_state()->last_query_text);
   free(block);
}

static void test_focus_zero_candidates_returns_null_block(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 0;

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 1, 0, 0, "irrelevant query", &block));
   TEST_ASSERT_NULL_MESSAGE(block, "zero candidates → NULL block (composer omits section)");
   /* focus_compose was still called — gate is at the top of build_focus_block,
    * empty result is the SUCCESS path. */
   TEST_ASSERT_EQUAL_INT(1, pb_focus_state()->call_count);
}

static void test_focus_embedding_unavailable_passes_null(void) {
   pb_embed_state()->available = false; /* engine not initialized */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "calendar_event", "no embed needed",
                                                        "calendar_occ:1" };

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "hi", &block));
   /* focus_compose still consulted — calendar adapter has
    * requires_embedding=false in 1d production; vector-only adapters
    * skip themselves when query_embedding=NULL. */
   TEST_ASSERT_EQUAL_INT(1, pb_focus_state()->call_count);
   TEST_ASSERT_FALSE(pb_focus_state()->last_had_query_embedding);
   TEST_ASSERT_EQUAL_size_t(0, pb_focus_state()->last_embed_dim);
   TEST_ASSERT_NOT_NULL(block);
   free(block);
}

static void test_focus_embedding_failure_passes_null(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_embed_state()->fail_embed = true;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "calendar_event", "still surfaces",
                                                        "calendar_occ:1" };

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "hi", &block));
   TEST_ASSERT_EQUAL_INT(1, pb_embed_state()->embed_call_count);
   TEST_ASSERT_EQUAL_INT(1, pb_focus_state()->call_count);
   TEST_ASSERT_FALSE_MESSAGE(pb_focus_state()->last_had_query_embedding,
                             "embed failure → NULL embedding to focus_compose");
   TEST_ASSERT_NOT_NULL(block);
   free(block);
}

/* ============================================================================
 * build_focus_block — failure modes (16-17)
 * ============================================================================ */

static void test_focus_compose_failure_returns_null_block(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->fail = true;

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(FAILURE, build_focus_block(s_dispatch, 1, 0, 0, "x", &block));
   TEST_ASSERT_NULL_MESSAGE(block, "focus_compose FAILURE must yield NULL block (composer omits)");
}

static void test_focus_filter_rejection_logged_no_section_when_zero_survivors(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 0;
   pb_focus_state()->rejection_count = 3; /* filter blocked 3 candidates */

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "x", &block));
   TEST_ASSERT_NULL_MESSAGE(block, "all rejected → 0 survivors → NULL block (1g chip later)");
}

/* ============================================================================
 * Composer + build_focus_block — cross-turn isolation (18-20)
 * ============================================================================ */

static void test_cross_turn_no_content_leak(void) {
   /* Simulate two consecutive turns: turn-1 surfaces "Pepper", turn-2
    * surfaces nothing.  Turn-2's block must NOT contain Pepper content
    * — this is the cross-turn isolation invariant in build_focus_block
    * itself (composer side is verified by test_composer_*). */
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* Turn 1 */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "Pepper birthday",
                                                        "fact:1" };
   char *block1 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 1, 0, 0, "tell me about Pepper", &block1));
   TEST_ASSERT_NOT_NULL(block1);
   TEST_ASSERT_NOT_NULL(strstr(block1, "Pepper"));
   free(block1);

   /* Turn 2 — empty result.  block2 must be NULL, NOT block1's stale
    * content reused. */
   pb_focus_state()->seeded_count = 0;
   char *block2 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 1, 0, 0, "what time is it?", &block2));
   TEST_ASSERT_NULL_MESSAGE(block2, "turn-2 with zero results must NOT inherit turn-1 content");
}

static void test_failed_then_successful_refresh_no_carry(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* Turn 1: focus_compose fails. */
   pb_focus_state()->fail = true;
   char *block1 = NULL;
   TEST_ASSERT_EQUAL_INT(FAILURE, build_focus_block(s_dispatch, 1, 0, 0, "fail-turn", &block1));
   TEST_ASSERT_NULL(block1);

   /* Turn 2: focus_compose succeeds; block reflects new turn only. */
   pb_focus_reset();
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "fresh content", "fact:9" };
   char *block2 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "ok-turn", &block2));
   TEST_ASSERT_NOT_NULL(block2);
   TEST_ASSERT_NOT_NULL(strstr(block2, "fresh content"));
   TEST_ASSERT_NULL_MESSAGE(strstr(block2, "fail-turn"),
                            "no prior-turn query text content in new block");
   free(block2);
}

static void test_disabled_after_enabled_returns_null(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };

   char *block1 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "first", &block1));
   TEST_ASSERT_NOT_NULL(block1);
   free(block1);

   /* Hot-flip to disabled; subsequent refresh must short-circuit. */
   g_config.memory.focus_injection.enabled = false;
   pb_focus_reset();
   pb_embed_reset();
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   char *block2 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "second", &block2));
   TEST_ASSERT_NULL(block2);
   TEST_ASSERT_EQUAL_INT(0, pb_focus_state()->call_count);
}

/* ============================================================================
 * Memory ownership + composer integration (21-23)
 * ============================================================================ */

static void test_memory_cycle_1000x(void) {
   /* 1000× build / compose / free.  ASan picks up any single-iteration
    * leak; the loop both stresses the cleanup path and serves as a
    * regression for the ownership contract. */
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 2;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };
   pb_focus_state()->seeded[1] = (pb_seed_candidate_t){ "calendar_event", "beta",
                                                        "calendar_occ:1" };

   for (int i = 0; i < 1000; i++) {
      char *focus = NULL;
      TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "query", &focus));
      /* A turn's prompt, built and freed: its sections joined, the focus
       * body as its context. */
      composed_prompt_t cp = { .volatile_block = focus };
      TEST_ASSERT_EQUAL_INT(0, prompt_sections_add(&cp, "persona", "your persona", "BASE"));
      cp.stable_prefix = prompt_sections_join(&cp);
      composed_prompt_free(&cp);
   }
}


static void test_focus_block_produces_one_line_per_candidate(void) {
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 4;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "a", "1" };
   pb_focus_state()->seeded[1] = (pb_seed_candidate_t){ "memory_entity", "b", "2" };
   pb_focus_state()->seeded[2] = (pb_seed_candidate_t){ "memory_summary", "c", "3" };
   pb_focus_state()->seeded[3] = (pb_seed_candidate_t){ "calendar_event", "d", "4" };

   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "test", &block));
   TEST_ASSERT_NOT_NULL(block);
   /* Count newlines — should equal candidate count. */
   int newlines = 0;
   for (const char *p = block; *p; p++)
      if (*p == '\n')
         newlines++;
   TEST_ASSERT_EQUAL_INT_MESSAGE(4, newlines,
                                 "build_focus_block must emit exactly one \\n-terminated line per "
                                 "surviving candidate");
   free(block);
}

/* ============================================================================
 * Phase 1f — per-turn focus-injection dedup
 *
 * These tests drive build_focus_block with a session_t pinned via
 * set_dispatch().  When the dispatch session is NULL
 * (the default for Phase 1e tests above), build_focus_block skips
 * dedup — that's why the pre-1f tests stay correct without changes.
 *
 * Strict-comparison contract: turns_since > recent_window_turns admits;
 * the design doc rev 3 §"Phase 1 — Per-Turn Focus" §"Dedup" formula.
 * ============================================================================ */

/* Helper: drive one turn and return whether the candidate text appeared
 * in the focus block.  Frees the block. */
static bool dedup_run_turn_and_check(int user_id,
                                     const char *turn_text,
                                     const char *expected_substr) {
   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, user_id, 0, 0, turn_text, &block));
   const bool found = (block != NULL) && (strstr(block, expected_substr) != NULL);
   free(block);
   return found;
}

static void test_dedup_baseline_no_dispatch_session(void) {
   /* No dispatch session published → dedup skipped, behavior matches
    * pre-1f.  Same candidate twice both surface. */
   set_dispatch(NULL);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };

   TEST_ASSERT_TRUE(dedup_run_turn_and_check(1, "first turn", "alpha"));
   /* Reset focus_compose's last-call state but keep the same seeded candidate. */
   pb_focus_state()->call_count = 0;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };
   TEST_ASSERT_TRUE_MESSAGE(dedup_run_turn_and_check(1, "second turn", "alpha"),
                            "dispatch session NULL → no dedup → repeat surfaces");
}

static void test_dedup_consecutive_turn_suppresses(void) {
   session_t s;
   pb_session_init(&s, 42);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };

   /* Turn 1 — admit. */
   TEST_ASSERT_TRUE(dedup_run_turn_and_check(1, "turn one", "alpha"));

   /* Turn 2 — same item, default window=8 → suppress. */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };
   char *block = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "turn two", &block));
   TEST_ASSERT_NULL_MESSAGE(block, "dedup must suppress repeat at turn 2 with default window=8");

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_strict_window_decay(void) {
   /* recent_window_turns=2: candidate at turn 1 → suppressed at turns 2
    * and 3 (turns_since=1, NOT > 2; turns_since=2, NOT > 2) → admitted
    * at turn 4 (turns_since=3, > 2). */
   session_t s;
   pb_session_init(&s, 1);
   set_dispatch(&s);
   g_config.memory.focus_injection.dedup.recent_window_turns = 2;
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };

   /* Turn 1: admit. */
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t1", &b));
   TEST_ASSERT_NOT_NULL(b);
   TEST_ASSERT_NOT_NULL(strstr(b, "X"));
   free(b);

   /* Turn 2: suppress (turns_since=1, NOT > 2). */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t2", &b));
   TEST_ASSERT_NULL_MESSAGE(b, "turn 2: turns_since=1 not > 2 → suppress");

   /* Turn 3: still suppress (turns_since=2, NOT > 2). */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t3", &b));
   TEST_ASSERT_NULL_MESSAGE(b, "turn 3: turns_since=2 not > 2 → suppress (strict gt)");

   /* Turn 4: admit (turns_since=3, > 2). */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t4", &b));
   TEST_ASSERT_NOT_NULL_MESSAGE(b, "turn 4: turns_since=3 > 2 → admit");
   free(b);

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_score_uplift_below_threshold_suppresses(void) {
   /* Turn 1 score 0.5; turn 2 score 0.7 — uplift threshold = 0.5 * 1.5 = 0.75,
    * 0.7 < 0.75 → suppress. */
   session_t s;
   pb_session_init(&s, 2);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "Y", "fact:Y" };
   pb_focus_set_seed_score(0, 0.5f);
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t1", &b));
   TEST_ASSERT_NOT_NULL(b);
   free(b);

   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "Y", "fact:Y" };
   pb_focus_set_seed_score(0, 0.7f);
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t2", &b));
   TEST_ASSERT_NULL_MESSAGE(b, "score 0.7 < 0.5 * 1.5 (=0.75) → suppress");

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_score_uplift_meets_threshold_admits(void) {
   /* Turn 1 score 0.5; turn 2 score 0.8 — 0.8 >= 0.75 → admit. */
   session_t s;
   pb_session_init(&s, 3);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "Z", "fact:Z" };
   pb_focus_set_seed_score(0, 0.5f);
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t1", &b));
   free(b);

   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "Z", "fact:Z" };
   pb_focus_set_seed_score(0, 0.8f);
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t2", &b));
   TEST_ASSERT_NOT_NULL_MESSAGE(b, "score 0.8 >= 0.5 * 1.5 → admit via uplift");
   TEST_ASSERT_NOT_NULL(strstr(b, "Z"));
   free(b);

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_window_zero_disables_time_dedup(void) {
   /* recent_window_turns=0: candidate at turn 1 → admitted at turn 2
    * (turns_since=1, > 0).  This is "time-based dedup effectively
    * disabled" — score-uplift becomes the only suppressor. */
   session_t s;
   pb_session_init(&s, 4);
   set_dispatch(&s);
   g_config.memory.focus_injection.dedup.recent_window_turns = 0;
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "W", "fact:W" };
   pb_focus_set_seed_score(0, 0.5f);
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t1", &b));
   free(b);

   /* Turn 2: turns_since=1, > 0 → admits via window path even though
    * score didn't change. */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "W", "fact:W" };
   pb_focus_set_seed_score(0, 0.5f);
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t2", &b));
   TEST_ASSERT_NOT_NULL_MESSAGE(b, "window=0: turn 2 admits via turns_since=1 > 0");
   free(b);

   set_dispatch(NULL);
   pb_session_destroy(&s);
}


static void test_dedup_session_start_does_not_record(void) {
   /* The SESSION_START path passes user_turn_text=NULL into build_focus_block,
    * which short-circuits at gate 2 — no focus_compose call, no record.
    * The dedup set must remain empty. */
   session_t s;
   pb_session_init(&s, 6);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* SESSION_START: NULL turn text. */
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, NULL, &b));
   TEST_ASSERT_NULL(b);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.injected_set.count,
                                 "SESSION_START must not record any candidate");

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_cross_user_isolation(void) {
   /* Two user_ids share one session in this stub setup; that's the wrong
    * shape for production but the dedup state is still per-session_t.
    * Verify by giving each "user" its own session_t — repeat candidate
    * across the two sessions both surface. */
   session_t s_a, s_b;
   pb_session_init(&s_a, 100);
   pb_session_init(&s_b, 200);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* user_a, session A */
   set_dispatch(&s_a);
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "shared", "fact:s" };
   char *ba = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "qa", &ba));
   TEST_ASSERT_NOT_NULL(ba);
   free(ba);

   /* user_b, session B — same candidate, fresh dedup state */
   set_dispatch(&s_b);
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "shared", "fact:s" };
   char *bb = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 2, 0, 0, "qb", &bb));
   TEST_ASSERT_NOT_NULL_MESSAGE(bb, "different session → independent injected_set → admit");
   TEST_ASSERT_NOT_NULL(strstr(bb, "shared"));
   free(bb);

   set_dispatch(NULL);
   pb_session_destroy(&s_a);
   pb_session_destroy(&s_b);
}

static void test_dedup_cross_session_same_user_isolation(void) {
   /* user_a's session_1 injects X; user_a's session_2 sees X fresh.
    * Verifies that two concurrent sessions for the same user have
    * INDEPENDENT injected_sets (per architecture review HIGH Q10). */
   session_t s1, s2;
   pb_session_init(&s1, 300);
   pb_session_init(&s2, 301);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   set_dispatch(&s1);
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   char *b1 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 7, 0, 0, "s1", &b1));
   TEST_ASSERT_NOT_NULL(b1);
   free(b1);

   set_dispatch(&s2);
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   char *b2 = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 7, 0, 0, "s2", &b2));
   TEST_ASSERT_NOT_NULL_MESSAGE(b2, "same user, different session → independent dedup → admit");
   free(b2);

   set_dispatch(NULL);
   pb_session_destroy(&s1);
   pb_session_destroy(&s2);
}

static void test_dedup_lru_cap_evicts_oldest(void) {
   /* Inject 257 unique candidates one per turn.  Set caps at 256.
    * The oldest by last_injected_turn (turn 1) must have been evicted
    * to make room for turn 257. */
   session_t s;
   pb_session_init(&s, 7);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   /* Disable time-window suppression so every candidate admits. */
   g_config.memory.focus_injection.dedup.recent_window_turns = 0;

   for (int i = 0; i < MAX_INJECTED_SET_SIZE + 1; i++) {
      pb_focus_state()->seeded_count = 1;
      char text[64], item_id[64];
      snprintf(text, sizeof(text), "txt_%d", i);
      snprintf(item_id, sizeof(item_id), "id_%d", i);
      pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", text, item_id };
      char *b = NULL;
      TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "loop", &b));
      free(b);
   }

   TEST_ASSERT_EQUAL_INT_MESSAGE(MAX_INJECTED_SET_SIZE, s.injected_set.count,
                                 "set must cap at MAX_INJECTED_SET_SIZE after overflow");

   /* The very first item (id_0) was the oldest; LRU should have evicted
    * it.  Newest item (id_256) must be present. */
   bool has_id0 = false;
   bool has_id256 = false;
   for (int i = 0; i < s.injected_set.count; i++) {
      if (strcmp(s.injected_set.entries[i].item_id, "id_0") == 0)
         has_id0 = true;
      if (strcmp(s.injected_set.entries[i].item_id, "id_256") == 0)
         has_id256 = true;
   }
   TEST_ASSERT_FALSE_MESSAGE(has_id0, "LRU eviction must drop oldest (id_0)");
   TEST_ASSERT_TRUE_MESSAGE(has_id256, "newest entry (id_256) must be present");

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_lru_correctness_with_reinjects(void) {
   /* 256 items at turn=N, then items 257-260 at turn=N+1.  The 4 oldest
    * items (those with original turn=N who weren't re-touched) must be
    * evicted, NOT the items with turn=N+1.
    *
    * We use _locked APIs directly here because driving 260 distinct
    * focus_compose calls with controlled scoring is tedious. */
   session_t s;
   pb_session_init(&s, 8);
   pthread_mutex_lock(&s.history_mutex);
   /* Pretend we're at turn=10 (matches what advance_turn would have produced). */
   s.injected_set.turn_counter = 10;
   for (int i = 0; i < MAX_INJECTED_SET_SIZE; i++) {
      char src[8] = "memfact";
      char iid[16];
      snprintf(iid, sizeof(iid), "id_%d", i);
      session_injected_set_record_locked(&s, src, iid, 0.5f);
   }
   /* Advance to turn=11; record 4 new items.  These should evict the
    * 4 entries whose last_injected_turn=10 (any 4 — implementation
    * picks oldest by linear scan). */
   s.injected_set.turn_counter = 11;
   for (int i = 0; i < 4; i++) {
      char src[8] = "memfact";
      char iid[16];
      snprintf(iid, sizeof(iid), "new_%d", i);
      session_injected_set_record_locked(&s, src, iid, 0.5f);
   }

   /* Set cap holds. */
   TEST_ASSERT_EQUAL_INT(MAX_INJECTED_SET_SIZE, s.injected_set.count);

   /* All 4 new entries present. */
   for (int i = 0; i < 4; i++) {
      char iid[16];
      snprintf(iid, sizeof(iid), "new_%d", i);
      injected_set_entry_t e;
      const int rc = session_injected_set_lookup_locked(&s, "memfact", iid, &e);
      TEST_ASSERT_EQUAL_INT_MESSAGE(SUCCESS, rc, "all 4 new entries must be retained");
      TEST_ASSERT_EQUAL_INT(11, e.last_injected_turn);
   }

   /* Exactly 4 of the original entries were evicted (count remained
    * MAX_INJECTED_SET_SIZE while we added 4 new ones). */
   int original_remaining = 0;
   for (int j = 0; j < s.injected_set.count; j++) {
      if (strncmp(s.injected_set.entries[j].item_id, "id_", 3) == 0)
         original_remaining++;
   }
   TEST_ASSERT_EQUAL_INT_MESSAGE(MAX_INJECTED_SET_SIZE - 4, original_remaining,
                                 "exactly 4 original entries must have been evicted");

   pthread_mutex_unlock(&s.history_mutex);
   pb_session_destroy(&s);
}

static void test_dedup_all_suppressed_warn_once_per_session(void) {
   /* Force scenario: seed dedup state with 4 entries at turn 1 with
    * very high scores; then refresh with same items at low scores.
    * Window=8 → time-based suppress; uplift fails (low/high < 1.5).
    * All 4 candidates get suppressed → all_suppressed_logged_once
    * flips to true on the first such turn.  Repeat the scenario in
    * the same session; flag stays true (warning does not re-fire). */
   session_t s;
   pb_session_init(&s, 9);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* Pre-seed entries at turn=1 with score=0.9. */
   pthread_mutex_lock(&s.history_mutex);
   s.injected_set.turn_counter = 1;
   for (int i = 0; i < 4; i++) {
      char iid[16];
      snprintf(iid, sizeof(iid), "f_%d", i);
      session_injected_set_record_locked(&s, "memory_fact", iid, 0.9f);
   }
   pthread_mutex_unlock(&s.history_mutex);

   /* Drive build_focus_block with the same 4 items at low score (0.1)
    * — uplift threshold = 0.9 * 1.5 = 1.35 → no uplift admit; window=8
    * → turn 2 turns_since=1 not > 8 → suppress all four. */
   pb_focus_state()->seeded_count = 4;
   for (int i = 0; i < 4; i++) {
      char iid_buf[16];
      static char iid_storage[4][16];
      static char text_storage[4][16];
      snprintf(iid_buf, sizeof(iid_buf), "f_%d", i);
      strncpy(iid_storage[i], iid_buf, sizeof(iid_storage[i]) - 1);
      iid_storage[i][sizeof(iid_storage[i]) - 1] = '\0';
      snprintf(text_storage[i], sizeof(text_storage[i]), "txt_%d", i);
      pb_focus_state()->seeded[i].source_id = "memory_fact";
      pb_focus_state()->seeded[i].text = text_storage[i];
      pb_focus_state()->seeded[i].item_id = iid_storage[i];
      pb_focus_set_seed_score(i, 0.1f);
   }

   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "trigger", &b));
   TEST_ASSERT_NULL_MESSAGE(b, "all 4 candidates suppressed → block NULL");
   TEST_ASSERT_TRUE_MESSAGE(s.injected_set.all_suppressed_logged_once,
                            "all-suppressed gate must flip to true on first such turn");

   /* Repeat — flag must remain true; warning gate prevents re-fire. */
   const bool flag_before = s.injected_set.all_suppressed_logged_once;
   pb_focus_state()->seeded_count = 4; /* re-seed */
   for (int i = 0; i < 4; i++) {
      static char iid_storage2[4][16];
      static char text_storage2[4][16];
      snprintf(iid_storage2[i], sizeof(iid_storage2[i]), "f_%d", i);
      snprintf(text_storage2[i], sizeof(text_storage2[i]), "txt_%d", i);
      pb_focus_state()->seeded[i].source_id = "memory_fact";
      pb_focus_state()->seeded[i].text = text_storage2[i];
      pb_focus_state()->seeded[i].item_id = iid_storage2[i];
      pb_focus_set_seed_score(i, 0.1f);
   }
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "trigger2", &b));
   TEST_ASSERT_NULL(b);
   TEST_ASSERT_TRUE_MESSAGE(s.injected_set.all_suppressed_logged_once == flag_before,
                            "all-suppressed gate must NOT re-fire (stays true)");

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

/* ----- Concurrency stress -------------------------------------------------
 *
 * Spawn N=4 PER_TURN-style threads + M=2 SESSION_START-style threads, all
 * pounding the SAME session 100 iterations each.  Run under TSan/ASan
 * (separate build flags) to validate history_mutex serializes correctly.
 *
 * The threads exercise the load-bearing primitives directly rather than
 * driving build_focus_block end-to-end — both the per-thread g_config and
 * the seed-score globals would race in the latter.
 * -------------------------------------------------------------------------- */

typedef struct {
   session_t *session;
   int iterations;
   int thread_idx;
} concurrent_args_t;

static void *concurrent_per_turn_worker(void *arg) {
   concurrent_args_t *a = (concurrent_args_t *)arg;
   for (int i = 0; i < a->iterations; i++) {
      pthread_mutex_lock(&a->session->history_mutex);
      session_injected_set_advance_turn_locked(a->session);
      char iid[32];
      snprintf(iid, sizeof(iid), "t%d_i%d", a->thread_idx, i);
      session_injected_set_record_locked(a->session, "memory_fact", iid, 0.5f);
      injected_set_entry_t out;
      session_injected_set_lookup_locked(a->session, "memory_fact", iid, &out);
      pthread_mutex_unlock(&a->session->history_mutex);
   }
   return NULL;
}

static void *concurrent_session_start_worker(void *arg) {
   concurrent_args_t *a = (concurrent_args_t *)arg;
   /* A new context, as session_new_context_locked resets it: under the lock. */
   for (int i = 0; i < a->iterations; i++) {
      pthread_mutex_lock(&a->session->history_mutex);
      memset(&a->session->injected_set, 0, sizeof(a->session->injected_set));
      pthread_mutex_unlock(&a->session->history_mutex);
   }
   return NULL;
}

static void test_dedup_concurrent_stress(void) {
   session_t s;
   pb_session_init(&s, 99);

   const int per_turn_threads = 4;
   const int session_start_threads = 2;
   const int iters = 100;

   pthread_t pt[per_turn_threads];
   pthread_t st[session_start_threads];
   concurrent_args_t pt_args[per_turn_threads];
   concurrent_args_t st_args[session_start_threads];

   for (int i = 0; i < per_turn_threads; i++) {
      pt_args[i] = (concurrent_args_t){ .session = &s, .iterations = iters, .thread_idx = i };
      pthread_create(&pt[i], NULL, concurrent_per_turn_worker, &pt_args[i]);
   }
   for (int i = 0; i < session_start_threads; i++) {
      st_args[i] = (concurrent_args_t){ .session = &s, .iterations = iters, .thread_idx = i };
      pthread_create(&st[i], NULL, concurrent_session_start_worker, &st_args[i]);
   }
   for (int i = 0; i < per_turn_threads; i++)
      pthread_join(pt[i], NULL);
   for (int i = 0; i < session_start_threads; i++)
      pthread_join(st[i], NULL);

   /* No assertion on final state — TSan / ASan are the load-bearing
    * gate here.  If we got here, the locks served correctly. */
   TEST_ASSERT_TRUE(s.injected_set.count >= 0);
   TEST_ASSERT_TRUE(s.injected_set.count <= MAX_INJECTED_SET_SIZE);

   pb_session_destroy(&s);
}

/* Concurrency invariant note:
 *   - Same-thread reentry into build_focus_block is impossible by
 *     construction (one thread, sequential execution).
 *   - Cross-thread same-session is covered by the stress test above.
 *   - Cross-thread cross-session is covered by lookup/record_locked
 *     touching only session->injected_set + session->history_mutex,
 *     which is per-session (no shared global state). */

/* ============================================================================
 * Phase 1g-i — score_breakdowns alignment, dedup-uplift base correctness,
 * lockstep compaction, broadcast call-site gating.
 * ============================================================================ */

static void test_dedup_uplift_uses_final_score_not_semantic(void) {
   /* Intent test for the 1f→1g-i bug fix.  Set up a config + seeds where
    * semantic_score and final_score DIVERGE.  Under the buggy 1f
    * implementation, dedup compared semantic_score (0.5 then 0.5) which
    * fails the uplift threshold and would suppress.  Under the fixed
    * 1g-i implementation, dedup compares final_score (0.9 then 1.4),
    * which exceeds 0.9*1.5=1.35 → admit.
    *
    * This test would FAIL on the 1f code (suppressed) and PASSES on
    * 1g-i. */
   session_t s;
   pb_session_init(&s, 42);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* Turn 1: semantic=0.5, final_score=0.9 — admit, record. */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   pb_focus_set_seed_score(0, 0.5f);
   pb_focus_set_seed_final_score(0, 0.9f);
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t1", &b));
   TEST_ASSERT_NOT_NULL(b);
   free(b);

   /* Turn 2: semantic=0.5 (unchanged), final_score=1.4 — uplift exceeds
    * threshold (0.9 * 1.5 = 1.35); 1g-i admits. */
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "X", "fact:X" };
   pb_focus_set_seed_score(0, 0.5f);
   pb_focus_set_seed_final_score(0, 1.4f);
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 0, 0, "t2", &b));
   TEST_ASSERT_NOT_NULL_MESSAGE(
       b, "1g-i: dedup uplift compares final_score (0.9 → 1.4 admits), not semantic_score");
   free(b);

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_dedup_compacts_both_arrays_lockstep(void) {
   /* Drive build_focus_block with 4 candidates where the middle two
    * are dedup-suppressed (re-injected within the recent window).
    * The 2 survivors must come out with their breakdowns intact and
    * index-aligned to the final candidate array.  We assert this by
    * inspecting the broadcast stub's snapshot — `last_first_final_score`
    * should equal what we seeded for the FIRST surviving candidate. */
   session_t s;
   pb_session_init(&s, 43);
   set_dispatch(&s);
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;

   /* Turn 1: seed 4 distinct items so they record in dedup state. */
   pb_focus_state()->seeded_count = 4;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "A", "fact:A" };
   pb_focus_state()->seeded[1] = (pb_seed_candidate_t){ "memory_fact", "B", "fact:B" };
   pb_focus_state()->seeded[2] = (pb_seed_candidate_t){ "memory_fact", "C", "fact:C" };
   pb_focus_state()->seeded[3] = (pb_seed_candidate_t){ "memory_fact", "D", "fact:D" };
   pb_focus_set_seed_score(0, 0.9f);
   pb_focus_set_seed_score(1, 0.85f);
   pb_focus_set_seed_score(2, 0.80f);
   pb_focus_set_seed_score(3, 0.75f);
   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 7, 100, "turn 1", &b));
   free(b);

   /* Turn 2: re-seed same 4 — A and D retain their slots; B and C
    * suppressed.  No — actually all 4 will suppress under default
    * window=8.  Let me re-construct: turn 2 with new ids E, F + repeat
    * A, B.  E and F are fresh → admit; A and B → suppress. */
   pb_focus_reset();
   pb_focus_state()->seeded_count = 4;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "E", "fact:E" }; /* fresh */
   pb_focus_state()->seeded[1] = (pb_seed_candidate_t){ "memory_fact", "A",
                                                        "fact:A" }; /* suppress */
   pb_focus_state()->seeded[2] = (pb_seed_candidate_t){ "memory_fact", "F", "fact:F" }; /* fresh */
   pb_focus_state()->seeded[3] = (pb_seed_candidate_t){ "memory_fact", "B",
                                                        "fact:B" }; /* suppress */
   pb_focus_set_seed_score(0, 0.95f);
   pb_focus_set_seed_score(1, 0.90f);
   pb_focus_set_seed_score(2, 0.85f);
   pb_focus_set_seed_score(3, 0.80f);

   pb_broadcast_reset();
   b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, build_focus_block(s_dispatch, 1, 7, 101, "turn 2", &b));
   TEST_ASSERT_NOT_NULL(b);
   /* Block must contain E and F; not A or B. */
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(b, "E"), "fresh candidate E must surface");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(b, "F"), "fresh candidate F must surface");
   free(b);

   /* Broadcast stub captured the post-dedup result.  Candidate count
    * must equal 2; first survivor's final_score must equal the first
    * fresh candidate's seeded score (0.95 — E was at idx 0). */
   pb_broadcast_mock_t *bc = pb_broadcast_state();
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, bc->call_count, "broadcast must fire once per turn");
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, bc->last_candidate_count,
                                 "after dedup, broadcast sees 2 survivors (E + F)");
   TEST_ASSERT_TRUE_MESSAGE(bc->last_had_breakdowns,
                            "score_breakdowns array must survive the lockstep compaction");
   TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
       1e-5f, 0.95f, bc->last_first_final_score,
       "first survivor's breakdown.final_score must be the one originally at slot 0 (E)");

   set_dispatch(NULL);
   pb_session_destroy(&s);
}

static void test_broadcast_fires_with_conv_id_set(void) {
   /* When build_focus_block is called with conv_id > 0, the broadcast
    * helper MUST fire — even if the result is empty (empty-state UX). */
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 0; /* zero candidates */

   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 1, /*conv_id*/ 7, /*turn_id*/ 99, "x", &b));
   TEST_ASSERT_NULL_MESSAGE(b, "empty result yields NULL block (composer omits section)");

   pb_broadcast_mock_t *bc = pb_broadcast_state();
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, bc->call_count,
                                 "empty-result turn still broadcasts (empty-state UX)");
   TEST_ASSERT_EQUAL_INT(1, bc->last_user_id);
   TEST_ASSERT_EQUAL_INT64(7, bc->last_conv_id);
   TEST_ASSERT_EQUAL_INT64(99, bc->last_turn_id);
   TEST_ASSERT_EQUAL_INT(0, bc->last_candidate_count);
}

static void test_broadcast_skipped_when_conv_id_zero(void) {
   /* A caller with no conversation passes conv_id=0: nothing is broadcast
    * (there is nowhere to show the items). */
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };

   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 1, /*conv_id*/ 0, /*turn_id*/ 0, "x", &b));
   TEST_ASSERT_NOT_NULL(b);
   free(b);

   TEST_ASSERT_EQUAL_INT_MESSAGE(0, pb_broadcast_state()->call_count,
                                 "conv_id=0 must NOT broadcast (SESSION_START / standalone path)");
}

static void test_broadcast_skipped_when_disabled(void) {
   /* Feature gate off → top-of-function short-circuit; broadcast never
    * runs.  Mirrors the disabled-no-fire requirement of the manual smoke
    * gate, in unit-test form. */
   g_config.memory.focus_injection.enabled = false;
   pb_embed_state()->available = true;
   pb_embed_state()->dims = 4;
   pb_focus_state()->seeded_count = 1;
   pb_focus_state()->seeded[0] = (pb_seed_candidate_t){ "memory_fact", "alpha", "fact:1" };

   char *b = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         build_focus_block(s_dispatch, 1, /*conv_id*/ 7, /*turn_id*/ 99, "x", &b));
   TEST_ASSERT_NULL(b);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, pb_broadcast_state()->call_count,
                                 "disabled feature must NOT broadcast (top-of-fn short circuit)");
}

int main(void) {
   UNITY_BEGIN();

   /* Composer two-segment shape */

   /* Drift hash */

   /* Feature gate (8-10) */
   RUN_TEST(test_focus_disabled_short_circuits);
   RUN_TEST(test_focus_unauthenticated_short_circuits);
   RUN_TEST(test_focus_empty_turn_text_short_circuits);

   /* Happy paths (11-15) */
   RUN_TEST(test_focus_renders_candidate_lines);
   RUN_TEST(test_focus_passes_user_id_and_text);
   RUN_TEST(test_focus_zero_candidates_returns_null_block);
   RUN_TEST(test_focus_embedding_unavailable_passes_null);
   RUN_TEST(test_focus_embedding_failure_passes_null);

   /* Failure modes (16-17) */
   RUN_TEST(test_focus_compose_failure_returns_null_block);
   RUN_TEST(test_focus_filter_rejection_logged_no_section_when_zero_survivors);

   /* Cross-turn isolation (18-20) */
   RUN_TEST(test_cross_turn_no_content_leak);
   RUN_TEST(test_failed_then_successful_refresh_no_carry);
   RUN_TEST(test_disabled_after_enabled_returns_null);

   /* Memory + integration (21-23) */
   RUN_TEST(test_memory_cycle_1000x);
   RUN_TEST(test_focus_block_produces_one_line_per_candidate);

   /* Phase 1f — dedup (24-37) */
   RUN_TEST(test_dedup_baseline_no_dispatch_session);
   RUN_TEST(test_dedup_consecutive_turn_suppresses);
   RUN_TEST(test_dedup_strict_window_decay);
   RUN_TEST(test_dedup_score_uplift_below_threshold_suppresses);
   RUN_TEST(test_dedup_score_uplift_meets_threshold_admits);
   RUN_TEST(test_dedup_window_zero_disables_time_dedup);
   RUN_TEST(test_dedup_session_start_does_not_record);
   RUN_TEST(test_dedup_cross_user_isolation);
   RUN_TEST(test_dedup_cross_session_same_user_isolation);
   RUN_TEST(test_dedup_lru_cap_evicts_oldest);
   RUN_TEST(test_dedup_lru_correctness_with_reinjects);
   RUN_TEST(test_dedup_all_suppressed_warn_once_per_session);
   RUN_TEST(test_dedup_concurrent_stress);

   /* Phase 1g-i — uplift base, lockstep compaction, broadcast gating */
   RUN_TEST(test_dedup_uplift_uses_final_score_not_semantic);
   RUN_TEST(test_dedup_compacts_both_arrays_lockstep);
   RUN_TEST(test_broadcast_fires_with_conv_id_set);
   RUN_TEST(test_broadcast_skipped_when_conv_id_zero);
   RUN_TEST(test_broadcast_skipped_when_disabled);

   return UNITY_END();
}
