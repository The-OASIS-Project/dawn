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
 * Unit tests for the shared fuzzy name matcher (extracted from Home
 * Assistant, now also used by Discord channel-name resolution).
 */

#include <string.h>

#include "core/str_fuzzy.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static void test_tolower_basic(void) {
   char out[32];
   str_fuzzy_tolower(out, "General-CHAT", sizeof(out));
   TEST_ASSERT_EQUAL_STRING("general-chat", out);
}

static void test_tolower_truncates_and_terminates(void) {
   char out[5];
   str_fuzzy_tolower(out, "abcdefgh", sizeof(out));
   TEST_ASSERT_EQUAL_STRING("abcd", out); /* 4 chars + NUL */
}

static void test_tolower_null_src(void) {
   char out[8] = "xyz";
   str_fuzzy_tolower(out, NULL, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_score_exact(void) {
   TEST_ASSERT_EQUAL_INT(STR_FUZZY_SCORE_EXACT, str_fuzzy_score("general", "general"));
}

static void test_score_substring(void) {
   TEST_ASSERT_EQUAL_INT(STR_FUZZY_SCORE_CONTAINS, str_fuzzy_score("dev general chat", "general"));
}

static void test_score_token_overlap(void) {
   /* Neither exact nor a contiguous substring (tokens are separated in the
    * haystack) → one bonus per matched token. */
   TEST_ASSERT_EQUAL_INT(2 * STR_FUZZY_SCORE_TOKEN_BONUS,
                         str_fuzzy_score("dev general chat", "dev chat"));
}

static void test_score_partial_token_overlap(void) {
   /* Only one of two needle tokens present → one bonus. */
   TEST_ASSERT_EQUAL_INT(STR_FUZZY_SCORE_TOKEN_BONUS,
                         str_fuzzy_score("announcements board", "dev announcements"));
}

static void test_score_no_match(void) {
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_score("announcements", "random"));
}

static void test_score_null_args(void) {
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_score(NULL, "x"));
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_score("x", NULL));
}

/* Two distinct channels both named "general" score identically (100) — the
 * tie the resolver uses to detect cross-server ambiguity. */
static void test_score_tie_is_detectable(void) {
   int a = str_fuzzy_score("general", "general");
   int b = str_fuzzy_score("general", "general");
   TEST_ASSERT_EQUAL_INT(a, b);
   TEST_ASSERT_EQUAL_INT(STR_FUZZY_SCORE_EXACT, a);
}

/* ---- str_fuzzy_ratio (normalized edit-distance similarity) ---- */

static void test_ratio_identical(void) {
   TEST_ASSERT_EQUAL_INT(100, str_fuzzy_ratio("shelley kersey", "shelley kersey"));
}

static void test_ratio_empty_or_null(void) {
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_ratio("", "x"));
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_ratio("x", ""));
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_ratio(NULL, "x"));
   TEST_ASSERT_EQUAL_INT(0, str_fuzzy_ratio("x", NULL));
}

/* The motivating case: an ASR-garbled surname must still read as a near-miss
 * (well above the phone resolver's suggest threshold of 50) where the token
 * scorer alone only sees the shared first name. */
static void test_ratio_asr_garble_is_near_miss(void) {
   int r = str_fuzzy_ratio("shelley kersey", "shelley curzy");
   TEST_ASSERT_TRUE(r >= 50);
   TEST_ASSERT_TRUE(str_fuzzy_score("shelley kersey", "shelley curzy") < 50);
}

/* An unrelated contact must stay well below threshold so it is not offered. */
static void test_ratio_unrelated_is_low(void) {
   TEST_ASSERT_TRUE(str_fuzzy_ratio("john smith", "shelley curzy") < 50);
}

/* Symmetry: edit distance is symmetric, so the ratio must be too. */
static void test_ratio_symmetric(void) {
   TEST_ASSERT_EQUAL_INT(str_fuzzy_ratio("kersey", "curzy"), str_fuzzy_ratio("curzy", "kersey"));
}

static void same_key(const char *a, const char *b) {
   char ka[STR_PHONETIC_MAXLEN + 1], kb[STR_PHONETIC_MAXLEN + 1];
   str_phonetic_key(a, ka, sizeof(ka));
   str_phonetic_key(b, kb, sizeof(kb));
   TEST_ASSERT_EQUAL_STRING_MESSAGE(ka, kb, a);
}

static void different_key(const char *a, const char *b) {
   char ka[STR_PHONETIC_MAXLEN + 1], kb[STR_PHONETIC_MAXLEN + 1];
   str_phonetic_key(a, ka, sizeof(ka));
   str_phonetic_key(b, kb, sizeof(kb));
   TEST_ASSERT_TRUE_MESSAGE(strcmp(ka, kb) != 0, a);
}

/* Names that sound alike share a key; names that don't, don't. */
static void test_phonetic_key(void) {
   same_key("Cris", "Chris");
   same_key("Jon", "John");
   same_key("Sean", "Shawn");
   same_key("Stephen", "Steven");
   same_key("Katherine", "Catherine");
   same_key("Phil", "Fill");
   different_key("Cris", "Mark");
   different_key("Sean", "Steve");
   different_key("Mary", "Gary");
   different_key("Cris", "Christine");
   char k[STR_PHONETIC_MAXLEN + 1];
   str_phonetic_key("Cris Kemp", k, sizeof(k)); /* one word */
   TEST_ASSERT_EQUAL_STRING("krs", k);
   str_phonetic_key(NULL, k, sizeof(k));
   TEST_ASSERT_EQUAL_STRING("", k);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_tolower_basic);
   RUN_TEST(test_tolower_truncates_and_terminates);
   RUN_TEST(test_tolower_null_src);
   RUN_TEST(test_score_exact);
   RUN_TEST(test_score_substring);
   RUN_TEST(test_score_token_overlap);
   RUN_TEST(test_score_partial_token_overlap);
   RUN_TEST(test_score_no_match);
   RUN_TEST(test_score_null_args);
   RUN_TEST(test_score_tie_is_detectable);
   RUN_TEST(test_ratio_identical);
   RUN_TEST(test_ratio_empty_or_null);
   RUN_TEST(test_ratio_asr_garble_is_near_miss);
   RUN_TEST(test_ratio_unrelated_is_low);
   RUN_TEST(test_ratio_symmetric);
   RUN_TEST(test_phonetic_key);
   return UNITY_END();
}
