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
 * Calendar service — multi-account routing, background sync, business logic.
 * Coordinates between caldav_client (network) and calendar_db (storage).
 */

#ifndef CALENDAR_SERVICE_H
#define CALENDAR_SERVICE_H

#include <stdbool.h>

#include "config/dawn_config.h"
#include "core/tz.h"
#include "tools/calendar_db.h"
#include "tools/calendar_ical.h"

/* ============================================================================
 * Return codes (0 = success).  Mutating operations (add_account / add / update /
 * delete) return one of these; each outcome has a DISJOINT value so no literal
 * ever means two different things across functions.  See each function's
 * @return for which codes it can produce.
 * ============================================================================ */
#define CALENDAR_RC_OK 0        /* success */
#define CALENDAR_RC_FAILURE 1   /* generic — network, CalDAV, or unmapped */
#define CALENDAR_RC_DUPLICATE 2 /* add_account: an account with this identity already exists */
#define CALENDAR_RC_READONLY 3  /* target calendar/account is read-only (add / update / delete) */
#define CALENDAR_RC_NOT_FOUND                                                            \
   4                                  /* add: no single writable calendar has that name; \
                                       * update / delete: no such event (or the server lost it) */
#define CALENDAR_RC_AMBIGUOUS 5       /* that UID is in more than one calendar: name one */
#define CALENDAR_RC_NOT_SYNCED 6      /* cached without its server path yet: after a sync */
#define CALENDAR_RC_RATE_LIMITED 7    /* the server wants fewer requests for now */
#define CALENDAR_RC_REJECTED 8        /* the server refused the change as sent */
#define CALENDAR_RC_CONFLICT 9        /* changed elsewhere while being written, twice */
#define CALENDAR_RC_BAD_RULE 10       /* the repeat rule is invalid or never occurs */
#define CALENDAR_RC_NOT_OCCURRENCE 11 /* not an instance of that event */
#define CALENDAR_RC_GUEST 12          /* someone else organizes it: reminders only */
#define CALENDAR_RC_ZONE 13           /* its times are in a zone DAWN can't resolve */
#define CALENDAR_RC_BAD_TIME 14       /* the end isn't after the start, or a bad reminder */
#define CALENDAR_RC_UNREADABLE 15     /* part of the event can't be read, so it isn't rewritten */

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Initialize the calendar service. Starts the background sync thread.
 * Must be called after auth_db_init().
 */
int calendar_service_init(const calendar_config_t *config);

/** Shut down the calendar service. Stops the sync thread. */
void calendar_service_shutdown(void);

/** Return true if at least one account has synced successfully */
bool calendar_service_available(void);

/* ============================================================================
 * Account Management (WebUI admin)
 * ============================================================================ */

/**
 * @brief Add (provision) a CalDAV account for a user.
 * @return CALENDAR_RC_OK on success, CALENDAR_RC_FAILURE on failure,
 *         CALENDAR_RC_DUPLICATE if an account with this identity already exists
 */
int calendar_service_add_account(int user_id,
                                 const char *name,
                                 const char *caldav_url,
                                 const char *username,
                                 const char *password,
                                 bool read_only,
                                 const char *auth_type,
                                 const char *oauth_account_key);

/** Encrypt a plaintext password into acct->encrypted_password */
int calendar_encrypt_password(const char *plaintext, calendar_account_t *acct);

/** Decrypt acct->encrypted_password into a caller-provided buffer */
int calendar_decrypt_password(const calendar_account_t *acct, char *out, size_t out_len);

int calendar_service_remove_account(int64_t account_id);

/**
 * Test connection to an account. Performs full RFC discovery.
 * On success, caches principal_url and calendar_home_url,
 * and populates calendar_calendars rows.
 */
int calendar_service_test_connection(int64_t account_id);

/**
 * @brief Ask for an account to be synced now.  Every sync runs on the
 *        calendar's sync thread, one at a time; this returns at once, and the
 *        owner's WebUI sessions get calendar_broadcast_sync_done when it ends.
 * @return SUCCESS, or FAILURE when the service isn't running or too many
 *         syncs are waiting
 */
int calendar_service_request_sync(int64_t account_id);

/**
 * Notify a user's WebUI sessions that their calendar data changed (refetch nudge).
 * Signal-only (no event data). Called change-gated after each account sync.
 * Layer-3→Layer-4 hook: strong override in src/webui/webui_broadcasts.c; a weak
 * no-op fallback in calendar_service.c keeps WebUI-off builds linking.
 * @param user_id Owner whose browser sessions receive the refetch nudge.
 */
void calendar_broadcast_events_changed(int user_id);

/**
 * Tell a user's WebUI sessions that a sync they (or a new account) asked for
 * ended, and whether it worked.  Same hook pattern as
 * calendar_broadcast_events_changed.
 */
void calendar_broadcast_sync_done(int user_id, int64_t account_id, bool ok);

/* ============================================================================
 * Query Operations (used by calendar_tool.c)
 * ============================================================================ */

/**
 * Get today's events: midnight to midnight in @p zone.
 * @param user_id        Authenticated user
 * @param zone           The user's zone
 * @param calendar_name  Filter by calendar name (case-insensitive), NULL for all
 * @param out            Output array
 * @param max_count      Array capacity
 * @return Number of occurrences found (0 if none or on error)
 */
int calendar_service_today(int user_id,
                           const tz_t *zone,
                           const char *calendar_name,
                           calendar_occurrence_t *out,
                           int max_count);

/**
 * Get events in the range [@p start, @p end), all-day ones by their dates in
 * @p zone (the user's).
 * @param calendar_name  Filter by calendar name (case-insensitive), NULL for all
 */
int calendar_service_range(int user_id,
                           const tz_t *zone,
                           time_t start,
                           time_t end,
                           const char *calendar_name,
                           calendar_occurrence_t *out,
                           int max_count);

/** Get the next upcoming event.
 *  @param calendar_name  Filter by calendar name (case-insensitive), NULL for all */
int calendar_service_next(int user_id, const char *calendar_name, calendar_occurrence_t *out);

/** Search events by text.
 *  @param calendar_name  Filter by calendar name (case-insensitive), NULL for all */
int calendar_service_search(int user_id,
                            const char *query,
                            const char *calendar_name,
                            calendar_occurrence_t *out,
                            int max_count);

/* ============================================================================
 * Mutation Operations (write-through to server)
 * ============================================================================ */

/**
 * Create a new event on the server, then cache it.
 * @param event         The event (its uid is generated here; a timed one with no
 *                      end lasts the configured default)
 * @param calendar_name Target calendar: matched without case, or the one writable
 *                      calendar whose name contains it (NULL = first writable)
 * @param uid_out       Buffer for created UID (at least 256 bytes)
 * @param calendar_out  Receives the calendar's label (calendar_pick_label; may be NULL)
 * @return CALENDAR_RC_OK, or READONLY / NOT_FOUND (no such writable calendar),
 *         BAD_RULE / BAD_TIME, RATE_LIMITED / REJECTED (from the server), FAILURE
 */
int calendar_service_add(int user_id,
                         const calendar_ical_new_t *event,
                         const char *calendar_name,
                         char *uid_out,
                         size_t uid_out_len,
                         char *calendar_out,
                         size_t calendar_out_len);

/**
 * Change an event (or, with change->occurrence, one instance of it) on its
 * server: read fresh, changed only as asked, written back if unchanged since.
 * @param calendar_name Narrows the UID to that calendar (NULL = any)
 * @return CALENDAR_RC_OK or one of the codes above saying why not
 */
int calendar_service_update(int user_id,
                            const char *uid,
                            const char *calendar_name,
                            const calendar_ical_change_t *change);

/** Delete an event, every instance of it, on its server.
 *  @return CALENDAR_RC_OK or one of the codes above saying why not */
int calendar_service_delete(int user_id, const char *uid, const char *calendar_name);

/** Seconds the server asked to wait after a CALENDAR_RC_RATE_LIMITED on this
 *  thread (0 if it didn't say). */
int calendar_service_retry_after(void);

/* ============================================================================
 * Access Summary (for LLM tool read-only awareness)
 * ============================================================================ */

/**
 * Build comma-separated lists of writable and read-only calendar names.
 * @return 1 if any read-only calendars exist, 0 if all writable or on error
 */
int calendar_service_get_access_summary(int user_id,
                                        char *writable,
                                        size_t w_len,
                                        char *read_only_out,
                                        size_t r_len);

#endif /* CALENDAR_SERVICE_H */
