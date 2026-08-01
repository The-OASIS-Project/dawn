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
#include "tools/calendar_db.h"

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

/** Trigger an immediate sync for an account */
int calendar_service_sync_now(int64_t account_id);

/**
 * Notify a user's WebUI sessions that their calendar data changed (refetch nudge).
 * Signal-only (no event data). Called change-gated from calendar_service_sync_now.
 * Layer-3→Layer-4 hook: strong override in src/webui/webui_broadcasts.c; a weak
 * no-op fallback in calendar_service.c keeps WebUI-off builds linking.
 * @param user_id Owner whose browser sessions receive the refetch nudge.
 */
void calendar_broadcast_events_changed(int user_id);

/* ============================================================================
 * Query Operations (used by calendar_tool.c)
 * ============================================================================ */

/**
 * Get events for today in the user's timezone.
 * @param user_id        Authenticated user
 * @param tz_name        IANA timezone name (e.g., "America/New_York")
 * @param calendar_name  Filter by calendar name (case-insensitive), NULL for all
 * @param out            Output array
 * @param max_count      Array capacity
 * @return Number of occurrences found (0 if none or on error)
 */
int calendar_service_today(int user_id,
                           const char *tz_name,
                           const char *calendar_name,
                           calendar_occurrence_t *out,
                           int max_count);

/**
 * Get events in a date/time range.
 * @param calendar_name  Filter by calendar name (case-insensitive), NULL for all
 */
int calendar_service_range(int user_id,
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
 * Create a new event. Writes to CalDAV server first, then caches.
 * @param user_id       Authenticated user
 * @param summary       Event title (required)
 * @param start         Start time (epoch)
 * @param end           End time (epoch, 0 = use default duration)
 * @param location      Location (may be NULL)
 * @param description   Description (may be NULL)
 * @param all_day       True for all-day event
 * @param calendar_name Target calendar name (NULL = first active)
 * @param rrule         Recurrence rule (NULL = non-recurring)
 * @param tz_name       User timezone for iCalendar DTSTART TZID
 * @param uid_out       Buffer for created UID (at least 256 bytes)
 */
int calendar_service_add(int user_id,
                         const char *summary,
                         time_t start,
                         time_t end,
                         const char *location,
                         const char *description,
                         bool all_day,
                         const char *calendar_name,
                         const char *rrule,
                         const char *tz_name,
                         char *uid_out,
                         size_t uid_out_len);

/**
 * Update an existing event by UID. Write-through to server.
 */
int calendar_service_update(int user_id,
                            const char *uid,
                            const char *summary,
                            time_t start,
                            time_t end,
                            const char *location,
                            const char *description);

/** Delete an event by UID. Write-through to server. */
int calendar_service_delete(int user_id, const char *uid);

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
