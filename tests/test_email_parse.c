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
 * Unit tests for the shared email header-field helpers (tools/email_parse.c):
 * RFC 2822 dates (timezone-aware), IMAP INTERNALDATE, FLAGS membership,
 * quote-aware paren matching, ENVELOPE parsing, the FETCH-response message
 * iterator (including the literal-truncation resync), RFC 2047 encoded-word
 * decoding (with control-byte stripping), and CR/LF header sanitization.
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

/* REGRESSION (Copilot #28): message 1's envelope carries a quoted "* " AND a
 * quoted "{" BEFORE the truncated literal.  A resync that hunts for the literal
 * "{" or the next bare "* " could latch onto the quoted text and mis-bound or
 * lose the batch; anchoring on a VALIDATED "* <seq> FETCH ... UID" boundary must
 * still recover message 2 intact. */
static void test_next_fetch_literal_truncation_quoted_star(void) {
   const char *resp =
       "* 1 FETCH (UID 100 FLAGS (\\Seen) INTERNALDATE \"06-Apr-2026 00:03:06 -0500\" "
       "ENVELOPE (\"trap {x} and * bait\" {121}"
       "* 2 FETCH (UID 200 FLAGS (\\Seen) INTERNALDATE \"08-Apr-2026 04:30:55 -0500\" "
       "ENVELOPE (\"d\" \"Recovered\" ((\"Z\" NIL \"z\" \"tdr.ro\")) NIL NIL NIL NIL NIL NIL "
       "\"<x>\"))\r\n";
   const char *seg = NULL;
   size_t len = 0;
   uint32_t uid = 0;
   const char *p = resp;
   char tmp[600];
   char subj[256], fname[256], faddr[256];

   /* Message 1: returned, correct UID, subject blank (literal, unrecoverable). */
   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_UINT32(100, uid);

   /* Message 2: RECOVERED — not lost to a quoted-"* " false resync. */
   p = email_imap_next_fetch(p, &seg, &len, &uid);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_UINT32(200, uid);
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

static void test_sanitize_header_value(void) {
   char out[32];
   email_sanitize_header_value("subject\r\ninjected: x", out, sizeof out);
   TEST_ASSERT_EQUAL_STRING("subjectinjected: x", out);
   email_sanitize_header_value(NULL, out, sizeof out);
   TEST_ASSERT_EQUAL_STRING("", out);
}

/* ============================================================================
 * email_imap_append_quoted / _search_key — IMAP SEARCH term quoting.
 *
 * The built command is URL-decoded again by libcurl (CUSTOMREQUEST), so a
 * literal '%' MUST be escaped as '%25' or a percent-encoded metacharacter (%22
 * -> ", %5C -> \) breaks out of the quotes on the wire.  These pin that contract
 * plus quote/backslash escaping, control stripping, and the match-all guard.
 * ============================================================================ */

static void quote_into(char *dst, size_t dstlen, const char *value) {
   size_t off = 0, rem = dstlen;
   dst[0] = '\0';
   email_imap_append_quoted(dst, &off, &rem, value);
}

static void test_quote_plain(void) {
   char out[64];
   quote_into(out, sizeof out, "sketch cards");
   TEST_ASSERT_EQUAL_STRING("\"sketch cards\"", out);
}

static void test_quote_escapes_quote_and_backslash(void) {
   char out[64];
   quote_into(out, sizeof out, "a\"b\\c"); /* a"b\c */
   TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\"", out);
}

static void test_quote_trailing_backslash(void) {
   char out[64];
   quote_into(out, sizeof out, "foo\\"); /* foo\  ->  "foo\\" (escaped, quote intact) */
   TEST_ASSERT_EQUAL_STRING("\"foo\\\\\"", out);
}

static void test_quote_percent_escaped(void) {
   /* '%' -> '%25' so curl's CUSTOMREQUEST URL-decode round-trips it to one '%'
    * (and a genuine "50% off" search no longer fails as CURLE_URL_MALFORMAT). */
   char out[64];
   quote_into(out, sizeof out, "50% off");
   TEST_ASSERT_EQUAL_STRING("\"50%25 off\"", out);
}

static void test_quote_percent_encoded_injection_neutralized(void) {
   /* A percent-encoded quote must not survive curl's decode as a bare " — the
    * '%' is escaped, so it decodes back to the literal text "%22", not '"'. */
   char out[64];
   quote_into(out, sizeof out, "x%22 OR HEADER x");
   TEST_ASSERT_EQUAL_STRING("\"x%2522 OR HEADER x\"", out);
}

static void test_quote_strips_control_bytes(void) {
   char out[64];
   quote_into(out, sizeof out, "a\r\n\tb"); /* CR/LF/TAB dropped */
   TEST_ASSERT_EQUAL_STRING("\"ab\"", out);
}

static void test_quote_null_is_empty(void) {
   char out[8];
   quote_into(out, sizeof out, NULL);
   TEST_ASSERT_EQUAL_STRING("\"\"", out);
}

static void test_quote_preserves_utf8(void) {
   char out[32];
   quote_into(out, sizeof out, "caf\xC3\xA9"); /* café — high-bit octets pass through */
   TEST_ASSERT_EQUAL_STRING("\"caf\xC3\xA9\"", out);
}

static void test_search_key_emits_key(void) {
   char out[64];
   out[0] = '\0';
   size_t off = 0, rem = sizeof out;
   email_imap_append_search_key(out, &off, &rem, "FROM", "bob@example.com");
   TEST_ASSERT_EQUAL_STRING(" FROM \"bob@example.com\"", out);
}

static void test_search_key_skips_empty(void) {
   char out[64];
   out[0] = '\0';
   size_t off = 0, rem = sizeof out;
   email_imap_append_search_key(out, &off, &rem, "SUBJECT", "");
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_search_key_skips_all_control(void) {
   /* All-control value sanitizes to "" -> KEY "" would match every message. */
   char out[64];
   out[0] = '\0';
   size_t off = 0, rem = sizeof out;
   email_imap_append_search_key(out, &off, &rem, "TEXT", "\r\n\x01");
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_search_key_null_skips(void) {
   char out[64];
   out[0] = '\0';
   size_t off = 0, rem = sizeof out;
   email_imap_append_search_key(out, &off, &rem, "FROM", NULL);
   TEST_ASSERT_EQUAL_STRING("", out);
}

/* ============================================================================
 * email_parse_valid_iso_date — shared YYYY-MM-DD validator (IMAP + Gmail).
 * ============================================================================ */

static void test_iso_date_valid(void) {
   TEST_ASSERT_TRUE(email_parse_valid_iso_date("2026-03-13"));
   TEST_ASSERT_TRUE(email_parse_valid_iso_date("2024-02-29")); /* leap day */
}

static void test_iso_date_trailing_junk_rejected(void) {
   /* The strict fix: strptime alone would accept the leading date and leave the
    * junk, so we require the whole string to be consumed. */
   TEST_ASSERT_FALSE(email_parse_valid_iso_date("2026-03-13xyz"));
   TEST_ASSERT_FALSE(email_parse_valid_iso_date("2026-03-13 10:00"));
}

static void test_iso_date_wrong_format_rejected(void) {
   TEST_ASSERT_FALSE(email_parse_valid_iso_date("03/13/2026"));
   TEST_ASSERT_FALSE(email_parse_valid_iso_date("2026-03")); /* no day */
   TEST_ASSERT_FALSE(email_parse_valid_iso_date("last week"));
}

static void test_iso_date_empty_and_null(void) {
   TEST_ASSERT_FALSE(email_parse_valid_iso_date(""));
   TEST_ASSERT_FALSE(email_parse_valid_iso_date(NULL));
}


/* =============================================================================
 * email_imap_select_newest_uids
 * ============================================================================= */

static void test_select_uids_ascending(void) {
   uint32_t out[3];
   int total = -1;
   int n = email_imap_select_newest_uids("* SEARCH 1 2 3 4 5\r\nA1 OK done\r\n", out, 3, &total);
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_INT(5, total);
   TEST_ASSERT_EQUAL_UINT32(3, out[0]);
   TEST_ASSERT_EQUAL_UINT32(4, out[1]);
   TEST_ASSERT_EQUAL_UINT32(5, out[2]);
}

static void test_select_uids_descending_and_shuffled(void) {
   uint32_t out[3];
   int total = 0;
   int n = email_imap_select_newest_uids("* SEARCH 50 40 30 20 10\r\n", out, 3, &total);
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_UINT32(30, out[0]);
   TEST_ASSERT_EQUAL_UINT32(50, out[2]);

   n = email_imap_select_newest_uids("* SEARCH 7 99 3 42 8 100 1\r\n", out, 3, &total);
   TEST_ASSERT_EQUAL_INT(7, total);
   TEST_ASSERT_EQUAL_UINT32(42, out[0]);
   TEST_ASSERT_EQUAL_UINT32(99, out[1]);
   TEST_ASSERT_EQUAL_UINT32(100, out[2]);
   (void)n;
}

static void test_select_uids_split_lines_and_case(void) {
   uint32_t out[4];
   int total = 0;
   int n = email_imap_select_newest_uids("* SEARCH 5 6\r\n* search 9 1\r\nA1 OK\r\n", out, 4,
                                         &total);
   TEST_ASSERT_EQUAL_INT(4, n);
   TEST_ASSERT_EQUAL_INT(4, total);
   TEST_ASSERT_EQUAL_UINT32(1, out[0]);
   TEST_ASSERT_EQUAL_UINT32(9, out[3]);
}

static void test_select_uids_fewer_than_wanted_and_empty(void) {
   uint32_t out[10];
   int total = -1;
   TEST_ASSERT_EQUAL_INT(2, email_imap_select_newest_uids("* SEARCH 8 3\r\n", out, 10, &total));
   TEST_ASSERT_EQUAL_INT(2, total);
   TEST_ASSERT_EQUAL_UINT32(3, out[0]);

   TEST_ASSERT_EQUAL_INT(0,
                         email_imap_select_newest_uids("* SEARCH\r\nA1 OK\r\n", out, 10, &total));
   TEST_ASSERT_EQUAL_INT(0, total);
   TEST_ASSERT_EQUAL_INT(0, email_imap_select_newest_uids(NULL, out, 10, &total));
   TEST_ASSERT_EQUAL_INT(0, email_imap_select_newest_uids("A1 OK\r\n", out, 10, &total));
}

static void test_select_uids_skips_invalid_values(void) {
   uint32_t out[5];
   int total = 0;
   int n = email_imap_select_newest_uids("* SEARCH 0 4294967296 7 4294967295\r\n", out, 5, &total);
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT(2, total);
   TEST_ASSERT_EQUAL_UINT32(7, out[0]);
   TEST_ASSERT_EQUAL_UINT32(4294967295u, out[1]);
}

static void test_select_uids_wanted_zero_still_counts(void) {
   uint32_t out[1];
   int total = 0;
   TEST_ASSERT_EQUAL_INT(0, email_imap_select_newest_uids("* SEARCH 1 2 3\r\n", out, 0, &total));
   TEST_ASSERT_EQUAL_INT(3, total);
}

/* =============================================================================
 * IMAP page token format/parse
 * ============================================================================= */

static void test_page_token_round_trip(void) {
   char tok[32];
   uint32_t uid = 0, v = 99;
   TEST_ASSERT_TRUE(email_imap_page_token_format(566060, 0, tok, sizeof(tok)));
   TEST_ASSERT_EQUAL_STRING("u566060", tok);
   TEST_ASSERT_TRUE(email_imap_page_token_parse(tok, &uid, &v));
   TEST_ASSERT_EQUAL_UINT32(566060, uid);
   TEST_ASSERT_EQUAL_UINT32(0, v);

   TEST_ASSERT_TRUE(email_imap_page_token_format(4294967295u, 1199169964, tok, sizeof(tok)));
   TEST_ASSERT_EQUAL_STRING("u4294967295.1199169964", tok);
   TEST_ASSERT_TRUE(email_imap_page_token_parse(tok, &uid, &v));
   TEST_ASSERT_EQUAL_UINT32(4294967295u, uid);
   TEST_ASSERT_EQUAL_UINT32(1199169964, v);
}

static void test_page_token_format_rejects(void) {
   char tok[8];
   TEST_ASSERT_FALSE(email_imap_page_token_format(1, 0, tok, sizeof(tok)));
   TEST_ASSERT_FALSE(email_imap_page_token_format(0, 0, tok, sizeof(tok)));
   TEST_ASSERT_FALSE(email_imap_page_token_format(566060, 1199169964, tok, sizeof(tok)));
   TEST_ASSERT_EQUAL_STRING("", tok);
}

static void test_page_token_parse_rejects(void) {
   static const char *const bad[] = {
      "",
      "u",
      "u0",
      "u1",
      "u01",
      "u12x",
      "12",
      "u+5",
      "u 5",
      "u5.",
      "u5.x",
      "u5.0",
      "u5.01",
      "u99999999999",
      "U5",
      "u5 ",
      "u4294967296",
      "u5.1.2",
      "u5.4294967296",
      "CiAKGhIYc3BhbS10b2tlbg", /* Gmail-shaped */
   };
   uint32_t uid = 0, v = 0;
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      TEST_ASSERT_FALSE_MESSAGE(email_imap_page_token_parse(bad[i], &uid, &v), bad[i]);
   }
   TEST_ASSERT_FALSE(email_imap_page_token_parse(NULL, &uid, &v));
}


static void test_uidvalidity_line(void) {
   uint32_t v = 0;
   const char *ok = "* OK [UIDVALIDITY 1199169964] UIDs valid\r\n";
   TEST_ASSERT_TRUE(email_imap_parse_uidvalidity(ok, strlen(ok), &v));
   TEST_ASSERT_EQUAL_UINT32(1199169964u, v);
   const char *lower = "* ok [uidvalidity 7]";
   TEST_ASSERT_TRUE(email_imap_parse_uidvalidity(lower, strlen(lower), &v));
   TEST_ASSERT_EQUAL_UINT32(7, v);

   static const char *const bad[] = {
      "* OK [UIDVALIDITY 0]",  "* OK [UIDVALIDITY ]",
      "* OK [UIDVALIDITY 12",  "* OK [UIDVALIDITY 4294967296]",
      "* OK [UIDNEXT 5]",      "* 3 FETCH (ENVELOPE (\"* OK [UIDVALIDITY 1]\"))",
      " * OK [UIDVALIDITY 5]", "* OK [UIDVALIDITY 12345678901]",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      v = 99;
      TEST_ASSERT_FALSE_MESSAGE(email_imap_parse_uidvalidity(bad[i], strlen(bad[i]), &v), bad[i]);
      TEST_ASSERT_EQUAL_UINT32(99, v);
   }
   /* length-bounded: the digits run off the end of the buffer */
   TEST_ASSERT_FALSE(email_imap_parse_uidvalidity(ok, 20, &v));
}


static void test_select_uids_collapses_duplicates(void) {
   uint32_t out[5];
   int total = 0;
   int n = email_imap_select_newest_uids("* SEARCH 4 9 9 4 7\r\n", out, 5, &total);
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_INT(3, total);
   TEST_ASSERT_EQUAL_UINT32(4, out[0]);
   TEST_ASSERT_EQUAL_UINT32(7, out[1]);
   TEST_ASSERT_EQUAL_UINT32(9, out[2]);
}


static void test_exists_line(void) {
   uint32_t v = 99;
   const char *ok = "* 35820 EXISTS\r\n";
   TEST_ASSERT_TRUE(email_imap_parse_exists(ok, strlen(ok), &v));
   TEST_ASSERT_EQUAL_UINT32(35820, v);
   TEST_ASSERT_TRUE(email_imap_parse_exists("* 0 exists", 10, &v));
   TEST_ASSERT_EQUAL_UINT32(0, v);
   static const char *const bad[] = {
      "* EXISTS",
      "* 12 EXIST",
      "* 12 EXISTSX",
      "* 12 RECENT",
      " * 12 EXISTS",
      "* 4294967296 EXISTS",
      "* 3 FETCH (ENVELOPE (\"* 9 EXISTS\"))",
      "*12 EXISTS",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      v = 99;
      TEST_ASSERT_FALSE_MESSAGE(email_imap_parse_exists(bad[i], strlen(bad[i]), &v), bad[i]);
      TEST_ASSERT_EQUAL_UINT32(99, v);
   }
   /* length-bounded: the suffix runs off the end of the buffer */
   TEST_ASSERT_FALSE(email_imap_parse_exists(ok, 10, &v));
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
   RUN_TEST(test_next_fetch_literal_truncation_quoted_star);
   RUN_TEST(test_next_fetch_null_and_empty);
   RUN_TEST(test_sanitize_header_value);
   RUN_TEST(test_quote_plain);
   RUN_TEST(test_quote_escapes_quote_and_backslash);
   RUN_TEST(test_quote_trailing_backslash);
   RUN_TEST(test_quote_percent_escaped);
   RUN_TEST(test_quote_percent_encoded_injection_neutralized);
   RUN_TEST(test_quote_strips_control_bytes);
   RUN_TEST(test_quote_null_is_empty);
   RUN_TEST(test_quote_preserves_utf8);
   RUN_TEST(test_search_key_emits_key);
   RUN_TEST(test_search_key_skips_empty);
   RUN_TEST(test_search_key_skips_all_control);
   RUN_TEST(test_search_key_null_skips);
   RUN_TEST(test_iso_date_valid);
   RUN_TEST(test_iso_date_trailing_junk_rejected);
   RUN_TEST(test_iso_date_wrong_format_rejected);
   RUN_TEST(test_iso_date_empty_and_null);
   RUN_TEST(test_select_uids_ascending);
   RUN_TEST(test_select_uids_descending_and_shuffled);
   RUN_TEST(test_select_uids_split_lines_and_case);
   RUN_TEST(test_select_uids_fewer_than_wanted_and_empty);
   RUN_TEST(test_select_uids_skips_invalid_values);
   RUN_TEST(test_select_uids_wanted_zero_still_counts);
   RUN_TEST(test_page_token_round_trip);
   RUN_TEST(test_page_token_format_rejects);
   RUN_TEST(test_page_token_parse_rejects);
   RUN_TEST(test_uidvalidity_line);
   RUN_TEST(test_select_uids_collapses_duplicates);
   RUN_TEST(test_exists_line);
   return UNITY_END();
}
