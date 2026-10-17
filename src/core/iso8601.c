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
 * ISO 8601 absolute timestamp parser implementation.  Extracted from
 * scheduler_tool.c / calendar_tool.c which previously held byte-identical
 * copies of this code; absorbed memory_extraction.c's parse_iso8601_date
 * for full consolidation.
 */

#define _GNU_SOURCE /* strptime, timegm, tm_gmtoff */

#include "core/iso8601.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void iso8601_format_date(int year, int month, int day, char *out, size_t out_size) {
   if (!out || out_size == 0) {
      return;
   }
   if (out_size < 11) {
      out[0] = '\0';
      return;
   }
   /* Normalize each component to a fixed printed width so the output is
    * provably 10 chars + NUL (see header note on -Wformat-truncation). */
   int y = ((year % 10000) + 10000) % 10000;
   int mo = ((month % 100) + 100) % 100;
   int d = ((day % 100) + 100) % 100;
   snprintf(out, out_size, "%04d-%02d-%02d", y, mo, d);
}

/**
 * @brief Parse timezone offset from ISO 8601 suffix.
 *
 * Handles 'Z' (UTC) and an offset of +HH, +HH:MM or +HHMM (or '-'), read by
 * digit position; a one-digit hour is accepted only before a colon (+5:30).
 *
 * @param suffix Pointer to the timezone part of the string (after seconds).
 * @param offset_sec Output: offset from UTC in seconds (e.g., -18000 for -05:00).
 * @return true if a timezone suffix was found and parsed, false if local time.
 */
static bool parse_tz_offset(const char *suffix, int *offset_sec) {
   if (!suffix || !suffix[0])
      return false;

   if (suffix[0] == 'Z' || suffix[0] == 'z') {
      *offset_sec = 0;
      return true;
   }

   if (suffix[0] == '+' || suffix[0] == '-') {
      const char *p = suffix + 1;
      int digits = 0, tz_h = 0, tz_m = 0;
      while (digits < 2 && isdigit((unsigned char)p[digits]))
         tz_h = tz_h * 10 + (p[digits++] - '0');
      if (digits == 0)
         return false;
      p += digits;
      if (*p == ':')
         p++;
      else if (digits < 2)
         p = ""; /* +5 has no minutes; +530 is ambiguous and not read */
      if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]))
         tz_m = (p[0] - '0') * 10 + (p[1] - '0');
      if (tz_h > 14 || tz_m > 59 || (tz_h == 14 && tz_m > 0))
         return false;
      *offset_sec = tz_h * 3600 + tz_m * 60;
      if (suffix[0] == '-')
         *offset_sec = -*offset_sec;
      return true;
   }

   return false;
}

bool iso8601_fields(const char *text, iso8601_fields_t *out) {
   if (!text || !text[0] || !out)
      return false;
   memset(out, 0, sizeof(*out));

   /* A time alone (HH:MM) */
   if (strlen(text) <= 5 && strchr(text, ':')) {
      if (sscanf(text, "%d:%d", &out->hour, &out->min) != 2 || out->hour < 0 || out->hour > 23 ||
          out->min < 0 || out->min > 59)
         return false;
      out->time_only = true;
      return true;
   }

   /* A date, with a time after a 'T' or a space */
   int parsed = sscanf(text, "%d-%d-%d%*1[T ]%d:%d:%d", &out->year, &out->month, &out->day,
                       &out->hour, &out->min, &out->sec);
   if (parsed < 3)
      return false;
   /* Out of range is an error, not something to roll over (Feb 31 isn't Mar 3) */
   static const int days_in[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   if (out->year < 1 || out->year > 9999 || out->month < 1 || out->month > 12)
      return false;
   const bool leap = (out->year % 4 == 0 && out->year % 100 != 0) || out->year % 400 == 0;
   const int last_day = days_in[out->month - 1] + (out->month == 2 && leap);
   if (out->day < 1 || out->day > last_day || out->hour < 0 || out->hour > 23 || out->min < 0 ||
       out->min > 59 || out->sec < 0 || out->sec > 60)
      return false;

   /* The offset follows the time's digits and any fractional seconds */
   const char *t_pos = strpbrk(text, "T ");
   if (!t_pos)
      return true;
   const char *tz_start = t_pos + 1;
   while (*tz_start == ':' || isdigit((unsigned char)*tz_start))
      tz_start++;
   if (*tz_start == '.')
      for (tz_start++; isdigit((unsigned char)*tz_start); tz_start++)
         ;
   out->has_offset = parse_tz_offset(tz_start, &out->offset_sec);
   return true;
}

int64_t iso8601_parse_date_utc(const char *iso_str) {
   if (!iso_str || !*iso_str)
      return 0;

   struct tm tm = { 0 };

   /* Try full date first: YYYY-MM-DD */
   char *end = strptime(iso_str, "%Y-%m-%d", &tm);
   if (!end) {
      /* Year-only fallback: YYYY -> Jan 1 of that year */
      memset(&tm, 0, sizeof(tm));
      end = strptime(iso_str, "%Y", &tm);
      if (!end)
         return 0;
      tm.tm_mday = 1;
      tm.tm_mon = 0;
   }

   /* Convert as UTC.  0 sentinel on failure mirrors the original semantics
    * — callers (e.g., relation valid_from/valid_to) treat 0 as "no bound". */
   time_t t = timegm(&tm);
   if (t == (time_t)-1)
      return 0;
   return (int64_t)t;
}
