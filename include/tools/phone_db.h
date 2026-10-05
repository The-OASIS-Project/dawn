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
 * Phone Database — SQLite CRUD for call and SMS logs.
 * Uses the shared auth_db handle (Pattern A).
 */

#ifndef PHONE_DB_H
#define PHONE_DB_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "tools/phone_number.h"

/* Call direction */
typedef enum {
   PHONE_DIR_OUTGOING = 0,
   PHONE_DIR_INCOMING = 1,
} phone_direction_t;

/* Call status */
typedef enum {
   PHONE_CALL_ANSWERED = 0,
   PHONE_CALL_MISSED = 1,
   PHONE_CALL_REJECTED = 2,
   PHONE_CALL_FAILED = 3,
} phone_call_status_t;

/* Call log entry */
typedef struct {
   int64_t id;
   int user_id;
   int direction; /* PHONE_DIR_OUTGOING or PHONE_DIR_INCOMING */
   char number[24];
   char contact_name[64];
   int duration_sec;
   time_t timestamp;
   int status; /* PHONE_CALL_ANSWERED, MISSED, REJECTED, FAILED */
} phone_call_log_t;

/* SMS log entry. body is sized to hold multi-segment concatenated SMS
 * reassembled by ECHO (PDU mode): up to ~1530 ASCII or ~670 UCS2 chars. */
typedef struct {
   int64_t id;
   int user_id;
   int direction;
   char number[24];
   char contact_name[64];
   char body[2048];
   time_t timestamp;
   int read;
} phone_sms_log_t;

/* Return codes for phone_db operations. */
#define PHONE_DB_SUCCESS 0
#define PHONE_DB_FAILURE 1
#define PHONE_DB_NOT_FOUND 2

/**
 * @brief Redact a phone number for audit logs: keep last 4 digits, mask the
 *        rest. "+15551234567" → "...4567". Short strings (<=4 chars) and
 *        empty/NULL inputs pass through unchanged (short codes like "911"
 *        keep forensic value; empty becomes "(none)").
 *
 * @param in       Raw phone number (may be NULL/empty).
 * @param out      Output buffer.
 * @param out_size Size of out buffer.
 */
void phone_number_redact(const char *in, char *out, size_t out_size);

/**
 * @brief Format a phone number for TTS: insert spaces between digits so Piper
 *        reads "+15551234567" as "1 5 5 5 1 2 3 4 5 6 7" (each digit spoken
 *        individually) instead of as a billion-dollar integer. Non-digits are
 *        dropped. Empty/NULL produces "".
 *
 * @param in       Raw phone number (may be NULL/empty).
 * @param out      Output buffer receiving space-separated digits.
 * @param out_size Size of out buffer.
 */
void phone_number_format_for_tts(const char *in, char *out, size_t out_size);

/* phone_number_normalize: tools/phone_number.h */

/**
 * @brief Insert a call log entry.
 * @param id_out Output: row ID of inserted entry (can be NULL).
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_call_log_insert(int user_id,
                             int direction,
                             const char *number,
                             const char *contact_name,
                             int duration_sec,
                             time_t timestamp,
                             int status,
                             int64_t *id_out);

/**
 * @brief Update a call log entry (e.g., set duration after call ends).
 * @return 0 on success, 1 on error.
 */
int phone_db_call_log_update(int64_t id, int duration_sec, int status);

/**
 * @brief Query recent call log entries.
 * @param user_id   User ID for isolation.
 * @param out       Output array.
 * @param max       Maximum entries to return.
 * @param count_out Output: number of entries returned.
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_call_log_recent(int user_id, phone_call_log_t *out, int max, int *count_out);

/**
 * @brief Insert an SMS log entry.
 * @param id_out Output: row ID of inserted entry (can be NULL).
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_sms_log_insert(int user_id,
                            int direction,
                            const char *number,
                            const char *contact_name,
                            const char *body,
                            time_t timestamp,
                            int64_t *id_out);

/**
 * @brief Get unread SMS entries for a user.
 * @param count_out Output: number of entries returned.
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_sms_get_unread(int user_id, phone_sms_log_t *out, int max, int *count_out);

/**
 * @brief Query recent SMS log entries.
 * @param count_out Output: number of entries returned.
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_sms_log_recent(int user_id, phone_sms_log_t *out, int max, int *count_out);

/**
 * @brief Mark an SMS as read.
 * @return 0 on success, 1 on error.
 */
int phone_db_sms_mark_read(int64_t id);

/**
 * @brief Delete old log entries beyond retention period.
 * @return 0 on success, 1 on error.
 */
int phone_db_cleanup(int call_retention_days, int sms_retention_days);

/**
 * @brief Delete a single SMS log entry by ID, scoped to user_id.
 * @param user_id User ID for isolation.
 * @param id      SMS row ID.
 * @return PHONE_DB_SUCCESS, PHONE_DB_NOT_FOUND, or PHONE_DB_FAILURE.
 */
int phone_db_sms_log_delete(int user_id, int64_t id);

/**
 * @brief Delete all SMS log entries matching a phone number, scoped to user_id.
 *        The number is normalized (strip spaces/dashes/parens, ensure leading +)
 *        before lookup so LLM-supplied formats match stored E.164 rows.
 * @param user_id   User ID for isolation.
 * @param number    Phone number (any format; normalized internally).
 * @param out_count Rows deleted (may be NULL).
 * @return PHONE_DB_SUCCESS (even if 0 rows), PHONE_DB_FAILURE on SQL error.
 */
int phone_db_sms_log_delete_by_number(int user_id, const char *number, int *out_count);

/**
 * @brief Count SMS log entries matching a phone number (for delete preview).
 *        Normalization matches phone_db_sms_log_delete_by_number.
 * @param user_id   User ID for isolation.
 * @param number    Phone number (any format; normalized internally).
 * @param out_count Matching row count (on success).
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_sms_log_count_by_number(int user_id, const char *number, int *out_count);

/**
 * @brief Fetch a single SMS log entry by ID, scoped to user_id.
 *        Avoids the 21 KB stack frame of recent-query when the caller knows
 *        the id (e.g., delete-by-id previews). Also works for entries
 *        outside the recent window.
 * @param user_id User ID for isolation.
 * @param id      SMS row ID.
 * @param out     Output row (populated on SUCCESS).
 * @return PHONE_DB_SUCCESS, PHONE_DB_NOT_FOUND, or PHONE_DB_FAILURE.
 */
int phone_db_sms_log_get_by_id(int user_id, int64_t id, phone_sms_log_t *out);

/**
 * @brief Delete all SMS log entries older than cutoff, scoped to user_id.
 * @param user_id   User ID for isolation.
 * @param cutoff    Unix timestamp — rows with timestamp < cutoff are deleted.
 * @param out_count Rows deleted (may be NULL).
 * @return PHONE_DB_SUCCESS (even if 0 rows), PHONE_DB_FAILURE on SQL error.
 */
int phone_db_sms_log_delete_older_than(int user_id, time_t cutoff, int *out_count);

/**
 * @brief Count SMS log entries older than cutoff (for delete preview).
 * @param user_id   User ID for isolation.
 * @param cutoff    Unix timestamp — rows with timestamp < cutoff are counted.
 * @param out_count Matching row count (on success).
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_sms_log_count_older_than(int user_id, time_t cutoff, int *out_count);

/**
 * @brief Delete a single call log entry by ID, scoped to user_id.
 * @param user_id User ID for isolation.
 * @param id      Call log row ID.
 * @return PHONE_DB_SUCCESS, PHONE_DB_NOT_FOUND, or PHONE_DB_FAILURE.
 */
int phone_db_call_log_delete(int user_id, int64_t id);

/**
 * @brief Fetch a single call log entry by ID, scoped to user_id.
 * @param user_id User ID for isolation.
 * @param id      Call log row ID.
 * @param out     Output row (populated on SUCCESS).
 * @return PHONE_DB_SUCCESS, PHONE_DB_NOT_FOUND, or PHONE_DB_FAILURE.
 */
int phone_db_call_log_get_by_id(int user_id, int64_t id, phone_call_log_t *out);

/**
 * @brief Delete call log entries older than cutoff, scoped to user_id.
 * @param user_id   User ID for isolation.
 * @param cutoff    Unix timestamp — rows with timestamp < cutoff are deleted.
 * @param out_count Rows deleted (may be NULL).
 * @return PHONE_DB_SUCCESS (even if 0 rows), PHONE_DB_FAILURE on SQL error.
 */
int phone_db_call_log_delete_older_than(int user_id, time_t cutoff, int *out_count);

/**
 * @brief Count call log entries older than cutoff (for delete preview).
 * @param user_id   User ID for isolation.
 * @param cutoff    Unix timestamp — rows with timestamp < cutoff are counted.
 * @param out_count Matching row count (on success).
 * @return PHONE_DB_SUCCESS or PHONE_DB_FAILURE.
 */
int phone_db_call_log_count_older_than(int user_id, time_t cutoff, int *out_count);

#endif /* PHONE_DB_H */
