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
 * US equity market session calendar — pure date math (a DST-aware US/Eastern clock
 * plus the NYSE holiday + early-close half-day rules). No external deps and no
 * clock reads of its own beyond the timestamp passed in, so it is fully
 * unit-testable. Holiday coverage is the fixed NYSE schedule (the 10 annual
 * holidays with weekend-observed shifting, Good Friday, and the three early-close
 * half-days); it does NOT cover unscheduled closures (e.g. a national day of
 * mourning), which need the live Schwab /markets endpoint.
 */

#ifndef MARKET_HOURS_H
#define MARKET_HOURS_H

#include <time.h>

typedef enum {
   MARKET_CLOSED = 0, /**< weekend, holiday, or outside 07:00–20:00 ET */
   MARKET_PRE,        /**< 07:00–09:30 ET pre-market */
   MARKET_REGULAR,    /**< 09:30–16:00 ET (13:00 close on a half-day) */
   MARKET_POST        /**< regular close–20:00 ET post-market */
} market_session_t;

/**
 * Classify @p utc into the current US-equity session in US/Eastern, honoring
 * weekends, NYSE full-close holidays, and early-close (13:00 ET) half-days.
 */
market_session_t market_hours_us_equity(time_t utc);

/** Lower-case wire token: "closed" | "pre" | "regular" | "post". Never NULL. */
const char *market_session_str(market_session_t s);

#endif /* MARKET_HOURS_H */
