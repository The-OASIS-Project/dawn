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
 * Unit tests for the compaction core (llm_compaction.c, linked as is): the
 * mechanical summary, the target, the range estimate and the escalation, plus
 * the compaction range (llm_compaction_range.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_compaction.h"
#include "llm/llm_compaction_range.h"
#include "llm/llm_context.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Helper: build a simple conversation message
 * ============================================================================= */

static struct json_object *make_msg(const char *role, const char *content) {
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string(role));
   json_object_object_add(msg, "content", json_object_new_string(content));
   return msg;
}

/* =============================================================================
 * compact_deterministic tests
 * ============================================================================= */

static void test_compact_deterministic_basic(void) {
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < 10; i++) {
      const char *role = (i % 2 == 0) ? "user" : "assistant";
      json_object_array_add(arr, make_msg(role, "This is a test message for compaction."));
   }

   char *result = llm_compaction_deterministic(arr, 150);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "deterministic returns non-NULL");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "truncated"), "output contains 'truncated' header");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "user:"), "output contains user role");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "assistant:"), "output contains assistant role");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_budget(void) {
   struct json_object *arr = json_object_new_array();
   char long_content[2000];
   memset(long_content, 'A', sizeof(long_content) - 1);
   long_content[sizeof(long_content) - 1] = '\0';

   for (int i = 0; i < 20; i++) {
      json_object_array_add(arr, make_msg("user", long_content));
   }

   int budget = 150;
   int max_bytes = budget * 4 + 128;
   char *result = llm_compaction_deterministic(arr, budget);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "deterministic with long content returns non-NULL");
   TEST_ASSERT_TRUE_MESSAGE((int)strlen(result) < max_bytes, "output stays within budget");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_empty(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("user", ""));
   json_object_array_add(arr, make_msg("assistant", ""));

   char *result = llm_compaction_deterministic(arr, 150);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "empty content messages don't crash");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "truncated"), "still has header");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_single(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("user", "Hello, how are you today?"));

   char *result = llm_compaction_deterministic(arr, 150);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "single message returns non-NULL");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "Hello"), "contains message content");

   free(result);
   json_object_put(arr);
}

static void test_compact_deterministic_many_messages(void) {
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < 500; i++) {
      json_object_array_add(arr, make_msg("user", "Short msg."));
   }

   int budget = 150;
   int max_bytes = budget * 4 + 128;
   char *result = llm_compaction_deterministic(arr, budget);
   TEST_ASSERT_NOT_NULL_MESSAGE(result, "500 messages returns non-NULL");
   TEST_ASSERT_TRUE_MESSAGE((int)strlen(result) < max_bytes,
                            "output stays within budget with many messages");

   free(result);
   json_object_put(arr);
}

/* =============================================================================
 * calculate_compaction_target tests
 * ============================================================================= */

static void test_calculate_target_normal(void) {
   int target = llm_compaction_target_tokens(8192, 0.80f);
   TEST_ASSERT_TRUE_MESSAGE(target > 4000, "8192 * 0.60 > 4000");
   TEST_ASSERT_TRUE_MESSAGE(target < 5500, "8192 * 0.60 < 5500");
   TEST_ASSERT_EQUAL_INT_MESSAGE((int)(8192 * 0.60f), target, "exact value matches 8192 * 0.60");
}

static void test_calculate_target_low_threshold(void) {
   int target = llm_compaction_target_tokens(4096, 0.40f);
   int floor_val = (int)(4096 * 0.30f);
   TEST_ASSERT_EQUAL_INT_MESSAGE(floor_val, target, "low threshold hits floor at 0.30");
}

static void test_calculate_target_high_threshold(void) {
   int target = llm_compaction_target_tokens(128000, 0.85f);
   int expected = (int)(128000 * 0.65f);
   TEST_ASSERT_EQUAL_INT_MESSAGE(expected, target, "128K context * 0.65 = ~83200");
}

static void test_calculate_target_below_clamp(void) {
   int target = llm_compaction_target_tokens(4096, 0.15f);
   int floor_val = (int)(4096 * 0.30f);
   TEST_ASSERT_EQUAL_INT_MESSAGE(floor_val, target, "threshold < 0.25 clamped, hits floor at 0.30");
}

/* =============================================================================
 * estimate_tokens_range tests
 * ============================================================================= */

static void test_estimate_tokens_range(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, make_msg("system", "You are a helpful assistant."));
   json_object_array_add(arr, make_msg("user", "Hello there, how are you doing today?"));
   json_object_array_add(arr, make_msg("assistant", "I am doing well, thank you for asking!"));

   int full = llm_compaction_estimate_range(arr, 0, 3);
   TEST_ASSERT_TRUE_MESSAGE(full > 0, "full range estimate is positive");

   int partial = llm_compaction_estimate_range(arr, 1, 3);
   TEST_ASSERT_TRUE_MESSAGE(partial > 0, "partial range estimate is positive");
   TEST_ASSERT_TRUE_MESSAGE(partial < full, "partial range is less than full");

   int single = llm_compaction_estimate_range(arr, 0, 1);
   TEST_ASSERT_TRUE_MESSAGE(single > 0, "single message estimate is positive");

   int empty = llm_compaction_estimate_range(arr, 2, 2);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, empty, "empty range returns 0");

   json_object_put(arr);
}

/* Regression: a large Claude tool_result payload lives in each block's "content"
 * (not "text").  The estimator must count it, or a huge result reads as ~0 tokens,
 * the compaction guard never fires, and the next request overflows the model window
 * -> provider HTTP 400 (observed 2026-07-07 with a 316 KB cbm_get_code_snippet). */
static void test_estimate_counts_claude_tool_result(void) {
   /* Build a Claude tool_result message: {role:user, content:[{type:tool_result,
    * tool_use_id, content:<big string>}]}. */
   char big[4001];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';

   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("tool_result"));
   json_object_object_add(block, "tool_use_id", json_object_new_string("toolu_1"));
   json_object_object_add(block, "content", json_object_new_string(big));
   struct json_object *content_arr = json_object_new_array();
   json_object_array_add(content_arr, block);
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_arr);

   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, msg);

   /* 4000 chars / 4 = ~1000 tokens; before the fix this returned ~5 (the flat +20). */
   int est = llm_compaction_estimate_range(arr, 0, 1);
   TEST_ASSERT_TRUE_MESSAGE(est > 900, "Claude tool_result content is counted (~1000 tokens)");

   json_object_put(arr);
}

/* =============================================================================
 * Level ordering test
 * ============================================================================= */

static void test_level_ordering(void) {
   TEST_ASSERT_TRUE_MESSAGE(LLM_COMPACT_NORMAL < LLM_COMPACT_AGGRESSIVE, "NORMAL < AGGRESSIVE");
   TEST_ASSERT_TRUE_MESSAGE(LLM_COMPACT_AGGRESSIVE < LLM_COMPACT_DETERMINISTIC,
                            "AGGRESSIVE < DETERMINISTIC");
   TEST_ASSERT_EQUAL_INT_MESSAGE(LLM_COMPACT_MAX_LEVEL, LLM_COMPACT_DETERMINISTIC,
                                 "DETERMINISTIC == MAX_LEVEL");
}

/* =============================================================================
 * Main
 * ============================================================================= */


/* =============================================================================
 * Compaction range: where the kept part starts, and which rows it summarizes
 * ============================================================================= */

static struct json_object *with_id(struct json_object *msg, int64_t id) {
   json_object_object_add(msg, "id", json_object_new_int64(id));
   return msg;
}

/* A Claude-shaped assistant tool call, or its results message (no row ids:
 * a live session doesn't stamp them). */
static struct json_object *claude_call(const char *id) {
   struct json_object *m = json_object_new_object(), *c = json_object_new_array(),
                      *b = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(b, "type", json_object_new_string("tool_use"));
   json_object_object_add(b, "id", json_object_new_string(id));
   json_object_array_add(c, b);
   json_object_object_add(m, "content", c);
   return m;
}

static struct json_object *claude_results(const char *id, int n) {
   struct json_object *m = json_object_new_object(), *c = json_object_new_array();
   json_object_object_add(m, "role", json_object_new_string("user"));
   for (int i = 0; i < n; i++) {
      struct json_object *b = json_object_new_object();
      json_object_object_add(b, "type", json_object_new_string("tool_result"));
      json_object_object_add(b, "tool_use_id", json_object_new_string(id));
      json_object_array_add(c, b);
   }
   json_object_object_add(m, "content", c);
   return m;
}

/* A directive or loop note between a turn's question and reply belongs to that
 * turn: the cut never falls after it. */
static void test_compaction_range_keeps_context_with_its_turn(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", "s"));
   json_object_array_add(h, make_msg("user", "q1"));
   json_object_array_add(h, make_msg("assistant", "a1"));
   json_object_array_add(h, make_msg("user", "q2")); /* 3 */
   struct json_object *directive = make_msg("system", "d");
   json_object_object_add(directive, "_kind", json_object_new_string("directive"));
   json_object_array_add(h, directive);
   struct json_object *note = make_msg("user", "n");
   json_object_object_add(note, "_kind", json_object_new_string("loop_note"));
   json_object_array_add(h, note);
   json_object_array_add(h, make_msg("assistant", "a2"));

   /* Keeping 1 would start at a2, after q2's directive and note: back to q2. */
   TEST_ASSERT_EQUAL_INT(3, llm_compaction_keep_start(h, 1, 1));
   json_object_put(h);
}

/* A question DAWN asked (a job's continuation, an envelope) starts a turn: the
 * cut lands on it, not past it into the turn before. */
static void test_compaction_range_cuts_at_an_envelope_question(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", "s"));
   json_object_array_add(h, make_msg("user", "q1"));
   json_object_array_add(h, claude_call("toolu_1"));
   json_object_array_add(h, claude_results("toolu_1", 1));
   struct json_object *go_on = make_msg("user", "Continue the task.");
   json_object_object_add(go_on, "_kind", json_object_new_string("envelope"));
   json_object_array_add(h, go_on); /* 4 */
   json_object_array_add(h, claude_call("toolu_2"));
   json_object_array_add(h, claude_results("toolu_2", 1));

   /* Keeping 2 would start inside the continuation's exchange: back to it. */
   TEST_ASSERT_EQUAL_INT(4, llm_compaction_keep_start(h, 1, 2));
   json_object_put(h);
}

/* A turn with parallel Claude results: the kept part starts at its question,
 * and the summarized rows end right before that question's row, past the
 * turn's unstamped calls and results (rows 101-107). */
static void test_compaction_range_claude_tool_turn(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", "s"));
   json_object_array_add(h, with_id(make_msg("user", "q1"), 90));
   json_object_array_add(h, with_id(make_msg("assistant", "a1"), 91));
   json_object_array_add(h, with_id(make_msg("user", "q2"), 100));
   json_object_array_add(h, claude_call("toolu_1"));
   json_object_array_add(h, claude_results("toolu_1", 7));
   json_object_array_add(h, with_id(make_msg("user", "q3"), 110)); /* 6 */
   json_object_array_add(h, claude_call("toolu_2"));
   json_object_array_add(h, claude_results("toolu_2", 1));
   json_object_array_add(h, make_msg("assistant", "a3"));

   /* Keeping 3 would start inside q3's tool exchange: it moves back to q3. */
   const int kept = llm_compaction_keep_start(h, 1, 3);
   TEST_ASSERT_EQUAL_INT(6, kept);

   int64_t first = 0, last = 0;
   llm_compaction_summary_ids(h, 1, kept, &first, &last);
   TEST_ASSERT_EQUAL_INT64(90, first);
   TEST_ASSERT_EQUAL_INT64(100, last); /* q2's call and results carry no id */

   /* Those are found by call id: q2's exchange, not q3's (kept). */
   struct json_object *ids = llm_compaction_tail_call_ids(h, 1, kept);
   TEST_ASSERT_NOT_NULL(ids);
   TEST_ASSERT_TRUE(json_object_array_length(ids) > 0);
   TEST_ASSERT_TRUE(llm_compaction_row_in_calls("tool", NULL, "toolu_1", ids));
   TEST_ASSERT_TRUE(llm_compaction_row_in_calls(
       "assistant", "[{\"id\":\"toolu_1\",\"type\":\"function\"}]", NULL, ids));
   /* A row written outside the session (a research result) isn't one of them. */
   TEST_ASSERT_FALSE(llm_compaction_row_in_calls("assistant", NULL, NULL, ids));
   TEST_ASSERT_FALSE(llm_compaction_row_in_calls("tool", NULL, "toolu_9", ids));
   /* An id that is only part of another's doesn't match. */
   TEST_ASSERT_FALSE(llm_compaction_row_in_calls(
       "assistant", "[{\"id\":\"toolu_10\",\"type\":\"function\"}]", NULL, ids));
   json_object_put(ids);

   /* The kept part starts at row 110: a raise over tail rows stops below it. */
   TEST_ASSERT_EQUAL_INT64(110, llm_compaction_kept_first_id(h, kept));

   /* The kept turn reusing the id (per-response numbering) can't tell the rows
    * apart, so it's left out. */
   json_object_array_add(h, claude_call("toolu_1"));
   json_object_array_add(h, claude_results("toolu_1", 1));
   ids = llm_compaction_tail_call_ids(h, 1, kept);
   TEST_ASSERT_EQUAL_INT(0, json_object_array_length(ids));
   json_object_put(ids);
   json_object_put(h);
}

/* Messages loaded after a compaction point that begin with results whose
 * call is in the summary lose those results, and only those. */
static void test_drop_leading_results(void) {
   struct json_object *h = json_object_new_array();
   struct json_object *summary = json_object_new_object();
   json_object_object_add(summary, "role", json_object_new_string("assistant"));
   json_object_object_add(summary, "content", json_object_new_string("summary"));
   json_object_array_add(h, summary);
   for (int i = 0; i < 2; i++) {
      struct json_object *t = json_object_new_object();
      json_object_object_add(t, "role", json_object_new_string("tool"));
      json_object_object_add(t, "tool_call_id", json_object_new_string("c"));
      json_object_object_add(t, "content", json_object_new_string("r"));
      json_object_array_add(h, t);
   }
   struct json_object *claude = json_object_new_object(), *parts = json_object_new_array(),
                      *part = json_object_new_object();
   json_object_object_add(claude, "role", json_object_new_string("user"));
   json_object_object_add(part, "type", json_object_new_string("tool_result"));
   json_object_array_add(parts, part);
   json_object_object_add(claude, "content", parts);
   json_object_array_add(h, claude);
   struct json_object *q = json_object_new_object();
   json_object_object_add(q, "role", json_object_new_string("user"));
   json_object_object_add(q, "content", json_object_new_string("next question"));
   json_object_array_add(h, q);
   struct json_object *later = json_object_new_object();
   json_object_object_add(later, "role", json_object_new_string("tool"));
   json_object_array_add(h, later);

   TEST_ASSERT_EQUAL_INT(3, llm_history_drop_leading_results(h, 1, NULL));
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(h)); /* summary, question, later */
   TEST_ASSERT_EQUAL_INT(0, llm_history_drop_leading_results(h, 1, NULL));
   json_object_put(h);
}

/* The escalation: an L1 summary that fits is used; a failing summarizer falls
 * through to the mechanical L3; the text is neutralized either way. */
typedef struct {
   const char *reply[2]; /* per level; NULL = the call fails */
   int calls;
} fake_summarizer_t;

static char *fake_summarize(struct json_object *in, llm_compaction_level_t level, void *ctx) {
   (void)in;
   fake_summarizer_t *f = ctx;
   f->calls++;
   return f->reply[level] ? strdup(f->reply[level]) : NULL;
}

static struct json_object *two_messages(void) {
   struct json_object *h = json_object_new_array();
   for (int i = 0; i < 2; i++) {
      struct json_object *m = json_object_new_object();
      json_object_object_add(m, "role", json_object_new_string(i ? "assistant" : "user"));
      json_object_object_add(m, "content",
                             json_object_new_string("a long enough message about the garage"));
      json_object_array_add(h, m);
   }
   return h;
}

static void test_summarize_uses_a_fitting_l1(void) {
   struct json_object *h = two_messages();
   fake_summarizer_t f = { .reply = { "short summary", "bullets" } };
   llm_compaction_level_t level = LLM_COMPACT_MAX_LEVEL;
   char *s = llm_compaction_summarize(h, 0, 10000, NULL, fake_summarize, &f, NULL, &level);
   TEST_ASSERT_EQUAL_STRING("short summary", s);
   TEST_ASSERT_EQUAL_INT(LLM_COMPACT_NORMAL, level);
   TEST_ASSERT_EQUAL_INT(1, f.calls);
   free(s);
   json_object_put(h);
}

static void test_summarize_falls_through_to_l3(void) {
   struct json_object *h = two_messages();
   fake_summarizer_t f = { .reply = { NULL, NULL } };
   llm_compaction_level_t level = LLM_COMPACT_NORMAL;
   char *s = llm_compaction_summarize(h, 0, 10000, NULL, fake_summarize, &f, NULL, &level);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_INT(LLM_COMPACT_DETERMINISTIC, level);
   TEST_ASSERT_EQUAL_INT(2, f.calls);
   TEST_ASSERT_NOT_NULL(strstr(s, "garage"));
   free(s);
   json_object_put(h);
}

static void test_summarize_stops_when_cancelled(void) {
   struct json_object *h = two_messages();
   fake_summarizer_t f = { .reply = { NULL, NULL } };
   atomic_bool cancel = true;
   llm_compaction_level_t level = LLM_COMPACT_NORMAL;
   TEST_ASSERT_NULL(llm_compaction_summarize(h, 0, 10000, NULL, fake_summarize, &f, &cancel,
                                             &level)); /* no mechanical fallback */
   TEST_ASSERT_EQUAL_INT(1, f.calls);
   json_object_put(h);
}

/* A model's density comes from how two requests grew: the fixed part cancels.
 * (The numbers are a real session's: two tool results added 76,109 tokens that
 * the estimate put at ~51,400.) */
static void test_the_density_is_learned_from_growth(void) {
   float f = llm_compaction_factor_update(1.0f, 0, 170545 - 94436, 51400);
   TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.48f, f); /* the first sample, whole */
   /* Growth too small to tell anything: no change. */
   TEST_ASSERT_EQUAL_FLOAT(f, llm_compaction_factor_update(f, 1, 900, 500));
   /* A later sample moves it part of the way; an absurd one is clamped. */
   const float g = llm_compaction_factor_update(f, 1, 2000, 2000);
   TEST_ASSERT_TRUE(g < f && g > 1.0f);
   TEST_ASSERT_TRUE(llm_compaction_factor_update(1.0f, 0, 100000, 1000) <=
                    LLM_COMPACTION_FACTOR_MAX);
}

/* A request's size is the fixed part (tools, system prompt: never in the
 * estimate) plus the history at the model's density; the inverse gives the
 * room a token budget leaves the history. */
static void test_a_request_is_fixed_part_plus_density(void) {
   const llm_compaction_calibration_t cal = {
      .known = true,
      .last_prompt = 170545,
      .last_estimate = 94000,
      .last_factor = 1.5f,
      .factor = 1.5f,
   };
   /* fixed = 170545 - 1.5 * 94000 = 29545 */
   TEST_ASSERT_INT_WITHIN(2, 29545 + 60000, llm_compaction_calibrated_tokens(&cal, 40000));
   TEST_ASSERT_INT_WITHIN(2, 40000, llm_compaction_estimate_budget(&cal, 29545 + 60000));
   TEST_ASSERT_EQUAL_INT(0, llm_compaction_estimate_budget(&cal, 1000)); /* no room */

   /* Another model reads the same text at its own density: the fixed part too. */
   llm_compaction_calibration_t other = cal;
   other.factor = 3.0f;
   TEST_ASSERT_INT_WITHIN(2, 2 * 29545 + 3 * 40000,
                          llm_compaction_calibrated_tokens(&other, 40000));

   /* Nothing measured: the estimate as it is. */
   const llm_compaction_calibration_t none = { 0 };
   TEST_ASSERT_EQUAL_INT(40000, llm_compaction_calibrated_tokens(&none, 40000));
   TEST_ASSERT_EQUAL_INT(40000, llm_compaction_estimate_budget(&none, 40000));
   TEST_ASSERT_EQUAL_INT(40000, llm_compaction_calibrated_tokens(NULL, 40000));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_compact_deterministic_basic);
   RUN_TEST(test_compact_deterministic_budget);
   RUN_TEST(test_compact_deterministic_empty);
   RUN_TEST(test_compact_deterministic_single);
   RUN_TEST(test_compact_deterministic_many_messages);
   RUN_TEST(test_calculate_target_normal);
   RUN_TEST(test_calculate_target_low_threshold);
   RUN_TEST(test_calculate_target_high_threshold);
   RUN_TEST(test_calculate_target_below_clamp);
   RUN_TEST(test_estimate_tokens_range);
   RUN_TEST(test_estimate_counts_claude_tool_result);
   RUN_TEST(test_level_ordering);
   RUN_TEST(test_compaction_range_claude_tool_turn);
   RUN_TEST(test_compaction_range_keeps_context_with_its_turn);
   RUN_TEST(test_compaction_range_cuts_at_an_envelope_question);
   RUN_TEST(test_the_density_is_learned_from_growth);
   RUN_TEST(test_a_request_is_fixed_part_plus_density);
   RUN_TEST(test_drop_leading_results);
   RUN_TEST(test_summarize_uses_a_fitting_l1);
   RUN_TEST(test_summarize_falls_through_to_l3);
   RUN_TEST(test_summarize_stops_when_cancelled);
   return UNITY_END();
}
