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
 * Which writable calendar a name means, never picking among equals.  Pure.
 */

#ifndef CALENDAR_PICK_H
#define CALENDAR_PICK_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/calendar_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Room for any label: a display name, an account name and an id. */
#define CALENDAR_PICK_LABEL_MAX                        \
   (sizeof(((calendar_calendar_t *)0)->display_name) + \
    sizeof(((calendar_calendar_t *)0)->account_name) + 32)

/** What calendar_pick_writable found. */
typedef enum {
   CALENDAR_PICK_OK,        /* *target set */
   CALENDAR_PICK_READONLY,  /* the name (or, with none, every calendar) is read-only */
   CALENDAR_PICK_NOT_FOUND, /* no single writable calendar has that name: ask */
} calendar_pick_rc_t;

/**
 * @brief The name to show for calendar @p i of @p cals: its display name, and
 *        when another calendar there shares it, its account in parentheses
 *        ("Home (iCloud)"), with its id when that account has both ("Home
 *        (Google #12)")
 */
void calendar_pick_label(const calendar_calendar_t *cals,
                         int count,
                         int i,
                         char *out,
                         size_t out_size);

/** Whether @p name (case-insensitive) is calendar @p i's display name or label. */
bool calendar_pick_names(const calendar_calendar_t *cals, int count, int i, const char *name);

/**
 * @brief The writable calendar @p name means (case-insensitive)
 *
 * No name: the first writable calendar.  A label (calendar_pick_label) or a
 * display name names one calendar; two writable calendars of that name are a
 * question, never a guess.  Else a part of a name that only one writable
 * calendar has ("family" for "Family Calendar").
 */
calendar_pick_rc_t calendar_pick_writable(const calendar_calendar_t *cals,
                                          int count,
                                          const char *name,
                                          int *target);

#ifdef __cplusplus
}
#endif

#endif /* CALENDAR_PICK_H */
