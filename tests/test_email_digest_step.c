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
 * Unit tests for the email digest's per-account paging decisions
 * (email_digest_next_step + email_digest_page_reached_cutoff, header-only in
 * tools/email_digest_internal.h).
 */

#include <string.h>

#include "tools/email_digest_internal.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* A mid-run state that, left alone, asks for another page. */
static email_digest_page_state_t base_state(void) {
   email_digest_page_state_t st = { 0 };
   st.depth = 200;
   st.fetched = 50;
   st.pages = 1;
   st.last_returned = 50;
   st.has_token = true;
   return st;
}

static void test_continues_when_more_remains(void) {
   email_digest_page_state_t st = base_state();
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_MORE, email_digest_next_step(&st));
}

static void test_stops_when_window_covered(void) {
   email_digest_page_state_t st = base_state();
   st.reached_cutoff = true;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_WINDOW, email_digest_next_step(&st));
}

static void test_window_wins_over_depth(void) {
   email_digest_page_state_t st = base_state();
   st.fetched = st.depth;
   st.reached_cutoff = true;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_WINDOW, email_digest_next_step(&st));
}

static void test_stops_when_exhausted(void) {
   email_digest_page_state_t st = base_state();
   st.has_token = false;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_EXHAUSTED, email_digest_next_step(&st));

   /* A full page with no token is still "no more mail", not a depth stop. */
   st = base_state();
   st.fetched = st.depth;
   st.has_token = false;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_EXHAUSTED, email_digest_next_step(&st));
}

static void test_empty_page_with_token_stops(void) {
   /* Gmail can return an empty page that still carries nextPageToken. */
   email_digest_page_state_t st = base_state();
   st.last_returned = 0;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_EXHAUSTED, email_digest_next_step(&st));
}

static void test_stops_at_depth(void) {
   email_digest_page_state_t st = base_state();
   st.depth = 10;
   st.fetched = 10;
   st.last_returned = 10;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_DEPTH, email_digest_next_step(&st));
}

static void test_repeated_token_stops(void) {
   email_digest_page_state_t st = base_state();
   st.token_repeated = true;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_PAGE_LIMIT, email_digest_next_step(&st));
}

static void test_page_ceiling_bounds_short_pages(void) {
   /* One row per page with a token forever: the page ceiling ends it. */
   email_digest_page_state_t st = base_state();
   st.fetched = EMAIL_DIGEST_MAX_PAGES;
   st.pages = EMAIL_DIGEST_MAX_PAGES;
   st.last_returned = 1;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_PAGE_LIMIT, email_digest_next_step(&st));

   st.pages = EMAIL_DIGEST_MAX_PAGES - 1;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_MORE, email_digest_next_step(&st));
}

static void test_max_depth_reachable_within_page_ceiling(void) {
   /* EMAIL_DIGEST_DEPTH_MAX must be reachable in full pages before the ceiling. */
   TEST_ASSERT_TRUE(EMAIL_DIGEST_MAX_PAGES * EMAIL_MAX_FETCH_RESULTS >= EMAIL_DIGEST_DEPTH_MAX);
   email_digest_page_state_t st = base_state();
   st.fetched = EMAIL_DIGEST_DEPTH_MAX;
   st.pages = EMAIL_DIGEST_MAX_PAGES;
   st.depth = EMAIL_DIGEST_DEPTH_MAX;
   TEST_ASSERT_EQUAL_INT(EMAIL_DIGEST_STOP_DEPTH, email_digest_next_step(&st));
}


/* ---- boundary row (reached_cutoff) ---- */

static email_summary_t row(uint32_t uid, time_t date) {
   email_summary_t r;
   memset(&r, 0, sizeof(r));
   r.uid = uid;
   r.date = date;
   return r;
}

static void test_imap_boundary_is_lowest_uid_not_last_element(void) {
   /* Server answered ascending: last element is the NEWEST row. */
   email_summary_t rows[3] = { row(100, 50), row(101, 900), row(102, 950) };
   TEST_ASSERT_TRUE(email_digest_page_reached_cutoff(rows, 3, 500)); /* uid 100 is old */
   email_summary_t fresh[3] = { row(100, 600), row(101, 900), row(102, 50) };
   /* uid 102 carries an old date (moved in); the boundary, uid 100, is in-window. */
   TEST_ASSERT_FALSE(email_digest_page_reached_cutoff(fresh, 3, 500));
}

static void test_imap_moved_old_message_does_not_stop_paging(void) {
   /* uid 200 was moved back into the inbox: newest UID, original (old) date. */
   email_summary_t rows[3] = { row(150, 800), row(151, 850), row(200, 10) };
   TEST_ASSERT_FALSE(email_digest_page_reached_cutoff(rows, 3, 500));
}

static void test_gmail_boundary_is_oldest_dated_row(void) {
   email_summary_t rows[3] = { row(0, 900), row(0, 100), row(0, 950) };
   TEST_ASSERT_TRUE(email_digest_page_reached_cutoff(rows, 3, 500));
   email_summary_t fresh[2] = { row(0, 900), row(0, 600) };
   TEST_ASSERT_FALSE(email_digest_page_reached_cutoff(fresh, 2, 500));
}

static void test_undated_boundary_never_proves_coverage(void) {
   email_summary_t imap[2] = { row(5, 0), row(9, 10) };
   TEST_ASSERT_FALSE(email_digest_page_reached_cutoff(imap, 2, 500));
   email_summary_t gmail[2] = { row(0, 0), row(0, 0) };
   TEST_ASSERT_FALSE(email_digest_page_reached_cutoff(gmail, 2, 500));
   TEST_ASSERT_FALSE(email_digest_page_reached_cutoff(NULL, 0, 500));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_continues_when_more_remains);
   RUN_TEST(test_stops_when_window_covered);
   RUN_TEST(test_window_wins_over_depth);
   RUN_TEST(test_stops_when_exhausted);
   RUN_TEST(test_empty_page_with_token_stops);
   RUN_TEST(test_stops_at_depth);
   RUN_TEST(test_repeated_token_stops);
   RUN_TEST(test_page_ceiling_bounds_short_pages);
   RUN_TEST(test_max_depth_reachable_within_page_ceiling);
   RUN_TEST(test_imap_boundary_is_lowest_uid_not_last_element);
   RUN_TEST(test_imap_moved_old_message_does_not_stop_paging);
   RUN_TEST(test_gmail_boundary_is_oldest_dated_row);
   RUN_TEST(test_undated_boundary_never_proves_coverage);
   return UNITY_END();
}
