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
 * Calendar database layer — SQLite CRUD for accounts, calendars, events,
 * and pre-expanded occurrences. Uses the shared auth_db handle (s_db).
 */

#ifndef CALENDAR_DB_H
#define CALENDAR_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CALENDAR_MAX_ACCOUNTS 16

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
   int64_t id;
   int user_id;
   char name[128];
   char caldav_url[512];
   char username[128];
   uint8_t encrypted_password[384]; /**< nonce + ciphertext (libsodium secretbox) */
   int encrypted_password_len;      /**< Length of encrypted_password data */
   char auth_type[16];              /**< "basic", "app_password", or "oauth" */
   char oauth_account_key[128];     /**< Links to oauth_tokens when auth_type="oauth" */
   char principal_url[512];
   char calendar_home_url[512];
   bool enabled;
   bool read_only; /**< User-set flag: prevent LLM from modifying this account */
   time_t last_sync;
   int sync_interval_sec;
   time_t created_at;
} calendar_account_t;

typedef struct {
   int64_t id;
   int64_t account_id;
   char caldav_path[512];
   char display_name[256];
   char color[16];
   bool is_active;
   char ctag[128];
   time_t created_at;
   bool account_read_only; /**< Populated by JOIN queries — reflects parent account's read_only */
} calendar_calendar_t;

typedef struct {
   int64_t id;
   int64_t calendar_id;
   char uid[256];
   char etag[128];
   char summary[512];
   char description[1024];
   char location[256];
   time_t dtstart;
   time_t dtend;
   int duration_sec;
   bool all_day;
   char dtstart_date[16];
   char dtend_date[16];
   char rrule[256];
   char *raw_ical; /**< Heap-allocated, caller frees */
   time_t last_synced;
} calendar_event_t;

typedef struct {
   int64_t id;
   int64_t event_id;
   time_t dtstart;
   time_t dtend;
   bool all_day;
   char dtstart_date[16];
   char dtend_date[16];
   char summary[512];
   char location[256];
   bool is_override;
   bool is_cancelled;
   char recurrence_id[32];
   char event_uid[256]; /**< Populated by JOIN queries — the parent event's UID */
   int64_t
       calendar_id; /**< Populated by range JOIN queries — the owning calendar's id (0 if absent) */
} calendar_occurrence_t;

/* ============================================================================
 * Account CRUD
 * ============================================================================ */

int calendar_db_account_create(const calendar_account_t *acct, int64_t *id_out);
int calendar_db_account_get(int64_t id, calendar_account_t *out);
int calendar_db_account_list(int user_id, calendar_account_t *out, int max_count, int *count_out);
int calendar_db_account_update(const calendar_account_t *acct);
int calendar_db_account_delete(int64_t id);
int calendar_db_account_update_sync(int64_t id, time_t last_sync);
int calendar_db_account_update_discovery(int64_t id,
                                         const char *principal_url,
                                         const char *calendar_home_url);

/** Set the read_only flag on an account (dedicated setter to avoid accidental clears) */
int calendar_db_account_set_read_only(int64_t id, bool read_only);

/** Set the enabled flag on an account */
int calendar_db_account_set_enabled(int64_t id, bool enabled);

/**
 * List all enabled accounts across all users (for background sync).
 * @param count_out  Number of accounts written to out (0 if none)
 * @return 0 on success, 1 on failure
 */
int calendar_db_account_list_enabled(calendar_account_t *out, int max_count, int *count_out);

/* ============================================================================
 * Calendar CRUD
 * ============================================================================ */

int calendar_db_calendar_create(const calendar_calendar_t *cal, int64_t *id_out);
int calendar_db_calendar_get(int64_t id, calendar_calendar_t *out);
int calendar_db_calendar_list(int64_t account_id,
                              calendar_calendar_t *out,
                              int max_count,
                              int *count_out);
int calendar_db_calendar_update_ctag(int64_t id, const char *ctag);
int calendar_db_calendar_set_active(int64_t id, bool active);
int calendar_db_calendar_delete(int64_t id);

/**
 * Get all active calendars for a user (across all enabled accounts).
 * @param count_out  Number of calendars written to out (0 if none)
 * @return 0 on success, 1 on failure
 */
int calendar_db_active_calendars_for_user(int user_id,
                                          calendar_calendar_t *out,
                                          int max_count,
                                          int *count_out);

/* ============================================================================
 * Event CRUD
 * ============================================================================ */

int calendar_db_event_upsert(const calendar_event_t *event, int64_t *id_out);
int calendar_db_event_get_by_uid(const char *uid, calendar_event_t *out);
int calendar_db_event_delete(int64_t id);
int calendar_db_event_delete_by_calendar(int64_t calendar_id);

/* ============================================================================
 * Occurrence CRUD
 * ============================================================================ */

int calendar_db_occurrence_insert(const calendar_occurrence_t *occ, int64_t *id_out);
int calendar_db_occurrence_delete_for_event(int64_t event_id);

/**
 * Query occurrences in a time range across given calendar IDs.
 * Only returns non-cancelled occurrences.
 * @param count_out  Number of occurrences written to out (0 if none)
 * @return 0 on success, 1 on failure
 */
int calendar_db_occurrences_in_range(const int64_t *calendar_ids,
                                     int calendar_count,
                                     time_t range_start,
                                     time_t range_end,
                                     calendar_occurrence_t *out,
                                     int max_count,
                                     int *count_out);

/**
 * Query all-day occurrences in a date range (string comparison).
 * @param count_out  Number of occurrences written to out (0 if none)
 * @return 0 on success, 1 on failure
 */
int calendar_db_allday_occurrences_in_range(const int64_t *calendar_ids,
                                            int calendar_count,
                                            const char *start_date,
                                            const char *end_date,
                                            calendar_occurrence_t *out,
                                            int max_count,
                                            int *count_out);

/**
 * Search occurrences by text (LIKE %query% on summary/location).
 * @param count_out  Number of occurrences written to out (0 if none)
 * @return 0 on success, 1 on failure
 */
int calendar_db_occurrences_search(const int64_t *calendar_ids,
                                   int calendar_count,
                                   const char *query,
                                   calendar_occurrence_t *out,
                                   int max_count,
                                   int *count_out);

/**
 * Get the next upcoming occurrence after a given time.
 */
int calendar_db_next_occurrence(const int64_t *calendar_ids,
                                int calendar_count,
                                time_t after,
                                calendar_occurrence_t *out);

#endif /* CALENDAR_DB_H */
