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
 * RFC 2822 dates (timezone-aware), IMAP INTERNALDATE, FLAGS membership,
 * quote-aware paren matching, ENVELOPE parsing, and the FETCH-response message
 * iterator (including the literal-truncation resync).
 */

#include <string.h>

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

/* ============================================================================
 * email_imap_match_paren — quote-aware paren matcher
 * ============================================================================ */

static void test_match_paren_simple(void) {
   const char *s = "(abc)def";
   const char *close = email_imap_match_paren(s);
   TEST_ASSERT_NOT_NULL(close);
   TEST_ASSERT_EQUAL_PTR(s + 4, close);
}

static void test_match_paren_nested(void) {
   const char *s = "(a (b) (c (d)) e)X";
   const char *close = email_imap_match_paren(s);
   TEST_ASSERT_NOT_NULL(close);
   TEST_ASSERT_EQUAL_CHAR(')', *close);
   TEST_ASSERT_EQUAL_CHAR('X', close[1]); /* the real outer close, not an inner one */
}

/* A ')' or '(' inside a quoted string must not affect nesting. */
static void test_match_paren_ignores_quoted(void) {
   const char *s = "(a \"b)c(d\" e)Z";
   const char *close = email_imap_match_paren(s);
   TEST_ASSERT_NOT_NULL(close);
   TEST_ASSERT_EQUAL_CHAR('Z', close[1]);
}

static void test_match_paren_escaped_quote(void) {
   const char *s = "(\"a\\\"b)\" c)Q"; /* quoted string contains \" then ) */
   const char *close = email_imap_match_paren(s);
   TEST_ASSERT_NOT_NULL(close);
   TEST_ASSERT_EQUAL_CHAR('Q', close[1]);
}

static void test_match_paren_unterminated(void) {
   TEST_ASSERT_NULL(email_imap_match_paren("(a b c"));
   TEST_ASSERT_NULL(email_imap_match_paren("no paren"));
   TEST_ASSERT_NULL(email_imap_match_paren(NULL));
}

/* ============================================================================
 * email_parse_envelope — against REAL server output (from augustus@ log)
 * ============================================================================ */

/* Real line 1: RFC2047-encoded From display name, plain subject. */
static void test_envelope_real_encoded_from(void) {
   const char *seg =
       "* 45621 FETCH (UID 562068 FLAGS (\\Seen) INTERNALDATE \"30-Mar-2026 01:09:06 -0500\" "
       "ENVELOPE (\"30 Mar 2026 08:07:06 +0200\" \"YD3211133\" "
       "((\"=?UTF-8?B?RVJJS0E=?=\" NIL \"erika.cozzani\" \"loyalytrade.de\")) "
       "((\"=?UTF-8?B?RVJJS0E=?=\" NIL \"erika.cozzani\" \"loyalytrade.de\")) "
       "((\"=?UTF-8?B?RVJJS0E=?=\" NIL \"erika.cozzani\" \"loyalytrade.de\")) "
       "((NIL NIL \"augustus\" \"linuxhardware.org\")) NIL NIL NIL "
       "\"<20260330080706.ABE956CBFAB91286@loyalytrade.de>\"))";
   char subj[256], fname[256], faddr[256];
   TEST_ASSERT_TRUE(
       email_parse_envelope(seg, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr));
   TEST_ASSERT_EQUAL_STRING("YD3211133", subj);
   /* from_name stays RAW (RFC2047) — the caller decodes it. */
   TEST_ASSERT_EQUAL_STRING("=?UTF-8?B?RVJJS0E=?=", fname);
   TEST_ASSERT_EQUAL_STRING("erika.cozzani@loyalytrade.de", faddr);
}

/* Real line 2: plain display name + subject. */
static void test_envelope_real_plain(void) {
   const char *seg =
       "* 45622 FETCH (UID 562098 FLAGS (\\Seen) INTERNALDATE \"31-Mar-2026 09:04:55 -0500\" "
       "ENVELOPE (\"31 Mar 2026 15:04:43 +0100\" \"Wire Transfer Receipt.\" "
       "((\"Diann David\" NIL \"info\" \"maxxpadel.com\")) NIL NIL NIL NIL NIL NIL "
       "\"<msgid@x>\"))";
   char subj[256], fname[256], faddr[256];
   TEST_ASSERT_TRUE(
       email_parse_envelope(seg, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr));
   TEST_ASSERT_EQUAL_STRING("Wire Transfer Receipt.", subj);
   TEST_ASSERT_EQUAL_STRING("Diann David", fname);
   TEST_ASSERT_EQUAL_STRING("info@maxxpadel.com", faddr);
}

/* NIL display name → empty from_name, addr still assembled. */
static void test_envelope_nil_name(void) {
   const char *seg = "ENVELOPE (\"d\" \"subj\" ((NIL NIL \"augustus\" \"linuxhardware.org\")) NIL)";
   char subj[256], fname[256], faddr[256];
   TEST_ASSERT_TRUE(
       email_parse_envelope(seg, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr));
   TEST_ASSERT_EQUAL_STRING("subj", subj);
   TEST_ASSERT_EQUAL_STRING("", fname);
   TEST_ASSERT_EQUAL_STRING("augustus@linuxhardware.org", faddr);
}

/* NIL subject and NIL from. */
static void test_envelope_nil_subject_and_from(void) {
   const char *seg = "ENVELOPE (\"d\" NIL NIL NIL)";
   char subj[256], fname[256], faddr[256];
   TEST_ASSERT_TRUE(
       email_parse_envelope(seg, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr));
   TEST_ASSERT_EQUAL_STRING("", subj);
   TEST_ASSERT_EQUAL_STRING("", fname);
   TEST_ASSERT_EQUAL_STRING("", faddr);
}

/* Escaped quote and backslash inside a quoted subject. */
static void test_envelope_escaped_quote_subject(void) {
   const char *seg = "ENVELOPE (\"d\" \"say \\\"hi\\\" now\" NIL NIL)";
   char subj[256];
   TEST_ASSERT_TRUE(email_parse_envelope(seg, subj, sizeof subj, NULL, 0, NULL, 0));
   TEST_ASSERT_EQUAL_STRING("say \"hi\" now", subj);
}

/* Primitive check of env_read_nstring's literal resilience: a literal ({N})
 * field followed by more content parses the following field.  (In real libcurl
 * output the octets AND the rest of the line after the literal are gone — that
 * production shape is covered by test_next_fetch_literal_truncation below; here
 * we only pin that env_read_nstring resumes past the "{N}" marker.) */
static void test_envelope_literal_subject_skips_to_from(void) {
   const char *seg = "ENVELOPE (\"d\" {5} ((\"Sender Name\" NIL \"user\" \"host\")) NIL)";
   char subj[256], fname[256], faddr[256];
   TEST_ASSERT_TRUE(
       email_parse_envelope(seg, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr));
   TEST_ASSERT_EQUAL_STRING("", subj); /* literal → unavailable */
   TEST_ASSERT_EQUAL_STRING("Sender Name", fname);
   TEST_ASSERT_EQUAL_STRING("user@host", faddr);
}

/* ENVELOPE present but malformed before the subject field → false, no crash. */
static void test_envelope_malformed_before_subject(void) {
   char subj[256];
   TEST_ASSERT_FALSE(/* no '(' after ENVELOPE */
                     email_parse_envelope("ENVELOPE oops", subj, sizeof subj, NULL, 0, NULL, 0));
   TEST_ASSERT_EQUAL_STRING("", subj);
   TEST_ASSERT_FALSE(/* unterminated date (field 0) */
                     email_parse_envelope("ENVELOPE (\"unterminated", subj, sizeof subj, NULL, 0,
                                          NULL, 0));
}

static void test_envelope_no_envelope(void) {
   char subj[256];
   TEST_ASSERT_FALSE(email_parse_envelope("* 1 FETCH (UID 5 FLAGS (\\Seen))", subj, sizeof subj,
                                          NULL, 0, NULL, 0));
   TEST_ASSERT_FALSE(email_parse_envelope(NULL, subj, sizeof subj, NULL, 0, NULL, 0));
}

/* ============================================================================
 * email_imap_next_fetch — message iterator (the batch-walk logic)
 * ============================================================================ */

static void test_next_fetch_two_clean(void) {
   const char *resp =
       "* 1 FETCH (UID 100 FLAGS (\\Seen) INTERNALDATE \"01-Jan-2026 00:00:00 +0000\" "
       "ENVELOPE (\"d\" \"Subj A\" ((\"A\" NIL \"a\" \"x\")) NIL NIL NIL NIL NIL NIL \"<1>\"))\r\n"
       "* 2 FETCH (UID 200 FLAGS () INTERNALDATE \"02-Jan-2026 00:00:00 +0000\" "
       "ENVELOPE (\"d\" \"Subj B\" ((\"B\" NIL \"b\" \"y\")) NIL NIL NIL NIL NIL NIL \"<2>\"))\r\n";
   const char *seg = NULL;
   size_t len = 0;
   uint32_t uid = 0;
   const char *p = resp;

   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_UINT32(100, uid);
   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_UINT32(200, uid);
   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NULL(p); /* no more messages */
}

/* REGRESSION (the 562213 bug): message 1's ENVELOPE subject is a literal {121}
 * — libcurl dropped the octets and jumped straight to "* 2 FETCH", leaving msg
 * 1's parens unclosed.  The iterator must still return msg 1 (bounded before
 * "* 2", blank subject) AND recover msg 2 — never drop the rest of the batch. */
static void test_next_fetch_literal_truncation(void) {
   const char *resp =
       "* 1 FETCH (UID 562213 FLAGS (\\Seen) INTERNALDATE \"06-Apr-2026 00:03:06 -0500\" "
       "ENVELOPE (\"Mon, 06 Apr 2026 05:02:52 -0000\" {121}"
       "* 2 FETCH (UID 562266 FLAGS (\\Seen) INTERNALDATE \"08-Apr-2026 04:30:55 -0500\" "
       "ENVELOPE (\"d\" \"Recovered\" ((\"Z\" NIL \"z\" \"tdr.ro\")) NIL NIL NIL NIL NIL NIL "
       "\"<x>\"))\r\n";
   const char *seg = NULL;
   size_t len = 0;
   uint32_t uid = 0;
   const char *p = resp;
   char tmp[600];
   char subj[256], fname[256], faddr[256];

   /* Message 1: returned, UID correct, subject blank (literal, unrecoverable). */
   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_UINT32(562213, uid);
   TEST_ASSERT_TRUE(len < sizeof(tmp));
   memcpy(tmp, seg, len);
   tmp[len] = '\0';
   email_parse_envelope(tmp, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr);
   TEST_ASSERT_EQUAL_STRING("", subj); /* literal subject → blank, not msg 2's */

   /* Message 2: RECOVERED (the bug dropped it entirely). */
   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_UINT32(562266, uid);
   TEST_ASSERT_TRUE(len < sizeof(tmp));
   memcpy(tmp, seg, len);
   tmp[len] = '\0';
   email_parse_envelope(tmp, subj, sizeof subj, fname, sizeof fname, faddr, sizeof faddr);
   TEST_ASSERT_EQUAL_STRING("Recovered", subj);

   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NULL(p);
}

static void test_next_fetch_null_and_empty(void) {
   const char *seg = NULL;
   size_t len = 0;
   uint32_t uid = 0;
   TEST_ASSERT_NULL(email_imap_next_fetch(NULL, &seg, &len, &uid));
   TEST_ASSERT_NULL(email_imap_next_fetch("no fetch here", &seg, &len, &uid));
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
   RUN_TEST(test_match_paren_simple);
   RUN_TEST(test_match_paren_nested);
   RUN_TEST(test_match_paren_ignores_quoted);
   RUN_TEST(test_match_paren_escaped_quote);
   RUN_TEST(test_match_paren_unterminated);
   RUN_TEST(test_envelope_real_encoded_from);
   RUN_TEST(test_envelope_real_plain);
   RUN_TEST(test_envelope_nil_name);
   RUN_TEST(test_envelope_nil_subject_and_from);
   RUN_TEST(test_envelope_escaped_quote_subject);
   RUN_TEST(test_envelope_literal_subject_skips_to_from);
   RUN_TEST(test_envelope_malformed_before_subject);
   RUN_TEST(test_envelope_no_envelope);
   RUN_TEST(test_next_fetch_two_clean);
   RUN_TEST(test_next_fetch_literal_truncation);
   RUN_TEST(test_next_fetch_null_and_empty);
   return UNITY_END();
}
