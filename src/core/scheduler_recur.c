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

#include "core/scheduler_recur.h"

#include <stdio.h>
#include <strings.h>

/* Seconds in a day on the clock */
#define SCHED_RECUR_DAY 86400

/* Whether weekday @p wday (0 = Sunday) is one @p recurrence fires on
 * (@p recurrence_days: "mon,wed" for a custom rule). */
static bool day_matches(int wday, sched_recurrence_t recurrence, const char *recurrence_days) {
   switch (recurrence) {
      case SCHED_RECUR_DAILY:
         return true;
      case SCHED_RECUR_WEEKDAYS:
         return (wday >= 1 && wday <= 5);
      case SCHED_RECUR_WEEKENDS:
         return (wday == 0 || wday == 6);
      case SCHED_RECUR_WEEKLY:
         return true; /* Same weekday as original */
      case SCHED_RECUR_CUSTOM: {
         if (!recurrence_days || !recurrence_days[0])
            return false;
         static const char *day_names[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
         const char *needle = day_names[wday];
         /* Search for day name in CSV */
         const char *p = recurrence_days;
         while (*p) {
            while (*p == ' ' || *p == ',')
               p++;
            if (strncasecmp(p, needle, 3) == 0) {
               char next = p[3];
               if (next == '\0' || next == ',' || next == ' ')
                  return true;
            }
            while (*p && *p != ',')
               p++;
         }
         return false;
      }
      default:
         return false;
   }
}

/* The occurrence's time of day, in seconds: original_time (HH:MM on the
 * owner's clock), else when it was set to fire (@p fire_wall, its reading). */
static time_t occurrence_clock(const char *original_time, time_t fire_wall) {
   int hour = 0, minute = 0;
   if (original_time && original_time[0] && sscanf(original_time, "%d:%d", &hour, &minute) == 2 &&
       hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59)
      return (time_t)hour * 3600 + (time_t)minute * 60;
   return fire_wall % SCHED_RECUR_DAY;
}

time_t sched_next_occurrence(const tz_t *zone,
                             time_t fire_at,
                             const char *original_time,
                             sched_recurrence_t recurrence,
                             const char *recurrence_days,
                             bool snoozed,
                             time_t now) {
   if (recurrence == SCHED_RECUR_ONCE)
      return 0;
   const time_t fire_wall = tz_wall(zone, fire_at);
   const time_t clock = occurrence_clock(original_time, fire_wall);

   time_t day = fire_wall - fire_wall % SCHED_RECUR_DAY; /* the occurrence's date */
   /* Snoozed past midnight: that day's occurrence would come after fire_at.
    * Compared as instants, since a snooze within the hour clocks go back
    * moves the clock backwards on the same day. */
   if (snoozed && tz_from_wall(zone, day + clock) > fire_at)
      day -= SCHED_RECUR_DAY;

   const time_t today = tz_wall(zone, now) / SCHED_RECUR_DAY * SCHED_RECUR_DAY;
   const time_t step = (recurrence == SCHED_RECUR_WEEKLY ? 7 : 1) * SCHED_RECUR_DAY;
   time_t next = day + step;
   if (next < today) /* missed while down: catch up, keeping a weekly's day */
      next += (today - next + step - 1) / step * step;

   /* The first matching day whose time is still ahead (8 days covers any
    * weekday set; a weekly rule matches every 7th). */
   for (int i = 0; i <= 8; i++, next += step) {
      struct tm tm_day;
      gmtime_r(&next, &tm_day);
      if (!day_matches(tm_day.tm_wday, recurrence, recurrence_days))
         continue;
      const time_t candidate = tz_from_wall(zone, next + clock);
      if (candidate > now)
         return candidate;
   }
   return 0;
}

time_t sched_rezone_occurrence(const tz_t *from,
                               const tz_t *to,
                               time_t fire_at,
                               const char *original_time,
                               sched_recurrence_t recurrence,
                               const char *recurrence_days,
                               time_t now) {
   if (recurrence == SCHED_RECUR_ONCE)
      return fire_at;
   const time_t wall = tz_wall(from, fire_at);
   const time_t day = wall - wall % SCHED_RECUR_DAY;
   const time_t moved = tz_from_wall(to, day + occurrence_clock(original_time, wall));
   struct tm tm_day;
   gmtime_r(&day, &tm_day);
   /* A row placed on another clock than @p from can read as a day its rule
    * skips: then, like one already past, the next one after now. */
   if (moved > now && day_matches(tm_day.tm_wday, recurrence, recurrence_days))
      return moved;
   return sched_next_occurrence(to, moved, original_time, recurrence, recurrence_days, false, now);
}
