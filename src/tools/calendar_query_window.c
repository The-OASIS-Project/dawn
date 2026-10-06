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
 * Which stretch of the calendar a message asks about.  See
 * calendar_query_window.h.
 */

#include "tools/calendar_query_window.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/iso8601.h"
#include "core/time_query_parser.h"
#include "dawn_error.h"
#include "memory/memory_terms.h"

/* Words of a message this looks at, and their longest length. */
#define CQW_WORDS_MAX 64
#define CQW_WORD_LEN 16
/* "Upcoming" / "soon": the next two weeks. */
#define CQW_UPCOMING_SECONDS (14 * 86400)

typedef struct {
   char w[CQW_WORDS_MAX][CQW_WORD_LEN];
   int n;
} words_t;

static void split_words(const char *text, words_t *out) {
   out->n = 0;
   const char *start = NULL;
   size_t len = 0;
   for (const char *p = text;
        out->n < CQW_WORDS_MAX && (p = memory_terms_next_word(p, &start, &len)) != NULL;) {
      /* A word too long to be one this looks for still takes its place, so
       * "next <word>" pairs stay adjacent. */
      const size_t n = len < CQW_WORD_LEN ? len : CQW_WORD_LEN - 1;
      for (size_t i = 0; i < n; i++) {
         out->w[out->n][i] = (char)memory_terms_fold_at(start, i);
      }
      out->w[out->n][n] = '\0';
      out->n++;
   }
}

static bool is(const words_t *ws, int i, const char *word) {
   return i >= 0 && i < ws->n && strcmp(ws->w[i], word) == 0;
}

static int find(const words_t *ws, const char *word) {
   for (int i = 0; i < ws->n; i++) {
      if (strcmp(ws->w[i], word) == 0) {
         return i;
      }
   }
   return -1;
}

/* Local midnight of @p t moved by @p days (DST-safe: mktime normalizes). */
static time_t day_start(time_t t, int days) {
   struct tm tm;
   localtime_r(&t, &tm);
   tm.tm_mday += days;
   tm.tm_hour = 0;
   tm.tm_min = 0;
   tm.tm_sec = 0;
   tm.tm_isdst = -1;
   return mktime(&tm);
}

static int weekday_of(time_t t) {
   struct tm tm;
   localtime_r(&t, &tm);
   return tm.tm_wday; /* 0 = Sunday */
}

static void set_days(calendar_window_t *out, time_t now, int from_day, int days) {
   out->asks = true;
   out->start = day_start(now, from_day);
   out->end = day_start(now, from_day + days);
}

static const char *const WEEKDAYS[] = { "sunday",   "monday", "tuesday", "wednesday",
                                        "thursday", "friday", "saturday" };

/* Month names and abbreviations. */
static const struct {
   const char *name;
   int month;       /* 0-11 */
   bool plain_word; /* also an ordinary word or a name: needs a month's context */
} MONTHS[] = {
   { "january", 0, false }, { "february", 1, false },  { "march", 2, true },
   { "april", 3, false },   { "may", 4, true },        { "june", 5, false },
   { "july", 6, false },    { "august", 7, false },    { "september", 8, false },
   { "october", 9, false }, { "november", 10, false }, { "december", 11, false },
   { "jan", 0, true },      { "feb", 1, true },        { "mar", 2, true },
   { "apr", 3, true },      { "jun", 5, true },        { "jul", 6, true },
   { "aug", 7, true },      { "sep", 8, true },        { "sept", 8, true },
   { "oct", 9, true },      { "nov", 10, true },       { "dec", 11, true },
};
#define MONTHS_COUNT (int)(sizeof(MONTHS) / sizeof(MONTHS[0]))

static bool day_words(const words_t *ws, time_t now, calendar_window_t *out) {
   if (find(ws, "yesterday") >= 0) {
      set_days(out, now, -1, 1);
   } else if (find(ws, "today") >= 0 || find(ws, "tonight") >= 0) {
      set_days(out, now, 0, 1);
   } else if (find(ws, "tomorrow") >= 0) {
      set_days(out, now, 1, 1);
   } else {
      return false;
   }
   return true;
}

static bool week_words(const words_t *ws, time_t now, calendar_window_t *out) {
   const int wday = weekday_of(now);
   const int weekend = find(ws, "weekend");
   if (weekend >= 0) {
      /* The coming Saturday; on a weekend day, this one. */
      int to_sat = (wday == 0) ? -1 : 6 - wday;
      if (is(ws, weekend - 1, "next")) {
         to_sat += 7;
      } else if (is(ws, weekend - 1, "last")) {
         to_sat -= 7;
      }
      set_days(out, now, to_sat, 2);
      return true;
   }
   const int week = find(ws, "week");
   if (week < 0) {
      return false;
   }
   const int to_monday = (wday == 0) ? 1 : 8 - wday;
   if (is(ws, week - 1, "next")) {
      set_days(out, now, to_monday, 7);
      return true;
   }
   if (is(ws, week - 1, "this")) {
      out->asks = true;
      out->start = now;
      out->end = day_start(now, to_monday);
      return true;
   }
   return false; /* "last week": time_query_parse */
}

/* Words that place a weekday or month in time ("on friday", "in may").
 * Includes time_query_parse's own prepositions (during, mid, ...). */
static bool placed_after(const words_t *ws, int i) {
   static const char *const WORDS[] = { "on",        "in",     "of",    "this",   "next",
                                        "last",      "by",     "until", "till",   "before",
                                        "after",     "for",    "since", "coming", "early",
                                        "late",      "during", "mid",   "around", "through",
                                        "throughout" };
   for (size_t k = 0; k < sizeof(WORDS) / sizeof(WORDS[0]); k++) {
      if (is(ws, i - 1, WORDS[k])) {
         return true;
      }
   }
   return false;
}

static bool weekday_words(const words_t *ws,
                          const char *assistant_name,
                          time_t now,
                          calendar_window_t *out) {
   const int wday = weekday_of(now);
   for (int i = 0; i < ws->n; i++) {
      int d = 0;
      while (d < 7 && strcmp(ws->w[i], WEEKDAYS[d]) != 0) {
         d++;
      }
      if (d == 7) {
         continue;
      }
      /* The assistant may be called by a weekday's name ("Friday, ..."): that
       * word is a day only where it's placed as one ("on friday"). */
      if (assistant_name && strcasecmp(assistant_name, WEEKDAYS[d]) == 0 && !placed_after(ws, i)) {
         continue;
      }
      int ahead = (d - wday + 7) % 7;
      if (is(ws, i - 1, "last")) {
         set_days(out, now, ahead == 0 ? -7 : ahead - 7, 1);
      } else {
         if (is(ws, i - 1, "next") && ahead == 0) {
            ahead = 7;
         }
         set_days(out, now, ahead, 1);
      }
      return true;
   }
   return false;
}

/* Whether the message asks about the past ("what did I do", "what happened"). */
static bool past_tense(const words_t *ws) {
   static const char *const WORDS[] = { "did", "was", "were", "happened", "had", "went" };
   for (size_t k = 0; k < sizeof(WORDS) / sizeof(WORDS[0]); k++) {
      if (find(ws, WORDS[k]) >= 0) {
         return true;
      }
   }
   return false;
}

/* A year written right after word @p i ("november 2027"), or 0. */
static int year_after(const words_t *ws, int i) {
   if (i + 1 >= ws->n || strlen(ws->w[i + 1]) != 4) {
      return 0;
   }
   for (int k = 0; k < 4; k++) {
      if (!isdigit((unsigned char)ws->w[i + 1][k])) {
         return 0;
      }
   }
   const int year = atoi(ws->w[i + 1]);
   return (year >= 1970 && year <= 2100) ? year : 0;
}

/* Whether word @p i is month table entry @p m placed as a month. Words that
 * are also ordinary words or names ("may", "march", "jan") count only after a
 * placing word, or before a day number or year. */
static bool month_word_at(const words_t *ws, int i, int m) {
   if (strcmp(ws->w[i], MONTHS[m].name) != 0) {
      return false;
   }
   return !MONTHS[m].plain_word || placed_after(ws, i) ||
          (i + 1 < ws->n && isdigit((unsigned char)ws->w[i + 1][0]));
}

static bool month_words(const words_t *ws, time_t now, calendar_window_t *out) {
   struct tm tm;
   localtime_r(&now, &tm);
   for (int i = 0; i < ws->n; i++) {
      for (int m = 0; m < MONTHS_COUNT; m++) {
         if (!month_word_at(ws, i, m)) {
            continue;
         }
         const int month = MONTHS[m].month;
         int year = tm.tm_year;
         const int written = year_after(ws, i);
         if (written) {
            year = written - 1900;
         } else if (is(ws, i - 1, "last")) {
            if (month >= tm.tm_mon) {
               year--;
            }
         } else if (is(ws, i - 1, "next")) {
            if (month <= tm.tm_mon) {
               year++;
            }
         } else if (past_tense(ws)) {
            if (month > tm.tm_mon) {
               year--; /* "what did I do in november": the last one */
            }
         } else if (month < tm.tm_mon) {
            year++; /* a month already past, named on its own: the coming one */
         }
         struct tm first = { .tm_year = year, .tm_mon = month, .tm_mday = 1, .tm_isdst = -1 };
         struct tm next = { .tm_year = year, .tm_mon = month + 1, .tm_mday = 1, .tm_isdst = -1 };
         out->asks = true;
         out->start = mktime(&first);
         out->end = mktime(&next);
         return true;
      }
   }
   return false;
}

/* Whether time_query_parse's match is a month word month_words() turned down
 * as an ordinary word ("they march on"): the parser takes any month name. */
static bool rejected_month_match(const words_t *ws, const char *matched) {
   for (int m = 0; m < MONTHS_COUNT; m++) {
      const size_t len = strlen(MONTHS[m].name);
      if (strncmp(matched, MONTHS[m].name, len) != 0 ||
          (matched[len] != '\0' && matched[len] != ' ')) {
         continue;
      }
      for (int i = 0; i < ws->n; i++) {
         if (month_word_at(ws, i, m)) {
            return false;
         }
      }
      return true;
   }
   return false;
}

static bool schedule_words(const words_t *ws) {
   static const char *const WORDS[] = { "calendar",     "schedule", "agenda",   "appointment",
                                        "appointments", "meeting",  "meetings", "event",
                                        "events",       "plans",    "busy",     "booked" };
   for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
      if (find(ws, WORDS[i]) >= 0) {
         return true;
      }
   }
   return false;
}

void calendar_query_window(const char *query,
                           const char *assistant_name,
                           time_t now,
                           calendar_window_t *out) {
   memset(out, 0, sizeof(*out));
   if (!query || !query[0]) {
      return;
   }
   words_t *ws = malloc(sizeof(*ws));
   if (!ws) {
      return;
   }
   split_words(query, ws);
   if (day_words(ws, now, out) || week_words(ws, now, out) ||
       weekday_words(ws, assistant_name, now, out) || month_words(ws, now, out)) {
      free(ws);
      return;
   }
   if (find(ws, "upcoming") >= 0 || find(ws, "soon") >= 0 ||
       (find(ws, "coming") >= 0 && is(ws, find(ws, "coming") + 1, "up"))) {
      out->asks = true;
      out->start = now;
      out->end = now + CQW_UPCOMING_SECONDS;
      free(ws);
      return;
   }
   time_query_t tq = { 0 };
   if (time_query_parse(query, (int64_t)now, &tq) == SUCCESS && tq.found &&
       !rejected_month_match(ws, tq.matched)) {
      out->asks = true;
      out->start = (time_t)(tq.target_ts - tq.window_seconds);
      out->end = (time_t)(tq.target_ts + tq.window_seconds);
   } else if (schedule_words(ws)) {
      out->asks = true;
      out->start = now - CALENDAR_WINDOW_DEFAULT_PAST_SECONDS;
      out->end = now + CALENDAR_WINDOW_DEFAULT_FUTURE_SECONDS;
   }
   free(ws);
}

void calendar_window_dates(time_t start, time_t end, char *start_date, char *end_date) {
   struct tm tm;
   localtime_r(&start, &tm);
   iso8601_format_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, start_date, CALENDAR_DATE_LEN);
   const time_t last = (end > start) ? end - 1 : start;
   localtime_r(&last, &tm);
   tm.tm_mday += 1;
   tm.tm_hour = 12; /* away from a DST edge while normalizing */
   tm.tm_isdst = -1;
   mktime(&tm);
   iso8601_format_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, end_date, CALENDAR_DATE_LEN);
}
