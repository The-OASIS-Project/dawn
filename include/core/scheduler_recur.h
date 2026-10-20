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
 * When a repeating scheduled event fires next, on its owner's clock.
 */

#ifndef SCHEDULER_RECUR_H
#define SCHEDULER_RECUR_H

#include <stdbool.h>
#include <time.h>

#include "core/scheduler_db.h"
#include "core/tz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The next time a repeating event fires, in @p zone.
 *
 * The occurrence a row stands for is @p original_time (HH:MM; else
 * @p fire_at's time of day) on @p fire_at's date, or, when @p snoozed and
 * that is after fire_at, the day before (a snooze moves fire_at later, past
 * midnight perhaps, never earlier).  The next is the
 * first matching day after it whose time is after @p now, stepping whole days
 * on the clock (a daylight-saving change doesn't move the time of day), so an
 * event dismissed after midnight doesn't skip a day, one cancelled ahead of
 * time skips only itself, and one missed while the daemon was down fires
 * once, not for every day it missed.
 *
 * @return The instant, or 0 when no day matches.
 */
time_t sched_next_occurrence(const tz_t *zone,
                             time_t fire_at,
                             const char *original_time,
                             sched_recurrence_t recurrence,
                             const char *recurrence_days,
                             bool snoozed,
                             time_t now);

/**
 * @brief Where a pending repeating occurrence goes when its owner's zone
 *        changes from @p from to @p to.
 *
 * The occurrence keeps its date and time of day (@p original_time, else
 * @p fire_at's time on @p from's clock) and is read on @p to's clock, so a
 * 05:00 alarm still rings at 05:00 where its owner now is.  When that has
 * already passed, the next occurrence after @p now (sched_next_occurrence).
 * A one-time event is returned unchanged: its instant was what was asked for.
 *
 * @return The instant (@p fire_at when nothing moves), or 0 when no day matches.
 */
time_t sched_rezone_occurrence(const tz_t *from,
                               const tz_t *to,
                               time_t fire_at,
                               const char *original_time,
                               sched_recurrence_t recurrence,
                               const char *recurrence_days,
                               time_t now);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_RECUR_H */
