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
 * Which writable calendar a name means, never picking among equals.
 */

#include "tools/calendar_pick.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/str_fuzzy.h"

/* Whether another calendar in @p cals shares calendar @p i's display name. */
static bool name_shared(const calendar_calendar_t *cals, int count, int i) {
   for (int j = 0; j < count; j++) {
      if (j != i && strcasecmp(cals[j].display_name, cals[i].display_name) == 0) {
         return true;
      }
   }
   return false;
}

/* Whether calendar @p i's account name tells it from every twin (another
 * calendar of its display name). */
static bool account_tells_apart(const calendar_calendar_t *cals, int count, int i) {
   if (!cals[i].account_name[0]) {
      return false;
   }
   for (int j = 0; j < count; j++) {
      if (j != i && strcasecmp(cals[j].display_name, cals[i].display_name) == 0 &&
          strcasecmp(cals[j].account_name, cals[i].account_name) == 0) {
         return false;
      }
   }
   return true;
}

void calendar_pick_label(const calendar_calendar_t *cals,
                         int count,
                         int i,
                         char *out,
                         size_t out_size) {
   if (!out || out_size == 0) {
      return;
   }
   if (!name_shared(cals, count, i)) {
      snprintf(out, out_size, "%s", cals[i].display_name);
   } else if (account_tells_apart(cals, count, i)) {
      snprintf(out, out_size, "%s (%s)", cals[i].display_name, cals[i].account_name);
   } else if (cals[i].account_name[0]) { /* twins in one account: its id tells them apart */
      snprintf(out, out_size, "%s (%s #%lld)", cals[i].display_name, cals[i].account_name,
               (long long)cals[i].id);
   } else {
      snprintf(out, out_size, "%s (#%lld)", cals[i].display_name, (long long)cals[i].id);
   }
}

bool calendar_pick_names(const calendar_calendar_t *cals, int count, int i, const char *name) {
   if (!name) {
      return false;
   }
   if (strcasecmp(cals[i].display_name, name) == 0) {
      return true;
   }
   char label[CALENDAR_PICK_LABEL_MAX];
   calendar_pick_label(cals, count, i, label, sizeof(label));
   return strcasecmp(label, name) == 0;
}

calendar_pick_rc_t calendar_pick_writable(const calendar_calendar_t *cals,
                                          int count,
                                          const char *name,
                                          int *target) {
   if (!name || !name[0]) {
      for (int i = 0; i < count; i++) {
         if (!cals[i].account_read_only) {
            *target = i;
            return CALENDAR_PICK_OK;
         }
      }
      return CALENDAR_PICK_READONLY; /* all calendars read-only */
   }
   /* A label or display name: one writable calendar of it, or a question.  A
    * label names one calendar, so it can't be two. */
   bool named_read_only = false;
   int found = -1;
   for (int i = 0; i < count; i++) {
      if (!calendar_pick_names(cals, count, i, name)) {
         continue;
      }
      if (cals[i].account_read_only) {
         named_read_only = true;
         continue;
      }
      if (found >= 0) {
         return CALENDAR_PICK_NOT_FOUND; /* two of that name */
      }
      found = i;
   }
   if (found >= 0) {
      *target = found;
      return CALENDAR_PICK_OK;
   }
   if (named_read_only) {
      return CALENDAR_PICK_READONLY; /* the user named a read-only calendar */
   }
   /* "family" for "Family Calendar": only when it picks one calendar. */
   char needle[128], hay[128];
   str_fuzzy_tolower(needle, name, sizeof(needle));
   for (int i = 0; i < count; i++) {
      str_fuzzy_tolower(hay, cals[i].display_name, sizeof(hay));
      if (!cals[i].account_read_only && strstr(hay, needle)) {
         if (found >= 0) {
            return CALENDAR_PICK_NOT_FOUND;
         }
         found = i;
      }
   }
   if (found < 0) {
      return CALENDAR_PICK_NOT_FOUND;
   }
   *target = found;
   return CALENDAR_PICK_OK;
}
