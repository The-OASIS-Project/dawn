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
 * Which stretch of the calendar a message asks about, if any.
 */

#ifndef CALENDAR_QUERY_WINDOW_H
#define CALENDAR_QUERY_WINDOW_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default span for a schedule question that names no time ("my calendar"). */
#define CALENDAR_WINDOW_DEFAULT_PAST_SECONDS (1 * 86400)
#define CALENDAR_WINDOW_DEFAULT_FUTURE_SECONDS (7 * 86400)

/** The part of the calendar a message asks about. */
typedef struct {
   bool asks;    /**< the message is about a time or the schedule */
   time_t start; /**< window start (inclusive), local time boundaries */
   time_t end;   /**< window end (exclusive) */
} calendar_window_t;

/**
 * @brief The calendar window @p query asks about
 *
 * Looks forward as well as back, unlike time_query_parse (which serves
 * retrieval scoring and only resolves the past):
 *   - today / tonight, tomorrow, yesterday: that local day;
 *   - this week (now to the end of Sunday), next week (Monday to Monday),
 *     the weekend (the coming Saturday and Sunday), next / last weekend;
 *   - a weekday name ("on friday"): its next occurrence, today included;
 *     "next friday" skips today, "last friday" looks back;
 *   - a month name or abbreviation: that month, the coming one if it has
 *     passed this year, or in a past-tense question ("what did I do in
 *     november") the last one; "last" / "next" choose, a written year
 *     ("may 2027") pins it.  Month words that are also ordinary words ("may", "march") count
 *     only where placed as months ("in may", "may 5");
 *   - upcoming / coming up / soon: the next two weeks;
 *   - past expressions ("last week", "3 days ago") through time_query_parse;
 *   - otherwise a schedule word (calendar, schedule, agenda, meeting,
 *     appointment, event, plans, busy, booked) with no time: the default span
 *     around @p now.
 * A message that is none of these doesn't ask (@p out->asks false).
 *
 * @param query          The user's message.
 * @param assistant_name The assistant's name, or NULL.  When it is a weekday
 *                       ("Friday"), that word counts as a day only where placed
 *                       as one ("on friday"), not when it addresses the assistant.
 * @param now            The reference time.
 * @param out            The window.
 */
void calendar_query_window(const char *query,
                           const char *assistant_name,
                           time_t now,
                           calendar_window_t *out);

/** Room for a YYYY-MM-DD date and its NUL. */
#define CALENDAR_DATE_LEN 16

/**
 * @brief All-day date bounds for the time window [@p start, @p end)
 *
 * All-day occurrences are stored by date, not time, so a window pull of them
 * needs local dates: @p start_date is @p start's local date and @p end_date
 * (exclusive) the day after the local date of the window's last second.  A
 * window ending at midnight doesn't reach into the next day.
 *
 * @param start       Window start.
 * @param end         Window end (exclusive); must be after @p start.
 * @param start_date  [out] YYYY-MM-DD, CALENDAR_DATE_LEN bytes.
 * @param end_date    [out] YYYY-MM-DD, CALENDAR_DATE_LEN bytes.
 */
void calendar_window_dates(time_t start, time_t end, char *start_date, char *end_date);

#ifdef __cplusplus
}
#endif

#endif /* CALENDAR_QUERY_WINDOW_H */
