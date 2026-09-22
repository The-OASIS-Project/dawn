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
 * Unit tests for audio_metadata_util.c: year parsing across tag date forms and
 * multi-value genre comma-joining with bounds.
 */

#include <string.h>

#include "audio/audio_decoder.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* =========================================================================
 * audio_metadata_parse_year
 * ========================================================================= */

static void test_year_plain(void) {
   TEST_ASSERT_EQUAL_UINT32(1985, audio_metadata_parse_year("1985"));
}

static void test_year_full_date_iso(void) {
   TEST_ASSERT_EQUAL_UINT32(1985, audio_metadata_parse_year("1985-06-01"));
}

static void test_year_slash_forms(void) {
   TEST_ASSERT_EQUAL_UINT32(1985, audio_metadata_parse_year("1985/06"));
   TEST_ASSERT_EQUAL_UINT32(1985, audio_metadata_parse_year("06/1985"));
}

static void test_year_null_and_empty(void) {
   TEST_ASSERT_EQUAL_UINT32(0, audio_metadata_parse_year(NULL));
   TEST_ASSERT_EQUAL_UINT32(0, audio_metadata_parse_year(""));
}

static void test_year_no_four_digit_run(void) {
   /* Three or five consecutive digits must not be read as a year. */
   TEST_ASSERT_EQUAL_UINT32(0, audio_metadata_parse_year("198"));
   TEST_ASSERT_EQUAL_UINT32(0, audio_metadata_parse_year("track 12"));
   TEST_ASSERT_EQUAL_UINT32(0, audio_metadata_parse_year("123456"));
}

static void test_year_out_of_range(void) {
   /* 0000-0999 is below the plausible floor. */
   TEST_ASSERT_EQUAL_UINT32(0, audio_metadata_parse_year("0999"));
}

static void test_year_embedded_in_text(void) {
   TEST_ASSERT_EQUAL_UINT32(1999, audio_metadata_parse_year("recorded 1999 live"));
}

/* =========================================================================
 * audio_metadata_append_genre
 * ========================================================================= */

static void test_genre_first_value(void) {
   audio_metadata_t m = { 0 };
   audio_metadata_append_genre(&m, "Rock");
   TEST_ASSERT_EQUAL_STRING("Rock", m.genre);
}

static void test_genre_comma_join(void) {
   audio_metadata_t m = { 0 };
   audio_metadata_append_genre(&m, "Rock");
   audio_metadata_append_genre(&m, "Pop");
   audio_metadata_append_genre(&m, "Jazz");
   TEST_ASSERT_EQUAL_STRING("Rock, Pop, Jazz", m.genre);
}

static void test_genre_ignores_null_and_empty(void) {
   audio_metadata_t m = { 0 };
   audio_metadata_append_genre(&m, NULL);
   audio_metadata_append_genre(&m, "");
   TEST_ASSERT_EQUAL_STRING("", m.genre);
   audio_metadata_append_genre(&m, "Rock");
   audio_metadata_append_genre(&m, "");
   TEST_ASSERT_EQUAL_STRING("Rock", m.genre);
}

static void test_genre_bounded_no_overflow(void) {
   audio_metadata_t m = { 0 };
   /* Append many values; result must stay within the fixed buffer and be
    * NUL-terminated (no overflow, no unterminated string). */
   for (int i = 0; i < 50; i++) {
      audio_metadata_append_genre(&m, "Electronic");
   }
   TEST_ASSERT_TRUE(strlen(m.genre) < AUDIO_GENRE_STRING_MAX);
   TEST_ASSERT_EQUAL_CHAR('\0', m.genre[AUDIO_GENRE_STRING_MAX - 1]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_year_plain);
   RUN_TEST(test_year_full_date_iso);
   RUN_TEST(test_year_slash_forms);
   RUN_TEST(test_year_null_and_empty);
   RUN_TEST(test_year_no_four_digit_run);
   RUN_TEST(test_year_out_of_range);
   RUN_TEST(test_year_embedded_in_text);
   RUN_TEST(test_genre_first_value);
   RUN_TEST(test_genre_comma_join);
   RUN_TEST(test_genre_ignores_null_and_empty);
   RUN_TEST(test_genre_bounded_no_overflow);
   return UNITY_END();
}
