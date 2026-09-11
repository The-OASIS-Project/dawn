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
 * Unit tests for the IMAP email parsing helpers (tools/email_parse.c):
 * RFC 2822 dates (timezone-aware), IMAP INTERNALDATE, and FLAGS membership.
 */

#include "tools/email_parse.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ============================================================================
 * email_parse_rfc822_date — timezone-aware, 0 on failure
 * ============================================================================ */

/* A UTC (+0000) timestamp resolves to a known epoch.
 * 2026-03-13 10:30:00 UTC = 1773397800. */
static void test_rfc822_utc(void) {
   TEST_ASSERT_EQUAL_INT64(1773397800, email_parse_rfc822_date("Thu, 13 Mar 2026 10:30:00 +0000"));
}

/* The SAME wall-clock time in a +0500 zone is EARLIER in absolute terms by 5h
 * (18000s): 1773397800 - 18000 = 1773379800.  This is the bug the %z fix
 * closes — the old parser ignored the offset. */
static void test_rfc822_positive_offset(void) {
   TEST_ASSERT_EQUAL_INT64(1773379800, email_parse_rfc822_date("Thu, 13 Mar 2026 10:30:00 +0500"));
}

/* A -0500 zone is LATER in absolute terms by 5h: 1773397800 + 18000 = 1773415800. */
static void test_rfc822_negative_offset(void) {
   TEST_ASSERT_EQUAL_INT64(1773415800, email_parse_rfc822_date("Thu, 13 Mar 2026 10:30:00 -0500"));
}

/* Day-of-week-less form with an offset still parses. */
static void test_rfc822_no_dow(void) {
   TEST_ASSERT_EQUAL_INT64(1773397800, email_parse_rfc822_date("13 Mar 2026 10:30:00 +0000"));
}

static void test_rfc822_null_and_empty(void) {
   TEST_ASSERT_EQUAL_INT64(0, email_parse_rfc822_date(NULL));
   TEST_ASSERT_EQUAL_INT64(0, email_parse_rfc822_date(""));
}

static void test_rfc822_garbage_returns_zero(void) {
   TEST_ASSERT_EQUAL_INT64(0, email_parse_rfc822_date("not a date at all"));
   TEST_ASSERT_EQUAL_INT64(0,
                           email_parse_rfc822_date("2026-03-13T10:30:00Z")); /* ISO, not RFC2822 */
}

/* ============================================================================
 * email_parse_imap_internaldate
 * ============================================================================ */

static void test_internaldate_utc(void) {
   TEST_ASSERT_EQUAL_INT64(1773397800, email_parse_imap_internaldate("13-Mar-2026 10:30:00 +0000"));
}

static void test_internaldate_offset(void) {
   TEST_ASSERT_EQUAL_INT64(1773379800, email_parse_imap_internaldate("13-Mar-2026 10:30:00 +0500"));
}

/* Tolerate a leading quote if the caller passes the value still quoted. */
static void test_internaldate_leading_quote(void) {
   TEST_ASSERT_EQUAL_INT64(1773397800,
                           email_parse_imap_internaldate("\"13-Mar-2026 10:30:00 +0000"));
}

/* RFC 3501 space-pads single-digit days (" 1-Sep-2026") — strptime %d skips the
 * leading space, so this must still parse.  2026-09-01 10:30:00 UTC = 1788258600. */
static void test_internaldate_space_padded_day(void) {
   TEST_ASSERT_EQUAL_INT64(1788258600, email_parse_imap_internaldate(" 1-Sep-2026 10:30:00 +0000"));
}

static void test_internaldate_bad(void) {
   TEST_ASSERT_EQUAL_INT64(0, email_parse_imap_internaldate(NULL));
   TEST_ASSERT_EQUAL_INT64(0, email_parse_imap_internaldate(""));
   TEST_ASSERT_EQUAL_INT64(0, email_parse_imap_internaldate("garbage"));
}

/* ============================================================================
 * email_imap_flags_contains — whole-token, case-insensitive
 * ============================================================================ */

static void test_flags_seen_present(void) {
   TEST_ASSERT_TRUE(email_imap_flags_contains("(\\Seen \\Answered)", "\\Seen"));
   TEST_ASSERT_TRUE(email_imap_flags_contains("(\\Seen \\Answered)", "\\Answered"));
}

static void test_flags_absent(void) {
   TEST_ASSERT_FALSE(email_imap_flags_contains("(\\Answered)", "\\Seen"));
   TEST_ASSERT_FALSE(email_imap_flags_contains("()", "\\Seen"));
}

static void test_flags_case_insensitive(void) {
   TEST_ASSERT_TRUE(email_imap_flags_contains("(\\SEEN)", "\\Seen"));
   TEST_ASSERT_TRUE(email_imap_flags_contains("(\\answered)", "\\Answered"));
}

/* A longer flag that merely CONTAINS the target as a prefix must not match. */
static void test_flags_no_partial_match(void) {
   TEST_ASSERT_FALSE(email_imap_flags_contains("(\\SeenLater)", "\\Seen"));
   TEST_ASSERT_FALSE(email_imap_flags_contains("(\\AnsweredByBot)", "\\Answered"));
}

/* Boundaries: first token, last token, mid-list. */
static void test_flags_boundaries(void) {
   TEST_ASSERT_TRUE(email_imap_flags_contains("(\\Seen \\Flagged \\Answered)", "\\Flagged"));
   TEST_ASSERT_TRUE(email_imap_flags_contains("\\Seen \\Answered", "\\Answered")); /* no parens */
}

static void test_flags_null_safe(void) {
   TEST_ASSERT_FALSE(email_imap_flags_contains(NULL, "\\Seen"));
   TEST_ASSERT_FALSE(email_imap_flags_contains("(\\Seen)", NULL));
   TEST_ASSERT_FALSE(email_imap_flags_contains("(\\Seen)", ""));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_rfc822_utc);
   RUN_TEST(test_rfc822_positive_offset);
   RUN_TEST(test_rfc822_negative_offset);
   RUN_TEST(test_rfc822_no_dow);
   RUN_TEST(test_rfc822_null_and_empty);
   RUN_TEST(test_rfc822_garbage_returns_zero);
   RUN_TEST(test_internaldate_utc);
   RUN_TEST(test_internaldate_offset);
   RUN_TEST(test_internaldate_leading_quote);
   RUN_TEST(test_internaldate_space_padded_day);
   RUN_TEST(test_internaldate_bad);
   RUN_TEST(test_flags_seen_present);
   RUN_TEST(test_flags_absent);
   RUN_TEST(test_flags_case_insensitive);
   RUN_TEST(test_flags_no_partial_match);
   RUN_TEST(test_flags_boundaries);
   RUN_TEST(test_flags_null_safe);
   return UNITY_END();
}
