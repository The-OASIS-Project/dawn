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
 * Unit tests for music_rank.c: field-weighted scoring and threshold/cap select.
 */

#include <string.h>

#include "audio/music_rank.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static music_search_result_t row(const char *artist,
                                 const char *title,
                                 const char *album,
                                 const char *genre) {
   music_search_result_t r;
   memset(&r, 0, sizeof(r));
   if (artist)
      strncpy(r.artist, artist, sizeof(r.artist) - 1);
   if (title)
      strncpy(r.title, title, sizeof(r.title) - 1);
   if (album)
      strncpy(r.album, album, sizeof(r.album) - 1);
   if (genre)
      strncpy(r.genre, genre, sizeof(r.genre) - 1);
   return r;
}

/* ================= scoring tiers ================= */

static void test_score_exact_beats_prefix_beats_substring(void) {
   music_search_result_t exact = row("Prince", "x", "", "");
   music_search_result_t prefix = row("Prince and the Revolution", "x", "", "");
   music_search_result_t sub = row("The Fresh Prince", "x", "", ""); /* trailing whole word */
   int se = music_rank_score(&exact, "Prince");
   int sp = music_rank_score(&prefix, "Prince");
   int ss = music_rank_score(&sub, "Prince");
   TEST_ASSERT_TRUE(se > sp);
   TEST_ASSERT_TRUE(sp > ss);
   TEST_ASSERT_TRUE(ss > 0);
}

static void test_score_whole_word_beats_substring(void) {
   /* "rock" as a whole word ("Hard Rock") should beat a mid-word substring
    * ("Blackrock"). */
   music_search_result_t word = row("x", "y", "z", "Hard Rock");
   music_search_result_t midword = row("x", "Blackrock & Roll", "z", "");
   TEST_ASSERT_TRUE(music_rank_score(&word, "rock") > music_rank_score(&midword, "rock"));
}

static void test_score_artist_weighted_over_album(void) {
   /* Same match quality (exact) but artist should outweigh album. */
   music_search_result_t a = row("Africa", "x", "", "");
   music_search_result_t b = row("x", "y", "Africa", "");
   TEST_ASSERT_TRUE(music_rank_score(&a, "Africa") > music_rank_score(&b, "Africa"));
}

static void test_score_no_match_is_zero(void) {
   music_search_result_t r = row("Metallica", "Enter Sandman", "Black Album", "Metal");
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, "Prince"));
}

static void test_score_empty_needle_is_zero(void) {
   music_search_result_t r = row("Prince", "1999", "", "");
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, ""));
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, NULL));
}

static void test_field_quality_word_boundary(void) {
   /* The cutoff the fielded artist/title filter uses: word-boundary-or-better is
    * kept, a mid-word substring is dropped. So artist:"Prince" keeps "Prince and
    * the Revolution" (prefix) but excludes "…Princeton Nassoons" (mid-word). */
   TEST_ASSERT_TRUE(music_rank_field_quality("Prince and the Revolution", "Prince") >=
                    MUSIC_RANK_WORD);
   TEST_ASSERT_TRUE(music_rank_field_quality("Ramin Djawadi", "Djawadi") >= MUSIC_RANK_WORD);
   TEST_ASSERT_TRUE(music_rank_field_quality("Ben Folds Presents: The Princeton Nassoons",
                                             "Prince") < MUSIC_RANK_WORD);
}

/* ================= select: threshold + cap + ordering ================= */

static void test_select_ranks_exact_above_substring(void) {
   /* Breadth model: exact-artist Prince ranks first; weak substring matches
    * (Princeton/Princess) are kept but sorted BELOW, not dropped. */
   music_search_result_t rows[] = {
      row("Ben Folds Presents: The Princeton Nassoons", "Time", "", ""), /* substring */
      row("John Williams", "Princess Leia's Theme", "Star Wars", ""),    /* substring */
      row("Prince", "1999", "1999", ""),                                 /* exact artist */
      row("Prince", "Kiss", "Parade", ""),                               /* exact artist */
   };
   int order[4];
   int n = music_rank_select(rows, 4, "Prince", 10, order);
   TEST_ASSERT_EQUAL_INT(4, n);                               /* all matches returned (breadth) */
   TEST_ASSERT_EQUAL_STRING("Prince", rows[order[0]].artist); /* exact ranks first */
   TEST_ASSERT_EQUAL_STRING("Prince", rows[order[1]].artist);
}

static void test_select_ranks_exact_artist_first(void) {
   /* "Queen" ranks the band first; titles that start with / contain "Queen"
    * follow lower — kept, so the caller (the LLM) can still choose them. */
   music_search_result_t rows[] = {
      row("Candace", "Queen Of Mars", "", ""),                      /* title prefix */
      row("John Williams", "Queen Amidala and the Palace", "", ""), /* title prefix */
      row("Everclear", "Queen Of The Air", "", ""),                 /* title prefix */
      row("Queen", "I Want To Break Free", "The Works", ""),        /* exact artist */
   };
   int order[4];
   int n = music_rank_select(rows, 4, "Queen", 10, order);
   TEST_ASSERT_EQUAL_INT(4, n);                              /* breadth: partials kept */
   TEST_ASSERT_EQUAL_STRING("Queen", rows[order[0]].artist); /* exact artist ranks first */
}

static void test_select_orders_best_first(void) {
   music_search_result_t rows[] = {
      row("The Fresh Prince", "x", "", ""), /* whole-word (trailing token) */
      row("Prince", "1999", "", ""),        /* exact */
   };
   int order[2];
   int n = music_rank_select(rows, 2, "Prince", 10, order);
   TEST_ASSERT_TRUE(n >= 1);
   TEST_ASSERT_EQUAL_STRING("Prince", rows[order[0]].artist); /* exact first */
}

static void test_select_respects_max_cap(void) {
   music_search_result_t rows[] = {
      row("Queen", "A", "", ""),
      row("Queen", "B", "", ""),
      row("Queen", "C", "", ""),
   };
   int order[3];
   int n = music_rank_select(rows, 3, "Queen", 2, order);
   TEST_ASSERT_EQUAL_INT(2, n); /* capped at max even though 3 match equally */
}

static void test_select_empty_text_keeps_input_order_capped(void) {
   music_search_result_t rows[] = {
      row("A", "1", "", ""),
      row("B", "2", "", ""),
      row("C", "3", "", ""),
   };
   int order[3];
   int n = music_rank_select(rows, 3, NULL, 2, order);
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT(0, order[0]);
   TEST_ASSERT_EQUAL_INT(1, order[1]);
}

static void test_select_zero_and_guards(void) {
   music_search_result_t r = row("Prince", "1999", "", "");
   int order[1];
   TEST_ASSERT_EQUAL_INT(0, music_rank_select(&r, 0, "Prince", 10, order));
   TEST_ASSERT_EQUAL_INT(0, music_rank_select(&r, 1, "Prince", 0, order));
   TEST_ASSERT_EQUAL_INT(0, music_rank_select(NULL, 1, "Prince", 10, order));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_score_exact_beats_prefix_beats_substring);
   RUN_TEST(test_score_whole_word_beats_substring);
   RUN_TEST(test_score_artist_weighted_over_album);
   RUN_TEST(test_score_no_match_is_zero);
   RUN_TEST(test_score_empty_needle_is_zero);
   RUN_TEST(test_field_quality_word_boundary);
   RUN_TEST(test_select_ranks_exact_above_substring);
   RUN_TEST(test_select_ranks_exact_artist_first);
   RUN_TEST(test_select_orders_best_first);
   RUN_TEST(test_select_respects_max_cap);
   RUN_TEST(test_select_empty_text_keeps_input_order_capped);
   RUN_TEST(test_select_zero_and_guards);
   return UNITY_END();
}
