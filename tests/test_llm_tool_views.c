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
 * The view stage's planning (llm_tool_views_plan.c): the batch budget, its
 * split, and the header.
 */

#include <stdint.h>
#include <string.h>

#include "llm/llm_tool_views.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* The budget follows the window along the curve through its two anchors. */
static void test_the_budget_follows_the_window(void) {
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_REF_CHARS,
                            llm_tool_views_budget_chars(LLM_TOOL_VIEWS_REF_WINDOW, 0, false));
   const size_t small = llm_tool_views_budget_chars(LLM_TOOL_VIEWS_SMALL_WINDOW, 0, false);
   TEST_ASSERT_TRUE(small >= LLM_TOOL_VIEWS_SMALL_CHARS - 1 && small <= LLM_TOOL_VIEWS_SMALL_CHARS);
   /* Between them, sub-linear: a 200K window gets about a third of 1M's. */
   const size_t mid = llm_tool_views_budget_chars(200000, 0, false);
   TEST_ASSERT_TRUE(mid > 15000 && mid < 18000);
   /* Above the reference window, and unknown: the reference budget. */
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_REF_CHARS,
                            llm_tool_views_budget_chars(2000000, 0, false));
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_REF_CHARS, llm_tool_views_budget_chars(0, 0, false));
   /* A tiny window: the floor. */
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_FLOOR_CHARS,
                            llm_tool_views_budget_chars(2000, 0, false));
}

/* A nearly full request takes less; a full one the floor, so the data stays
 * reachable. */
static void test_the_room_left_clamps_the_budget(void) {
   TEST_ASSERT_EQUAL_size_t(5000 * LLM_TOOL_VIEWS_WORST_CHARS_PER_TOKEN,
                            llm_tool_views_budget_chars(1000000, 5000, true));
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_REF_CHARS,
                            llm_tool_views_budget_chars(1000000, 500000, true));
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_FLOOR_CHARS,
                            llm_tool_views_budget_chars(1000000, -3000, true));
}

/* Under the budget, everything passes whole. */
static void test_a_batch_that_fits_passes_whole(void) {
   const size_t demands[] = { 100, 2000, 300 };
   size_t shares[3];
   llm_tool_views_split(demands, NULL, 3, 10000, shares);
   TEST_ASSERT_EQUAL_size_t(100, shares[0]);
   TEST_ASSERT_EQUAL_size_t(2000, shares[1]);
   TEST_ASSERT_EQUAL_size_t(300, shares[2]);
}

/* The small ones pass whole; the big one gets what's left. */
static void test_small_results_pass_and_the_big_one_takes_the_rest(void) {
   const size_t demands[] = { 1000, 150000, 2000 };
   size_t shares[3];
   llm_tool_views_split(demands, NULL, 3, 40000, shares);
   TEST_ASSERT_EQUAL_size_t(1000, shares[0]);
   TEST_ASSERT_EQUAL_size_t(2000, shares[2]);
   TEST_ASSERT_EQUAL_size_t(37000, shares[1]);
}

/* Two big ones share what's left in proportion to their size, within it. */
static void test_big_results_share_in_proportion(void) {
   const size_t demands[] = { 64814, 27000, 27000, 500 };
   size_t shares[4];
   llm_tool_views_split(demands, NULL, 4, 40000, shares);
   TEST_ASSERT_EQUAL_size_t(500, shares[3]);
   TEST_ASSERT_TRUE(shares[0] > shares[1]);
   TEST_ASSERT_EQUAL_size_t(shares[1], shares[2]);
   TEST_ASSERT_TRUE(shares[0] + shares[1] + shares[2] + shares[3] <= 40000);
   TEST_ASSERT_TRUE(shares[0] + shares[1] + shares[2] + shares[3] >= 39990);
}

/* A result left alone takes no share and is left as it is. */
static void test_a_result_left_alone_takes_no_share(void) {
   const size_t demands[] = { SIZE_MAX, 30000 };
   size_t shares[2];
   llm_tool_views_split(demands, NULL, 2, 20000, shares);
   TEST_ASSERT_EQUAL_size_t(SIZE_MAX, shares[0]);
   TEST_ASSERT_EQUAL_size_t(20000, shares[1]);
}

/* A tool asking to be shown whole (within its ask) is served first; the rest
 * share what's left. */
static void test_a_result_asked_whole_is_served_first(void) {
   const size_t demands[] = { 30000, 30000, 1000 };
   const bool first[] = { true, false, false };
   size_t shares[3];
   llm_tool_views_split(demands, first, 3, 40000, shares);
   TEST_ASSERT_EQUAL_size_t(30000, shares[0]);
   TEST_ASSERT_EQUAL_size_t(1000, shares[2]);
   TEST_ASSERT_EQUAL_size_t(9000, shares[1]);
}

/* ...but never past the budget: one that doesn't fit is split like any. */
static void test_an_ask_never_passes_the_budget(void) {
   const size_t demands[] = { 50000, 1000 };
   const bool first[] = { true, false };
   size_t shares[2];
   llm_tool_views_split(demands, first, 2, 40000, shares);
   TEST_ASSERT_EQUAL_size_t(1000, shares[1]);
   TEST_ASSERT_EQUAL_size_t(39000, shares[0]);
   /* Two asks that can't both fit: the smaller is served whole. */
   const size_t two[] = { 30000, 20000 };
   const bool both[] = { true, true };
   llm_tool_views_split(two, both, 2, 40000, shares);
   TEST_ASSERT_EQUAL_size_t(20000, shares[1]);
   TEST_ASSERT_EQUAL_size_t(20000, shares[0]);
}

static size_t header(const llm_tool_views_header_t *h, char *out, size_t size) {
   return llm_tool_views_header(h, out, size);
}

/* A stored JSON result: the handle and every read. */
static void test_the_header_names_the_handle_and_how_to_read(void) {
   char out[LLM_TOOL_VIEWS_HEADER_MAX];
   const llm_tool_views_header_t h = {
      .tag = "dawn-ctx-12345678",
      .chars = 64814,
      .handle = "trs_abcdefghijkl",
      .json = true,
      .offers_read = true,
      .narrow = "limit",
   };
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_NOT_NULL(
       strstr(out, "[Tool result shortened (dawn-ctx-12345678): 64,814 chars, shown"));
   TEST_ASSERT_NULL(strstr(out, "tokens"));
   TEST_ASSERT_NOT_NULL(strstr(out, "[tool-result trs_abcdefghijkl]"));
   TEST_ASSERT_NOT_NULL(strstr(out, "path ("));
   /* A tally points at count / distinct, one call over the whole array. */
   TEST_ASSERT_NOT_NULL(strstr(out, "count / distinct ($.a, field) tally a whole array"));
   TEST_ASSERT_NOT_NULL(strstr(out, "The tool takes \"limit\""));
   TEST_ASSERT_EQUAL_CHAR('\n', out[strlen(out) - 1]);
   TEST_ASSERT_EQUAL_CHAR(']', out[strlen(out) - 2]);
}

/* A text result offers only the reads text has. */
static void test_a_text_result_offers_lines_and_grep(void) {
   char out[LLM_TOOL_VIEWS_HEADER_MAX];
   const llm_tool_views_header_t h = {
      .chars = 90000,
      .handle = "trs_abcdefghijkl",
      .json = false,
      .offers_read = true,
      .head_tail_only = true,
   };
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_NOT_NULL(strstr(out, "[Tool result shortened: 90,000 chars"));
   TEST_ASSERT_NULL(strstr(out, "path ("));
   TEST_ASSERT_NOT_NULL(strstr(out, "grep (text)"));
   TEST_ASSERT_NOT_NULL(strstr(out, "first and last parts"));
}

/* Without a handle (a guest, a failed store, no result_read): call again. */
static void test_without_a_handle_the_header_says_call_again(void) {
   char out[LLM_TOOL_VIEWS_HEADER_MAX];
   llm_tool_views_header_t h = { .chars = 50000 };
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_NULL(strstr(out, "tool-result"));
   TEST_ASSERT_NOT_NULL(strstr(out, "narrower arguments"));
   /* A handle but no result_read in the request: the handle isn't offered. */
   h.handle = "trs_abcdefghijkl";
   h.offers_read = false;
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_NULL(strstr(out, "tool-result"));
}

/* result_read's own answer: read a smaller part, and no narrowing line. */
static void test_a_reads_view_says_read_less(void) {
   char out[LLM_TOOL_VIEWS_HEADER_MAX];
   const llm_tool_views_header_t h = {
      .chars = 40000,
      .is_read = true,
      .narrow = "query",
   };
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_NOT_NULL(strstr(out, "Read a smaller part"));
   TEST_ASSERT_NULL(strstr(out, "takes \""));
   /* A JSON read's view points a tally at count / distinct; a text one doesn't. */
   llm_tool_views_header_t j = h;
   j.json = true;
   TEST_ASSERT_TRUE(header(&j, out, sizeof(out)) > 0);
   TEST_ASSERT_NOT_NULL(strstr(out, "count / distinct"));
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_NULL(strstr(out, "count / distinct"));
}

static void test_a_header_that_doesnt_fit_is_empty(void) {
   char out[30];
   const llm_tool_views_header_t h = { .chars = 50000 };
   TEST_ASSERT_EQUAL_size_t(0, header(&h, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("", out);
}

/* Too little room for the full header: the shortest one, handle kept. */
static void test_a_tight_header_keeps_the_handle(void) {
   char out[96];
   const llm_tool_views_header_t h = {
      .chars = 50000,
      .handle = "trs_abcdefghijkl",
      .json = true,
      .offers_read = true,
   };
   TEST_ASSERT_TRUE(header(&h, out, sizeof(out)) > 0);
   TEST_ASSERT_EQUAL_STRING(
       "[Tool result shortened: 50,000 chars. Full result: [tool-result trs_abcdefghijkl].]\n",
       out);
}

/* A read's share: the batch's fair share less a header, never under the least. */
static void test_a_read_gets_its_fair_share(void) {
   TEST_ASSERT_EQUAL_size_t(40000 - LLM_TOOL_VIEWS_HEADER_MAX, llm_tool_views_read_chars(40000, 1));
   TEST_ASSERT_EQUAL_size_t(10000 - LLM_TOOL_VIEWS_HEADER_MAX, llm_tool_views_read_chars(40000, 4));
   TEST_ASSERT_EQUAL_size_t(40000 - LLM_TOOL_VIEWS_HEADER_MAX, llm_tool_views_read_chars(40000, 0));
   TEST_ASSERT_EQUAL_size_t(LLM_TOOL_VIEWS_READ_MIN_CHARS, llm_tool_views_read_chars(6000, 8));
}

static void test_only_listed_parameters_narrow(void) {
   TEST_ASSERT_TRUE(llm_tool_views_narrows("limit"));
   TEST_ASSERT_TRUE(llm_tool_views_narrows("max_results"));
   TEST_ASSERT_FALSE(llm_tool_views_narrows("url"));
   TEST_ASSERT_FALSE(llm_tool_views_narrows(NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_the_budget_follows_the_window);
   RUN_TEST(test_the_room_left_clamps_the_budget);
   RUN_TEST(test_a_batch_that_fits_passes_whole);
   RUN_TEST(test_small_results_pass_and_the_big_one_takes_the_rest);
   RUN_TEST(test_big_results_share_in_proportion);
   RUN_TEST(test_a_result_left_alone_takes_no_share);
   RUN_TEST(test_a_result_asked_whole_is_served_first);
   RUN_TEST(test_an_ask_never_passes_the_budget);
   RUN_TEST(test_the_header_names_the_handle_and_how_to_read);
   RUN_TEST(test_a_text_result_offers_lines_and_grep);
   RUN_TEST(test_without_a_handle_the_header_says_call_again);
   RUN_TEST(test_a_reads_view_says_read_less);
   RUN_TEST(test_a_header_that_doesnt_fit_is_empty);
   RUN_TEST(test_a_tight_header_keeps_the_handle);
   RUN_TEST(test_a_read_gets_its_fair_share);
   RUN_TEST(test_only_listed_parameters_narrow);
   return UNITY_END();
}
