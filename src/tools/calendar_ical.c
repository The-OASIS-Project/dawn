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
 * event a server sent (bounding the work its time zones can cost), building a
 * new one, and editing one in place.  An edit changes only what it was asked
 * to; the rest of the server's text (zones, alarms, attendees, unknown
 * properties) is written back as it came.
 */

#include "tools/calendar_ical.h"

#include <ctype.h>
#include <libical/ical.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dawn_error.h"
#include "logging.h"
#include "tools/calendar_rrule.h"

/* Most RRULEs a zone of a name libical doesn't know may have (all its blocks
 * together).  Outlook and Exchange send two. */
#define CALENDAR_ICAL_TZ_MAX_RULES 16

/* Earliest year a repeating block may start: Windows zones start in 1601. */
#define CALENDAR_ICAL_TZ_MIN_START_YEAR 1600


static void warn_zone_once(calendar_ical_warned_t *warned, const char *tzid, const char *why) {
   const char *name = tzid ? tzid : "?";
   if (!warned)
      return;
   for (int i = 0; i < warned->count; i++) {
      if (strncmp(warned->names[i], name, sizeof(warned->names[i]) - 1) == 0)
         return;
   }
   const int cap = (int)(sizeof(warned->names) / sizeof(warned->names[0]));
   if (warned->count > cap)
      return; /* already said further ones are not logged */
   if (warned->count == cap) {
      warned->count++;
      OLOG_WARNING("caldav: more unusable time zones in this calendar; not logging them");
      return;
   }
   snprintf(warned->names[warned->count++], sizeof(warned->names[0]), "%s", name);
   OLOG_WARNING("caldav: ignoring time zone '%.64s' (%s); its events are read as UTC", name, why);
}

/* Entries in a libical BY* list (ICAL_RECURRENCE_ARRAY_MAX ends it). */
static int by_count(const short *list, int size) {
   int n = 0;
   while (n < size && list[n] != ICAL_RECURRENCE_ARRAY_MAX)
      n++;
   return n;
}

/* Whether the days of the month in @p r are seven in a row, all counted from
 * the start or all from the end (the older "BYMONTHDAY=8,...,14;BYDAY=SU"). */
static bool week_of_month_days(const struct icalrecurrencetype *r, int n_mday, int days) {
   if (n_mday != 7)
      return false;
   int lo = r->by_month_day[0];
   int hi = lo;
   for (int k = 0; k < n_mday; k++) {
      const int d = r->by_month_day[k];
      if (d == 0 || abs(d) > days || (d > 0) != (lo > 0))
         return false;
      lo = d < lo ? d : lo;
      hi = d > hi ? d : hi;
      for (int j = 0; j < k; j++) {
         if (r->by_month_day[j] == d)
            return false;
      }
   }
   return hi - lo == 6;
}

/* Whether a time zone rule names one real date a year, the way real zones
 * do, so libical never searches for a date that doesn't exist: in one month,
 * a weekday by position ("second Sunday", "last Sunday", also written with
 * BYSETPOS), one day of the month, or seven days in a row with a weekday.
 * Interval 1 only (a two-year interval from the wrong year never matches),
 * at most one hour, minute and second, and no non-Gregorian RSCALE (libical's
 * search for one can never end). */
static bool zone_rule_is_bounded(const struct icalrecurrencetype *r) {
   static const int month_days[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   if (r->rscale || r->freq != ICAL_YEARLY_RECURRENCE || r->interval > 1 ||
       by_count(r->by_month, ICAL_BY_MONTH_SIZE) != 1 || r->by_month[0] < 1 ||
       r->by_month[0] > 12 || by_count(r->by_year_day, ICAL_BY_YEARDAY_SIZE) != 0 ||
       by_count(r->by_week_no, ICAL_BY_WEEKNO_SIZE) != 0 ||
       by_count(r->by_hour, ICAL_BY_HOUR_SIZE) > 1 ||
       by_count(r->by_minute, ICAL_BY_MINUTE_SIZE) > 1 ||
       by_count(r->by_second, ICAL_BY_SECOND_SIZE) > 1)
      return false;
   const int days = month_days[r->by_month[0] - 1];
   const int n_day = by_count(r->by_day, ICAL_BY_DAY_SIZE);
   const int n_mday = by_count(r->by_month_day, ICAL_BY_MONTHDAY_SIZE);
   const int n_setpos = by_count(r->by_set_pos, ICAL_BY_SETPOS_SIZE);
   const int pos = n_day == 1 ? icalrecurrencetype_day_position(r->by_day[0]) : 0;
   if (n_day > 1 || abs(pos) > 5)
      return false;

   if (n_mday == 0) {
      if (n_setpos == 0)
         return n_day == 1 && pos != 0;
      return n_day == 1 && pos == 0 && n_setpos == 1 && r->by_set_pos[0] != 0 &&
             abs(r->by_set_pos[0]) <= 5;
   }
   if (n_setpos != 0)
      return false;
   if (n_day == 0)
      return n_mday == 1 && r->by_month_day[0] != 0 && abs(r->by_month_day[0]) <= days;
   return pos == 0 && week_of_month_days(r, n_mday, days);
}

/* Whether libical can resolve times against @p vtz with bounded work: it
 * expands every RRULE of every block from the block's start to the year asked
 * for, so a rule changing more than once a year (a fuzz input's FREQ=HOURLY
 * took ~20 s per event), one that never matches, an early start or many rules
 * stall the sync.  Real zones change at most twice a year. */
static bool timezone_is_bounded(icalcomponent *vtz) {
   int rules = 0;
   for (icalcomponent *obs = icalcomponent_get_first_component(vtz, ICAL_ANY_COMPONENT); obs;
        obs = icalcomponent_get_next_component(vtz, ICAL_ANY_COMPONENT)) {
      icalproperty *p = icalcomponent_get_first_property(obs, ICAL_RRULE_PROPERTY);
      if (!p)
         continue; /* single changes (DTSTART/RDATE): cheap */
      /* The property's own value: the component form would resolve a TZID. */
      icalproperty *start = icalcomponent_get_first_property(obs, ICAL_DTSTART_PROPERTY);
      if (!start || icalproperty_get_dtstart(start).year < CALENDAR_ICAL_TZ_MIN_START_YEAR)
         return false;
      for (; p; p = icalcomponent_get_next_property(obs, ICAL_RRULE_PROPERTY)) {
         struct icalrecurrencetype r = icalproperty_get_rrule(p);
         if (++rules > CALENDAR_ICAL_TZ_MAX_RULES || !zone_rule_is_bounded(&r))
            return false;
      }
   }
   return true;
}

/* Whether to set aside the calendar's own definition of @p vtz: always when
 * libical has a zone of that name (its copy is the one used then: the same
 * rules, without trusting the server's), and otherwise when it isn't bounded
 * (an event in it is then read as UTC). */
static bool timezone_set_aside(icalcomponent *vtz, calendar_ical_warned_t *warned) {
   icalproperty *id = icalcomponent_get_first_property(vtz, ICAL_TZID_PROPERTY);
   const char *tzid = id ? icalproperty_get_tzid(id) : NULL;
   if (!tzid)
      return true;
   if (calendar_rrule_zone(tzid))
      return true;
   if (timezone_is_bounded(vtz))
      return false;
   warn_zone_once(warned, tzid, "not a real zone's rules");
   return true;
}

/* Set aside the time zones in @p parent (its direct VTIMEZONE children, the
 * only ones libical looks up for a component under it). */
static void set_aside_timezones(icalcomponent *parent, calendar_ical_warned_t *warned) {
   icalcomponent *vtz = icalcomponent_get_first_component(parent, ICAL_VTIMEZONE_COMPONENT);
   while (vtz) {
      icalcomponent *next = icalcomponent_get_next_component(parent, ICAL_VTIMEZONE_COMPONENT);
      if (timezone_set_aside(vtz, warned)) {
         icalcomponent_remove_component(parent, vtz);
         icalcomponent_free(vtz);
      }
      vtz = next;
   }
}

/* The properties whose values are dates or times, which a TZID may qualify. */
static const icalproperty_kind k_date_props[] = { ICAL_DTSTART_PROPERTY, ICAL_DTEND_PROPERTY,
                                                  ICAL_EXDATE_PROPERTY, ICAL_RDATE_PROPERTY,
                                                  ICAL_RECURRENCEID_PROPERTY };

/* Whether @p root (or @p vevent) defines a VTIMEZONE named @p tzid.  Walks
 * with an external iterator: callers are walking @p root's VEVENTs with its
 * own (libical keeps one per component, which a nested walk would reset). */
static bool defines_zone(icalcomponent *root, icalcomponent *vevent, const char *tzid) {
   icalcomponent *parents[] = { root, vevent };
   for (size_t k = 0; k < 2; k++) {
      icalcompiter it = icalcomponent_begin_component(parents[k], ICAL_VTIMEZONE_COMPONENT);
      for (icalcomponent *vtz = icalcompiter_deref(&it); vtz; vtz = icalcompiter_next(&it)) {
         icalproperty *id = icalcomponent_get_first_property(vtz, ICAL_TZID_PROPERTY);
         if (id && icalproperty_get_tzid(id) && strcmp(icalproperty_get_tzid(id), tzid) == 0)
            return true;
      }
   }
   return false;
}

/* The name libical lists for the zone @p zone ("UTC" for UTC). */
static const char *zone_location(icaltimezone *zone) {
   const char *name = zone == icaltimezone_get_utc_timezone() ? NULL
                                                              : icaltimezone_get_location(zone);
   return name ? name : "UTC";
}

/* Check every TZID on the dates of @p vevent: one naming neither a zone
 * calendar_rrule_zone resolves nor one the calendar defines is dropped (libical
 * would look the name up as a zone file).  With @p rewrite, one naming a zone
 * by an old name is renamed to the current one, so libical resolves it among
 * its own.  Returns whether one was unknown. */
static bool check_tzids(icalcomponent *root,
                        icalcomponent *vevent,
                        bool rewrite,
                        calendar_ical_warned_t *warned) {
   bool found = false;
   for (size_t k = 0; k < sizeof(k_date_props) / sizeof(k_date_props[0]); k++) {
      for (icalproperty *p = icalcomponent_get_first_property(vevent, k_date_props[k]); p;
           p = icalcomponent_get_next_property(vevent, k_date_props[k])) {
         icalparameter *param = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
         const char *tzid = param ? icalparameter_get_tzid(param) : NULL;
         if (!param)
            continue;
         icaltimezone *zone = tzid ? calendar_rrule_zone(tzid) : NULL;
         if (zone) {
            if (rewrite && strcmp(zone_location(zone), tzid) != 0)
               icalparameter_set_tzid(param, zone_location(zone));
         } else if (!tzid || !defines_zone(root, vevent, tzid)) {
            warn_zone_once(warned, tzid, "unknown zone");
            icalproperty_remove_parameter_by_kind(p, ICAL_TZID_PARAMETER);
            found = true;
         }
      }
   }
   return found;
}

/* Case-insensitive match of @p word at @p p, reading through folded lines
 * (a line break followed by a space or tab, which iCalendar unfolds). */
static bool matches_unfolded(const char *p, const char *word) {
   for (; *word; word++) {
      while ((p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')) ||
             (p[0] == '\n' && (p[1] == ' ' || p[1] == '\t')))
         p += p[0] == '\r' ? 3 : 2;
      if (toupper((unsigned char)*p) != *word)
         return false;
      p++;
   }
   return true;
}

/* Whether @p ical begins more than CALENDAR_ICAL_MAX_VTIMEZONES time zones, counted
 * before parsing (libical's handling of each is linear in their number). */
static bool too_many_vtimezones(const char *ical) {
   int count = 0;
   for (const char *p = ical; *p; p++) {
      const bool line_start = p == ical || (p[-1] == '\n' && *p != ' ' && *p != '\t');
      if (line_start && matches_unfolded(p, "BEGIN:VTIMEZONE") &&
          ++count > CALENDAR_ICAL_MAX_VTIMEZONES)
         return true;
   }
   return false;
}

/* The event a resource is about: its series (the VEVENT without a
 * RECURRENCE-ID), which a server may list after a changed occurrence. */
static icalcomponent *master_event(icalcomponent *comp) {
   icalcomponent *first = icalcomponent_get_first_component(comp, ICAL_VEVENT_COMPONENT);
   for (icalcomponent *v = first; v;
        v = icalcomponent_get_next_component(comp, ICAL_VEVENT_COMPONENT)) {
      if (!icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY))
         return v;
   }
   return first;
}

/* "YYYY-MM-DD", each field bounded to its printed width (so every compiler
 * can see it fits the 16 bytes the event structs give a date). */
static void format_date(int year, int month, int day, char *out, size_t out_len) {
   snprintf(out, out_len, "%04u-%02u-%02u", (unsigned)year % 10000u, (unsigned)month % 100u,
            (unsigned)day % 100u);
}

/* An all-day date's fields, and its midnight (UTC) as an epoch. */
static time_t date_epoch(icaltimetype t, char *date, size_t date_len) {
   format_date(t.year, t.month, t.day, date, date_len);
   struct tm tm_date = { 0 };
   tm_date.tm_year = t.year - 1900;
   tm_date.tm_mon = t.month - 1;
   tm_date.tm_mday = t.day;
   return timegm(&tm_date);
}

/* A timed value as an epoch: converted to UTC components, then timegm(),
 * which (unlike mktime()) the process-wide TZ doesn't affect. */
static time_t time_epoch(icaltimetype t) {
   icaltimetype utc = icaltime_convert_to_zone(t, icaltimezone_get_utc_timezone());
   struct tm tm_utc = { 0 };
   tm_utc.tm_year = utc.year - 1900;
   tm_utc.tm_mon = utc.month - 1;
   tm_utc.tm_mday = utc.day;
   tm_utc.tm_hour = utc.hour;
   tm_utc.tm_min = utc.minute;
   tm_utc.tm_sec = utc.second;
   return timegm(&tm_utc);
}

/* Fill @p evt from one resource's iCalendar text.  False when it holds no
 * event DAWN can read (then it is skipped). */
/* ============================================================================
 * Reading
 * ============================================================================ */

/* The zone a value's TZID names, for reading: libical's own zone of that name,
 * else the calendar's (kept only when bounded), else none (read as UTC). */
static icaltimezone *read_zone(icalcomponent *root, icalproperty *p) {
   icalparameter *param = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
   const char *tzid = param ? icalparameter_get_tzid(param) : NULL;
   if (!tzid)
      return NULL;
   icaltimezone *zone = calendar_rrule_zone(tzid);
   return zone ? zone : icalcomponent_get_timezone(root, tzid);
}

/* The instant a date or date-time value names: a date as its midnight UTC, a
 * time in its zone (UTC when it has none).  @p as_date reads a time as its
 * date, for an all-day series. */
static time_t value_epoch(icalcomponent *root, icalproperty *p, icaltimetype t, bool as_date) {
   char date[16];
   if (t.is_date || as_date)
      return date_epoch(t, date, sizeof(date));
   if (!icaltime_is_utc(t)) {
      icaltimezone *zone = read_zone(root, p);
      if (zone)
         t = icaltime_set_timezone(&t, zone);
   }
   return time_epoch(t);
}

/* Read one moved or cancelled instance from its VEVENT. */
static void read_override(icalcomponent *root,
                          icalcomponent *v,
                          icalproperty *rid,
                          const caldav_event_t *evt,
                          caldav_override_t *o) {
   memset(o, 0, sizeof(*o));
   o->original = value_epoch(root, rid, icalproperty_get_recurrenceid(rid), evt->all_day);
   o->cancelled = icalcomponent_get_status(v) == ICAL_STATUS_CANCELLED;
   o->sequence = icalcomponent_get_sequence(v);
   icaltimetype start = icalcomponent_get_dtstart(v);
   if (icaltime_is_null_time(start)) {
      o->cancelled = true; /* nothing to show */
      return;
   }
   o->all_day = start.is_date;
   o->dtstart = start.is_date ? date_epoch(start, o->dtstart_date, sizeof(o->dtstart_date))
                              : time_epoch(start);
   icaltimetype end = icalcomponent_get_dtend(v);
   if (!icaltime_is_null_time(end)) {
      o->dtend = end.is_date ? date_epoch(end, o->dtend_date, sizeof(o->dtend_date))
                             : time_epoch(end);
   } else {
      o->dtend = o->dtstart + (o->all_day ? 86400 : evt->duration_sec);
      if (o->all_day) {
         struct tm tm_end;
         gmtime_r(&o->dtend, &tm_end);
         format_date(tm_end.tm_year + 1900, tm_end.tm_mon + 1, tm_end.tm_mday, o->dtend_date,
                     sizeof(o->dtend_date));
      }
   }
   const char *summary = icalcomponent_get_summary(v);
   snprintf(o->summary, sizeof(o->summary), "%s", summary ? summary : evt->summary);
   const char *loc = icalcomponent_get_location(v);
   snprintf(o->location, sizeof(o->location), "%s", loc ? loc : evt->location);
}

/* The dates the series cancels (EXDATE) and its moved or cancelled instances,
 * at most CALENDAR_ICAL_MAX_EXCEPTIONS of each.  Two VEVENTs for one instance:
 * the higher SEQUENCE, then the later one. */
static void read_exceptions(icalcomponent *root, icalcomponent *master, caldav_event_t *evt) {
   int n = icalcomponent_count_properties(master, ICAL_EXDATE_PROPERTY);
   if (n > CALENDAR_ICAL_MAX_EXCEPTIONS) {
      OLOG_WARNING("caldav: '%s' cancels %d dates; reading the first %d", evt->href, n,
                   CALENDAR_ICAL_MAX_EXCEPTIONS);
      n = CALENDAR_ICAL_MAX_EXCEPTIONS;
   }
   evt->exdates = n > 0 ? calloc((size_t)n, sizeof(*evt->exdates)) : NULL;
   for (icalproperty *p = icalcomponent_get_first_property(master, ICAL_EXDATE_PROPERTY);
        p && evt->exdates && evt->exdate_count < n;
        p = icalcomponent_get_next_property(master, ICAL_EXDATE_PROPERTY)) {
      evt->exdates[evt->exdate_count++] = value_epoch(root, p, icalproperty_get_exdate(p),
                                                      evt->all_day);
   }

   int vevents = icalcomponent_count_components(root, ICAL_VEVENT_COMPONENT) - 1;
   if (vevents > CALENDAR_ICAL_MAX_EXCEPTIONS)
      vevents = CALENDAR_ICAL_MAX_EXCEPTIONS;
   evt->overrides = vevents > 0 ? calloc((size_t)vevents, sizeof(*evt->overrides)) : NULL;
   if (!evt->overrides)
      return;
   for (icalcomponent *v = icalcomponent_get_first_component(root, ICAL_VEVENT_COMPONENT); v;
        v = icalcomponent_get_next_component(root, ICAL_VEVENT_COMPONENT)) {
      icalproperty *rid = icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY);
      const char *uid = icalcomponent_get_uid(v);
      if (v == master || !rid || !uid || strcmp(uid, evt->uid) != 0)
         continue;
      if (icalproperty_get_first_parameter(rid, ICAL_RANGE_PARAMETER))
         OLOG_WARNING("caldav: '%s' moves a range of instances; read as one", evt->href);
      caldav_override_t o;
      read_override(root, v, rid, evt, &o);
      int k = 0;
      while (k < evt->override_count && evt->overrides[k].original != o.original)
         k++;
      if (k < evt->override_count) {
         if (o.sequence >= evt->overrides[k].sequence)
            evt->overrides[k] = o;
      } else if (evt->override_count < vevents) {
         evt->overrides[evt->override_count++] = o;
      }
   }
}

/* Here, beside the reader that allocates what it frees. */
void caldav_event_free(caldav_event_t *evt) {
   free(evt->raw_ical);
   evt->raw_ical = NULL;
   free(evt->exdates);
   evt->exdates = NULL;
   evt->exdate_count = 0;
   free(evt->overrides);
   evt->overrides = NULL;
   evt->override_count = 0;
}

bool calendar_ical_read(const char *ical, caldav_event_t *evt, calendar_ical_warned_t *warned) {
   if (too_many_vtimezones(ical)) {
      OLOG_WARNING("caldav: skipping '%s': more than %d time zones", evt->href,
                   CALENDAR_ICAL_MAX_VTIMEZONES);
      return false;
   }
   icalcomponent *comp = icalparser_parse_string(ical);
   if (!comp)
      return false;
   icalcomponent *vevent = master_event(comp);
   if (!vevent) {
      icalcomponent_free(comp);
      return false;
   }
   /* Before any time is read: libical resolves a TZID through the event's own
    * VTIMEZONEs, then the calendar's, then a zone file of that name. */
   set_aside_timezones(comp, warned);
   for (icalcomponent *v = icalcomponent_get_first_component(comp, ICAL_VEVENT_COMPONENT); v;
        v = icalcomponent_get_next_component(comp, ICAL_VEVENT_COMPONENT)) {
      set_aside_timezones(v, warned);
      check_tzids(comp, v, true, warned);
   }

   const char *uid = icalcomponent_get_uid(vevent);
   if (uid)
      snprintf(evt->uid, sizeof(evt->uid), "%s", uid);
   const char *summary = icalcomponent_get_summary(vevent);
   if (summary)
      snprintf(evt->summary, sizeof(evt->summary), "%s", summary);
   const char *desc = icalcomponent_get_description(vevent);
   if (desc)
      snprintf(evt->description, sizeof(evt->description), "%s", desc);
   const char *loc = icalcomponent_get_location(vevent);
   if (loc)
      snprintf(evt->location, sizeof(evt->location), "%s", loc);

   icaltimetype dtstart = icalcomponent_get_dtstart(vevent);
   if (!icaltime_is_null_time(dtstart)) {
      if (dtstart.is_date) {
         evt->all_day = true;
         evt->dtstart = date_epoch(dtstart, evt->dtstart_date, sizeof(evt->dtstart_date));
      } else {
         evt->dtstart = time_epoch(dtstart);
         /* The zone it repeats in, when libical has it built in. */
         icaltimezone *zone = (icaltimezone *)dtstart.zone;
         const char *name = zone ? icaltimezone_get_location(zone) : NULL;
         if (name && calendar_rrule_zone(name))
            snprintf(evt->tzid, sizeof(evt->tzid), "%s", name);
      }
   }
   icaltimetype dtend = icalcomponent_get_dtend(vevent);
   if (!icaltime_is_null_time(dtend)) {
      evt->dtend = dtend.is_date ? date_epoch(dtend, evt->dtend_date, sizeof(evt->dtend_date))
                                 : time_epoch(dtend);
   }
   if (evt->dtstart && evt->dtend)
      evt->duration_sec = (int)(evt->dtend - evt->dtstart);

   icalproperty *rrprop = icalcomponent_get_first_property(vevent, ICAL_RRULE_PROPERTY);
   if (rrprop) {
      struct icalrecurrencetype rrule = icalproperty_get_rrule(rrprop);
      const char *rrstr = icalrecurrencetype_as_string(&rrule);
      /* A cut rule would change meaning (a lost COUNT or UNTIL never ends). */
      if (rrstr &&
          (size_t)snprintf(evt->rrule, sizeof(evt->rrule), "%s", rrstr) >= sizeof(evt->rrule)) {
         OLOG_WARNING("caldav: '%s' has a recurrence rule too long to keep; shown once", evt->href);
         evt->rrule[0] = '\0';
      }
   }
   if (evt->rrule[0])
      read_exceptions(comp, vevent, evt);
   icalcomponent_free(comp);
   return true;
}

/* ============================================================================
 * Instance keys
 * ============================================================================ */

void calendar_ical_instance_key(time_t start, bool all_day, char *out, size_t out_len) {
   struct tm t;
   gmtime_r(&start, &t);
   if (all_day)
      snprintf(out, out_len, "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
   else
      snprintf(out, out_len, "%04d%02d%02dT%02d%02d%02dZ", t.tm_year + 1900, t.tm_mon + 1,
               t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

time_t calendar_ical_instance_key_time(const char *key) {
   struct tm t = { 0 };
   int n = 0;
   if (!key)
      return 0;
   if (strlen(key) == 8 && sscanf(key, "%4d%2d%2d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &n) == 3 &&
       n == 8) {
      /* all-day */
   } else if (strlen(key) == 16 && sscanf(key, "%4d%2d%2dT%2d%2d%2dZ", &t.tm_year, &t.tm_mon,
                                          &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
      /* timed */
   } else {
      return 0;
   }
   t.tm_year -= 1900;
   t.tm_mon -= 1;
   return timegm(&t);
}

/* ============================================================================
 * Writing: time values
 * ============================================================================ */

/* How a date property is written. */
typedef enum {
   WHEN_DATE,    /* VALUE=DATE */
   WHEN_UTC,     /* ...Z */
   WHEN_ZONED,   /* TZID=<IANA name>:local time, the zone built into libical */
   WHEN_UNKNOWN, /* a floating time, or a zone libical doesn't have built in */
} when_kind_t;

typedef struct {
   when_kind_t kind;
   icaltimezone *zone; /* WHEN_ZONED */
   char tzid[64];      /* WHEN_ZONED: the IANA name */
} when_t;

static when_t zoned_or_utc(const char *tzid) {
   when_t w = { .kind = WHEN_UTC };
   icaltimezone *zone = calendar_rrule_zone(tzid);
   if (zone && zone != icaltimezone_get_utc_timezone()) {
      w.kind = WHEN_ZONED;
      w.zone = zone;
      snprintf(w.tzid, sizeof(w.tzid), "%s", zone_location(zone));
   }
   return w;
}

static icaltimetype prop_time(icalproperty *p) {
   switch (icalproperty_isa(p)) {
      case ICAL_DTSTART_PROPERTY:
         return icalproperty_get_dtstart(p);
      case ICAL_DTEND_PROPERTY:
         return icalproperty_get_dtend(p);
      case ICAL_EXDATE_PROPERTY:
         return icalproperty_get_exdate(p);
      case ICAL_RECURRENCEID_PROPERTY:
         return icalproperty_get_recurrenceid(p);
      default:
         return icaltime_null_time();
   }
}

static void prop_set_time(icalproperty *p, icaltimetype t) {
   switch (icalproperty_isa(p)) {
      case ICAL_DTSTART_PROPERTY:
         icalproperty_set_dtstart(p, t);
         break;
      case ICAL_DTEND_PROPERTY:
         icalproperty_set_dtend(p, t);
         break;
      case ICAL_EXDATE_PROPERTY:
         icalproperty_set_exdate(p, t);
         break;
      case ICAL_RECURRENCEID_PROPERTY:
         icalproperty_set_recurrenceid(p, t);
         break;
      default:
         break;
   }
}

/* How @p p is written, read from the property alone: its zone is looked up
 * only among libical's own, never in the server's VTIMEZONE. */
static when_t when_of(icalproperty *p) {
   when_t w = { .kind = WHEN_UNKNOWN };
   const icaltimetype t = prop_time(p);
   if (t.is_date) {
      w.kind = WHEN_DATE;
   } else if (icaltime_is_utc(t)) {
      w.kind = WHEN_UTC;
   } else {
      icalparameter *param = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
      icaltimezone *zone = param ? calendar_rrule_zone(icalparameter_get_tzid(param)) : NULL;
      if (zone) {
         w.kind = WHEN_ZONED;
         w.zone = zone;
         snprintf(w.tzid, sizeof(w.tzid), "%s", icalparameter_get_tzid(param));
      }
   }
   return w;
}

/* The instant @p p names (a date: its midnight UTC), read as @p w says. */
static time_t instant_of(icalproperty *p, const when_t *w) {
   const icaltimetype t = prop_time(p);
   char date[16];
   switch (w->kind) {
      case WHEN_DATE:
         return date_epoch(t, date, sizeof(date));
      case WHEN_ZONED:
         return icaltime_as_timet_with_zone(t, w->zone);
      default:
         return icaltime_as_timet(t);
   }
}

/* Write @p instant into @p p as @p w says (a date: the UTC date of it). */
static void put_instant(icalproperty *p, time_t instant, const when_t *w) {
   icaltimetype t;
   icalproperty_remove_parameter_by_kind(p, ICAL_TZID_PARAMETER);
   icalproperty_remove_parameter_by_kind(p, ICAL_VALUE_PARAMETER);
   if (w->kind == WHEN_DATE) {
      t = icaltime_from_timet_with_zone(instant, 1, NULL);
      icalproperty_add_parameter(p, icalparameter_new_value(ICAL_VALUE_DATE));
   } else if (w->kind == WHEN_ZONED) {
      t = icaltime_from_timet_with_zone(instant, 0, w->zone);
      t.zone = NULL; /* its local fields, qualified by the TZID below */
      icalproperty_add_parameter(p, icalparameter_new_tzid(w->tzid));
   } else {
      t = icaltime_from_timet_with_zone(instant, 0, icaltimezone_get_utc_timezone());
   }
   prop_set_time(p, t);
}

/* A wall-clock reading of @p instant (its local fields as seconds), so that
 * adding a day or an hour lands on the same local time across a
 * daylight-saving change. */
static time_t wall_of(time_t instant, const when_t *w) {
   if (w->kind != WHEN_ZONED)
      return instant;
   const icaltimetype t = icaltime_from_timet_with_zone(instant, 0, w->zone);
   struct tm tm = { 0 };
   tm.tm_year = t.year - 1900;
   tm.tm_mon = t.month - 1;
   tm.tm_mday = t.day;
   tm.tm_hour = t.hour;
   tm.tm_min = t.minute;
   tm.tm_sec = t.second;
   return timegm(&tm);
}

static time_t from_wall(time_t wall, const when_t *w) {
   if (w->kind != WHEN_ZONED)
      return wall;
   struct tm tm;
   gmtime_r(&wall, &tm);
   icaltimetype t = icaltime_null_time();
   t.year = tm.tm_year + 1900;
   t.month = tm.tm_mon + 1;
   t.day = tm.tm_mday;
   t.hour = tm.tm_hour;
   t.minute = tm.tm_min;
   t.second = tm.tm_sec;
   return icaltime_as_timet_with_zone(t, w->zone);
}

/* A "YYYY-MM-DD" date's midnight UTC, or 0. */
static time_t date_string_epoch(const char *date) {
   int y = 0, m = 0, d = 0, n = 0;
   if (!date || sscanf(date, "%4d-%2d-%2d%n", &y, &m, &d, &n) != 3 || n != 10 || m < 1 || m > 12 ||
       d < 1 || d > 31)
      return 0;
   struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d };
   return timegm(&tm);
}

/* Add the VTIMEZONE @p w names to @p root unless one of that name is there:
 * libical's copy, named by the IANA name (libical's own TZID carries a
 * prefix other servers don't know). */
static void ensure_vtimezone(icalcomponent *root, const when_t *w) {
   if (w->kind != WHEN_ZONED)
      return;
   for (icalcomponent *vtz = icalcomponent_get_first_component(root, ICAL_VTIMEZONE_COMPONENT); vtz;
        vtz = icalcomponent_get_next_component(root, ICAL_VTIMEZONE_COMPONENT)) {
      icalproperty *id = icalcomponent_get_first_property(vtz, ICAL_TZID_PROPERTY);
      if (id && strcmp(icalproperty_get_tzid(id), w->tzid) == 0)
         return;
   }
   icalcomponent *src = icaltimezone_get_component(w->zone);
   if (!src)
      return;
   icalcomponent *vtz = icalcomponent_new_clone(src);
   icalproperty *id = icalcomponent_get_first_property(vtz, ICAL_TZID_PROPERTY);
   if (id)
      icalproperty_set_tzid(id, w->tzid);
   icalcomponent_add_component(root, vtz);
}

/* ============================================================================
 * Writing: text, reminders, stamps
 * ============================================================================ */

/* @p in without control characters other than newline and tab (libical
 * escapes those two and passes the rest through raw). */
static char *clean_text(const char *in) {
   char *out = malloc(strlen(in) + 1);
   if (!out)
      return NULL;
   size_t n = 0;
   for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
      if (*p >= 0x20 ? *p != 0x7f : (*p == '\n' || *p == '\t'))
         out[n++] = (char)*p;
   }
   out[n] = '\0';
   return out;
}

/* Set a text property of @p v (NULL or "" leaves it as it is: a model fills
 * fields it means to leave alone with ""). */
static calendar_ical_rc_t set_text(icalcomponent *v, icalproperty_kind kind, const char *text) {
   if (!text || !text[0])
      return CALENDAR_ICAL_OK;
   icalproperty *p;
   while ((p = icalcomponent_get_first_property(v, kind)) != NULL) {
      icalcomponent_remove_property(v, p);
      icalproperty_free(p);
   }
   char *clean = clean_text(text);
   if (!clean)
      return CALENDAR_ICAL_ERR_ALLOC;
   p = icalproperty_new(kind);
   icalproperty_set_value(p, icalvalue_new_text(clean));
   icalcomponent_add_property(v, p);
   free(clean);
   return CALENDAR_ICAL_OK;
}

static calendar_ical_rc_t apply_text(icalcomponent *v, const calendar_ical_change_t *c) {
   calendar_ical_rc_t rc = set_text(v, ICAL_SUMMARY_PROPERTY, c->summary);
   if (rc == CALENDAR_ICAL_OK)
      rc = set_text(v, ICAL_LOCATION_PROPERTY, c->location);
   if (rc == CALENDAR_ICAL_OK)
      rc = set_text(v, ICAL_DESCRIPTION_PROPERTY, c->description);
   return rc;
}

/* Replace @p v's pop-up reminders (ACTION:DISPLAY; other kinds are kept)
 * with one @p minutes before the start, or none for -1. */
static void set_reminder(icalcomponent *v, int minutes) {
   icalcomponent *a = icalcomponent_get_first_component(v, ICAL_VALARM_COMPONENT);
   while (a) {
      icalcomponent *next = icalcomponent_get_next_component(v, ICAL_VALARM_COMPONENT);
      icalproperty *action = icalcomponent_get_first_property(a, ICAL_ACTION_PROPERTY);
      if (!action || icalproperty_get_action(action) == ICAL_ACTION_DISPLAY) {
         icalcomponent_remove_component(v, a);
         icalcomponent_free(a);
      }
      a = next;
   }
   if (minutes < 0)
      return;
   struct icaltriggertype trigger = { .time = icaltime_null_time(),
                                      .duration = icaldurationtype_from_int(-minutes * 60) };
   const char *summary = icalcomponent_get_summary(v);
   icalcomponent *alarm = icalcomponent_vanew(
       ICAL_VALARM_COMPONENT, icalproperty_new_action(ICAL_ACTION_DISPLAY),
       icalproperty_new_description(summary && summary[0] ? summary : "Reminder"),
       icalproperty_new_trigger(trigger), (void *)0);
   icalcomponent_add_component(v, alarm);
}

/* DTSTAMP and LAST-MODIFIED to now. */
static void stamp(icalcomponent *v) {
   const icaltimetype now = icaltime_current_time_with_zone(icaltimezone_get_utc_timezone());
   icalcomponent_set_dtstamp(v, now);
   icalproperty *p = icalcomponent_get_first_property(v, ICAL_LASTMODIFIED_PROPERTY);
   if (p)
      icalproperty_set_lastmodified(p, now);
   else
      icalcomponent_add_property(v, icalproperty_new_lastmodified(now));
}

static void bump_sequence(icalcomponent *v) {
   icalcomponent_set_sequence(v, icalcomponent_get_sequence(v) + 1);
}

static void remove_properties(icalcomponent *v, icalproperty_kind kind) {
   icalproperty *p;
   while ((p = icalcomponent_get_first_property(v, kind)) != NULL) {
      icalcomponent_remove_property(v, p);
      icalproperty_free(p);
   }
}

/* ============================================================================
 * Writing: repeat rules
 * ============================================================================ */

/* Longest rule written: what DAWN reads back into an event. */
#define CALENDAR_ICAL_MAX_RULE (sizeof(((caldav_event_t *)0)->rrule) - 1)

/* How far ahead of its start a new rule must occur at least once. */
#define CALENDAR_ICAL_RULE_CHECK_SPAN (5 * 366 * 86400L)

static void count_occurrence(time_t start, void *ctx) {
   (void)start;
   (void)ctx;
}

/* Where an event starts and ends, as its start property is written. */
typedef struct {
   when_t w;
   bool all_day;
   time_t start, end;
   bool has_duration; /* DURATION rather than DTEND */
   bool end_unknown;  /* DTEND in a zone DAWN can't resolve */
} anchor_t;

/* Whether @p rule (a normalized string) is one DAWN expands with bounded work
 * and that occurs at least once from @p a's start. */
static bool rule_occurs(const char *rule, const anchor_t *a) {
   char date[16] = "";
   if (a->all_day) {
      struct tm t;
      gmtime_r(&a->start, &t);
      format_date(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, date, sizeof(date));
   }
   const int duration = a->end > a->start ? (int)(a->end - a->start) : 3600;
   int count = 0;
   return calendar_rrule_expand(rule, a->start, a->w.kind == WHEN_ZONED ? a->w.tzid : NULL,
                                a->all_day, date, duration, a->start,
                                a->start + CALENDAR_ICAL_RULE_CHECK_SPAN, count_occurrence, NULL,
                                &count) == SUCCESS &&
          count > 0;
}

/* @p text as a rule for an event that starts as @p a says, checked and with
 * its UNTIL in the form RFC 5545 requires (a date for an all-day event, else
 * UTC; a date given for a timed event means the end of that day).  Returns
 * the property, or NULL when the rule is refused. */
static icalproperty *checked_rule(const char *text, const anchor_t *a) {
   struct icalrecurrencetype r = icalrecurrencetype_from_string(text);
   icalproperty *prop = NULL;
   if (r.freq == ICAL_NO_RECURRENCE || r.rscale || (r.count > 0 && !icaltime_is_null_time(r.until)))
      goto done;
   if (!icaltime_is_null_time(r.until)) {
      if (a->all_day) {
         r.until.is_date = 1;
         r.until.hour = r.until.minute = r.until.second = 0;
         r.until.zone = NULL;
      } else if (!icaltime_is_utc(r.until)) {
         icaltimetype local = r.until;
         if (local.is_date) {
            local.is_date = 0;
            local.hour = 23;
            local.minute = 59;
            local.second = 59;
         }
         local.zone = NULL;
         const time_t until = a->w.kind == WHEN_ZONED
                                  ? icaltime_as_timet_with_zone(local, a->w.zone)
                                  : icaltime_as_timet(local);
         r.until = icaltime_from_timet_with_zone(until, 0, icaltimezone_get_utc_timezone());
      }
   }
   char *normal = icalrecurrencetype_as_string_r(&r);
   if (normal && strlen(normal) <= CALENDAR_ICAL_MAX_RULE && rule_occurs(normal, a))
      prop = icalproperty_new_rrule(r);
   free(normal);
done:
   free(r.rscale);
   return prop;
}

/* ============================================================================
 * Editing
 * ============================================================================ */

typedef struct {
   icalcomponent *root;
   icalcomponent *master;
   anchor_t a;
} edit_t;

/* Whether libical's message @p why reports a text property with no value
 * ("No value for LOCATION property. ..."): dropping one of those loses
 * nothing; an empty date, rule or ID is a broken event. */
static bool empty_text_property(const char *why) {
   /* libical names an extension property just "X". */
   static const char *const kinds[] = { "DESCRIPTION", "LOCATION",   "SUMMARY",
                                        "COMMENT",     "CATEGORIES", "X" };
   if (strncmp(why, "No value for ", 13) != 0)
      return false;
   why += 13;
   for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
      const size_t n = strlen(kinds[i]);
      if (strncmp(why, kinds[i], n) == 0 && why[n] == ' ')
         return true;
   }
   return false;
}

/* Whether libical dropped anything it couldn't read (it leaves X-LIC-ERROR
 * in its place): writing the event back would lose that part.  A property
 * of text with no value (Google writes "DESCRIPTION:" and "LOCATION:") is the
 * one exception: dropping it loses nothing, so its marker is just removed. */
static bool has_unreadable_part(icalcomponent *c) {
   icalproperty *p = icalcomponent_get_first_property(c, ICAL_XLICERROR_PROPERTY);
   while (p) {
      icalproperty *next = icalcomponent_get_next_property(c, ICAL_XLICERROR_PROPERTY);
      const char *why = icalproperty_get_xlicerror(p);
      if (!why || !empty_text_property(why))
         return true;
      icalcomponent_remove_property(c, p);
      icalproperty_free(p);
      p = next;
   }
   for (icalcomponent *k = icalcomponent_get_first_component(c, ICAL_ANY_COMPONENT); k;
        k = icalcomponent_get_next_component(c, ICAL_ANY_COMPONENT)) {
      if (has_unreadable_part(k))
         return true;
   }
   return false;
}

/* Whether @p p (an ORGANIZER or ATTENDEE) names @p account, by its address
 * or its EMAIL parameter. */
static bool names_account(icalproperty *p, const char *account) {
   const char *addr = icalproperty_get_value_as_string(p);
   if (addr && strncasecmp(addr, "mailto:", 7) == 0)
      addr += 7;
   if (addr && strcasecmp(addr, account) == 0)
      return true;
   icalparameter *email = icalproperty_get_first_parameter(p, ICAL_EMAIL_PARAMETER);
   return email && icalparameter_get_email(email) &&
          strcasecmp(icalparameter_get_email(email), account) == 0;
}

/* Whether the event is the user's copy of someone else's invitation: another
 * organizer, with the account among its attendees.  (An organizer address
 * alone can't tell: a shared calendar's own events name the calendar.) */
static bool is_guest_copy(icalcomponent *master, const char *account) {
   icalproperty *org = icalcomponent_get_first_property(master, ICAL_ORGANIZER_PROPERTY);
   if (!org || !account || !account[0] || names_account(org, account))
      return false;
   for (icalproperty *p = icalcomponent_get_first_property(master, ICAL_ATTENDEE_PROPERTY); p;
        p = icalcomponent_get_next_property(master, ICAL_ATTENDEE_PROPERTY)) {
      if (names_account(p, account))
         return true;
   }
   return false;
}

/* Where @p v starts and ends.  WHEN_UNKNOWN when its start isn't readable
 * without a server zone (an all-day event never is). */
static anchor_t read_anchor(icalcomponent *v) {
   anchor_t a = { .w = { .kind = WHEN_UNKNOWN } };
   icalproperty *start = icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY);
   if (!start)
      return a;
   a.w = when_of(start);
   if (a.w.kind == WHEN_UNKNOWN)
      return a;
   a.all_day = a.w.kind == WHEN_DATE;
   a.start = instant_of(start, &a.w);
   icalproperty *end = icalcomponent_get_first_property(v, ICAL_DTEND_PROPERTY);
   icalproperty *dur = icalcomponent_get_first_property(v, ICAL_DURATION_PROPERTY);
   if (end) {
      const when_t ew = when_of(end);
      a.end_unknown = ew.kind == WHEN_UNKNOWN;
      a.end = a.end_unknown ? a.start : instant_of(end, &ew);
   } else if (dur) {
      a.has_duration = true;
      a.end = from_wall(
          wall_of(a.start, &a.w) + icaldurationtype_as_int(icalproperty_get_duration(dur)), &a.w);
   } else {
      a.end = a.start + (a.all_day ? 86400 : 0);
   }
   return a;
}

/* Write @p v's start and end as @p c asks, keeping its length when only the
 * start moves (on the wall clock, across a daylight-saving change), and its
 * DURATION when it has one and the end isn't set.  @p a is updated. */
static calendar_ical_rc_t change_times(icalcomponent *v,
                                       anchor_t *a,
                                       const calendar_ical_change_t *c) {
   time_t start = a->start;
   time_t end = a->end;
   if (a->all_day) {
      if (c->set_start && !(start = date_string_epoch(c->start_date)))
         return CALENDAR_ICAL_ERR_BAD_TIME;
      if (c->set_end) {
         if (!(end = date_string_epoch(c->end_date)))
            return CALENDAR_ICAL_ERR_BAD_TIME;
      } else {
         end = start + (a->end - a->start);
      }
   } else {
      if (c->set_start)
         start = c->start;
      if (c->set_end)
         end = c->end;
      else
         end = from_wall(
             wall_of(start, &a->w) + (wall_of(a->end, &a->w) - wall_of(a->start, &a->w)), &a->w);
   }
   if (end < start || (end == start && (a->all_day || c->set_end)))
      return CALENDAR_ICAL_ERR_BAD_TIME;

   put_instant(icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY), start, &a->w);
   if (a->has_duration && c->set_end) {
      remove_properties(v, ICAL_DURATION_PROPERTY);
      a->has_duration = false;
   }
   if (!a->has_duration) {
      icalproperty *p = icalcomponent_get_first_property(v, ICAL_DTEND_PROPERTY);
      if (!p) {
         p = icalproperty_new_dtend(icaltime_null_time());
         icalcomponent_add_property(v, p);
      }
      put_instant(p, end, &a->w);
   }
   a->start = start;
   a->end = end;
   return CALENDAR_ICAL_OK;
}

/* Where an exception lands when the series changes: @p inst moved by a
 * wall-clock delta (the series' start moved), or to the series' new time of
 * day on its own date (the series moved to a zone). */
typedef struct {
   bool to_time_of_day;
   time_t delta;
} remap_t;

/* The midnight starting the day of wall-clock reading @p wall. */
static time_t day_of(time_t wall) {
   return wall - (((wall % 86400) + 86400) % 86400);
}

static time_t remap(const edit_t *e, const remap_t *m, time_t inst) {
   if (!m->to_time_of_day)
      return from_wall(wall_of(inst, &e->a.w) + m->delta, &e->a.w);
   /* The exception's date at the series start's offset (the old series kept
    * one offset all year), so a DST change near midnight can't move it a day. */
   const time_t start = wall_of(e->a.start, &e->a.w);
   const time_t day = day_of(inst + (start - e->a.start));
   return from_wall(day + (start - day_of(start)), &e->a.w);
}

/* Move the series' cancelled dates and its moved instances' RECURRENCE-IDs as
 * @p m says, so they still name the same instances; an instance retitled but
 * never moved (its start was its RECURRENCE-ID) moves with the series. */
static calendar_ical_rc_t remap_exceptions(edit_t *e, const remap_t *m) {
   for (icalproperty *p = icalcomponent_get_first_property(e->master, ICAL_EXDATE_PROPERTY); p;
        p = icalcomponent_get_next_property(e->master, ICAL_EXDATE_PROPERTY)) {
      const when_t w = when_of(p);
      if (w.kind == WHEN_UNKNOWN)
         return CALENDAR_ICAL_ERR_ZONE;
      put_instant(p, remap(e, m, instant_of(p, &w)), &e->a.w);
   }
   const char *uid = icalcomponent_get_uid(e->master);
   for (icalcomponent *v = icalcomponent_get_first_component(e->root, ICAL_VEVENT_COMPONENT); v;
        v = icalcomponent_get_next_component(e->root, ICAL_VEVENT_COMPONENT)) {
      icalproperty *rid = icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY);
      const char *vuid = icalcomponent_get_uid(v);
      if (v == e->master || !rid || !vuid || !uid || strcmp(vuid, uid) != 0)
         continue;
      const when_t w = when_of(rid);
      if (w.kind == WHEN_UNKNOWN)
         return CALENDAR_ICAL_ERR_ZONE;
      const time_t was = instant_of(rid, &w);
      const time_t now = remap(e, m, was);
      put_instant(rid, now, &e->a.w);
      anchor_t oa = read_anchor(v);
      if (oa.w.kind != WHEN_UNKNOWN && oa.start == was) {
         if (oa.end_unknown)
            return CALENDAR_ICAL_ERR_ZONE;
         const time_t len = oa.end - oa.start;
         put_instant(icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY), now, &e->a.w);
         icalproperty *end = icalcomponent_get_first_property(v, ICAL_DTEND_PROPERTY);
         if (end)
            put_instant(end, now + len, &e->a.w);
      }
   }
   return CALENDAR_ICAL_OK;
}

/* Whether the series' rule pins its instances to particular days (weekdays,
 * days of the month or year, months, weeks): moving its start to another day
 * then needs a new rule too. */
static bool rule_pins_days(icalcomponent *master) {
   icalproperty *rr = icalcomponent_get_first_property(master, ICAL_RRULE_PROPERTY);
   if (!rr)
      return false;
   const struct icalrecurrencetype r = icalproperty_get_rrule(rr);
   return r.by_day[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_month_day[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_month[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_year_day[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_week_no[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_set_pos[0] != ICAL_RECURRENCE_ARRAY_MAX;
}

/* Whether the series' rule pins its instances to hours, minutes or seconds:
 * moving its start then needs a new rule too. */
static bool rule_pins_times(icalcomponent *master) {
   icalproperty *rr = icalcomponent_get_first_property(master, ICAL_RRULE_PROPERTY);
   if (!rr)
      return false;
   const struct icalrecurrencetype r = icalproperty_get_rrule(rr);
   return r.by_hour[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_minute[0] != ICAL_RECURRENCE_ARRAY_MAX ||
          r.by_second[0] != ICAL_RECURRENCE_ARRAY_MAX;
}

/* Whether the series' current rule still occurs from its (new) start. */
static bool current_rule_occurs(edit_t *e) {
   icalproperty *rr = icalcomponent_get_first_property(e->master, ICAL_RRULE_PROPERTY);
   if (!rr)
      return true;
   struct icalrecurrencetype r = icalproperty_get_rrule(rr);
   char *rule = icalrecurrencetype_as_string_r(&r);
   const bool ok = rule && rule_occurs(rule, &e->a);
   free(rule);
   return ok;
}

/* The VEVENT that moves or cancels the instance starting at @p occ, or NULL. */
static icalcomponent *find_override(edit_t *e, time_t occ) {
   const char *uid = icalcomponent_get_uid(e->master);
   for (icalcomponent *v = icalcomponent_get_first_component(e->root, ICAL_VEVENT_COMPONENT); v;
        v = icalcomponent_get_next_component(e->root, ICAL_VEVENT_COMPONENT)) {
      icalproperty *rid = icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY);
      const char *vuid = icalcomponent_get_uid(v);
      if (v == e->master || !rid || !vuid || !uid || strcmp(vuid, uid) != 0)
         continue;
      const when_t w = when_of(rid);
      if (w.kind != WHEN_UNKNOWN && instant_of(rid, &w) == occ)
         return v;
   }
   return NULL;
}

typedef struct {
   time_t want;
   bool found;
} instance_ctx_t;

static void match_instance(time_t start, void *ctx) {
   instance_ctx_t *x = ctx;
   x->found = x->found || start == x->want;
}

/* Whether the series' rule has an instance starting at @p occ. */
static bool is_instance(edit_t *e, time_t occ) {
   icalproperty *rr = icalcomponent_get_first_property(e->master, ICAL_RRULE_PROPERTY);
   if (!rr)
      return false;
   struct icalrecurrencetype r = icalproperty_get_rrule(rr);
   char *rule = icalrecurrencetype_as_string_r(&r);
   if (!rule)
      return false;
   char date[16] = "";
   if (e->a.all_day) {
      struct tm t;
      gmtime_r(&e->a.start, &t);
      format_date(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, date, sizeof(date));
   }
   const int duration = e->a.end > e->a.start ? (int)(e->a.end - e->a.start) : 3600;
   instance_ctx_t x = { .want = occ };
   int count = 0;
   calendar_rrule_expand(rule, e->a.start, e->a.w.kind == WHEN_ZONED ? e->a.w.tzid : NULL,
                         e->a.all_day, date, duration, occ, occ, match_instance, &x, &count);
   free(rule);
   return x.found;
}

/* A new VEVENT for the instance at @p occ: a copy of the series (so it keeps
 * its reminders, attendees and status) without its rule, for that one time. */
static icalcomponent *new_override(edit_t *e, time_t occ) {
   icalcomponent *v = icalcomponent_new_clone(e->master);
   remove_properties(v, ICAL_RRULE_PROPERTY);
   remove_properties(v, ICAL_RDATE_PROPERTY);
   remove_properties(v, ICAL_EXDATE_PROPERTY);
   remove_properties(v, ICAL_EXRULE_PROPERTY);
   icalproperty *rid = icalproperty_new_recurrenceid(icaltime_null_time());
   put_instant(rid, occ, &e->a.w);
   icalcomponent_add_property(v, rid);
   put_instant(icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY), occ, &e->a.w);
   icalproperty *end = icalcomponent_get_first_property(v, ICAL_DTEND_PROPERTY);
   if (end) {
      const time_t len = wall_of(e->a.end, &e->a.w) - wall_of(e->a.start, &e->a.w);
      put_instant(end, from_wall(wall_of(occ, &e->a.w) + len, &e->a.w), &e->a.w);
   }
   icalcomponent_add_component(e->root, v);
   return v;
}

static bool changes_reminder(const calendar_ical_change_t *c) {
   return c->remove_reminders || c->reminder_minutes >= 0;
}

/* Change one instance of the series: move or retitle it, or cancel it. */
static calendar_ical_rc_t edit_occurrence(edit_t *e, const calendar_ical_change_t *c) {
   if (c->rrule)
      return CALENDAR_ICAL_ERR_BAD_RULE; /* a rule belongs to the series */
   if (!icalcomponent_get_first_property(e->master, ICAL_RRULE_PROPERTY))
      return CALENDAR_ICAL_ERR_NOT_OCCURRENCE;
   icalcomponent *ov = find_override(e, c->occurrence);
   if (!ov && !is_instance(e, c->occurrence))
      return CALENDAR_ICAL_ERR_NOT_OCCURRENCE;

   bool listed = false;
   for (icalproperty *p = icalcomponent_get_first_property(e->master, ICAL_EXDATE_PROPERTY);
        p && !listed; p = icalcomponent_get_next_property(e->master, ICAL_EXDATE_PROPERTY)) {
      const when_t w = when_of(p);
      listed = w.kind != WHEN_UNKNOWN && instant_of(p, &w) == c->occurrence;
   }
   if (c->cancel) {
      if (!listed) {
         icalproperty *ex = icalproperty_new_exdate(icaltime_null_time());
         put_instant(ex, c->occurrence, &e->a.w);
         icalcomponent_add_property(e->master, ex);
      }
      if (ov) {
         icalcomponent_remove_component(e->root, ov);
         icalcomponent_free(ov);
      }
      bump_sequence(e->master);
      stamp(e->master);
      return CALENDAR_ICAL_OK;
   }

   if (listed || (ov && icalcomponent_get_status(ov) == ICAL_STATUS_CANCELLED))
      return CALENDAR_ICAL_ERR_NOT_OCCURRENCE; /* cancelled: nothing to change */
   if (!ov)
      ov = new_override(e, c->occurrence);
   calendar_ical_rc_t rc = apply_text(ov, c);
   if (rc == CALENDAR_ICAL_OK && (c->set_start || c->set_end)) {
      anchor_t oa = read_anchor(ov);
      if (oa.w.kind == WHEN_UNKNOWN || oa.end_unknown)
         return CALENDAR_ICAL_ERR_ZONE;
      rc = change_times(ov, &oa, c);
      bump_sequence(ov);
   }
   if (rc == CALENDAR_ICAL_OK && changes_reminder(c))
      set_reminder(ov, c->remove_reminders ? -1 : c->reminder_minutes);
   stamp(ov);
   return rc;
}

/* Give the series @p text as its rule.  A timed event written in UTC moves to
 * @p user_tzid first, so it repeats at the same local time across a
 * daylight-saving change. */
static calendar_ical_rc_t set_rule(edit_t *e, const char *text, const char *user_tzid) {
   if (!e->a.all_day && e->a.w.kind == WHEN_UTC) {
      const when_t zoned = zoned_or_utc(user_tzid);
      if (zoned.kind == WHEN_ZONED) {
         e->a.w = zoned;
         put_instant(icalcomponent_get_first_property(e->master, ICAL_DTSTART_PROPERTY), e->a.start,
                     &e->a.w);
         icalproperty *end = icalcomponent_get_first_property(e->master, ICAL_DTEND_PROPERTY);
         if (end)
            put_instant(end, e->a.end, &e->a.w);
         ensure_vtimezone(e->root, &e->a.w);
         /* Its instances now fall at the same local time all year: the ones
          * it cancels or moves follow, on their own dates. */
         const remap_t m = { .to_time_of_day = true };
         const calendar_ical_rc_t rc = remap_exceptions(e, &m);
         if (rc != CALENDAR_ICAL_OK)
            return rc;
      }
   }
   icalproperty *rule = checked_rule(text, &e->a);
   if (!rule)
      return CALENDAR_ICAL_ERR_BAD_RULE;
   remove_properties(e->master, ICAL_RRULE_PROPERTY);
   icalcomponent_add_property(e->master, rule);
   return CALENDAR_ICAL_OK;
}

/* Change the series itself. */
static calendar_ical_rc_t edit_series(edit_t *e, const calendar_ical_change_t *c) {
   calendar_ical_rc_t rc = apply_text(e->master, c);
   const bool timing = c->set_start || c->set_end || c->rrule;
   if (rc == CALENDAR_ICAL_OK && (c->set_start || c->set_end)) {
      const bool repeats = icalcomponent_get_first_property(e->master, ICAL_RRULE_PROPERTY);
      const time_t old_wall = wall_of(e->a.start, &e->a.w);
      rc = change_times(e->master, &e->a, c);
      const time_t delta = wall_of(e->a.start, &e->a.w) - old_wall;
      if (rc == CALENDAR_ICAL_OK && repeats && delta != 0) {
         /* A rule on weekdays (or dates) wouldn't follow its start to another
          * day; extra dates (RDATE) don't move with a delta. */
         const bool new_day = day_of(old_wall) != day_of(old_wall + delta);
         if ((!c->rrule &&
              (rule_pins_times(e->master) || (new_day && rule_pins_days(e->master)))) ||
             icalcomponent_get_first_property(e->master, ICAL_RDATE_PROPERTY))
            return CALENDAR_ICAL_ERR_BAD_RULE;
         const remap_t m = { .delta = delta };
         rc = remap_exceptions(e, &m);
      }
      if (rc == CALENDAR_ICAL_OK && repeats && !c->rrule && !current_rule_occurs(e))
         rc = CALENDAR_ICAL_ERR_BAD_RULE; /* moved past its own end */
   }
   if (rc == CALENDAR_ICAL_OK && c->rrule)
      rc = set_rule(e, c->rrule, c->user_tzid);
   if (rc != CALENDAR_ICAL_OK)
      return rc;
   if (changes_reminder(c)) {
      /* The series' reminder is every instance's, moved ones included. */
      const char *uid = icalcomponent_get_uid(e->master);
      for (icalcomponent *v = icalcomponent_get_first_component(e->root, ICAL_VEVENT_COMPONENT); v;
           v = icalcomponent_get_next_component(e->root, ICAL_VEVENT_COMPONENT)) {
         const char *vuid = icalcomponent_get_uid(v);
         if (v == e->master || (vuid && uid && strcmp(vuid, uid) == 0))
            set_reminder(v, c->remove_reminders ? -1 : c->reminder_minutes);
      }
   }
   if (timing)
      bump_sequence(e->master);
   stamp(e->master);
   return CALENDAR_ICAL_OK;
}

static calendar_ical_rc_t edit_apply(edit_t *e, const calendar_ical_change_t *c) {
   if (icalcomponent_isa(e->root) != ICAL_VCALENDAR_COMPONENT || has_unreadable_part(e->root))
      return CALENDAR_ICAL_ERR_UNREADABLE;
   e->master = master_event(e->root);
   if (!e->master || icalcomponent_get_first_property(e->master, ICAL_RECURRENCEID_PROPERTY))
      return CALENDAR_ICAL_ERR_UNREADABLE;
   /* A TZID naming no zone libical has or the calendar defines is never looked
    * up; one here means the text isn't safe to rewrite (dropping it would
    * change the event's times). */
   for (icalcomponent *v = icalcomponent_get_first_component(e->root, ICAL_VEVENT_COMPONENT); v;
        v = icalcomponent_get_next_component(e->root, ICAL_VEVENT_COMPONENT)) {
      if (check_tzids(e->root, v, false, NULL))
         return CALENDAR_ICAL_ERR_UNREADABLE;
   }
   if (c->reminder_minutes > CALENDAR_ICAL_MAX_REMINDER_MIN || c->reminder_minutes < -1)
      return CALENDAR_ICAL_ERR_BAD_TIME;

   const bool timing = c->set_start || c->set_end || c->rrule || c->occurrence || c->cancel;
   const bool text = (c->summary && c->summary[0]) || (c->location && c->location[0]) ||
                     (c->description && c->description[0]);
   if ((timing || text) && is_guest_copy(e->master, c->account))
      return CALENDAR_ICAL_ERR_GUEST;
   e->a = read_anchor(e->master);
   if (timing && (e->a.w.kind == WHEN_UNKNOWN || e->a.end_unknown))
      return CALENDAR_ICAL_ERR_ZONE;
   return c->occurrence ? edit_occurrence(e, c) : edit_series(e, c);
}

bool calendar_ical_has_uid(const char *ical, const char *uid) {
   if (!ical || !uid || too_many_vtimezones(ical))
      return false;
   icalcomponent *root = icalparser_parse_string(ical);
   if (!root)
      return false;
   icalcomponent *master = master_event(root);
   const char *have = master ? icalcomponent_get_uid(master) : NULL;
   const bool same = have && strcmp(have, uid) == 0;
   icalcomponent_free(root);
   return same;
}

calendar_ical_rc_t calendar_ical_edit(const char *ical,
                                      const calendar_ical_change_t *c,
                                      char **out) {
   *out = NULL;
   if (!ical || too_many_vtimezones(ical))
      return CALENDAR_ICAL_ERR_UNREADABLE;
   icalcomponent *root = icalparser_parse_string(ical);
   if (!root)
      return CALENDAR_ICAL_ERR_UNREADABLE;
   edit_t e = { .root = root };
   calendar_ical_rc_t rc = edit_apply(&e, c);
   if (rc == CALENDAR_ICAL_OK) {
      *out = icalcomponent_as_ical_string_r(root);
      if (!*out)
         rc = CALENDAR_ICAL_ERR_ALLOC;
   }
   icalcomponent_free(root);
   return rc;
}

/* ============================================================================
 * Building
 * ============================================================================ */

calendar_ical_rc_t calendar_ical_build(const calendar_ical_new_t *n, char **out) {
   *out = NULL;
   icalcomponent *root = icalcomponent_vanew(ICAL_VCALENDAR_COMPONENT,
                                             icalproperty_new_version("2.0"),
                                             icalproperty_new_prodid("-//DAWN//OASIS//EN"),
                                             (void *)0);
   icalcomponent *v = icalcomponent_new_vevent();
   icalcomponent_add_component(root, v);
   icalcomponent_set_uid(v, n->uid);

   const calendar_ical_change_t text = { .summary = n->summary,
                                         .location = n->location,
                                         .description = n->description };
   calendar_ical_rc_t rc = apply_text(v, &text);

   anchor_t a = { .all_day = n->all_day };
   if (rc == CALENDAR_ICAL_OK) {
      if (n->all_day) {
         a.w.kind = WHEN_DATE;
         a.start = date_string_epoch(n->start_date);
         a.end = date_string_epoch(n->end_date);
      } else {
         a.w = zoned_or_utc(n->tzid);
         a.start = n->start;
         a.end = n->end;
      }
      if (!a.start || a.end <= a.start)
         rc = CALENDAR_ICAL_ERR_BAD_TIME;
   }
   if (rc == CALENDAR_ICAL_OK) {
      icalproperty *start = icalproperty_new_dtstart(icaltime_null_time());
      put_instant(start, a.start, &a.w);
      icalcomponent_add_property(v, start);
      icalproperty *end = icalproperty_new_dtend(icaltime_null_time());
      put_instant(end, a.end, &a.w);
      icalcomponent_add_property(v, end);
      ensure_vtimezone(root, &a.w);
      if (n->rrule && n->rrule[0]) {
         icalproperty *rule = checked_rule(n->rrule, &a);
         if (rule)
            icalcomponent_add_property(v, rule);
         else
            rc = CALENDAR_ICAL_ERR_BAD_RULE;
      }
   }
   if (rc == CALENDAR_ICAL_OK && n->reminder_minutes > CALENDAR_ICAL_MAX_REMINDER_MIN)
      rc = CALENDAR_ICAL_ERR_BAD_TIME;
   if (rc == CALENDAR_ICAL_OK) {
      if (n->reminder_minutes >= 0)
         set_reminder(v, n->reminder_minutes);
      icalcomponent_set_dtstamp(v,
                                icaltime_current_time_with_zone(icaltimezone_get_utc_timezone()));
      *out = icalcomponent_as_ical_string_r(root);
      if (!*out)
         rc = CALENDAR_ICAL_ERR_ALLOC;
   }
   icalcomponent_free(root);
   return rc;
}
