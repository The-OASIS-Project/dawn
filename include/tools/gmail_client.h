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
 * Gmail REST API client — fetches and sends email via Gmail API v1.
 * Used automatically for OAuth+Gmail accounts; IMAP remains the fallback.
 */

#ifndef GMAIL_CLIENT_H
#define GMAIL_CLIENT_H

#include <stdbool.h>

#include "tools/email_types.h"

/**
 * @brief Fetch recent emails from Gmail, newest-first.
 * @param token            Bearer access token (caller handles OAuth refresh)
 * @param label_query      Gmail query fragment (e.g. "in:inbox", "in:sent", "label:Receipts").
 *                         If NULL/empty, defaults to "in:inbox".
 * @param count            Number of messages to fetch
 * @param unread_only      If true, only fetch unread messages
 * @param page_token       Pagination token from previous call (NULL for first page)
 * @param out              Output array of email summaries
 * @param max_out          Size of output array
 * @param out_count        Output: number of results written
 * @param next_page_token  Output: token for next page (empty if no more pages)
 * @param npt_len          Size of next_page_token buffer
 * @return 0 on success, 1 on failure
 */
int gmail_fetch_recent(const char *token,
                       const char *label_query,
                       int count,
                       bool unread_only,
                       const char *page_token,
                       email_summary_t *out,
                       int max_out,
                       int *out_count,
                       char *next_page_token,
                       size_t npt_len);

/* gmail_read_message return value for an HTTP 404 — the message id is not in
 * this mailbox (stale, deleted, or from a different account), as distinct from a
 * transport/network failure (1).  Lets the service layer report an accurate
 * "not found" rather than "backend unreachable". */
#define GMAIL_RC_NOT_FOUND 404

/**
 * @brief Read a full message by Gmail message ID.
 * @param token          Bearer access token
 * @param message_id     Gmail hex message ID
 * @param max_body_chars Maximum body characters to return
 * @param out            Output message (caller frees via email_message_free())
 * @return 0 on success, GMAIL_RC_NOT_FOUND on HTTP 404, 1 on any other failure
 */
int gmail_read_message(const char *token,
                       const char *message_id,
                       int max_body_chars,
                       email_message_t *out);

/**
 * @brief Search emails using Gmail search syntax.
 * @param token            Bearer access token
 * @param params           Search parameters (from, subject, text, dates, unread, page_token)
 * @param max_results      Maximum number of results
 * @param out              Output array of email summaries
 * @param max_out          Size of output array
 * @param out_count        Output: number of results written
 * @param next_page_token  Output: token for next page (empty if no more pages)
 * @param npt_len          Size of next_page_token buffer
 * @return 0 on success, 1 on failure
 */
int gmail_search(const char *token,
                 const email_search_params_t *params,
                 int max_results,
                 email_summary_t *out,
                 int max_out,
                 int *out_count,
                 char *next_page_token,
                 size_t npt_len);

/**
 * @brief Send an email via Gmail API.
 * @param token        Bearer access token
 * @param from_addr    Sender email address
 * @param display_name Sender display name (may be empty)
 * @param to_addr      Recipient email address
 * @param to_name      Recipient display name (may be NULL)
 * @param subject      Email subject
 * @param body         Email body text
 * @return 0 on success, 1 on failure
 */
int gmail_send(const char *token,
               const char *from_addr,
               const char *display_name,
               const char *to_addr,
               const char *to_name,
               const char *subject,
               const char *body);

/**
 * @brief Test Gmail API connectivity.
 * @param token     Bearer access token
 * @param email_out Output buffer for email address (may be NULL)
 * @param email_len Size of email_out buffer
 * @return 0 on success, 1 on failure
 */
int gmail_test_connection(const char *token, char *email_out, size_t email_len);

/**
 * @brief List Gmail labels (folders).
 * @param token    Bearer access token
 * @param out      Output buffer for formatted label list
 * @param out_len  Size of output buffer
 * @return 0 on success, 1 on failure
 */
int gmail_list_labels(const char *token, char *out, size_t out_len);

/**
 * @brief Move a message to Trash.
 * @param token       Bearer access token
 * @param message_id  Gmail hex message ID
 * @return 0 on success, 1 on failure
 */
int gmail_trash_message(const char *token, const char *message_id);

/**
 * @brief Archive a message (remove from Inbox, keep in All Mail).
 * @param token       Bearer access token
 * @param message_id  Gmail hex message ID
 * @return 0 on success, 1 on failure
 */
int gmail_archive_message(const char *token, const char *message_id);

#endif /* GMAIL_CLIENT_H */
