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
 * libFuzzer harness for calendar_rrule_expand: a recurrence rule from a
 * calendar's server must expand in bounded time (libFuzzer's -timeout).
 * Input: one byte of flags (bit 0: all-day; bit 1: repeat in New York), then
 * the RRULE text.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tools/calendar_rrule.h"

#define JAN_1_2020 1577836800
#define OCT_1_2026 1790812800

static void count_one(time_t start, void *ctx) {
   (void)start;
   (*(int *)ctx)++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   if (size < 1 || size > 4096)
      return 0;
   const bool all_day = (data[0] & 1) != 0;
   char *rule = malloc(size);
   if (!rule)
      return 0;
   memcpy(rule, data + 1, size - 1);
   rule[size - 1] = '\0';

   int seen = 0;
   int count = 0;
   const char *tzid = (data[0] & 2) ? "America/New_York" : NULL;
   calendar_rrule_expand(rule, JAN_1_2020 + 9 * 3600, tzid, all_day, "2020-01-01", 3600, OCT_1_2026,
                         OCT_1_2026 + 395 * 86400, count_one, &seen, &count);
   if (count != seen || count > CALENDAR_RRULE_MAX_OCCURRENCES)
      abort();
   free(rule);
   return 0;
}
