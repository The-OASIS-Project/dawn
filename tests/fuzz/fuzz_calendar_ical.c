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
 * libFuzzer harness for calendar_ical: an event a calendar's server sent is
 * read, and edited in place, in bounded time and memory.  The first byte
 * picks the edit; the rest is the event's iCalendar text.
 * Built by tests/fuzz/run_fuzzers.sh.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tools/calendar_ical.h"

/* Mid-October 2026, inside the seeds' series. */
#define FUZZ_WHEN 1792445400

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   if (size < 1)
      return 0;
   const uint8_t op = data[0];
   char *ical = malloc(size);
   if (!ical)
      return 0;
   memcpy(ical, data + 1, size - 1);
   ical[size - 1] = '\0';

   caldav_event_t evt;
   memset(&evt, 0, sizeof(evt));
   if (calendar_ical_read(ical, &evt, NULL) && (evt.exdate_count > CALENDAR_ICAL_MAX_EXCEPTIONS ||
                                                evt.override_count > CALENDAR_ICAL_MAX_EXCEPTIONS))
      abort();
   caldav_event_free(&evt);

   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   c.user_tzid = "America/New_York";
   static const char *const me[] = { "me@example.com" };
   c.accounts = me;
   c.account_count = 1;
   switch (op % 6) {
      case 0:
         c.summary = "renamed";
         break;
      case 1:
         c.set_start = true;
         c.start = FUZZ_WHEN + (op >> 3) * 1800;
         break;
      case 2:
         c.rrule = (op & 0x80) ? "" : "FREQ=WEEKLY;BYDAY=MO,TU;UNTIL=20261231";
         break;
      case 3:
         c.reminder_minutes = op;
         break;
      case 4:
         c.occurrence = FUZZ_WHEN;
         c.cancel = (op & 0x80) != 0;
         c.summary = "one";
         break;
      default:
         c.occurrence = FUZZ_WHEN;
         c.set_start = true;
         c.start = FUZZ_WHEN + 3600;
         break;
   }
   char *out = NULL;
   if (calendar_ical_edit(ical, &c, &out) == CALENDAR_ICAL_OK && !out)
      abort();
   if (out) {
      /* What DAWN writes, it can read back. */
      memset(&evt, 0, sizeof(evt));
      calendar_ical_read(out, &evt, NULL);
      caldav_event_free(&evt);
      free(out);
   }
   free(ical);
   return 0;
}
