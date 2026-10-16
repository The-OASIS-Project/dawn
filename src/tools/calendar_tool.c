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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Calendar LLM tool — voice-controlled CalDAV calendar access.
 * Actions: calendars, today, range, next, search, add, update, delete
 */

#include "tools/calendar_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/iso8601.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "logging.h"
#include "tools/calendar_pick.h"
#include "tools/calendar_service.h"
#include "tools/oauth_client.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MAX_EVENTS 20

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *calendar_tool_callback(const char *action, char *value, int *should_respond);
static int calendar_tool_init(void);
static void calendar_tool_cleanup(void);
static bool calendar_tool_available(void);

/* =============================================================================
 * JSON Helpers
 * ============================================================================= */

static const char *json_get_str(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static bool json_get_bool(struct json_object *obj, const char *key, bool def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_boolean(val);
}

/* =============================================================================
 * Occurrence Formatter
 * ============================================================================= */

/** Format time for natural speech: "10 AM", "2:30 PM" (not "10:00 AM", "02:30 PM") */
static void format_time_speech(const struct tm *t, char *buf, size_t buf_len) {
   int hour = t->tm_hour % 12;
   if (hour == 0)
      hour = 12;
   const char *ampm = t->tm_hour >= 12 ? "PM" : "AM";

   if (t->tm_min == 0)
      snprintf(buf, buf_len, "%d %s", hour, ampm);
   else
      snprintf(buf, buf_len, "%d:%02d %s", hour, t->tm_min, ampm);
}

/* If the most recent oauth refresh on this thread detected invalid_grant,
 * return a heap-allocated "re-link" message naming the revoked account.
 * Caller checks for non-NULL and returns it instead of a generic failure.
 * Returns NULL when no revocation has been detected on this thread. */
static char *check_oauth_revoked(void) {
   char revoked_account[128] = { 0 };
   if (!oauth_was_last_refresh_revoked(revoked_account, sizeof(revoked_account))) {
      return NULL;
   }
   char *msg = malloc(384);
   if (!msg)
      return NULL;
   snprintf(msg, 384,
            TOOL_RESULT_ERROR_MARK
            "Error: OAuth tokens for '%s' have been revoked at the provider. Tell the user "
            "to re-link this calendar account in WebUI Settings -> Calendar.  Do NOT retry — "
            "retrying with the same tokens will keep failing until re-authorized.",
            revoked_account[0] ? revoked_account : "this account");
   return msg;
}

/* An instance's key as the model passes it back in 'occurrence': its
 * original start, a date for an all-day event, else local time with its UTC
 * offset (exact in the hour a daylight-saving change repeats). */
static void format_occurrence_key(const char *recurrence_id, char *out, size_t out_len) {
   const time_t t = calendar_ical_instance_key_time(recurrence_id);
   if (strlen(recurrence_id) == 8) {
      snprintf(out, out_len, "%.4s-%.2s-%.2s", recurrence_id, recurrence_id + 4, recurrence_id + 6);
      return;
   }
   struct tm tm;
   localtime_r(&t, &tm);
   const long off = tm.tm_gmtoff / 60;
   const unsigned hours = (unsigned)(labs(off) / 60) % 100;
   const unsigned minutes = (unsigned)(labs(off) % 60);
   char base[24];
   strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
   snprintf(out, out_len, "%.19s%c%02u:%02u", base, off < 0 ? '-' : '+', hours, minutes);
}

/* Append a single occurrence line to the strbuf.  Returns 0 on success or
 * STRBUF_E_OOM if the buffer cap was reached (caller should bail). */
static int format_occurrence_sb(const calendar_occurrence_t *occ, strbuf_t *sb) {
   int rc;
   if (occ->all_day) {
      rc = strbuf_appendf(sb, "- %s (all day: %s)%s%s", occ->summary, occ->dtstart_date,
                          occ->location[0] ? " @ " : "", occ->location);
   } else {
      struct tm start_tm, end_tm;
      localtime_r(&occ->dtstart, &start_tm);
      localtime_r(&occ->dtend, &end_tm);

      char start_str[32], end_str[32];
      format_time_speech(&start_tm, start_str, sizeof(start_str));
      format_time_speech(&end_tm, end_str, sizeof(end_str));

      char date_str[32];
      strftime(date_str, sizeof(date_str), "%a %b %d", &start_tm);

      if (start_tm.tm_yday != end_tm.tm_yday || start_tm.tm_year != end_tm.tm_year) {
         char date_end_str[32];
         strftime(date_end_str, sizeof(date_end_str), "%a %b %d", &end_tm);
         rc = strbuf_appendf(sb, "- %s (%s %s - %s %s)%s%s", occ->summary, date_str, start_str,
                             date_end_str, end_str, occ->location[0] ? " @ " : "", occ->location);
      } else {
         rc = strbuf_appendf(sb, "- %s (%s, %s - %s)%s%s", occ->summary, date_str, start_str,
                             end_str, occ->location[0] ? " @ " : "", occ->location);
      }
   }
   if (rc < 0)
      return STRBUF_E_OOM;

   if (occ->event_uid[0])
      rc = strbuf_appendf(sb, " [UID: %s]", occ->event_uid);
   if (rc >= 0 && occ->recurrence_id[0]) {
      char key[40];
      format_occurrence_key(occ->recurrence_id, key, sizeof(key));
      rc = strbuf_appendf(sb, " [repeats; occurrence: %s]", key);
   }
   return (rc < 0) ? STRBUF_E_OOM : 0;
}

/* =============================================================================
 * Access Summary Footer (appended to query results when read-only accounts exist)
 * ============================================================================= */

static void append_access_summary_sb(strbuf_t *sb, int user_id) {
   char writable[512] = { 0 }, ro[512] = { 0 };
   if (calendar_service_get_access_summary(user_id, writable, sizeof(writable), ro, sizeof(ro)) >
       0) {
      strbuf_appendf(sb, "\n\nWritable calendars: %s\nRead-only calendars (no AI edits): %s",
                     writable[0] ? writable : "(none)", ro);
   }
}

/* Helper: finalize a calendar tool response buffer.  Mirrors the pattern in
 * memory_callback — return strbuf contents via strbuf_steal, falling back to
 * a friendly error string on OOM.  Caller must free the returned heap pointer. */
static char *finalize_calendar_response(strbuf_t *sb) {
   if (strbuf_oom(sb)) {
      strbuf_free(sb);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: response buffer exceeded safety cap.");
   }
   char *out = strbuf_steal(sb);
   return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Error: out of memory.");
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_calendars(int user_id) {
   calendar_account_t accounts[CALENDAR_MAX_ACCOUNTS];
   int acct_count = 0;
   calendar_db_account_list(user_id, accounts, 16, &acct_count);

   if (acct_count <= 0)
      return strdup("No calendar accounts configured.");

   /* Use strbuf so a deeply-nested account/calendar list cannot silently
    * truncate mid-row the way the prior fixed 4KB buffer did. */
   strbuf_t sb;
   strbuf_init(&sb, 2048);
   strbuf_appendf(&sb, "Calendar accounts (%d):\n", acct_count);

   for (int a = 0; a < acct_count; a++) {
      const char *status = accounts[a].enabled ? "" : " [DISABLED]";
      const char *ro = accounts[a].read_only ? " [READ-ONLY]" : "";
      if (strbuf_appendf(&sb, "\n%s%s%s:\n", accounts[a].name, ro, status) < 0)
         break;

      calendar_calendar_t cals[16];
      int cal_count = 0;
      calendar_db_calendar_list(accounts[a].id, cals, 16, &cal_count);
      if (cal_count <= 0) {
         strbuf_append(&sb, "  (no calendars synced yet)\n");
      } else {
         for (int c = 0; c < cal_count; c++) {
            const char *active = cals[c].is_active ? "" : " [disabled]";
            if (strbuf_appendf(&sb, "  - %s%s\n", cals[c].display_name, active) < 0)
               break;
         }
      }
   }

   return finalize_calendar_response(&sb);
}

static char *handle_today(struct json_object *details, int user_id) {
   const char *tz = g_config.localization.timezone;
   const char *calendar_name = json_get_str(details, "calendar");

   calendar_occurrence_t events[MAX_EVENTS];
   int count = calendar_service_today(user_id, tz, calendar_name, events, MAX_EVENTS);

   strbuf_t sb;
   strbuf_init(&sb, 2048);

   if (count <= 0) {
      if (calendar_name)
         strbuf_appendf(&sb,
                        "No events scheduled for today on calendar '%s'. "
                        "Use the 'calendars' action to list available calendars.",
                        calendar_name);
      else
         strbuf_append(&sb, "No events scheduled for today.");
   } else {
      strbuf_appendf(&sb, "Today's events (%d):\n", count);
      for (int i = 0; i < count; i++) {
         if (format_occurrence_sb(&events[i], &sb) != 0)
            break;
         if (strbuf_append(&sb, "\n") < 0)
            break;
      }
   }
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_range(struct json_object *details, int user_id) {
   const char *start_str = json_get_str(details, "start");
   const char *end_str = json_get_str(details, "end");
   const char *calendar_name = json_get_str(details, "calendar");

   if (!start_str) {
      return strdup("Error: 'start' is required for range query (ISO 8601 datetime)");
   }

   time_t start = iso8601_parse(start_str);
   if (start == (time_t)-1)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: invalid 'start' datetime format. Use ISO 8601 — e.g. "
                    "'2026-05-27T14:00:00' (local time), '2026-05-27T14:00:00Z' (UTC), or "
                    "'2026-05-27' (date only).");

   time_t end;
   if (end_str) {
      end = iso8601_parse(end_str);
      if (end == (time_t)-1)
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: invalid 'end' datetime format. Use ISO 8601 — e.g. "
                       "'2026-05-27T15:00:00' (local time) or '2026-05-27T15:00:00Z' (UTC).");
   } else {
      end = start + 86400; /* Default: 24 hours */
   }

   calendar_occurrence_t events[MAX_EVENTS];
   int count = calendar_service_range(user_id, start, end, calendar_name, events, MAX_EVENTS);

   strbuf_t sb;
   strbuf_init(&sb, 2048);

   if (count <= 0) {
      if (calendar_name)
         strbuf_appendf(&sb,
                        "No events in the specified range on calendar '%s'. "
                        "Use the 'calendars' action to list available calendars.",
                        calendar_name);
      else
         strbuf_append(&sb, "No events in the specified range.");
   } else {
      strbuf_appendf(&sb, "Events in range (%d):\n", count);
      for (int i = 0; i < count; i++) {
         if (format_occurrence_sb(&events[i], &sb) != 0)
            break;
         if (strbuf_append(&sb, "\n") < 0)
            break;
      }
   }
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_next(struct json_object *details, int user_id) {
   const char *calendar_name = json_get_str(details, "calendar");

   calendar_occurrence_t occ;
   int rc = calendar_service_next(user_id, calendar_name, &occ);

   if (rc != 0) {
      if (calendar_name) {
         char msg[256];
         snprintf(msg, sizeof(msg),
                  "No upcoming events found on calendar '%s'. "
                  "Use the 'calendars' action to list available calendars.",
                  calendar_name);
         return strdup(msg);
      }
      return strdup("No upcoming events found.");
   }

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_append(&sb, "Next event:\n");
   format_occurrence_sb(&occ, &sb);
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

static char *handle_search(struct json_object *details, int user_id) {
   const char *query = json_get_str(details, "query");
   if (!query || !query[0])
      return strdup("Error: 'query' text is required for search");

   const char *calendar_name = json_get_str(details, "calendar");

   calendar_occurrence_t events[MAX_EVENTS];
   int count = calendar_service_search(user_id, query, calendar_name, events, 20);

   strbuf_t sb;
   strbuf_init(&sb, 2048);

   if (count <= 0) {
      if (calendar_name)
         strbuf_appendf(&sb,
                        "No events matching '%s' on calendar '%s'. "
                        "Use the 'calendars' action to list available calendars.",
                        query, calendar_name);
      else
         strbuf_appendf(&sb, "No events matching '%s'.", query);
   } else {
      strbuf_appendf(&sb, "Search results for '%s' (%d):\n", query, count);
      for (int i = 0; i < count; i++) {
         if (format_occurrence_sb(&events[i], &sb) != 0)
            break;
         if (strbuf_append(&sb, "\n") < 0)
            break;
      }
   }
   append_access_summary_sb(&sb, user_id);
   return finalize_calendar_response(&sb);
}

/* A time argument: @p epoch_out as iso8601_parse reads it (local without an
 * offset), and @p date_out its date when it starts with one ("" otherwise),
 * for an all-day event.  False when it can't be read. */
static bool parse_when(const char *text, time_t *epoch_out, char *date_out, size_t date_len) {
   date_out[0] = '\0';
   const time_t t = iso8601_parse(text);
   if (t == (time_t)-1)
      return false;
   *epoch_out = t;
   if (strlen(text) >= 10 && text[4] == '-' && text[7] == '-')
      snprintf(date_out, date_len, "%.10s", text);
   return true;
}

/* The day after "YYYY-MM-DD", into @p out. */
static void next_day(const char *date, char *out, size_t out_len) {
   const time_t t = (time_t)iso8601_parse_date_utc(date) + 86400;
   struct tm tm;
   gmtime_r(&t, &tm);
   iso8601_format_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, out, out_len);
}

/* An instance's start as shown with "occurrence:" (a date for an all-day
 * event, else a local time with its UTC offset), read back: midnight UTC of
 * the date, or the instant.  0 when it can't be read. */
static time_t parse_occurrence(const char *text) {
   if (!text)
      return 0;
   if (strlen(text) == 10)
      return (time_t)iso8601_parse_date_utc(text);
   const time_t t = iso8601_parse(text);
   return t == (time_t)-1 ? 0 : t;
}

/* "reminder_minutes": minutes before the start, or -1 when not given. */
static int json_get_reminder(struct json_object *obj) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, "reminder_minutes", &val) || !val ||
       json_object_is_type(val, json_type_null))
      return -1;
   const int n = json_object_get_int(val);
   return n < 0 ? CALENDAR_ICAL_MAX_REMINDER_MIN + 1 : n; /* refused below */
}

/* What to tell the model when a change didn't go through. */
static char *write_error(int rc, const char *verb) {
   char *revoked = rc == CALENDAR_RC_FAILURE ? check_oauth_revoked() : NULL;
   if (revoked)
      return revoked;
   char *msg = malloc(512);
   if (!msg)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: out of memory.");
   const char *why;
   switch (rc) {
      case CALENDAR_RC_READONLY:
         why = "the event's account is read-only: the user has restricted it from AI changes.";
         break;
      case CALENDAR_RC_NOT_FOUND:
         why = "no event with that UID on the user's calendars (re-run 'range' or 'search' "
               "for current UIDs), or no single writable calendar has that name.";
         break;
      case CALENDAR_RC_AMBIGUOUS:
         why = "that UID is on more than one calendar; pass 'calendar' to say which.";
         break;
      case CALENDAR_RC_NOT_SYNCED:
         why = "the event hasn't been synced from its server yet; try after the next sync.";
         break;
      case CALENDAR_RC_RATE_LIMITED: {
         const int wait = calendar_service_retry_after();
         snprintf(msg, 512,
                  TOOL_RESULT_ERROR_MARK "Error: could not %s the event: the calendar server "
                                         "is limiting requests; try again in %d seconds.",
                  verb, wait > 0 ? wait : 30);
         return msg;
      }
      case CALENDAR_RC_REJECTED:
         why = "the calendar server refused this change. Don't retry the same change; tell the "
               "user.";
         break;
      case CALENDAR_RC_CONFLICT:
         why = "the event kept changing on the server while being written; read it again "
               "before retrying.";
         break;
      case CALENDAR_RC_BAD_RULE:
         why = "the repeat rule is invalid, never occurs, uses COUNT with UNTIL, or is more "
               "frequent than hourly; or the change moves a series to another day without its "
               "new rule, or past its own end ('rrule' applies to the whole series, not one "
               "occurrence).";
         break;
      case CALENDAR_RC_NOT_OCCURRENCE:
         why = "that event doesn't repeat at that time; 'occurrence' must be the value shown "
               "after 'occurrence:' on one of its rows.";
         break;
      case CALENDAR_RC_GUEST:
         why = "the user is a guest on this event (someone else organizes it), so only its "
               "reminder can be changed here.";
         break;
      case CALENDAR_RC_ZONE:
         why = "the event's times are in a time zone DAWN can't resolve, so its times, repeat "
               "rule and single occurrences can't be changed safely here (the whole series' "
               "title, notes and reminder can).";
         break;
      case CALENDAR_RC_BAD_TIME:
         why = "the end must be after the start, and reminder_minutes between 0 and 40320.";
         break;
      case CALENDAR_RC_UNREADABLE:
         why = "part of the event's data can't be read, so it isn't rewritten (that part "
               "would be lost). The user can change it in their calendar app.";
         break;
      default:
         why = "the calendar server couldn't be reached or failed; retry once.";
         break;
   }
   snprintf(msg, 512, TOOL_RESULT_ERROR_MARK "Error: could not %s the event: %s", verb, why);
   return msg;
}

static char *handle_add(struct json_object *details, int user_id) {
   const char *summary = json_get_str(details, "summary");
   if (!summary || !summary[0])
      return strdup("Error: 'summary' (event title) is required");

   const char *start_str = json_get_str(details, "start");
   if (!start_str)
      return strdup("Error: 'start' datetime is required (ISO 8601)");

   calendar_ical_new_t n = { .summary = summary,
                             .location = json_get_str(details, "location"),
                             .description = json_get_str(details, "description"),
                             .all_day = json_get_bool(details, "all_day", false),
                             .tzid = g_config.localization.timezone,
                             .rrule = json_get_str(details, "rrule"),
                             .reminder_minutes = json_get_reminder(details) };
   char start_date[16], end_date[16];
   if (!parse_when(start_str, &n.start, start_date, sizeof(start_date)) ||
       (n.all_day && !start_date[0]))
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: invalid 'start' datetime format. Use ISO 8601 — e.g. "
                    "'2026-05-27T14:00:00' (local time), '2026-05-27T14:00:00Z' (UTC), or "
                    "'2026-05-27' (date only).");
   const char *end_str = json_get_str(details, "end");
   end_date[0] = '\0';
   if (end_str && !parse_when(end_str, &n.end, end_date, sizeof(end_date)))
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: invalid 'end' datetime format. Use ISO 8601 — e.g. "
                    "'2026-05-27T15:00:00' (local time) or '2026-05-27T15:00:00Z' (UTC).");
   if (n.all_day) {
      if (!end_date[0])
         next_day(start_date, end_date, sizeof(end_date));
      n.start_date = start_date;
      n.end_date = end_date;
   }
   const char *calendar_name = json_get_str(details, "calendar");

   char cal_used[CALENDAR_PICK_LABEL_MAX] = ""; /* a label, whole */
   char uid[256] = { 0 };
   int rc = calendar_service_add(user_id, &n, calendar_name, uid, sizeof(uid), cal_used,
                                 sizeof(cal_used));
   if (rc == CALENDAR_RC_NOT_FOUND) {
      /* Never a guess: the calendar named, or the user picks. */
      char writable[512] = { 0 }, ro[512] = { 0 };
      calendar_service_get_access_summary(user_id, writable, sizeof(writable), ro, sizeof(ro));
      char *msg = malloc(1024);
      if (!msg)
         return strdup(TOOL_RESULT_ERROR_MARK "Memory allocation failed");
      snprintf(msg, 1024,
               "No single writable calendar is named '%s'. Writable calendars: %s. Ask the user "
               "which one, then pass its name.",
               calendar_name ? calendar_name : "", writable[0] ? writable : "(none)");
      return msg;
   }
   if (rc != CALENDAR_RC_OK)
      return write_error(rc, "create");

   char *buf = malloc(640);
   if (!buf)
      return strdup("Event created successfully.");

   char time_str[80];
   if (n.all_day) {
      struct tm st;
      const time_t day = (time_t)iso8601_parse_date_utc(start_date);
      gmtime_r(&day, &st);
      strftime(time_str, sizeof(time_str), "%A, %B %d, %Y", &st);
   } else {
      struct tm st;
      localtime_r(&n.start, &st);
      char t_str[16];
      format_time_speech(&st, t_str, sizeof(t_str));
      char date_str[48];
      strftime(date_str, sizeof(date_str), "%A, %B %d", &st);
      snprintf(time_str, sizeof(time_str), "%s at %s", date_str, t_str);
   }

   int pos = snprintf(buf, 640, "Event created on the '%s' calendar: '%s' on %s.%s%s%s%s", cal_used,
                      summary, time_str, n.location ? " Location: " : "",
                      n.location ? n.location : "", n.rrule && n.rrule[0] ? " Repeats: " : "",
                      n.rrule && n.rrule[0] ? n.rrule : "");
   if (uid[0] && pos >= 0 && pos < 640)
      snprintf(buf + pos, 640 - (size_t)pos, "\nUID: %s", uid);
   return buf;
}

/* A change to an event from @p details.  Returns an error message, or NULL. */
static char *read_change(struct json_object *details,
                         calendar_ical_change_t *c,
                         char *start_date,
                         char *end_date,
                         size_t date_len) {
   /* "" is a field left alone (models fill the ones they don't mean). */
   c->summary = json_get_str(details, "summary");
   c->location = json_get_str(details, "location");
   c->description = json_get_str(details, "description");
   c->rrule = json_get_str(details, "rrule");
   if (c->rrule && !c->rrule[0])
      c->rrule = NULL; /* "" is a field left alone, not "stop repeating" */
   c->reminder_minutes = json_get_reminder(details);
   c->remove_reminders = json_get_bool(details, "remove_reminders", false);
   const char *start = json_get_str(details, "start");
   const char *end = json_get_str(details, "end");
   if (start && !start[0])
      start = NULL;
   if (end && !end[0])
      end = NULL;
   if (start) {
      if (!parse_when(start, &c->start, start_date, date_len))
         return strdup(TOOL_RESULT_ERROR_MARK "Error: invalid 'start' (use ISO 8601).");
      c->set_start = true;
      c->start_date = start_date;
   }
   if (end) {
      if (!parse_when(end, &c->end, end_date, date_len))
         return strdup(TOOL_RESULT_ERROR_MARK "Error: invalid 'end' (use ISO 8601).");
      c->set_end = true;
      c->end_date = end_date;
   }
   struct json_object *occ = NULL;
   if (json_object_object_get_ex(details, "occurrence", &occ) &&
       !(c->occurrence = parse_occurrence(json_object_get_string(occ))))
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: invalid 'occurrence': pass the value shown after 'occurrence:' "
                    "on the event's row.");
   return NULL;
}

static char *handle_update(struct json_object *details, int user_id) {
   const char *uid = json_get_str(details, "uid");
   if (!uid || !uid[0])
      return strdup("Error: 'uid' of the event to update is required");

   calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
   char start_date[16], end_date[16];
   char *err = read_change(details, &c, start_date, end_date, sizeof(start_date));
   if (err)
      return err;
   const int rc = calendar_service_update(user_id, uid, json_get_str(details, "calendar"), &c);
   if (rc != CALENDAR_RC_OK)
      return write_error(rc, "update");
   char *msg = malloc(640);
   if (!msg)
      return strdup("Event updated.");
   int n = snprintf(msg, 640,
                    "%s:", c.occurrence ? "That occurrence was updated" : "Event updated");
   if (c.summary && c.summary[0])
      n += snprintf(msg + n, 640 - n, " titled '%.200s';", c.summary);
   if (c.set_start || c.set_end)
      n += snprintf(msg + n, 640 - n, " moved;");
   if (c.rrule)
      n += snprintf(msg + n, 640 - n, " repeats %.200s;", c.rrule);
   if (c.remove_reminders)
      n += snprintf(msg + n, 640 - n, " reminders removed;");
   else if (c.reminder_minutes >= 0)
      n += snprintf(msg + n, 640 - n, " reminder %d min before;", c.reminder_minutes);
   if ((c.location && c.location[0]) || (c.description && c.description[0]))
      n += snprintf(msg + n, 640 - n, " details changed;");
   snprintf(msg + n, 640 - n, " nothing else about it changed%s.",
            c.occurrence ? " (the rest of the series is as it was)" : "");
   return msg;
}

static char *handle_delete(struct json_object *details, int user_id) {
   const char *uid = json_get_str(details, "uid");
   if (!uid || !uid[0])
      return strdup("Error: 'uid' of the event to delete is required");
   const char *calendar_name = json_get_str(details, "calendar");
   struct json_object *occ = NULL;
   if (json_object_object_get_ex(details, "occurrence", &occ)) {
      /* Present at all, it asks for one occurrence: never the whole series. */
      calendar_ical_change_t c = CALENDAR_ICAL_CHANGE_INIT;
      c.cancel = true;
      if (!(c.occurrence = parse_occurrence(json_object_get_string(occ))))
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: invalid 'occurrence': pass the value shown after "
                       "'occurrence:' on the event's row.");
      const int rc = calendar_service_update(user_id, uid, calendar_name, &c);
      if (rc != CALENDAR_RC_OK)
         return write_error(rc, "cancel that occurrence of");
      return strdup("That occurrence was cancelled; the rest of the series is unchanged.");
   }
   const int rc = calendar_service_delete(user_id, uid, calendar_name);
   if (rc != CALENDAR_RC_OK)
      return write_error(rc, "delete");
   return strdup("Event deleted (every occurrence, if it repeated).");
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *calendar_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!action || !action[0])
      return strdup("Error: action is required");

   /* 'calendars' takes no arguments, so tolerate a prose `arguments` value (a
    * model may still narrate despite the schema); every other action reads
    * fields, where a non-JSON value stays a hard error rather than silently
    * dropping the caller's request. */
   struct json_object *details = tool_parse_details(value, strcmp(action, "calendars") == 0);
   if (!details)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: invalid JSON in details parameter");

   int user_id = tool_get_current_user_id();
   if (user_id <= 0) {
      json_object_put(details);
      return strdup(TOOL_GUEST_REFUSAL);
   }

   char *result = NULL;

   if (strcmp(action, "calendars") == 0) {
      result = handle_calendars(user_id);
   } else if (strcmp(action, "today") == 0) {
      result = handle_today(details, user_id);
   } else if (strcmp(action, "range") == 0) {
      result = handle_range(details, user_id);
   } else if (strcmp(action, "next") == 0) {
      result = handle_next(details, user_id);
   } else if (strcmp(action, "search") == 0) {
      result = handle_search(details, user_id);
   } else if (strcmp(action, "add") == 0) {
      result = handle_add(details, user_id);
   } else if (strcmp(action, "update") == 0) {
      result = handle_update(details, user_id);
   } else if (strcmp(action, "delete") == 0) {
      result = handle_delete(details, user_id);
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               TOOL_RESULT_ERROR_MARK
               "Error: unknown action '%s'. Valid: calendars, today, range, next, search, add, "
               "update, delete",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Tool Lifecycle
 * ============================================================================= */

static int calendar_tool_init(void) {
   return calendar_service_init(&g_config.calendar);
}

static void calendar_tool_cleanup(void) {
   calendar_service_shutdown();
}

static bool calendar_tool_available(void) {
   return calendar_service_available();
}

/* =============================================================================
 * Tool Parameter Definition
 * ============================================================================= */

static const treg_param_t calendar_params[] = {
   {
       .name = "action",
       .description = "The calendar action: 'calendars' (list all calendar accounts and their "
                      "calendars), 'today' (today's events), 'range' (events in date range), "
                      "'next' (next upcoming event), 'search' (find events by text), "
                      "'add' (create new event), 'update' (modify event), 'delete' (remove event)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "calendars", "today", "range", "next", "search", "add", "update",
                        "delete" },
       .enum_count = 8,
   },
   {
       .name = "arguments",
       .description =
           "JSON object of the action's arguments, passed as a JSON-encoded string.  "
           "Omit entirely for an action that takes no arguments; never fill it with a "
           "description or rationale.\n"
           "For 'calendars': no fields needed.\n"
           "For 'today': {calendar?}.\n"
           "For 'range': {start (ISO 8601, required), end (ISO 8601, optional, default "
           "+24h), calendar?}.\n"
           "For 'next': {calendar?}.\n"
           "For 'search': {query (text to search in event titles/locations), calendar?}.\n"
           "For 'add': {summary (required), start (ISO 8601, required), end (ISO 8601, "
           "optional), location (optional), description (optional), all_day (bool, "
           "optional), calendar?, rrule (RFC 5545 recurrence, optional — e.g. "
           "'FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=10'), reminder_minutes (optional, minutes "
           "before the start)}.\n"
           "For 'update': {uid (required, from previous query — emitted as '[UID: ...]' on "
           "each result row), calendar?, summary, start, end, location, description, rrule "
           "(replaces the repeat rule; to end a series, give its rule with an UNTIL), "
           "reminder_minutes, remove_reminders (bool), occurrence — only the fields given "
           "are changed}.  Without 'occurrence', a repeating event's whole series changes; "
           "with it (the value after 'occurrence:' on a row marked [repeats]), only that one "
           "instance does.  Moving a series' start to another day needs its new rrule too.\n"
           "For 'delete': {uid (required), calendar?, occurrence?}.  Without 'occurrence' it "
           "deletes the event, every occurrence of a repeating one; with it, it cancels only "
           "that occurrence.\n"
           "  Rows list only occurrences in the cached window.\n"
           "  calendar?: the calendar DISPLAY NAME as returned by action='calendars' (e.g. "
           "'Family', 'Work'). NOT an account name and NOT an arbitrary guess; if "
           "uncertain, run action='calendars' first to enumerate. Omit to search across all "
           "calendars on all accounts.\n"
           "  start/end ISO 8601 format: no timezone suffix = LOCAL time "
           "('2026-05-27T14:00:00'); 'Z' = UTC; '+05:00' = explicit offset; bare date "
           "'2026-05-27' is accepted too.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const tool_action_kind_entry_t s_calendar_action_kinds[] = {
   { "calendars", TOOL_KIND_READ, NULL }, { "today", TOOL_KIND_READ, NULL },
   { "range", TOOL_KIND_READ, NULL },     { "next", TOOL_KIND_READ, NULL },
   { "search", TOOL_KIND_READ, NULL },
};

static const tool_metadata_t calendar_metadata = {
   .name = "calendar",
   .action_kinds = s_calendar_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_calendar_action_kinds),
   .device_string = "calendar",
   .topic = "dawn",
   .aliases = { "cal", "schedule", "events", "appointment" },
   .alias_count = 4,

   .description = "Access and manage calendar events via CalDAV. "
                  "List available calendars ('which calendars are available'), "
                  "check today's schedule ('what's on my calendar today'), "
                  "find upcoming events ('when is my next meeting'), "
                  "search events ('do I have anything with dentist'), "
                  "create events ('add a meeting tomorrow at 2pm'), "
                  "update or delete events by UID.",
   .params = calendar_params,
   .param_count = TOOL_PARAM_COUNT(calendar_params),

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SECRETS | TOOL_CAP_SCHEDULABLE,
   .default_local = true,
   .default_remote = true,

   .init = calendar_tool_init,
   .cleanup = calendar_tool_cleanup,
   .is_available = calendar_tool_available,
   .callback = calendar_tool_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int calendar_tool_register(void) {
   return tool_registry_register(&calendar_metadata);
}
