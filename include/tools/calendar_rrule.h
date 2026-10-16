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
 * A recurring event's occurrences within a window, with bounded work.
 */

#ifndef CALENDAR_RRULE_H
#define CALENDAR_RRULE_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most occurrences one event yields. */
#define CALENDAR_RRULE_MAX_OCCURRENCES 500

struct _icaltimezone;

/**
 * @brief The zone @p tzid names: one libical lists, UTC by any spelling, or
 *        an old name tzdata's own list (tzdata.zi) gives a listed current
 *        name ("Europe/Kiev" to "Europe/Kyiv").  NULL for any other name.
 *
 * Asked before libical looks a name up: for a name it doesn't list, libical
 * opens a zone file of that name and keeps it, so a server sending endless
 * aliases ("America//New_York", "posix/...") would grow the daemon without
 * bound.  Nothing is loaded here but a zone libical lists.
 */
struct _icaltimezone *calendar_rrule_zone(const char *tzid);

/**
 * @brief The zone the user's times are read in: @p configured
 *        ([localization] timezone) when set, else the system's zone name
 *        (TZ, then /etc/localtime, then /etc/timezone), else "".
 */
const char *calendar_rrule_user_tzid(const char *configured);

/** Receives one occurrence's start (UTC) and the caller's @p ctx. */
typedef void (*calendar_rrule_emit_fn)(time_t start, void *ctx);

/**
 * @brief Call @p emit for each occurrence of @p rrule that overlaps
 *        [@p range_start, @p range_end], in order, at most
 *        CALENDAR_RRULE_MAX_OCCURRENCES
 *
 * A timed event repeats in its own zone (@p tzid), so a weekly 10:00 stays at
 * 10:00 across a daylight-saving change; a zone libical doesn't have built in
 * repeats in UTC.
 *
 * The rule comes from the calendar's server (and a shared calendar from its
 * owner), so the work is bounded whatever it says: a rule with no COUNT
 * starts at the window, every rule ends at it, a COUNT rule starting too long
 * before it is refused, and rules libical can search for seconds or forever
 * are refused: every second or minute, a non-Gregorian RSCALE, and filters
 * that can never match (a sixth Monday, February 30th).  RFC 5545 allows
 * those, but no calendar app makes them.
 *
 * @param rrule       RFC 5545 RRULE value
 * @param dtstart     First start (UTC); ignored for an all-day event
 * @param tzid        The event's IANA zone, or NULL/empty for UTC
 * @param all_day     Whether the event is all-day
 * @param start_date  The all-day event's first date, YYYY-MM-DD
 * @param duration    Each occurrence's length, seconds (> 0)
 * @param range_start Window start (UTC)
 * @param range_end   Window end (UTC)
 * @param emit        Called with each occurrence's start
 * @param ctx         Passed to @p emit
 * @param count_out   Receives the occurrences emitted
 * @return SUCCESS, or FAILURE for a rule libical can't read or that is
 *         refused (nothing is emitted)
 */
int calendar_rrule_expand(const char *rrule,
                          time_t dtstart,
                          const char *tzid,
                          bool all_day,
                          const char *start_date,
                          int duration,
                          time_t range_start,
                          time_t range_end,
                          calendar_rrule_emit_fn emit,
                          void *ctx,
                          int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* CALENDAR_RRULE_H */
