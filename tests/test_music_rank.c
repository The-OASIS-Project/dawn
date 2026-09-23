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
 * Unit tests for music_rank.c: folding, token tiers, row scoring, album keys.
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
                    MUSIC_RANK_FIELD_MIN);
   TEST_ASSERT_TRUE(music_rank_field_quality("Ramin Djawadi", "Djawadi") >= MUSIC_RANK_FIELD_MIN);
   TEST_ASSERT_TRUE(music_rank_field_quality("Ben Folds Presents: The Princeton Nassoons",
                                             "Prince") < MUSIC_RANK_FIELD_MIN);
}

/* ================= folding + token tiers ================= */

static void test_fold_basic(void) {
   char out[MUSIC_RANK_FOLD_MAX];
   music_rank_fold("Rockin' the Suburbs", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("rockin the suburbs", out);
   music_rank_fold("AC/DC", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ac dc", out);
   music_rank_fold("Simon & Garfunkel", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("simon and garfunkel", out);
   music_rank_fold("  Don\xE2\x80\x99t  Stop!! ", out, sizeof(out)); /* curly apostrophe */
   TEST_ASSERT_EQUAL_STRING("dont stop", out);
   music_rank_fold("Beyonc\xC3\xA9", out, sizeof(out)); /* UTF-8 kept */
   TEST_ASSERT_EQUAL_STRING("beyonc\xC3\xA9", out);
   music_rank_fold(NULL, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_fold_truncates_safely(void) {
   char out[8];
   size_t n = music_rank_fold("abc def ghi jkl", out, sizeof(out));
   TEST_ASSERT_TRUE(n < sizeof(out));
   TEST_ASSERT_EQUAL_size_t(strlen(out), n);
   TEST_ASSERT_TRUE(n == 0 || out[n - 1] != ' ');
   /* A stray lead byte at end of string must not read past the NUL. */
   music_rank_fold("x\xE2", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("x\xE2", out);
}

static void test_punctuation_mismatch_is_exact(void) {
   /* The live bug: the query omits punctuation the stored title has. */
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT,
                         music_rank_field_quality("Rockin' the Suburbs", "Rockin the Suburbs"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT, music_rank_field_quality("AC/DC", "AC DC"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT,
                         music_rank_field_quality("Guns N' Roses", "Guns N Roses"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT,
                         music_rank_field_quality("Simon & Garfunkel", "Simon and Garfunkel"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT,
                         music_rank_field_quality("Simon and Garfunkel", "Simon & Garfunkel"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT,
                         music_rank_field_quality("Don't Stop Believin'", "dont stop believin"));
}

static void test_tokens_any_order(void) {
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_ALL_WORDS, music_rank_field_quality("Ben Folds", "Folds Ben"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_ALL_WORDS,
                         music_rank_field_quality("Rockin' the Suburbs", "Rockin Suburbs"));
   TEST_ASSERT_TRUE(MUSIC_RANK_ALL_WORDS >= MUSIC_RANK_FIELD_MIN);
}

static void test_missing_token_is_no_match(void) {
   TEST_ASSERT_EQUAL_INT(0, music_rank_field_quality("Ben Folds", "Ben Folds Five"));
   TEST_ASSERT_EQUAL_INT(0, music_rank_field_quality("Rockin' the Suburbs", "Rockin the City"));
}

static void test_mid_word_tokens_rank_low(void) {
   /* Every token present, but only inside longer words. */
   int q = music_rank_field_quality("Stairway to Heaven", "Stair Heav");
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_ALL_PRESENT, q);
   TEST_ASSERT_TRUE(q < MUSIC_RANK_FIELD_MIN); /* excluded by a fielded filter */
   /* Contiguous mid-word run keeps the substring tier. */
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_SUBSTRING, music_rank_field_quality("Ben Folds", "en Fold"));
}

static void test_tier_ordering_constants(void) {
   TEST_ASSERT_TRUE(MUSIC_RANK_EXACT > MUSIC_RANK_PREFIX);
   TEST_ASSERT_TRUE(MUSIC_RANK_PREFIX > MUSIC_RANK_WORD);
   TEST_ASSERT_TRUE(MUSIC_RANK_WORD > MUSIC_RANK_ALL_WORDS);
   TEST_ASSERT_TRUE(MUSIC_RANK_ALL_WORDS > MUSIC_RANK_SUBSTRING);
   TEST_ASSERT_TRUE(MUSIC_RANK_SUBSTRING > MUSIC_RANK_ALL_PRESENT);
   /* Cross-field floors sit below the weakest weighted single-field match
    * (ALL_PRESENT in genre at 60%). */
   TEST_ASSERT_TRUE(MUSIC_RANK_CROSS_WORDS < MUSIC_RANK_ALL_PRESENT * 60 / 100);
   TEST_ASSERT_TRUE(MUSIC_RANK_CROSS_PRESENT > 0);
}

/* ================= row score ================= */

static void test_score_cross_field(void) {
   /* "Artist Title" free text: no single field has every token, the row does. */
   music_search_result_t r = row("Ben Folds", "Rockin' the Suburbs", "Rockin' the Suburbs", "");
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_CROSS_WORDS, music_rank_score(&r, "Ben Folds Suburbs"));
   music_search_result_t other = row("Ben Folds", "Zak and Sara", "Rockin' the Suburbs", "");
   /* Album holds every token → single-field match beats cross-field. */
   TEST_ASSERT_TRUE(music_rank_score(&other, "Rockin Suburbs") >
                    music_rank_score(&r, "Ben Folds Suburbs"));
   /* Two-token query with a token found nowhere → no match. */
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, "Folds Kate"));
}

static void test_score_single_token_never_cross(void) {
   music_search_result_t r = row("Queen", "Bohemian Rhapsody", "", "");
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, "Prince"));
}

static void test_score_fields_null_safe(void) {
   music_rank_needle_t n;
   music_rank_needle_prepare("Queen", &n);
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT, music_rank_score_fields("Queen", NULL, NULL, NULL, &n));
   TEST_ASSERT_EQUAL_INT(0, music_rank_score_fields(NULL, NULL, NULL, NULL, &n));
   music_rank_needle_prepare("!!", &n); /* folds to nothing */
   TEST_ASSERT_EQUAL_INT(0, n.ntok);
   TEST_ASSERT_EQUAL_INT(0, music_rank_score_fields("!!", "!!", "", "", &n));
}

static void test_needle_token_cap(void) {
   music_rank_needle_t n;
   music_rank_needle_prepare("a b c d e f g h i j k l m n o p q r s t", &n);
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_MAX_TOKENS, n.ntok);
}

static void test_filler_words_not_required(void) {
   music_search_result_t r = row("Ben Folds", "Rockin' the Suburbs", "Rockin' the Suburbs", "");
   /* "by" appears in no field, but it's filler — still a (cross-field) match. */
   TEST_ASSERT_TRUE(music_rank_score(&r, "Rockin the Suburbs by Ben Folds") > 0);
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_ALL_WORDS,
                         music_rank_field_quality("Guns N' Roses", "Guns and Roses"));
   /* An all-filler needle requires its tokens. */
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT, music_rank_field_quality("The The", "the the"));
   TEST_ASSERT_EQUAL_INT(0, music_rank_field_quality("Queen", "the"));
}

static void test_short_tokens_whole_word_only(void) {
   /* "dc" must not hit "Jack Dcosta"-style mid-word runs... */
   TEST_ASSERT_EQUAL_INT(0, music_rank_field_quality("Madcap Laughs", "dc"));
   /* ...but matches as a word. */
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT, music_rank_field_quality("AC/DC", "ac dc"));
   TEST_ASSERT_EQUAL_INT(0, music_rank_field_quality("Jackson Browne", "ac dc"));
}

static void test_partial_tier_long_query(void) {
   music_search_result_t r = row("Ben Folds", "Rockin' the Suburbs", "", "");
   /* One typo in a 4-token query → lowest tier, not dropped. */
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_PARTIAL, music_rank_score(&r, "Ben Folds Rockin Suburbz"));
   /* Two misses → no match. */
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, "Ben Xyzzy Rockin Suburbz"));
   /* Short queries get no partial credit. */
   TEST_ASSERT_EQUAL_INT(0, music_rank_score(&r, "Ben Kate"));
   TEST_ASSERT_TRUE(MUSIC_RANK_PARTIAL < MUSIC_RANK_CROSS_PRESENT);
}

static void test_unicode_punctuation_separates(void) {
   /* Curly double quotes, no-break space, en dash act like ASCII punctuation. */
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT,
                         music_rank_field_quality("\xE2\x80\x9CHello\xE2\x80\x9D", "Hello"));
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT, music_rank_field_quality("Rockin' the\xC2\xA0Suburbs",
                                                                    "Rockin the Suburbs"));
   char a[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key("Speed Graphic \xE2\x80\x93 EP", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("speed graphic", a);
}

static void test_album_key_content_words_block_strip(void) {
   char a[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key("Songs for Silverman (Live Edition)", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("songs for silverman live edition", a);
   music_rank_album_key("Rockin' the Suburbs (Acoustic Version) [Deluxe]", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("rockin the suburbs acoustic version", a);
}

static void test_field_match_requires_filler(void) {
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_ALL_WORDS,
                         music_rank_field_quality("Dave Matthews Band", "The Band"));
   /* "the" only occurs mid-word ("Matthews") → below the fielded cutoff. */
   TEST_ASSERT_TRUE(music_rank_field_match("Dave Matthews Band", "The Band") <
                    MUSIC_RANK_FIELD_MIN);
   TEST_ASSERT_EQUAL_INT(MUSIC_RANK_EXACT, music_rank_field_match("The Band", "the band"));
}

/* ================= album key ================= */

static void test_album_key_strips_editions(void) {
   char a[MUSIC_RANK_FOLD_MAX];
   char b[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key("Rockin' the Suburbs (Expanded Edition)", NULL, a, sizeof(a));
   music_rank_album_key("Rockin' The Suburbs", NULL, b, sizeof(b));
   TEST_ASSERT_EQUAL_STRING(b, a);
   music_rank_album_key("Whatever and Ever Amen (Remastered) [Explicit]", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("whatever and ever amen", a);
   music_rank_album_key("So There - Deluxe Edition", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("so there", a);
}

static void test_album_key_formats_from_library(void) {
   /* Real variants seen in the library. */
   char a[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key("Speed Graphic (EP)", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("speed graphic", a);
   music_rank_album_key("iTunes Originals - Ben Folds", "Ben Folds", a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("itunes originals", a);
   music_rank_album_key("iTunes Originals - Ben Folds", "Someone Else", a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("itunes originals ben folds", a);
   music_rank_album_key("Landed - Single", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("landed", a);
   music_rank_album_key("Live - Disc 1", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("live", a);
   music_rank_album_key("Songs for Silverman [Clean]", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("songs for silverman", a);
   music_rank_album_key("Whatever and Ever Amen (Remastered 2005)", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("whatever and ever amen", a);
   /* "ep" is a whole-token match, never a substring of another word. */
   music_rank_album_key("Songs (Deep Cuts)", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("songs deep cuts", a);
   music_rank_album_key("Fearless (Taylor's Version)", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("fearless taylors version", a);
}

static void test_album_key_keeps_content_qualifiers(void) {
   char a[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key("Ben Folds Live (Live)", NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("ben folds live live", a);
   music_rank_album_key("(Deluxe Edition)", NULL, a, sizeof(a)); /* would strip to nothing */
   TEST_ASSERT_EQUAL_STRING("deluxe edition", a);
   music_rank_album_key("The Unauthorized Biography of Reinhold Messner - Reinhold", NULL, a,
                        sizeof(a));
   TEST_ASSERT_EQUAL_STRING("the unauthorized biography of reinhold messner reinhold", a);
   music_rank_album_key(NULL, NULL, a, sizeof(a));
   TEST_ASSERT_EQUAL_STRING("", a);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_score_exact_beats_prefix_beats_substring);
   RUN_TEST(test_score_whole_word_beats_substring);
   RUN_TEST(test_score_artist_weighted_over_album);
   RUN_TEST(test_score_no_match_is_zero);
   RUN_TEST(test_score_empty_needle_is_zero);
   RUN_TEST(test_field_quality_word_boundary);
   RUN_TEST(test_fold_basic);
   RUN_TEST(test_fold_truncates_safely);
   RUN_TEST(test_punctuation_mismatch_is_exact);
   RUN_TEST(test_tokens_any_order);
   RUN_TEST(test_missing_token_is_no_match);
   RUN_TEST(test_mid_word_tokens_rank_low);
   RUN_TEST(test_tier_ordering_constants);
   RUN_TEST(test_score_cross_field);
   RUN_TEST(test_score_single_token_never_cross);
   RUN_TEST(test_score_fields_null_safe);
   RUN_TEST(test_needle_token_cap);
   RUN_TEST(test_album_key_strips_editions);
   RUN_TEST(test_album_key_formats_from_library);
   RUN_TEST(test_album_key_keeps_content_qualifiers);
   RUN_TEST(test_filler_words_not_required);
   RUN_TEST(test_short_tokens_whole_word_only);
   RUN_TEST(test_partial_tier_long_query);
   RUN_TEST(test_unicode_punctuation_separates);
   RUN_TEST(test_album_key_content_words_block_strip);
   RUN_TEST(test_field_match_requires_filler);
   return UNITY_END();
}
