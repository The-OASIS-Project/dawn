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
 * Pure parsing helpers for the IMAP email backend: RFC 2822 Date headers,
 * IMAP INTERNALDATE, FLAGS membership, quote-aware paren matching, ENVELOPE
 * parsing, and the FETCH-response message iterator.  Extracted from
 * email_client.c so the parsing logic is independently unit-testable.
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
 * Case-insensitive, whole-token test for an IMAP system flag within a FLAGS
 * group.  `flags_group` is the text of a "FLAGS (...)" list (with or without
 * the surrounding parens); `flag` is the flag name to find (e.g. "\\Seen",
 * "\\Answered").  Matches only a complete token bounded by whitespace or
 * parens, so "\\Seen" does not match "\\SeenLater".  NULL-safe (returns false).
 */
bool email_imap_flags_contains(const char *flags_group, const char *flag);

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

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_PARSE_H */
