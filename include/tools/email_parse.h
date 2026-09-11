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
 * IMAP INTERNALDATE, and IMAP FLAGS membership.  Extracted from email_client.c
 * so the timezone/date math and flag matching are independently unit-testable.
 */

#ifndef EMAIL_PARSE_H
#define EMAIL_PARSE_H

#include <stdbool.h>
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

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_PARSE_H */
