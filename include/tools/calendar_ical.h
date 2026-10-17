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
 * iCalendar for the calendar tool, through libical, with no I/O: reading an
 * event a server sent (with its cancelled and moved instances), building a
 * new one, and editing one in place so that what wasn't asked to change is
 * written back as it came.
 */

#ifndef CALENDAR_ICAL_H
#define CALENDAR_ICAL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>


#ifdef __cplusplus
extern "C" {
#endif

/** Most cancelled dates, and most moved instances, one event may carry; the
 *  rest are ignored (a real series has a handful). */
#define CALENDAR_ICAL_MAX_EXCEPTIONS 500

/** Most VTIMEZONEs one event may carry: libical's own bookkeeping of them is
 *  quadratic, and a real event names two or three zones. */
#define CALENDAR_ICAL_MAX_VTIMEZONES 16

/** Longest reminder: four weeks, the most Google allows. */
#define CALENDAR_ICAL_MAX_REMINDER_MIN 40320

/** One instance of a recurring event that is cancelled or moved: a VEVENT
 *  with a RECURRENCE-ID in the same resource. */
typedef struct {
   time_t original;       /**< Start of the instance it replaces (all-day: midnight UTC) */
   bool cancelled;        /**< STATUS:CANCELLED */
   time_t dtstart;        /**< Its own start */
   time_t dtend;          /**< Its own end */
   bool all_day;          /**< Whether its own start is a date */
   char dtstart_date[16]; /**< All-day: "2026-03-10" */
   char dtend_date[16];   /**< All-day: the day after the last */
   char summary[512];     /**< Its title (the series' when it has none) */
   char location[256];    /**< Its location (the series' when it has none) */
   int sequence;          /**< Its SEQUENCE, to pick between two for one instance */
} caldav_override_t;

/** An event as read from a server: its series, and the instances it cancels or moves */
typedef struct {
   char uid[256];          /**< iCalendar UID */
   char etag[128];         /**< Server ETag */
   char summary[512];      /**< Event title */
   char description[1024]; /**< Event description */
   char location[256];     /**< Event location */
   time_t dtstart;         /**< Start time (epoch) */
   time_t dtend;           /**< End time (epoch) */
   bool all_day;           /**< True for VALUE=DATE events */
   char dtstart_date[16];  /**< All-day: "2026-03-10" */
   char dtend_date[16];    /**< All-day: "2026-03-11" (exclusive) */
   char rrule[256];        /**< Recurrence rule string, or empty */
   char tzid[64];          /**< IANA zone a timed event repeats in, or empty for UTC */
   char *raw_ical;         /**< Full iCalendar data (heap-allocated) */
   int duration_sec;       /**< Duration in seconds (for recurring) */
   char href[512];         /**< Resource path on server */
   time_t *exdates;        /**< Cancelled instance starts (EXDATE; all-day: midnight UTC), heap */
   int exdate_count;
   caldav_override_t *overrides; /**< Moved or cancelled instances, heap */
   int override_count;
} caldav_event_t;

/** Free what one event owns (its iCalendar text and exceptions), keeping the struct. */
void caldav_event_free(caldav_event_t *evt);

/** Results of building and editing. */
typedef enum {
   CALENDAR_ICAL_OK = 0,
   CALENDAR_ICAL_ERR_UNREADABLE = 2, /**< not an event DAWN can safely rewrite */
   CALENDAR_ICAL_ERR_BAD_RULE,       /**< the repeat rule is invalid or never occurs */
   CALENDAR_ICAL_ERR_NOT_OCCURRENCE, /**< not an instance of the event's rule */
   CALENDAR_ICAL_ERR_GUEST,          /**< someone else organizes it: reminders only */
   CALENDAR_ICAL_ERR_ZONE,           /**< its times are in a zone DAWN can't resolve */
   CALENDAR_ICAL_ERR_BAD_TIME,       /**< the end isn't after the start */
   CALENDAR_ICAL_ERR_ALLOC,
} calendar_ical_rc_t;

/** Zone names warned about while reading one response, so a hostile calendar
 *  logs each once. */
typedef struct {
   char names[8][64];
   int count;
} calendar_ical_warned_t;

/**
 * @brief Fill @p evt from one resource's iCalendar text: its series (the
 *        VEVENT without a RECURRENCE-ID), the dates it cancels (EXDATE) and
 *        its moved or cancelled instances (the other VEVENTs).
 *
 * Time zones are made safe first: a TZID with ".." is dropped from every
 * date, and a VTIMEZONE whose rules libical can't resolve with bounded work
 * is set aside (its times are then read as UTC).
 *
 * @return true when the text holds an event; false when it doesn't (skip it).
 *         @p evt's exceptions are freed by caldav_event_free().
 */
bool calendar_ical_read(const char *ical, caldav_event_t *evt, calendar_ical_warned_t *warned);

/** A new event. */
typedef struct {
   const char *uid;
   const char *summary;
   const char *location;    /**< or NULL */
   const char *description; /**< or NULL */
   bool all_day;
   time_t start, end;      /**< timed */
   const char *start_date; /**< all-day, YYYY-MM-DD */
   const char *end_date;   /**< all-day, YYYY-MM-DD, the day after the last */
   const char *tzid;       /**< zone a timed event is written in; NULL = UTC */
   const char *rrule;      /**< RFC 5545 rule, or NULL */
   int reminder_minutes;   /**< minutes before the start, or -1 for none */
} calendar_ical_new_t;

/**
 * @brief Write a new event as an iCalendar object (a timed one in @p tzid
 *        with its VTIMEZONE, so it repeats at the same local time).
 * @param out Receives the text (free()), or NULL on failure
 */
calendar_ical_rc_t calendar_ical_build(const calendar_ical_new_t *n, char **out);

/** A change to an event; what is left unset is written back as it came. */
typedef struct {
   const char *summary;     /**< NULL or "" = unchanged */
   const char *location;    /**< NULL or "" = unchanged */
   const char *description; /**< NULL or "" = unchanged */
   bool set_start, set_end;
   time_t start, end;      /**< timed */
   const char *start_date; /**< all-day, YYYY-MM-DD */
   const char *end_date;   /**< all-day, the day after the last */
   const char *rrule;    /**< the series' new rule (to end it, one with UNTIL); NULL = unchanged */
   int reminder_minutes; /**< -1 = unchanged */
   bool remove_reminders;
   time_t occurrence;           /**< 0 = the series; else the instance's original start
                                 *   (all-day: midnight UTC of its date) */
   bool cancel;                 /**< with @p occurrence: cancel that instance */
   const char *const *accounts; /**< set by calendar_service: the account's addresses */
   int account_count;
   const char *user_tzid; /**< the user's zone (set by the caller): times of day are theirs */
} calendar_ical_change_t;

/** A change with nothing set. */
#define CALENDAR_ICAL_CHANGE_INIT \
   { .reminder_minutes = -1 }

/**
 * @brief Apply @p c to the event in @p ical and write the whole object back.
 *
 * Text left NULL or "" is unchanged.  Refuses (and writes nothing) when
 * libical couldn't read part of the event (it would drop that part), when it
 * is the user's copy of someone else's invitation and the change isn't a
 * reminder, when a change to its times meets a zone DAWN can't resolve, and
 * when its start moves to another day under a rule pinned to days (weekdays,
 * dates) without a new rule, and when it is an override governing a range of
 * instances.  On a repeating timed series, start changes its time of day
 * (its date is ignored unless a new rule is given) and end its time of day,
 * keeping the event as many whole days long, both read in the user's zone;
 * given with a new rule, they first move a series written in another zone to
 * the user's, so the rule's days are the user's (a series written in UTC
 * moves too, unless it keeps a rule naming hours, or days its start falls
 * on differently in the user's zone, or has extra dates).  A new length reaches instances edited
 * only for their text.  An all-day series' dates change only with a new rule.  Moving a series
 * moves its cancelled and moved instances with it; a new title, place or notes reaches moved
 * instances that still had the old one; a new rule drops the moved instances it no longer has.
 *
 * @param out Receives the text (free()), or NULL on failure
 */
calendar_ical_rc_t calendar_ical_edit(const char *ical,
                                      const calendar_ical_change_t *c,
                                      char **out);

/** @brief Whether the event in @p ical is the one with UID @p uid. */
bool calendar_ical_has_uid(const char *ical, const char *uid);

/**
 * @brief An instance's key as stored in calendar_occurrences.recurrence_id:
 *        "YYYYMMDDTHHMMSSZ", or "YYYYMMDD" for an all-day event.
 */
void calendar_ical_instance_key(time_t start, bool all_day, char *out, size_t out_len);

/**
 * @brief The instant a key from calendar_ical_instance_key() names, or 0.
 */
time_t calendar_ical_instance_key_time(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* CALENDAR_ICAL_H */
