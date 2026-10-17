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
 * Clock times in a named zone, without touching the process-wide TZ: zone
 * lookup, conversion between instants and local clock times, and parsing an
 * ISO 8601 time read in a given zone.  Built on libical's zones.
 */

#ifndef TZ_H
#define TZ_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _icaltimezone;
typedef struct _icaltimezone tz_t;

/**
 * @brief The zone @p name names: one libical lists, UTC by any spelling, or
 *        an old name tzdata's own list (tzdata.zi) gives a listed current
 *        name ("Europe/Kiev" to "Europe/Kyiv").  NULL for any other name.
 *
 * Safe on any thread and for text from outside DAWN: it searches a copy of
 * libical's zone names taken once, and never asks libical for a name it
 * doesn't list (for one it doesn't, libical opens a zone file of that name
 * and adds it to its list, so endless aliases would grow the daemon).
 */
tz_t *tz_lookup(const char *name);

/** @brief The IANA name of a zone tz_lookup() or tz_daemon() returned ("UTC"
 *         for UTC and anything else); valid for the life of the process. */
const char *tz_name(const tz_t *zone);

/**
 * @brief Resolve the daemon's zone, once, at startup before other threads
 *        run: @p configured ([localization] timezone) when set, else the
 *        system's (TZ, /etc/localtime, /etc/timezone), else UTC.
 *
 * Unlike tz_lookup(), this may load any zone in the system's zoneinfo
 * ("CET", "EST5EDT").  Logs a warning when the name can't be resolved, or
 * when the C library's offsets for the process disagree with the zone found.
 */
void tz_daemon_init(const char *configured);

/** @brief The daemon's zone (UTC before tz_daemon_init). Never NULL. */
tz_t *tz_daemon(void);

/**
 * @brief @p instant's local clock reading in @p zone, as seconds (the local
 *        date and time read as if UTC), so whole days can be added to it.
 */
time_t tz_wall(const tz_t *zone, time_t instant);

/**
 * @brief The instant whose local clock reading in @p zone is @p wall.
 *
 * A reading a daylight-saving change skips moves forward by the gap (02:30
 * on the morning clocks go from 02:00 to 03:00 is 03:30, as glibc's mktime
 * has it), and one it repeats is the earlier of the two (RFC 5545 §3.3.5;
 * glibc and libical alone take the later).
 */
time_t tz_from_wall(const tz_t *zone, time_t wall);

/**
 * @brief @p instant in @p zone as a struct tm: the date and time fields and
 *        tm_gmtoff; tm_isdst is -1 (unknown) and tm_zone is not set (print
 *        the zone's name instead).
 */
void tz_local(const tz_t *zone, time_t instant, struct tm *out);

/**
 * @brief The instant @p tm's fields name in @p zone (they may be out of
 *        range, as for mktime: day 32 is the next month's 1st), by
 *        tz_from_wall()'s rule.
 */
time_t tz_mktime(const tz_t *zone, const struct tm *tm);

/**
 * @brief Parse an ISO 8601 time into @p out.
 *
 * Accepts "YYYY-MM-DD" (midnight), "YYYY-MM-DDTHH:MM[:SS[.fff]]" with an
 * optional "Z" or offset, a space for the 'T', and "HH:MM" alone (the next
 * time that clock time comes: today, either pass of a repeated hour, or
 * tomorrow).  A time with an offset
 * names that instant; one without is read in @p zone.
 *
 * @return true on success; false for text that isn't such a time or names an
 *         impossible one (Feb 30, hour 24).
 */
bool tz_parse(const char *text, const tz_t *zone, time_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TZ_H */
