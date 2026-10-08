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

time_t iso8601_parse(const char *iso_str) {
   if (!iso_str || !iso_str[0])
      return -1;

   struct tm tm_info;
   memset(&tm_info, 0, sizeof(tm_info));
   tm_info.tm_isdst = -1; /* Let mktime determine DST */

   /* Time-only format (HH:MM) — assume today, or tomorrow if already past */
   if (strlen(iso_str) <= 5 && strchr(iso_str, ':')) {
      int hour = 0, min = 0;
      if (sscanf(iso_str, "%d:%d", &hour, &min) != 2)
         return -1;
      if (hour < 0 || hour > 23 || min < 0 || min > 59)
         return -1;

      time_t now = time(NULL);
      localtime_r(&now, &tm_info);
      tm_info.tm_hour = hour;
      tm_info.tm_min = min;
      tm_info.tm_sec = 0;

      time_t result = mktime(&tm_info);
      if (result <= now)
         result += 86400;
      return result;
   }

   /* Full ISO 8601; a space between date and time is accepted as well as 'T' */
   int year, month, day, hour = 0, min = 0, sec = 0;
   int parsed = sscanf(iso_str, "%d-%d-%d%*1[T ]%d:%d:%d", &year, &month, &day, &hour, &min, &sec);
   if (parsed < 3)
      return -1;
   /* Out of range is an error, not something for mktime to roll over (Feb 31 isn't Mar 3) */
   static const int days_in[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   if (month < 1 || month > 12)
      return -1;
   const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
   const int last_day = days_in[month - 1] + (month == 2 && leap);
   if (day < 1 || day > last_day || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 ||
       sec > 60)
      return -1;

   tm_info.tm_year = year - 1900;
   tm_info.tm_mon = month - 1;
   tm_info.tm_mday = day;
   tm_info.tm_hour = hour;
   tm_info.tm_min = min;
   tm_info.tm_sec = sec;

   /* Find timezone suffix after the time portion */
   const char *tz_start = iso_str;
   const char *t_pos = strpbrk(iso_str, "T ");
   if (t_pos) {
      tz_start = t_pos + 1;
      /* Skip past HH:MM:SS digits */
      while (*tz_start && (*tz_start == ':' || (*tz_start >= '0' && *tz_start <= '9')))
         tz_start++;
   } else {
      tz_start = iso_str + strlen(iso_str); /* No T, no timezone */
   }

   int tz_offset_sec = 0;
   if (parse_tz_offset(tz_start, &tz_offset_sec)) {
      /* Timezone-aware: mktime assumes local, so adjust by the difference
       * between the local offset and the input's explicit offset. */
      time_t local_result = mktime(&tm_info);
      if (local_result == (time_t)-1)
         return -1;

      struct tm local_tm;
      localtime_r(&local_result, &local_tm);
      long local_offset = local_tm.tm_gmtoff; /* seconds east of UTC */

      return local_result + (local_offset - tz_offset_sec);
   }

   /* No timezone suffix: interpret as local time (uses process TZ) */
   return mktime(&tm_info);
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
