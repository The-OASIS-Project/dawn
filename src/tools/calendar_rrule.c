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
 * A recurring event's occurrences within a window, with bounded work.
 */

#include "tools/calendar_rrule.h"

#include <ctype.h>
#include <libical/ical.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dawn_error.h"
#include "logging.h"

/* Recurrence steps libical may walk to reach the window for a rule with a
 * COUNT (which has to be counted from its start).  Its inner loop tries every
 * period, matching or not, so this bounds that work too. */
#define CALENDAR_RRULE_MAX_PERIODS 100000

/* Most recurrence steps taken for one event, counting the ones before the
 * window (a COUNT rule starting long before it). */
#define CALENDAR_RRULE_MAX_STEPS 10000

/* A period's approximate length in seconds (months and years rounded down,
 * which only overestimates the count). */
static long period_seconds(icalrecurrencetype_frequency freq) {
   switch (freq) {
      case ICAL_HOURLY_RECURRENCE:
         return 3600;
      case ICAL_DAILY_RECURRENCE:
         return 86400;
      case ICAL_WEEKLY_RECURRENCE:
         return 7 * 86400;
      case ICAL_MONTHLY_RECURRENCE:
         return 28 * 86400;
      default:
         return 365 * 86400;
   }
}

/* Entries in a libical BY* list (ICAL_RECURRENCE_ARRAY_MAX ends it). */
static int by_count(const short *list, int size) {
   int n = 0;
   while (n < size && list[n] != ICAL_RECURRENCE_ARRAY_MAX)
      n++;
   return n;
}

/* Periods (years, or months for a monthly rule ×12) the probe looks through:
 * the Gregorian calendar's dates and weekdays line up the same way every 28
 * years within a century. */
#define CALENDAR_RRULE_PROBE_YEARS 28

static bool leap_year(int y) {
   return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_length(int y, int m) {
   static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   return m == 2 && leap_year(y) ? 29 : days[m - 1];
}

/* 1 = Sunday .. 7 = Saturday, as libical numbers weekdays. */
static int weekday_of(int y, int m, int d) {
   static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
   if (m < 3)
      y--;
   return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7 + 1;
}

static bool in_by(const short *list, int size, int value) {
   for (int k = 0; k < size && list[k] != ICAL_RECURRENCE_ARRAY_MAX; k++) {
      if (list[k] == value)
         return true;
   }
   return false;
}

/* Whether a day of the month or year, counted from the start (@p n of
 * @p len), is listed in @p list as a positive or a from-the-end number. */
static bool in_by_signed(const short *list, int size, int n, int len) {
   return in_by(list, size, n) || in_by(list, size, n - len - 1);
}

/* Whether date y-m-d passes a monthly or yearly rule's date filters. */
static bool date_matches(const struct icalrecurrencetype *r,
                         struct icaltimetype first,
                         int y,
                         int m,
                         int d) {
   const bool monthly = r->freq == ICAL_MONTHLY_RECURRENCE;
   const int n_month = by_count(r->by_month, ICAL_BY_MONTH_SIZE);
   const int n_mday = by_count(r->by_month_day, ICAL_BY_MONTHDAY_SIZE);
   const int n_yday = by_count(r->by_year_day, ICAL_BY_YEARDAY_SIZE);
   const int n_day = by_count(r->by_day, ICAL_BY_DAY_SIZE);
   if (n_month && !in_by(r->by_month, ICAL_BY_MONTH_SIZE, m))
      return false;
   const int mlen = month_length(y, m);
   if (n_mday && !in_by_signed(r->by_month_day, ICAL_BY_MONTHDAY_SIZE, d, mlen))
      return false;
   int yday = d;
   for (int k = 1; k < m; k++)
      yday += month_length(y, k);
   const int ylen = leap_year(y) ? 366 : 365;
   if (n_yday && !in_by_signed(r->by_year_day, ICAL_BY_YEARDAY_SIZE, yday, ylen))
      return false;
   if (n_day) {
      /* A position counts within the month (monthly, or yearly in listed
       * months) or the year. */
      const bool in_month = monthly || n_month;
      const int n = in_month ? d : yday;
      const int len = in_month ? mlen : ylen;
      const int wd = weekday_of(y, m, d);
      bool any = false;
      for (int k = 0; k < n_day && !any; k++) {
         const int pos = icalrecurrencetype_day_position(r->by_day[k]);
         if ((int)icalrecurrencetype_day_day_of_week(r->by_day[k]) != wd)
            continue;
         any = pos == 0 || (pos > 0 && (n - 1) / 7 + 1 == pos) ||
               (pos < 0 && (len - n) / 7 + 1 == -pos);
      }
      if (!any)
         return false;
   }
   /* With no day filters, the rule repeats on its first date's day (and,
    * yearly with no months listed, month). */
   if (!n_mday && !n_yday && !n_day)
      return d == first.day && (monthly || n_month || m == first.month);
   return true;
}

/* Whether a monthly or yearly rule has a date in the periods it repeats in
 * (INTERVAL apart from its first), within CALENDAR_RRULE_PROBE_YEARS from
 * @p from_year: libical searches for a match out to year 20000 (with ICU),
 * seconds for a monthly or yearly rule that never matches, so one is refused
 * here first.  Exact for month, day of month or year, weekday and position,
 * and BYSETPOS (against each period's dates times its hours, minutes and
 * seconds); a rule with BYWEEKNO, or with both BYYEARDAY and BYMONTHDAY, is
 * taken as possible. */
static bool rule_has_a_date(const struct icalrecurrencetype *r,
                            struct icaltimetype first,
                            int from_year) {
   if (r->freq != ICAL_MONTHLY_RECURRENCE && r->freq != ICAL_YEARLY_RECURRENCE)
      return true;
   if (by_count(r->by_week_no, ICAL_BY_WEEKNO_SIZE) ||
       (by_count(r->by_year_day, ICAL_BY_YEARDAY_SIZE) &&
        by_count(r->by_month_day, ICAL_BY_MONTHDAY_SIZE)))
      return true;
   const bool monthly = r->freq == ICAL_MONTHLY_RECURRENCE;
   const int interval = r->interval > 0 ? r->interval : 1;
   const int n_hour = by_count(r->by_hour, ICAL_BY_HOUR_SIZE);
   const int n_minute = by_count(r->by_minute, ICAL_BY_MINUTE_SIZE);
   const int n_second = by_count(r->by_second, ICAL_BY_SECOND_SIZE);
   const int per_date = (n_hour ? n_hour : 1) * (n_minute ? n_minute : 1) *
                        (n_second ? n_second : 1);
   const int n_setpos = by_count(r->by_set_pos, ICAL_BY_SETPOS_SIZE);
   int need = n_setpos ? INT_MAX : 1; /* candidates a period needs */
   for (int k = 0; k < n_setpos; k++) {
      if (r->by_set_pos[k] != 0 && abs(r->by_set_pos[k]) < need)
         need = abs(r->by_set_pos[k]);
   }

   /* Periods counted in months (monthly) or years from the first date. */
   const long first_p = monthly ? (long)first.year * 12 + first.month - 1 : first.year;
   const long from_p = monthly ? (long)from_year * 12 : from_year;
   long p = first_p;
   if (from_p > first_p)
      p += (from_p - first_p + interval - 1) / interval * interval;
   const int periods = CALENDAR_RRULE_PROBE_YEARS * (monthly ? 12 : 1);
   for (int j = 0; j < periods; j++, p += interval) {
      const int y = (int)(monthly ? p / 12 : p);
      if (y > 9999)
         return true; /* past what it can check: libical bounds this itself */
      long dates = 0;
      for (int m = monthly ? (int)(p % 12) + 1 : 1; m <= (monthly ? (int)(p % 12) + 1 : 12); m++) {
         for (int d = 1; d <= month_length(y, m); d++)
            dates += date_matches(r, first, y, m, d);
      }
      if (dates * per_date >= need)
         return true;
   }
   return false;
}

/* @p t's wall-clock time as seconds (its fields read as UTC): the clock an
 * hourly rule steps on, which a daylight-saving change doesn't move. */
static time_t wall_seconds(struct icaltimetype t) {
   struct tm tm = { 0 };
   tm.tm_year = t.year - 1900;
   tm.tm_mon = t.month - 1;
   tm.tm_mday = t.day;
   tm.tm_hour = t.hour;
   tm.tm_min = t.minute;
   tm.tm_sec = t.second;
   return timegm(&tm);
}

/* tzdata's own list of old zone names ("L <current> <old>" lines): read once,
 * so an old name resolves whether the system installs it as a link, a copy,
 * or not at all (newer systems ship old names only in an optional package). */
#define CALENDAR_RRULE_TZDATA_ZI "/usr/share/zoneinfo/tzdata.zi"

/* Most old names kept (tzdata has a few hundred). */
#define CALENDAR_RRULE_MAX_LINKS 1024

static struct {
   pthread_once_t once;
   char (*names)[2][64]; /* [i][0] old, [i][1] current */
   int count;
} s_links = { .once = PTHREAD_ONCE_INIT };

static void load_links(void) {
   FILE *f = fopen(CALENDAR_RRULE_TZDATA_ZI, "r");
   if (!f)
      return;
   s_links.names = calloc(CALENDAR_RRULE_MAX_LINKS, sizeof(*s_links.names));
   char line[256];
   while (s_links.names && s_links.count < CALENDAR_RRULE_MAX_LINKS &&
          fgets(line, sizeof(line), f)) {
      char current[64], old[64];
      if (sscanf(line, "L %63s %63s", current, old) == 2) {
         memcpy(s_links.names[s_links.count][0], old, sizeof(old));
         memcpy(s_links.names[s_links.count][1], current, sizeof(current));
         s_links.count++;
      }
   }
   fclose(f);
}

/* The current name tzdata gives old name @p tzid, or NULL. */
static const char *current_name(const char *tzid) {
   pthread_once(&s_links.once, load_links);
   for (int i = 0; i < s_links.count; i++) {
      if (strcmp(s_links.names[i][0], tzid) == 0)
         return s_links.names[i][1];
   }
   return NULL;
}

/* libical's own zone named @p location, or NULL; never loads a zone it
 * doesn't list (for one it doesn't, libical opens a file of that name and
 * keeps it, so endless aliases would grow the daemon). */
static icaltimezone *listed_zone(const char *location) {
   if (strcmp(location, "UTC") == 0)
      return icaltimezone_get_utc_timezone();
   icalarray *zones = icaltimezone_get_builtin_timezones();
   for (size_t i = 0; zones && i < zones->num_elements; i++) {
      icaltimezone *z = icalarray_element_at(zones, i);
      const char *name = icaltimezone_get_location(z);
      if (name && strcmp(name, location) == 0)
         return icaltimezone_get_builtin_timezone(location);
   }
   return NULL;
}

/* Whether @p name is a spelling of UTC. */
static bool utc_name(const char *name) {
   static const char *const names[] = { "UTC",       "GMT",           "UCT",      "Zulu",
                                        "Universal", "Greenwich",     "Etc/UTC",  "Etc/GMT",
                                        "Etc/UCT",   "Etc/Universal", "Etc/Zulu", "Etc/Greenwich" };
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      if (strcmp(name, names[i]) == 0)
         return true;
   }
   return false;
}

/* The system's zone name, read once.  A set TZ is the zone (as for the C
 * library, which then ignores /etc/localtime): its name when one is
 * listed, else none.  Otherwise the zone file /etc/localtime links to, else
 * /etc/timezone. */
static char s_system_tzid[64];
static pthread_once_t s_system_once = PTHREAD_ONCE_INIT;

static void read_system_tzid(void) {
   const char *tz = getenv("TZ");
   if (tz) {
      if (*tz == ':')
         tz++;
      const char *path = strstr(tz, "zoneinfo/");
      if (path)
         tz = path + strlen("zoneinfo/");
      if (calendar_rrule_zone(tz))
         snprintf(s_system_tzid, sizeof(s_system_tzid), "%s", tz);
      return;
   }
   char link[256];
   const ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
   if (n > 0) {
      link[n] = '\0';
      const char *name = strstr(link, "zoneinfo/");
      if (name) {
         snprintf(s_system_tzid, sizeof(s_system_tzid), "%s", name + strlen("zoneinfo/"));
         return;
      }
   }
   FILE *f = fopen("/etc/timezone", "r");
   if (f) {
      if (fgets(s_system_tzid, sizeof(s_system_tzid), f))
         s_system_tzid[strcspn(s_system_tzid, " \t\r\n")] = '\0';
      fclose(f);
   }
}

const char *calendar_rrule_user_tzid(const char *configured) {
   if (configured && configured[0])
      return configured;
   pthread_once(&s_system_once, read_system_tzid);
   return s_system_tzid;
}

icaltimezone *calendar_rrule_zone(const char *tzid) {
   if (!tzid || !tzid[0])
      return NULL;
   if (utc_name(tzid))
      return icaltimezone_get_utc_timezone();
   icaltimezone *zone = listed_zone(tzid);
   if (zone)
      return zone;
   /* An old name ("Europe/Kiev", "US/Eastern"): tzdata names its current one. */
   const char *current = current_name(tzid);
   if (!current)
      return NULL;
   return utc_name(current) ? icaltimezone_get_utc_timezone() : listed_zone(current);
}

/* The zone a timed event repeats in: @p tzid's, else UTC. */
static icaltimezone *repeat_zone(const char *tzid) {
   icaltimezone *zone = calendar_rrule_zone(tzid);
   return zone ? zone : icaltimezone_get_utc_timezone();
}

/* calendar_rrule_expand for a parsed rule (@p recur is changed: UNTIL). */
static int expand_parsed(const char *rrule,
                         struct icalrecurrencetype recur,
                         time_t dtstart,
                         const char *tzid,
                         bool all_day,
                         const char *start_date,
                         int duration,
                         time_t range_start,
                         time_t range_end,
                         calendar_rrule_emit_fn emit,
                         void *ctx,
                         int *count_out) {
   /* Rules repeating every second or minute: no calendar app makes them, and
    * libical tries every second (or minute) between matches.  A non-Gregorian
    * calendar scale (RSCALE) can make libical's search never end. */
   if (recur.freq == ICAL_NO_RECURRENCE || recur.freq == ICAL_SECONDLY_RECURRENCE ||
       recur.freq == ICAL_MINUTELY_RECURRENCE || recur.rscale)
      return FAILURE;

   icaltimezone *utc = icaltimezone_get_utc_timezone();
   /* Repeat in the event's own zone, so a weekly 10:00 stays at 10:00 across a
    * daylight-saving change. */
   icaltimezone *zone = all_day ? utc : repeat_zone(tzid);
   struct icaltimetype first;
   if (all_day) {
      first = icaltime_from_string(start_date && start_date[0] ? start_date : "19700101");
      first.is_date = 1;
   } else {
      first = icaltime_from_timet_with_zone(dtstart, 0, zone);
   }
   const time_t first_t = icaltime_as_timet_with_zone(first, zone);
   struct tm window_tm;
   gmtime_r(&range_start, &window_tm);
   if (!rule_has_a_date(&recur, first, window_tm.tm_year + 1900))
      return FAILURE;
   const long period = period_seconds(recur.freq) * (recur.interval > 0 ? recur.interval : 1);

   /* Nothing after the window matters, and ending the rule there bounds the
    * search libical runs inside each step (for hourly and daily rules: it
    * checks UNTIL only between periods for the others). */
   if (icaltime_is_null_time(recur.until) ||
       icaltime_as_timet_with_zone(recur.until, utc) > range_end)
      recur.until = icaltime_from_timet_with_zone(range_end, all_day, utc);

   /* A long occurrence still overlapping the window started before it, but no
    * earlier than the window is long: a huge duration mustn't move the start. */
   const time_t lookback = (time_t)duration < range_end - range_start ? (time_t)duration
                                                                      : range_end - range_start;
   const time_t window_start = range_start - lookback;
   if (recur.count > 0 && window_start > first_t &&
       (window_start - first_t) / period > CALENDAR_RRULE_MAX_PERIODS) {
      OLOG_WARNING("calendar: RRULE '%s' starts too long before the window", rrule);
      return FAILURE;
   }
   /* An hourly rule with no COUNT starts at the window, moved by whole periods
    * of wall-clock hours (what it steps on) so its hours stay where they were
    * across a daylight-saving change (set_start re-aligns them); one period
    * short, so the change can't put the start past the window's. */
   if (recur.count == 0 && recur.freq == ICAL_HOURLY_RECURRENCE && window_start > first_t) {
      const struct icaltimetype ws = icaltime_from_timet_with_zone(window_start, 0, zone);
      const time_t periods = (wall_seconds(ws) - wall_seconds(first)) / period - 1;
      if (periods > 0)
         icaltime_adjust(&first, 0, (int)(periods * (period / 3600)), 0, 0);
   }

   icalrecur_iterator *iter = icalrecur_iterator_new(recur, first);
   if (!iter)
      return SUCCESS; /* a rule that can never occur */

   /* Any other rule with no COUNT starts at the window too (with a COUNT, the
    * occurrences before it are what is counted). */
   if (recur.count == 0 && recur.freq != ICAL_HOURLY_RECURRENCE && window_start > first_t &&
       !icalrecur_iterator_set_start(iter,
                                     icaltime_from_timet_with_zone(window_start, all_day, zone))) {
      icalrecur_iterator_free(iter);
      return SUCCESS; /* nothing left in the window */
   }

   int count = 0;
   int steps = 0;
   for (struct icaltimetype next = icalrecur_iterator_next(iter);
        !icaltime_is_null_time(next) && count < CALENDAR_RRULE_MAX_OCCURRENCES;
        next = icalrecur_iterator_next(iter)) {
      if (++steps > CALENDAR_RRULE_MAX_STEPS) {
         OLOG_WARNING("calendar: RRULE '%s' stopped after %d steps", rrule, steps - 1);
         if (count == 0) {
            /* Never reached the window: refused, not "no occurrences". */
            icalrecur_iterator_free(iter);
            return FAILURE;
         }
         break;
      }
      const time_t start = icaltime_as_timet_with_zone(next, zone);
      if (start + duration < range_start)
         continue;
      if (start > range_end)
         break;
      emit(start, ctx);
      count++;
   }
   icalrecur_iterator_free(iter);
   *count_out = count;
   return SUCCESS;
}

int calendar_rrule_expand(const char *rrule,
                          time_t dtstart,
                          const char *tzid,
                          bool all_day,
                          const char *start_date,
                          int duration,
                          time_t range_start,
                          time_t range_end,
                          calendar_rrule_emit_fn emit,
                          void *ctx,
                          int *count_out) {
   *count_out = 0;
   struct icalrecurrencetype recur = icalrecurrencetype_from_string(rrule);
   const int rc = expand_parsed(rrule, recur, dtstart, tzid, all_day, start_date, duration,
                                range_start, range_end, emit, ctx, count_out);
   /* libical copies an RSCALE value into the rule and leaves it to the caller. */
   free(recur.rscale);
   return rc;
}
