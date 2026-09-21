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
 * Unit tests for common/src/tts/tts_preprocessing.cpp via the C wrapper.
 */

#include <string.h>

#include "tts/tts_preprocessing.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── remove_chars ────────────────────────────────────────────────────────── */

static void test_remove_chars_basic(void) {
   char buf[64] = "**hello** world";
   remove_chars(buf, "*");
   TEST_ASSERT_EQUAL_STRING("hello world", buf);
}

static void test_remove_chars_multiple_chars(void) {
   char buf[64] = "(hello) [world]";
   remove_chars(buf, "()[]");
   TEST_ASSERT_EQUAL_STRING("hello world", buf);
}

static void test_remove_chars_no_match(void) {
   char buf[64] = "hello world";
   remove_chars(buf, "xyz");
   TEST_ASSERT_EQUAL_STRING("hello world", buf);
}

static void test_remove_chars_empty_input(void) {
   char buf[8] = "";
   remove_chars(buf, "*");
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_remove_chars_null_safe(void) {
   /* Should not crash */
   remove_chars(NULL, "x");
   char buf[8] = "abc";
   remove_chars(buf, NULL);
   TEST_ASSERT_EQUAL_STRING("abc", buf);
}

/* ── is_emoji ────────────────────────────────────────────────────────────── */

static void test_is_emoji_ascii(void) {
   TEST_ASSERT_FALSE(is_emoji('A'));
   TEST_ASSERT_FALSE(is_emoji(' '));
   TEST_ASSERT_FALSE(is_emoji('1'));
}

static void test_is_emoji_emoji_block(void) {
   /* U+1F600 grinning face */
   TEST_ASSERT_TRUE(is_emoji(0x1F600));
   /* U+1F4A9 pile of poo */
   TEST_ASSERT_TRUE(is_emoji(0x1F4A9));
}

static void test_is_emoji_misc_symbols(void) {
   /* U+2600 (☀) sun */
   TEST_ASSERT_TRUE(is_emoji(0x2600));
   /* U+2764 (❤) heart */
   TEST_ASSERT_TRUE(is_emoji(0x2764));
}

static void test_is_emoji_zwj_and_variation_selector(void) {
   /* U+200D Zero Width Joiner */
   TEST_ASSERT_TRUE(is_emoji(0x200D));
   /* U+FE0F Variation Selector-16 */
   TEST_ASSERT_TRUE(is_emoji(0xFE0F));
}

static void test_is_emoji_normal_unicode(void) {
   /* U+00E9 é (Latin small e with acute) */
   TEST_ASSERT_FALSE(is_emoji(0x00E9));
   /* U+4E2D 中 (Chinese) */
   TEST_ASSERT_FALSE(is_emoji(0x4E2D));
}

/* ── remove_emojis ───────────────────────────────────────────────────────── */

static void test_remove_emojis_simple(void) {
   /* "Hello 👋 world" — U+1F44B (4 bytes UTF-8: F0 9F 91 8B) */
   char buf[64] = "Hello \xF0\x9F\x91\x8B world";
   remove_emojis(buf);
   TEST_ASSERT_EQUAL_STRING("Hello  world", buf);
}

static void test_remove_emojis_preserves_normal_unicode(void) {
   /* "café" — é is U+00E9 (2 bytes: C3 A9) */
   char buf[64] = "caf\xC3\xA9";
   remove_emojis(buf);
   TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9", buf);
}

static void test_remove_emojis_only_ascii(void) {
   char buf[64] = "no emoji here";
   remove_emojis(buf);
   TEST_ASSERT_EQUAL_STRING("no emoji here", buf);
}

static void test_remove_emojis_null_safe(void) {
   remove_emojis(NULL);
   /* Just ensures no crash */
   TEST_PASS();
}

/* ── preprocess_text_for_tts_c: NULL/error handling ──────────────────────── */

static void test_preprocess_null_input(void) {
   char out[64];
   int written = -1;
   TEST_ASSERT_EQUAL_INT(1, preprocess_text_for_tts_c(NULL, out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_INT(0, written);
}

static void test_preprocess_null_output(void) {
   int written = -1;
   TEST_ASSERT_EQUAL_INT(1, preprocess_text_for_tts_c("hello", NULL, 64, &written));
   TEST_ASSERT_EQUAL_INT(0, written);
}

static void test_preprocess_zero_buffer(void) {
   char out[64];
   int written = -1;
   TEST_ASSERT_EQUAL_INT(1, preprocess_text_for_tts_c("hello", out, 0, &written));
}

static void test_preprocess_null_bytes_written_ok(void) {
   /* bytes_written may be NULL — should still succeed */
   char out[64];
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("hello", out, sizeof(out), NULL));
   TEST_ASSERT_EQUAL_STRING("hello", out);
}

/* ── preprocess_text_for_tts_c: passthrough cases ────────────────────────── */

static void test_preprocess_plain_text(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Hello world", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("Hello world", out);
   TEST_ASSERT_EQUAL_INT(11, written);
}

static void test_preprocess_empty_string(void) {
   char out[64] = "preset";
   int written = -1;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("", out);
   TEST_ASSERT_EQUAL_INT(0, written);
}

/* ── preprocess_text_for_tts_c: transformations ──────────────────────────── */

static void test_preprocess_strips_asterisks(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("**bold** text", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("bold text", out);
}

/* Multi-hash runs (`##`, `###`, etc.) inline must be stripped so Piper doesn't
 * read "hash hash hash" out loud.  Single '#' is preserved so C#, hashtags,
 * and "#1" still vocalize naturally.  The leading-markdown scrubber upstream
 * of the per-char loop already handles `# heading` at line start; these
 * cases pin the inline branch. */
static void test_preprocess_collapses_inline_double_hash(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("see section ## 3 here", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_EQUAL_STRING("see section  3 here", out);
}

static void test_preprocess_collapses_inline_triple_hash(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("foo ### heading inline", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_EQUAL_STRING("foo  heading inline", out);
}

static void test_preprocess_collapses_long_hash_run(void) {
   char out[64];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("a ######## b", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("a  b", out);
}

static void test_preprocess_preserves_single_hash(void) {
   char out[64];
   int written = 0;
   /* C#, hashtags, and "#1" must vocalize naturally — single '#' stays. */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("I write C# code", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("I write C# code", out);

   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("#trending now", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("#trending now", out);
}

static void test_preprocess_leading_header_still_stripped(void) {
   char out[64];
   int written = 0;
   /* The line-start scrubber already handled "## heading" at the very start —
    * the per-char loop never sees these '#'s.  Pinning this so the new inline
    * rule doesn't accidentally double-strip and consume content. */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("## Heading line", out, sizeof(out), &written));
   TEST_ASSERT_EQUAL_STRING("Heading line", out);
}

static void test_preprocess_strips_emoji(void) {
   char out[64];
   int written = 0;
   /* "ok 👍" — U+1F44D F0 9F 91 8D */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("ok \xF0\x9F\x91\x8D", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_EQUAL_STRING("ok ", out);
}

static void test_preprocess_em_dash_to_comma(void) {
   char out[64];
   int written = 0;
   /* em-dash U+2014 = E2 80 94 */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("yes\xE2\x80\x94no", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_EQUAL_STRING("yes,no", out);
}

static void test_preprocess_temperature_fahrenheit(void) {
   char out[128];
   int written = 0;
   /* "72°F" — degree sign U+00B0 = C2 B0 */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("72\xC2\xB0"
                                                      "F outside",
                                                      out, sizeof(out), &written));
   /* Output: "72 degrees Fahrenheit outside" */
   TEST_ASSERT_NOT_NULL(strstr(out, "degrees Fahrenheit"));
}

static void test_preprocess_temperature_celsius(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("22\xC2\xB0"
                                                      "C",
                                                      out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "degrees Celsius"));
}

static void test_preprocess_currency_dollar_singular(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("$1", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 dollar"));
   /* Should NOT contain "dollars" (plural) */
   TEST_ASSERT_NULL(strstr(out, "dollars"));
}

static void test_preprocess_currency_dollar_plural(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("$5", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "5 dollars"));
}

static void test_preprocess_currency_magnitude_million(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("$1M", out, sizeof(out), &written));
   /* "1 million dollars" */
   TEST_ASSERT_NOT_NULL(strstr(out, "million"));
   TEST_ASSERT_NOT_NULL(strstr(out, "dollars"));
}

/* ── Large-number reading (number_to_words integration) ──────────────────── */

static void test_number_small_unchanged(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("I have 52 apples", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "52")); /* small number left for espeak */
}

static void test_number_year_unchanged(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("in 2024 we", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "2024"));
}

static void test_number_time_unchanged(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("at 3:30 today", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "3:30"));
}

static void test_number_trillions_to_words(void) {
   char out[512];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("it is 1234567890123 now", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "trillion"));
   TEST_ASSERT_NULL(strstr(out, "1234567890123")); /* digits replaced by words */
}

static void test_number_commas_to_words(void) {
   char out[512];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("about 1,234,567,890,123 things", out,
                                                      sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "trillion"));
}

static void test_number_negative_to_words(void) {
   char out[512];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("balance -1234567890123 dollars", out,
                                                      sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "negative one trillion"));
}

static void test_number_factorial_result(void) {
   char out[1024];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(
       0, preprocess_text_for_tts_c(
              "equals 80658175170943878571660636856403766975289505440883277824000000000000 today",
              out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "unvigintillion"));
}

static void test_preprocess_state_abbrev(void) {
   char out[128];
   int written = 0;
   /* "CA" should expand to "California" with proper word boundaries */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Visit CA today", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "California"));
}

/* Ambiguous state codes (ID, OK, OR, ...) expand ONLY with positive context. */
static void test_state_collider_city_comma_expands(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Boise, ID", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "Idaho"));
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Portland, OR", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "Oregon"));
   /* multi-word city: the last word before the comma is the capitalized token */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("New York, NY", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "New York")); /* NY -> New York */
}

/* Bare ambiguous codes are left for espeak (spelled/word), not expanded. */
static void test_state_collider_bare_not_expanded(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Show me your ID", out, sizeof(out), &written));
   TEST_ASSERT_NULL(strstr(out, "Idaho"));
   TEST_ASSERT_NOT_NULL(strstr(out, "ID"));
   /* the live bug: sentence-initial "OK," must not become "Oklahoma," */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("OK, here's the plan", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NULL(strstr(out, "Oklahoma"));
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("in the OR now", out, sizeof(out), &written));
   TEST_ASSERT_NULL(strstr(out, "Oregon"));
   /* interjection stoplist: capitalized word before the comma is not a city */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Yes, OK, I'll do it", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NULL(strstr(out, "Oklahoma"));
}

/* Comma-gated ZIP is a state signal; an ID + number without a comma is not. */
static void test_state_collider_comma_zip(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Apt 4, ID 83702", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "Idaho 8 3 7 0 2")); /* ZIP spelled digit-by-digit */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("order ID 83702", out, sizeof(out), &written));
   TEST_ASSERT_NULL(strstr(out, "Idaho")); /* no comma -> not a state */
   /* 5 digits run into a letter -> not a ZIP */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Apt 4, ID 12345x", out, sizeof(out), &written));
   TEST_ASSERT_NULL(strstr(out, "Idaho"));
   /* ZIP+4: the '-' is a valid boundary after 5 digits (non-city, so only the
    * comma-ZIP signal can fire here). */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Apt 4, ID 83702-1234", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "Idaho"));
}

/* Non-collider codes keep expanding unconditionally (no context needed). */
static void test_state_noncollider_always_expands(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Visit CA today", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "California")); /* CA is not a collider */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("Washington, DC", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "D.C."));
}

static void test_preprocess_day_abbrev(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Mon meeting", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "Monday"));
}

static void test_preprocess_month_abbrev(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Jan 1st", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "January"));
}

/* ── phone-number expansion ──────────────────────────────────────────────── */

static void test_phone_e164_spelled(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Call +16786432695 now", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 6 7 8 6 4 3 2 6 9 5")); /* spelled digit-by-digit */
   TEST_ASSERT_NULL(strstr(out, "16786432695"));               /* no unspaced run */
}

static void test_phone_e164_with_formatting(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("at +1 (678) 643-2695.", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 6 7 8 6 4 3 2 6 9 5"));
   TEST_ASSERT_NULL(strstr(out, "(678)")); /* punctuation consumed */
   TEST_ASSERT_NULL(strstr(out, "+"));     /* the '+' is dropped, not leaked */
}

static void test_phone_us_parens(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Reach me at (678) 643-2695.", out,
                                                      sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "6 7 8 6 4 3 2 6 9 5"));
   TEST_ASSERT_NOT_NULL(strstr(out, ".")); /* sentence period preserved */
}

static void test_phone_us_dashes_with_country_code(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("dial 1-678-643-2695 please", out,
                                                      sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 6 7 8 6 4 3 2 6 9 5"));
}

static void test_phone_bare_run_untouched(void) {
   char out[256];
   int written = 0;
   /* No '+' and no separators — treated as an ordinary number, left for espeak. */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("order 6786432695 shipped", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "6786432695"));
}

static void test_phone_oversized_groups_rejected(void) {
   char out[256];
   int written = 0;
   /* 5+5 grouping is not a phone number (groups exceed 4 digits). */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("codes 12345 67890 here", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "12345 67890"));
}

static void test_phone_year_untouched(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("back in 2024", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "2024")); /* too short to be a phone number */
}

static void test_phone_plus_below_e164_min_untouched(void) {
   char out[128];
   int written = 0;
   /* 7 digits after '+' is below the E.164 minimum — not a phone. */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("code +1234567 x", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "+1234567"));
}

static void test_phone_trailing_number_not_merged(void) {
   char out[256];
   int written = 0;
   /* Finding #1: a phone followed by " <digit>" must still expand (retry with
    * space-separator disabled), and the trailing number stays intact. */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("dial 678-643-2695 2 times", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "6 7 8 6 4 3 2 6 9 5"));
   TEST_ASSERT_NOT_NULL(strstr(out, "2 times"));
}

static void test_phone_two_in_one_sentence(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("call 678-643-2695 or 555.123.4567", out,
                                                      sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "6 7 8 6 4 3 2 6 9 5"));
   TEST_ASSERT_NOT_NULL(strstr(out, "5 5 5 1 2 3 4 5 6 7"));
}

static void test_phone_leading_nonone_11_untouched(void) {
   char out[256];
   int written = 0;
   /* Finding #4: an 11-digit dashed number NOT starting with 1 is not a US
    * phone, and must not have its 10-digit tail mangled. */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("id 2-678-643-2695 ok", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "2-678-643-2695"));
}

/* An IPv4 literal is now verbalized (was passed through). The phone pass still
 * must not treat it as a phone number ({3,3,1,3} is not a phone shape). */
static void test_ipv4_verbalized(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("host 192.168.1.100 up", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 9 2 dot 1 6 8 dot 1 dot 1 0 0")); /* digit-by-digit */
   TEST_ASSERT_NULL(strstr(out, "192.168")); /* no raw dotted run left */
}

/* Non-IP dotted/numeric shapes must pass through unchanged. */
static void test_ipv4_negatives(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("version 1.2.3 now", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1.2.3")); /* semver: 3 groups, not an IP */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("pi is 3.14", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "3.14")); /* decimal */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("999.1.1.1 bad", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "999.1.1.1")); /* octet > 255 */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("1.2.3.4.5 nope", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1.2.3.4.5")); /* 5 groups */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("build v1.2.3.4", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "v1.2.3.4")); /* alnum prefix -> not an IP */
}

/* An IP inside a URL is left to the URL pass (which speaks the host and drops the
 * path). If the IP pass fired first it would break the URL parse and leak the
 * path — so the guard is verified by the path NOT appearing in the output. */
static void test_ipv4_in_url_guarded(void) {
   char out[256];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("http://192.168.1.1:3000/api", out,
                                                      sizeof(out), &written));
   TEST_ASSERT_NULL(strstr(out, "api")); /* path dropped by the URL pass -> guard held */
   TEST_ASSERT_NOT_NULL(strstr(out, "192 dot 168 dot 1 dot 1")); /* URL pass spoke the host */
}

/* A bare IP with a trailing port/CIDR verbalizes the IP; the suffix stays. */
static void test_ipv4_bare_with_suffix(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("10.0.0.1/24", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 0 dot 0 dot 0 dot 1"));
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("192.168.1.1:8080", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1 9 2 dot 1 6 8 dot 1 dot 1"));
}

/* Override map: only the specific espeak-misread tokens are rewritten. */
static void test_override_terms(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("use IPv4 here", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "I.P.v. four"));
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("IPv4/IPv6", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "I.P.v. four"));
   TEST_ASSERT_NOT_NULL(strstr(out, "I.P.v. six"));
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("(IPv4)", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "I.P.v. four")); /* punctuation boundaries */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("on AWS now", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "A.W.S."));
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("the MAC address", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "mac address"));
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("400 kHz clock", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "kilohertz"));
}

/* Override map must NOT fire inside a longer word or on the wrong case. */
static void test_override_boundaries(void) {
   char out[128];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("the CIRCUS act", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "CIRCUS")); /* "CI" must not fire mid-word */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("lowercase ipv4 text", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "ipv4")); /* exact-case: lowercase untouched */
   TEST_ASSERT_NULL(strstr(out, "version"));
   /* espeak already spells these correctly, so we leave them untouched. */
   TEST_ASSERT_EQUAL_INT(0,
                         preprocess_text_for_tts_c("the FBI report", out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "FBI"));
}

static void test_phone_iso_datetime_untouched(void) {
   char out[128];
   int written = 0;
   /* Finding #3: "YYYY-MM-DD HH:MM" ({4,2,2} + time) is not a phone shape. */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("on 2026-06-16 12:30 today", out, sizeof(out),
                                                      &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "2026-06-16"));
   TEST_ASSERT_NOT_NULL(strstr(out, "12:30"));
}

static void test_phone_adversarial_bounded(void) {
   /* Security: a long separator-dense run must return promptly (bounded scan)
    * and pass through unchanged — pins the O(n) behavior. */
   char in[600];
   for (int k = 0; k < 299; k++) {
      in[2 * k] = '1';
      in[2 * k + 1] = '.';
   }
   in[598] = '1';
   in[599] = '\0';
   char out[700];
   int written = 0;
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c(in, out, sizeof(out), &written));
   TEST_ASSERT_NOT_NULL(strstr(out, "1.1.1")); /* left as ordinary text */
}

/* ── preprocess_text_for_tts_c: buffer truncation ────────────────────────── */

static void test_preprocess_buffer_too_small(void) {
   char out[6];
   int written = 0;
   /* "Hello world" is 11 chars but buffer is only 6 (5 + null) */
   TEST_ASSERT_EQUAL_INT(0, preprocess_text_for_tts_c("Hello world", out, sizeof(out), &written));
   /* Output should be null-terminated and 5 chars */
   TEST_ASSERT_EQUAL_INT(5, written);
   TEST_ASSERT_EQUAL_INT('\0', out[5]);
   TEST_ASSERT_EQUAL_INT(5, (int)strlen(out));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* remove_chars */
   RUN_TEST(test_remove_chars_basic);
   RUN_TEST(test_remove_chars_multiple_chars);
   RUN_TEST(test_remove_chars_no_match);
   RUN_TEST(test_remove_chars_empty_input);
   RUN_TEST(test_remove_chars_null_safe);

   /* is_emoji */
   RUN_TEST(test_is_emoji_ascii);
   RUN_TEST(test_is_emoji_emoji_block);
   RUN_TEST(test_is_emoji_misc_symbols);
   RUN_TEST(test_is_emoji_zwj_and_variation_selector);
   RUN_TEST(test_is_emoji_normal_unicode);

   /* remove_emojis */
   RUN_TEST(test_remove_emojis_simple);
   RUN_TEST(test_remove_emojis_preserves_normal_unicode);
   RUN_TEST(test_remove_emojis_only_ascii);
   RUN_TEST(test_remove_emojis_null_safe);

   /* preprocess: error handling */
   RUN_TEST(test_preprocess_null_input);
   RUN_TEST(test_preprocess_null_output);
   RUN_TEST(test_preprocess_zero_buffer);
   RUN_TEST(test_preprocess_null_bytes_written_ok);

   /* preprocess: passthrough */
   RUN_TEST(test_preprocess_plain_text);
   RUN_TEST(test_preprocess_empty_string);

   /* preprocess: transformations */
   RUN_TEST(test_preprocess_strips_asterisks);
   RUN_TEST(test_preprocess_collapses_inline_double_hash);
   RUN_TEST(test_preprocess_collapses_inline_triple_hash);
   RUN_TEST(test_preprocess_collapses_long_hash_run);
   RUN_TEST(test_preprocess_preserves_single_hash);
   RUN_TEST(test_preprocess_leading_header_still_stripped);
   RUN_TEST(test_preprocess_strips_emoji);
   RUN_TEST(test_preprocess_em_dash_to_comma);
   RUN_TEST(test_preprocess_temperature_fahrenheit);
   RUN_TEST(test_preprocess_temperature_celsius);
   RUN_TEST(test_preprocess_currency_dollar_singular);
   RUN_TEST(test_preprocess_currency_dollar_plural);
   RUN_TEST(test_preprocess_currency_magnitude_million);
   RUN_TEST(test_number_small_unchanged);
   RUN_TEST(test_number_year_unchanged);
   RUN_TEST(test_number_time_unchanged);
   RUN_TEST(test_number_trillions_to_words);
   RUN_TEST(test_number_commas_to_words);
   RUN_TEST(test_number_negative_to_words);
   RUN_TEST(test_number_factorial_result);
   RUN_TEST(test_preprocess_state_abbrev);
   RUN_TEST(test_state_collider_city_comma_expands);
   RUN_TEST(test_state_collider_bare_not_expanded);
   RUN_TEST(test_state_collider_comma_zip);
   RUN_TEST(test_state_noncollider_always_expands);
   RUN_TEST(test_preprocess_day_abbrev);
   RUN_TEST(test_preprocess_month_abbrev);

   RUN_TEST(test_phone_e164_spelled);
   RUN_TEST(test_phone_e164_with_formatting);
   RUN_TEST(test_phone_us_parens);
   RUN_TEST(test_phone_us_dashes_with_country_code);
   RUN_TEST(test_phone_bare_run_untouched);
   RUN_TEST(test_phone_oversized_groups_rejected);
   RUN_TEST(test_phone_year_untouched);
   RUN_TEST(test_phone_plus_below_e164_min_untouched);
   RUN_TEST(test_phone_trailing_number_not_merged);
   RUN_TEST(test_phone_two_in_one_sentence);
   RUN_TEST(test_phone_leading_nonone_11_untouched);
   RUN_TEST(test_ipv4_verbalized);
   RUN_TEST(test_ipv4_negatives);
   RUN_TEST(test_ipv4_in_url_guarded);
   RUN_TEST(test_ipv4_bare_with_suffix);
   RUN_TEST(test_override_terms);
   RUN_TEST(test_override_boundaries);
   RUN_TEST(test_phone_iso_datetime_untouched);
   RUN_TEST(test_phone_adversarial_bounded);

   /* preprocess: truncation */
   RUN_TEST(test_preprocess_buffer_too_small);

   return UNITY_END();
}
