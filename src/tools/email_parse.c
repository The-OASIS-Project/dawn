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
 * Pure parsing helpers for the IMAP email backend — see email_parse.h.
 */

#define _GNU_SOURCE /* strptime, timegm, struct tm.tm_gmtoff */

#include "tools/email_parse.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Convert a strptime-filled tm that carries a zone offset (%z -> tm_gmtoff)
 * into a UTC epoch.  timegm() reads the broken-down time AS UTC, so subtract
 * the parsed offset to recover the true instant.  Returns 0 on overflow/failure
 * so a bad parse reads as "unknown" rather than a 1970 timestamp. */
static time_t tm_with_offset_to_utc(struct tm *tm) {
   long off = tm->tm_gmtoff;
   time_t utc = timegm(tm);
   if (utc <= 0)
      return 0;
   utc -= off;
   return utc > 0 ? utc : 0;
}

time_t email_parse_rfc822_date(const char *date_str) {
   if (!date_str || !date_str[0])
      return 0;

   struct tm tm_info;

   /* Preferred: forms carrying a numeric zone offset, so the result is a true
    * UTC instant independent of the sender's timezone. */
   static const char *const tz_fmts[] = {
      "%a, %d %b %Y %H:%M:%S %z",
      "%d %b %Y %H:%M:%S %z",
   };
   for (size_t i = 0; i < sizeof(tz_fmts) / sizeof(tz_fmts[0]); i++) {
      memset(&tm_info, 0, sizeof(tm_info));
      if (strptime(date_str, tz_fmts[i], &tm_info))
         return tm_with_offset_to_utc(&tm_info);
   }

   /* Fallback: no zone offset present.  Assume local time (best effort — the
    * historical behavior).  Returns 0 on total parse failure. */
   static const char *const notz_fmts[] = {
      "%a, %d %b %Y %H:%M:%S",
      "%d %b %Y %H:%M:%S",
   };
   for (size_t i = 0; i < sizeof(notz_fmts) / sizeof(notz_fmts[0]); i++) {
      memset(&tm_info, 0, sizeof(tm_info));
      if (strptime(date_str, notz_fmts[i], &tm_info)) {
         tm_info.tm_isdst = -1;
         time_t t = mktime(&tm_info);
         return t > 0 ? t : 0;
      }
   }
   return 0;
}

time_t email_parse_imap_internaldate(const char *idate) {
   if (!idate || !idate[0])
      return 0;

   /* Skip a leading quote if the caller passed the value with its quotes. */
   if (*idate == '"')
      idate++;

   struct tm tm_info;
   memset(&tm_info, 0, sizeof(tm_info));
   /* RFC 3501 date-time: "10-Sep-2026 15:45:00 +0000".  A single leading space
    * for one-digit days ("%e"-style) is tolerated by strptime's %d. */
   if (strptime(idate, "%d-%b-%Y %H:%M:%S %z", &tm_info))
      return tm_with_offset_to_utc(&tm_info);
   return 0;
}

bool email_imap_flags_contains(const char *flags_group, const char *flag) {
   if (!flags_group || !flag || !flag[0])
      return false;

   size_t flen = strlen(flag);
   const char *p = flags_group;
   while ((p = strcasestr(p, flag)) != NULL) {
      /* Left boundary: start-of-string, whitespace, or '('. */
      bool left_ok = (p == flags_group);
      if (!left_ok) {
         char prev = p[-1];
         left_ok = (prev == ' ' || prev == '\t' || prev == '(');
      }
      /* Right boundary: end-of-string, whitespace, or ')'. */
      char next = p[flen];
      bool right_ok = (next == '\0' || next == ' ' || next == '\t' || next == ')');

      if (left_ok && right_ok)
         return true;
      p += 1; /* overlapping search — advance one char and retry */
   }
   return false;
}
