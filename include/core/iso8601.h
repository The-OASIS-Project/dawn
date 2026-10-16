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
 * ISO 8601 absolute timestamp parser — shared by tools that accept
 * date/time arguments from the LLM (scheduler, calendar, etc.).
 *
 * Distinct from time_query_parser, which extracts fuzzy temporal
 * expressions from natural-language search queries.  This module parses
 * concrete RFC 3339 / ISO 8601 strings into Unix timestamps.
 */

#ifndef ISO8601_H
#define ISO8601_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The fields of an ISO 8601 time, as written (no zone applied). */
typedef struct {
   int year, month, day; /**< unset for a time alone */
   int hour, min, sec;
   bool time_only;  /**< "HH:MM" alone */
   bool has_offset; /**< "Z" or an offset was given */
   int offset_sec;  /**< east of UTC, when has_offset */
} iso8601_fields_t;

/**
 * @brief Read an ISO 8601 time's fields, checked for range (years 1-9999;
 *        Feb 30 and hour 24 are errors, not rolled over).
 *
 * Accepted formats:
 *   - "2026-02-19"                    (date only; time fields 0)
 *   - "2026-02-19T15:30[:00[.123]]"   ('T' or a space; fractional seconds skipped)
 *   - "...Z", "...-05:00", "...+0530" (an offset)
 *   - "15:30"                         (a time alone)
 *
 * Converting the fields to an instant takes a zone: see tz_parse() in
 * core/tz.h.
 *
 * @return true when @p text is such a time.
 */
bool iso8601_fields(const char *text, iso8601_fields_t *out);

/**
 * @brief Parse a date-only ISO 8601 string as midnight UTC.
 *
 * Accepted formats:
 *   - "YYYY-MM-DD"  — midnight UTC of that date
 *   - "YYYY"        — January 1 midnight UTC of that year
 *
 * Distinct from tz_parse() in three ways:
 *   - UTC interpretation only (no zone involved)
 *   - Date-only input (no time-of-day, no offsets)
 *   - Returns 0 on parse failure / NULL input — callers treat 0 as
 *     "no bound" (e.g., open-ended valid_to in bitemporal relations)
 *
 * Used for date bounds stored in SQLite INTEGER columns where the 0
 * sentinel naturally represents "unset".
 *
 * @param iso_str Input string.  May be NULL (returns 0).
 * @return Unix timestamp at 00:00:00 UTC for the parsed date,
 *         or 0 on parse failure.
 */
int64_t iso8601_parse_date_utc(const char *iso_str);

/**
 * @brief Format a Gregorian date as "YYYY-MM-DD" into @p out.
 *
 * Convenience for the many sites that print struct tm fields.  Components are
 * normalized to their valid printed widths (year mod 10000, month/day mod 100,
 * forced non-negative), so the result is always exactly 10 chars + NUL.  That
 * also lets the compiler prove the output fits, which silences
 * -Wformat-truncation at the call sites — `struct tm` fields are plain `int`,
 * a range the compiler otherwise can't bound.
 *
 * @param year     Full year (e.g. tm_year + 1900).
 * @param month    Month 1-12 (e.g. tm_mon + 1).
 * @param day      Day of month 1-31.
 * @param out      Output buffer; needs >= 11 bytes.  NUL-terminated on return
 *                 (set empty if @p out_size < 11).
 * @param out_size Size of @p out.
 */
void iso8601_format_date(int year, int month, int day, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* ISO8601_H */
