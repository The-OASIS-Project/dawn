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
 * US equity market session calendar — see market_hours.h.
 */

#include "core/market_hours.h"

#include <stdbool.h>

/* nth (1-based) occurrence of weekday (0=Sun) in month0 (0-based) of year → mday. */
static int nth_weekday(int year, int month0, int weekday, int nth) {
   struct tm t = { 0 };
   t.tm_year = year - 1900;
   t.tm_mon = month0;
   t.tm_mday = 1;
   time_t first = timegm(&t);
   struct tm g;
   gmtime_r(&first, &g);
   return 1 + ((weekday - g.tm_wday + 7) % 7) + (nth - 1) * 7;
}

/* Last occurrence of weekday in month0 → mday (the 5th if it exists, else the 4th). */
static int last_weekday(int year, int month0, int weekday) {
   static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   int dim = mdays[month0];
   if (month0 == 1) {
      bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
      if (leap) {
         dim = 29;
      }
   }
   int d = nth_weekday(year, month0, weekday, 5);
   return d <= dim ? d : d - 7;
}

/* Weekday (0=Sun) of a specific calendar date. */
static int weekday_of(int year, int month0, int day) {
   struct tm t = { 0 };
   t.tm_year = year - 1900;
   t.tm_mon = month0;
   t.tm_mday = day;
   time_t s = timegm(&t);
   struct tm g;
   gmtime_r(&s, &g);
   return g.tm_wday;
}

/* Observed mday for a fixed mid-month holiday: Sat→Fri (day−1), Sun→Mon (day+1).
 * Only used for Jun 19 / Jul 4 / Dec 25, which never shift across a month edge. */
static int observed_day(int year, int month0, int day) {
   int w = weekday_of(year, month0, day);
   if (w == 6) {
      return day - 1;
   }
   if (w == 0) {
      return day + 1;
   }
   return day;
}

/* Good Friday (Easter Sunday − 2), Easter via the anonymous Gregorian computus. */
static void good_friday(int year, int *month0, int *mday) {
   int a = year % 19;
   int b = year / 100, c = year % 100;
   int d = b / 4, e = b % 4;
   int f = (b + 8) / 25;
   int g = (b - f + 1) / 3;
   int h = (19 * a + b - d - g + 15) % 30;
   int i = c / 4, k = c % 4;
   int l = (32 + 2 * e + 2 * i - h - k) % 7;
   int m = (a + 11 * h + 22 * l) / 451;
   int month = (h + l - 7 * m + 114) / 31;            /* 3 = March, 4 = April */
   int easter_day = ((h + l - 7 * m + 114) % 31) + 1; /* Easter Sunday */
   struct tm t = { 0 };
   t.tm_year = year - 1900;
   t.tm_mon = month - 1;
   t.tm_mday = easter_day - 2; /* Good Friday; timegm normalizes a month underflow */
   time_t s = timegm(&t);
   struct tm gtm;
   gmtime_r(&s, &gtm);
   *month0 = gtm.tm_mon;
   *mday = gtm.tm_mday;
}

/* True if the given US/Eastern calendar date is a full NYSE close. */
static bool is_holiday(int year, int mon0, int mday) {
   /* New Year's Day (Jan): closed on Jan 1 if a weekday; Sun → observed Mon (Jan 2);
    * Sat → NYSE does NOT close (no Dec-31 closure), so no shift. */
   if (mon0 == 0) {
      int w1 = weekday_of(year, 0, 1);
      if (w1 == 0) {
         if (mday == 2) {
            return true;
         }
      } else if (w1 != 6) {
         if (mday == 1) {
            return true;
         }
      }
   }

   /* Fixed mid-month holidays with weekend-observed shifting. */
   if (mon0 == 5 && year >= 2022 && mday == observed_day(year, 5, 19)) {
      return true; /* Juneteenth — NYSE first observed it in 2022 */
   }
   if (mon0 == 6 && mday == observed_day(year, 6, 4)) {
      return true; /* Independence Day */
   }
   if (mon0 == 11 && mday == observed_day(year, 11, 25)) {
      return true; /* Christmas */
   }

   /* Floating Monday/Thursday holidays. */
   if (mon0 == 0 && mday == nth_weekday(year, 0, 1, 3)) {
      return true; /* MLK — 3rd Monday of January */
   }
   if (mon0 == 1 && mday == nth_weekday(year, 1, 1, 3)) {
      return true; /* Washington's Birthday — 3rd Monday of February */
   }
   if (mon0 == 4 && mday == last_weekday(year, 4, 1)) {
      return true; /* Memorial Day — last Monday of May */
   }
   if (mon0 == 8 && mday == nth_weekday(year, 8, 1, 1)) {
      return true; /* Labor Day — 1st Monday of September */
   }
   if (mon0 == 10 && mday == nth_weekday(year, 10, 4, 4)) {
      return true; /* Thanksgiving — 4th Thursday of November */
   }

   int gfm, gfd;
   good_friday(year, &gfm, &gfd);
   if (mon0 == gfm && mday == gfd) {
      return true;
   }
   return false;
}

/* True if the date is an early-close (13:00 ET) NYSE half-day. @p wday is the
 * date's weekday (0=Sun). Each half-day is guarded so it never overlaps the full
 * close it neighbors (e.g. July 3 is the observed July-4 holiday when July 4 is a
 * Saturday, not a half-day). */
static bool is_half_day(int year, int mon0, int mday, int wday) {
   /* Day after Thanksgiving (Friday). */
   if (mon0 == 10 && mday == nth_weekday(year, 10, 4, 4) + 1) {
      return true;
   }
   /* Christmas Eve (Dec 24) on a weekday, unless it is itself the observed close. */
   if (mon0 == 11 && mday == 24 && wday >= 1 && wday <= 5 && observed_day(year, 11, 25) != 24) {
      return true;
   }
   /* July 3 on a weekday, unless it is itself the observed Independence Day close. */
   if (mon0 == 6 && mday == 3 && wday >= 1 && wday <= 5 && observed_day(year, 6, 4) != 3) {
      return true;
   }
   return false;
}

/* US Eastern DST: 2nd Sunday of March 07:00 UTC (02:00 EST) → 1st Sunday of
 * November 06:00 UTC (02:00 EDT). */
static bool us_eastern_is_dst(time_t utc) {
   struct tm g;
   gmtime_r(&utc, &g);
   int year = g.tm_year + 1900;
   struct tm st = { 0 };
   st.tm_year = year - 1900;
   st.tm_mon = 2;
   st.tm_mday = nth_weekday(year, 2, 0, 2);
   st.tm_hour = 7;
   struct tm en = { 0 };
   en.tm_year = year - 1900;
   en.tm_mon = 10;
   en.tm_mday = nth_weekday(year, 10, 0, 1);
   en.tm_hour = 6;
   return utc >= timegm(&st) && utc < timegm(&en);
}

market_session_t market_hours_us_equity(time_t utc) {
   time_t et = utc - (us_eastern_is_dst(utc) ? 4 * 3600 : 5 * 3600);
   struct tm e;
   gmtime_r(&et, &e);
   int year = e.tm_year + 1900;

   if (e.tm_wday == 0 || e.tm_wday == 6) {
      return MARKET_CLOSED;
   }
   if (is_holiday(year, e.tm_mon, e.tm_mday)) {
      return MARKET_CLOSED;
   }

   int mins = e.tm_hour * 60 + e.tm_min;
   int reg_close = is_half_day(year, e.tm_mon, e.tm_mday, e.tm_wday) ? 13 * 60 : 16 * 60;
   if (mins >= 9 * 60 + 30 && mins < reg_close) {
      return MARKET_REGULAR;
   }
   if (mins >= 7 * 60 && mins < 9 * 60 + 30) {
      return MARKET_PRE;
   }
   if (mins >= reg_close && mins < 20 * 60) {
      return MARKET_POST;
   }
   return MARKET_CLOSED;
}

const char *market_session_str(market_session_t s) {
   switch (s) {
      case MARKET_REGULAR:
         return "regular";
      case MARKET_PRE:
         return "pre";
      case MARKET_POST:
         return "post";
      case MARKET_CLOSED:
      default:
         return "closed";
   }
}
