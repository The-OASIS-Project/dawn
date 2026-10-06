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
 * Pure header-field utilities shared by both email backends (IMAP + Gmail):
 * RFC 2822 Date / IMAP INTERNALDATE parsing, IMAP FLAGS membership, quote-aware
 * paren matching, ENVELOPE parsing, the FETCH-response message iterator, RFC 2047
 * encoded-word decoding, and CR/LF header sanitization.  A libc-only leaf, so
 * the logic is independently unit-testable.
 */

#ifndef EMAIL_PARSE_H
#define EMAIL_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse an RFC 2822 Date header into a UTC epoch.
 *
 * Handles the "Thu, 13 Mar 2026 10:30:00 +0000" and day-of-week-less
 * "13 Mar 2026 10:30:00 +0000" forms.  A numeric timezone offset (`%z`) is
 * honored so the result is a true UTC instant regardless of the sender's zone;
 * when the string carries no offset the local timezone is assumed (best effort,
 * matching the historical behavior).  Returns 0 (NOT a negative value, and NOT
 * a local-epoch guess) when the string is missing or unparseable, so callers
 * can treat 0 as "unknown" rather than a bogus 1970 timestamp.
 */
time_t email_parse_rfc822_date(const char *date_str);

/**
 * Parse an IMAP INTERNALDATE ("10-Sep-2026 15:45:00 +0000") into a UTC epoch.
 * INTERNALDATE always carries a zone offset per RFC 3501.  Returns 0 on a
 * missing/unparseable value.  This is the reliable server-side receive time,
 * preferred over the free-form Date header when both are available.
 */
time_t email_parse_imap_internaldate(const char *idate);

/**
 * True iff @p iso is exactly a valid YYYY-MM-DD calendar date (the format the
 * email search `since`/`before` params use).  Requires the whole string to be
 * consumed, so trailing junk is rejected.  NULL/empty -> false.  Shared by both
 * the IMAP and Gmail search backends so an invalid date is dropped identically.
 */
bool email_parse_valid_iso_date(const char *iso);

/**
 * Append @p value to @p buf as an IMAP quoted string ("...") for safe
 * interpolation into a SEARCH command, tracking the running offset/remaining in
 * @p off / @p rem (BUF_PRINTF convention).  Control chars are dropped, `"` and
 * `\` are backslash-escaped, and — because libcurl URL-DECODES the built
 * CUSTOMREQUEST before sending — every literal `%` is escaped as `%25`.  This is
 * the sole injection defense for LLM-supplied search terms; see the CRITICAL note
 * at the definition.  NULL @p value is treated as empty.
 */
void email_imap_append_quoted(char *buf, size_t *off, size_t *rem, const char *value);

/**
 * Append ` KEY "value"` (KEY quoted via email_imap_append_quoted) to an IMAP
 * SEARCH command, or nothing when @p value is NULL/empty or reduces to empty
 * after control-char stripping (avoids KEY "" = match-all).
 */
void email_imap_append_search_key(char *buf,
                                  size_t *off,
                                  size_t *rem,
                                  const char *key,
                                  const char *value);

/**
 * Case-insensitive, whole-token test for an IMAP system flag within a FLAGS
 * group.  `flags_group` is the text of a "FLAGS (...)" list (with or without
 * the surrounding parens); `flag` is the flag name to find (e.g. "\\Seen",
 * "\\Answered").  Matches only a complete token bounded by whitespace or
 * parens, so "\\Seen" does not match "\\SeenLater".  NULL-safe (returns false).
 */
bool email_imap_flags_contains(const char *flags_group, const char *flag);

/**
 * Copy @p src to @p dst (NUL-terminated, bounded by @p dst_len) with CR and LF
 * removed, to prevent SMTP header injection via a user-supplied header value.
 * Shared by both backends' send paths.  NULL-safe.
 */
void email_sanitize_header_value(const char *src, char *dst, size_t dst_len);

/**
 * "Name" <addr> (RFC 5322 quoted display name; " and \ escaped), just the
 * address with no name, or just "Name" with no address: a name with a comma
 * ("Smith, Bob") can't split the header into two recipients, and a shown
 * sender's name can't pass for an address.  Both are already free of CR/LF
 * (email_sanitize_header_value).  Cut to fit on a whole character.
 */
void email_format_mailbox(const char *name, const char *addr, char *dst, size_t dst_len);

/**
 * A sender as shown to the user or the model (never a header): as
 * email_format_mailbox, except a name containing '@' that isn't the address
 * itself is dropped (it would pass for the sender's address), and a sender
 * with no address says "(no address)".
 */
void email_display_mailbox(const char *name, const char *addr, char *dst, size_t dst_len);

/**
 * Find the ')' matching the '(' at @p open, honoring IMAP quoted strings (so a
 * '(' , ')' or '"' inside a quoted string does not affect nesting; a backslash
 * escapes the next char inside a quote).  Returns the matching ')' or NULL if
 * @p open is not '(' or the group is unterminated.  Used to bound one message's
 * FETCH response robustly (a subject containing "* " or ")" cannot truncate it).
 */
const char *email_imap_match_paren(const char *open);

/**
 * Parse an IMAP ENVELOPE structure out of a FETCH response segment.
 *
 * Extracts the Subject (RFC 3501 field 2) and the FIRST From address (field 3:
 * name + mailbox@host).  The returned subject and from_name are the RAW envelope
 * strings — still RFC 2047-encoded if the sender encoded them — for the caller
 * to decode; from_addr is assembled plain as "mailbox@host".  A NIL field yields
 * an empty output.  A value delivered as an IMAP literal ({N}) — which libcurl's
 * custom-command path discards — is treated as unavailable (empty), never a
 * misparse.  Returns true if an ENVELOPE was found and parsed through the subject
 * field; false if no ENVELOPE is present OR the envelope is malformed before the
 * subject (missing '(', or an unterminated date/subject).
 *
 * @param seg          NUL-terminated FETCH response text containing "ENVELOPE (".
 * @param subject      RAW subject out (may be NULL to skip).
 * @param subject_sz   Size of the subject buffer.
 * @param from_name    RAW first-From display name out (may be NULL to skip).
 * @param from_name_sz Size of the from_name buffer.
 * @param from_addr    First-From "mailbox@host" out (may be NULL to skip).
 * @param from_addr_sz Size of the from_addr buffer.
 * @return true if parsed through the subject field, else false.
 */
bool email_parse_envelope(const char *seg,
                          char *subject,
                          size_t subject_sz,
                          char *from_name,
                          size_t from_name_sz,
                          char *from_addr,
                          size_t from_addr_sz);

/**
 * Iterate the messages in an IMAP FETCH response buffer.
 *
 * Given cursor @p p into a NUL-terminated response, locate the next
 * "* <seq> FETCH (...)" untagged message.  On success sets @p out_seg to the
 * message text (from "* "), @p out_seg_len to its length bounded to just this
 * message, and @p out_uid to its UID, and returns the pointer to resume from.
 * Returns NULL when no further message is found.
 *
 * Each message is bounded at its item-list's matching ')' (quote-aware).  When
 * that paren never closes — the ENVELOPE carried an IMAP literal ({N}) the
 * transport discarded, dropping the rest of the line and jumping straight to the
 * next "* <seq> FETCH" — it RESYNCS to that next "* " (searched after the literal
 * marker so a "* " inside an earlier quoted field can't false-match), so a
 * literal-bearing message never swallows the rest of the batch.  Every returned
 * pointer is strictly past @p p (forward progress guaranteed).
 *
 * @param p            NUL-terminated response cursor (NULL yields NULL).
 * @param out_seg      Set to the message start (may be NULL).
 * @param out_seg_len  Set to the message length (may be NULL).
 * @param out_uid      Set to the message UID (may be NULL).
 * @return Pointer to resume scanning from, or NULL when no message remains.
 */
const char *email_imap_next_fetch(const char *p,
                                  const char **out_seg,
                                  size_t *out_seg_len,
                                  uint32_t *out_uid);

/**
 * @brief Select the @p wanted newest (largest) UIDs from an IMAP UID SEARCH response.
 *
 * RFC 3501/9051 give no ordering guarantee for "* SEARCH" results, and some
 * servers/proxies split a long result across several "* SEARCH" lines, so this
 * scans EVERY such line and keeps the N largest UIDs order-independently (a
 * size-N min-heap).  Zero and out-of-range values are skipped.
 *
 * @param response   NUL-terminated raw UID SEARCH response (NULL = no results).
 * @param out        Receives the selected UIDs in ASCENDING order (>= @p wanted slots).
 * @param wanted     Maximum UIDs to select (<= 0 selects none).
 * @param total_out  Receives the total number of UIDs the server matched (may be NULL).
 * @return Number of UIDs written to @p out (min(total, wanted)).
 */
int email_imap_select_newest_uids(const char *response, uint32_t *out, int wanted, int *total_out);

/**
 * @brief Format an IMAP paging cursor ("return UIDs below @p before_uid").
 *
 * Grammar: "u<before_uid>" or, when @p uidvalidity is non-zero,
 * "u<before_uid>.<uidvalidity>" so the next page can assert the mailbox's
 * UIDVALIDITY epoch hasn't changed underneath the cursor.
 *
 * @return true on success; false if @p before_uid < 2 or @p out is too small.
 */
bool email_imap_page_token_format(uint32_t before_uid,
                                  uint32_t uidvalidity,
                                  char *out,
                                  size_t out_len);

/**
 * @brief Strictly parse an IMAP paging cursor produced by email_imap_page_token_format().
 *
 * Accepts only "u" + 1-10 digits (value 2..UINT32_MAX, no leading zero), optionally
 * followed by "." + 1-10 digits (value 1..UINT32_MAX, no leading zero), with the
 * whole string consumed.  Anything else — including a Gmail page token handed to an
 * IMAP account — is rejected.
 *
 * @param uidvalidity  Receives the epoch, or 0 when the token carries none.
 * @return true when @p tok is a valid cursor.
 */
bool email_imap_page_token_parse(const char *tok, uint32_t *before_uid, uint32_t *uidvalidity);

/**
 * @brief Extract the mailbox epoch from a SELECT/EXAMINE "* OK [UIDVALIDITY n]" line.
 *
 * Anchored at the start of the line, case-insensitive, 1-10 digits, value
 * 1..UINT32_MAX.  Line anchoring alone is NOT enough to trust the value: libcurl
 * also delivers the lines inside IMAP literals (e.g. a subject containing CRLF), so
 * callers must only apply this to lines answering a SELECT/EXAMINE.
 *
 * @param line  Raw server line (need not be NUL-terminated).
 * @param len   Bytes available at @p line.
 * @return true and sets @p out when the line carries a valid UIDVALIDITY.
 */
bool email_imap_parse_uidvalidity(const char *line, size_t len, uint32_t *out);

/**
 * @brief Extract the message count from a SELECT/EXAMINE "* <n> EXISTS" line.
 *
 * Anchored at the start of the line, 1-10 digits (0 allowed: an empty mailbox),
 * followed by " EXISTS" and then end of line.  Same trust rule as
 * email_imap_parse_uidvalidity: apply only to lines answering a SELECT/EXAMINE.
 *
 * @return true and sets @p out when the line is a valid EXISTS response.
 */
bool email_imap_parse_exists(const char *line, size_t len, uint32_t *out);

/**
 * @brief Split an IMAP message id, "folder:uid" (a bare uid means INBOX).
 *
 * Splits at the last colon.  Refuses a folder too long for @p folder rather
 * than cutting it or falling back to INBOX: either would act on a message in
 * another mailbox.  The folder name itself is the caller's to validate.
 *
 * @return true and fills @p folder and @p uid when the id is one; false when
 *         the folder doesn't fit or the uid isn't a decimal 1..UINT32_MAX.
 */
bool email_imap_id_parse(const char *message_id, char *folder, size_t folder_size, uint32_t *uid);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_PARSE_H */
