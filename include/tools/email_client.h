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
 * Email client — IMAP/SMTP operations via libcurl.
 *
 * IMPORTANT: email_conn_t contains credentials and MUST be zeroed with
 * sodium_memzero() after use on all code paths (use goto cleanup pattern).
 */

#ifndef EMAIL_CLIENT_H
#define EMAIL_CLIENT_H

#include "tools/email_types.h"

typedef struct {
   char imap_url[512]; /* "imaps://imap.gmail.com:993" */
   char smtp_url[512]; /* "smtps://smtp.gmail.com:465" */
   char username[128];
   char password[256];     /* Empty when using OAuth */
   char bearer_token[512]; /* OAuth access token, empty for basic auth */
   char display_name[64];
   int max_body_chars;
} email_conn_t;

/**
 * @brief Fetch recent emails from an IMAP folder, sorted newest-first.
 * @param folder       IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @param unread_only  If true, only fetch unread (UNSEEN) emails
 * @return 0 on success, 1 on failure
 */
int email_fetch_recent(const email_conn_t *conn,
                       const char *folder,
                       int count,
                       bool unread_only,
                       email_summary_t *out,
                       int max_out,
                       int *out_count);

/**
 * @brief Read a full message by UID from an IMAP folder.
 * Allocates msg->body on heap; caller must call email_message_free().
 * @param folder  IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @return 0 on success, 1 on failure
 */
int email_read_message(const email_conn_t *conn,
                       const char *folder,
                       uint32_t uid,
                       email_message_t *out);

/**
 * @brief Search emails by criteria in an IMAP folder.
 * @param folder  IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @param auth_denied  Optional out (may be NULL): set true when the failure was
 *                     an IMAP login/credential rejection (CURLE_LOGIN_DENIED),
 *                     so the caller can surface an actionable "login failed".
 * @param timed_out    Optional out (may be NULL): set true when the failure was
 *                     a transfer timeout (CURLE_OPERATION_TIMEDOUT) — typically a
 *                     large mailbox with no server-side full-text index, so the
 *                     caller can hint the LLM to bound the search with a date.
 * @return 0 on success, 1 on failure
 */
int email_search(const email_conn_t *conn,
                 const char *folder,
                 const email_search_params_t *params,
                 email_summary_t *out,
                 int max_out,
                 int *out_count,
                 bool *auth_denied,
                 bool *timed_out);

/**
 * @brief Does @p iso parse as a valid IMAP search date (YYYY-MM-DD)?
 *
 * Shares the exact validation the SEARCH builder uses, so a caller can tell
 * whether a search will actually be date-bounded (e.g. to phrase a timeout hint
 * correctly) rather than guessing from raw param presence.
 */
bool email_search_date_valid(const char *iso);

/**
 * @brief List available IMAP folders.
 * @param out      Output buffer for formatted folder list
 * @param out_len  Size of output buffer
 * @return 0 on success, 1 on failure
 */
int email_list_folders(const email_conn_t *conn, char *out, size_t out_len);

/**
 * @brief Send an email via SMTP.
 * @return 0 on success, 1 on failure
 */
int email_send(const email_conn_t *conn,
               const char *to_addr,
               const char *to_name,
               const char *subject,
               const char *body);

/**
 * @brief Test IMAP and SMTP connectivity.
 * @param imap_ok  Output: true if IMAP connected successfully
 * @param smtp_ok  Output: true if SMTP connected successfully
 * @return 0 if both succeeded, 1 if either failed
 */
int email_test_connection(const email_conn_t *conn, bool *imap_ok, bool *smtp_ok);

/**
 * @brief Trash a message via IMAP (COPY to Trash + delete from source).
 * @param folder         Source folder (e.g. "INBOX")
 * @param uid            Message UID
 * @param is_gmail       True if Gmail IMAP server (uses [Gmail]/Trash)
 * @return 0 on success, 1 on failure
 */
int email_trash_message(const email_conn_t *conn, const char *folder, uint32_t uid, bool is_gmail);

/**
 * @brief Archive a message via IMAP (COPY to Archive + delete from source).
 * @param folder         Source folder (e.g. "INBOX")
 * @param uid            Message UID
 * @param is_gmail       True if Gmail IMAP server (uses [Gmail]/All Mail)
 * @return 0 on success, 1 on failure
 */
int email_archive_message(const email_conn_t *conn,
                          const char *folder,
                          uint32_t uid,
                          bool is_gmail);

#endif /* EMAIL_CLIENT_H */
