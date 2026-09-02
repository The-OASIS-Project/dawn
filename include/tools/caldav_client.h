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
 * CalDAV protocol client — RFC 4791 calendar operations via libcurl + libxml2.
 * Handles discovery (3-step RFC-compliant), event fetch/create/update/delete.
 */

#ifndef CALDAV_CLIENT_H
#define CALDAV_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================================
 * Error Codes
 * ============================================================================ */

typedef enum {
   CALDAV_OK = 0,
   CALDAV_ERR_NETWORK,            /**< curl or DNS failure */
   CALDAV_ERR_AUTH,               /**< 401 Unauthorized */
   CALDAV_ERR_FORBIDDEN,          /**< 403 Forbidden */
   CALDAV_ERR_NOT_FOUND,          /**< 404 Not Found */
   CALDAV_ERR_CONFLICT,           /**< 412 Precondition Failed (ETag mismatch) */
   CALDAV_ERR_SERVER,             /**< 5xx server error */
   CALDAV_ERR_PARSE,              /**< XML or iCalendar parse failure */
   CALDAV_ERR_NO_CALENDARS,       /**< Discovery found no calendar collections */
   CALDAV_ERR_ALLOC,              /**< Memory allocation failure */
   CALDAV_ERR_SYNC_TOKEN_INVALID, /**< RFC 6578 sync-token rejected (stale/expired) — re-baseline */
} caldav_error_t;

/* ============================================================================
 * Discovery Types
 * ============================================================================ */

/** Info about a single discovered calendar collection */
typedef struct {
   char path[512];         /**< Full URL path to calendar collection */
   char display_name[256]; /**< Calendar display name */
   char color[16];         /**< Hex color (#FF5733), may be empty */
   char ctag[128];         /**< Collection tag for change detection */
   bool read_only;         /**< True if no write privileges */
} caldav_calendar_info_t;

/** Result of full calendar discovery */
typedef struct {
   char principal_url[512];           /**< Resolved principal URL */
   char calendar_home_url[512];       /**< Resolved calendar-home-set URL */
   caldav_calendar_info_t *calendars; /**< Array of discovered calendars */
   int calendar_count;
} caldav_discovery_result_t;

/* ============================================================================
 * Event Types
 * ============================================================================ */

/** A single fetched event from the server */
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
   char *raw_ical;         /**< Full iCalendar data (heap-allocated) */
   int duration_sec;       /**< Duration in seconds (for recurring) */
   char href[512];         /**< Resource path on server */
} caldav_event_t;

/** Array of fetched events */
typedef struct {
   caldav_event_t *events;
   int count;
} caldav_event_list_t;

/* ============================================================================
 * Sync-Collection Types (RFC 6578)
 * ============================================================================ */

/** One member reported by a sync-collection REPORT. */
typedef struct {
   char href[512]; /**< Resolved resource path (matches calendar_events.href on fetch). */
   bool gone;      /**< True when the member reported a 404 status (removed upstream). */
} caldav_sync_change_t;

/**
 * Result of a sync-collection REPORT (paged to completion internally).
 *
 * `gone` entries are the removal signal; non-gone entries are added/modified
 * members (hrefs only — bodies are fetched separately by the time-range query).
 * `sync_token` is the new baseline to persist for the next incremental run.
 * `complete` is false when paging was capped or a page failed: the caller MUST
 * NOT prune-by-set and MUST NOT persist the token when complete is false, but
 * may still apply the `gone` deletions (a confirmed 404 is always safe to act on).
 */
typedef struct {
   caldav_sync_change_t *changes;
   int count;
   char sync_token[1024]; /**< Opaque server token; sized with headroom to avoid truncation
                           *   (a truncated token would be rejected → perpetual re-baseline). */
   bool complete;
} caldav_sync_result_t;

/* ============================================================================
 * Authentication
 * ============================================================================ */

typedef struct {
   const char *username;
   const char *password;     /**< NULL when using OAuth */
   const char *bearer_token; /**< OAuth access token, NULL for basic auth */
} caldav_auth_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Full RFC-compliant calendar discovery (3-step with adaptive fallback).
 *
 * Steps: PROPFIND base_url → current-user-principal → calendar-home-set →
 *        enumerate calendar collections. Falls back to base URL as
 *        calendar-home if step 1 fails (simple servers like Radicale).
 */
caldav_error_t caldav_discover(const char *base_url,
                               const caldav_auth_t *auth,
                               caldav_discovery_result_t *result);

/** Free discovery result internals */
void caldav_discovery_free(caldav_discovery_result_t *result);

/**
 * Get the current ctag for a calendar (PROPFIND for change detection).
 * @param ctag_out  Buffer for ctag string (at least 128 bytes)
 */
caldav_error_t caldav_get_ctag(const char *calendar_url,
                               const caldav_auth_t *auth,
                               char *ctag_out,
                               size_t ctag_len);

/**
 * Fetch events in a time range (REPORT with calendar-query).
 * Caller must free result with caldav_event_list_free().
 */
caldav_error_t caldav_fetch_events(const char *calendar_url,
                                   const caldav_auth_t *auth,
                                   time_t range_start,
                                   time_t range_end,
                                   caldav_event_list_t *result);

/** Free event list internals */
void caldav_event_list_free(caldav_event_list_t *list);

/**
 * RFC 6578 sync-collection REPORT: enumerate changes since a sync-token.
 *
 * @param sync_token_in  Prior token for an incremental run, or "" / NULL to
 *                       baseline (server enumerates the whole collection and
 *                       mints a fresh token).
 * @param result_out     Filled with changed/removed hrefs + the new token +
 *                       a `complete` flag. Caller frees with
 *                       caldav_sync_result_free().
 *
 * Returns CALDAV_ERR_SYNC_TOKEN_INVALID when a non-empty token was supplied and
 * the server rejected it (stale/expired) — the caller should retry with "" to
 * re-baseline. Returns other caldav_error_t values on auth/network/server
 * failure or when the server does not support sync-collection.
 */
caldav_error_t caldav_sync_collection(const char *calendar_url,
                                      const caldav_auth_t *auth,
                                      const char *sync_token_in,
                                      caldav_sync_result_t *result_out);

/** Free sync-collection result internals. */
void caldav_sync_result_free(caldav_sync_result_t *result);

/**
 * Create a new event (PUT with If-None-Match: *).
 * @param ical_data  Complete iCalendar string (BEGIN:VCALENDAR...END:VCALENDAR)
 * @param uid        Event UID (used to construct resource path)
 * @param etag_out   Buffer for returned ETag (at least 128 bytes), may be NULL
 */
caldav_error_t caldav_create_event(const char *calendar_url,
                                   const caldav_auth_t *auth,
                                   const char *uid,
                                   const char *ical_data,
                                   char *etag_out,
                                   size_t etag_len);

/**
 * Update an existing event (PUT with If-Match: etag).
 * @param href       Resource path from original fetch
 * @param etag       ETag from cache (optimistic concurrency)
 * @param ical_data  Modified iCalendar string
 * @param etag_out   Buffer for new ETag
 */
caldav_error_t caldav_update_event(const char *href,
                                   const caldav_auth_t *auth,
                                   const char *etag,
                                   const char *ical_data,
                                   char *etag_out,
                                   size_t etag_len);

/**
 * Delete an event (DELETE with If-Match: etag).
 * @param href  Resource path from original fetch
 * @param etag  ETag from cache
 */
caldav_error_t caldav_delete_event(const char *href, const caldav_auth_t *auth, const char *etag);

/**
 * Return human-readable error string for a caldav_error_t.
 */
const char *caldav_strerror(caldav_error_t err);

#endif /* CALDAV_CLIENT_H */
