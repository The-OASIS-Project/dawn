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
 * IMAP paging cursor.  UIDs only grow within one UIDVALIDITY epoch, so "the next
 * page" is "matches with a UID below the oldest one already returned".
 */
typedef struct {
   uint32_t before_uid;       /* in:  only match UIDs below this (0 = first page) */
   uint32_t uidvalidity;      /* in:  epoch the cursor was issued under (0 = don't assert) */
   uint32_t next_before_uid;  /* out: cursor for the next page (0 = no more matches) */
   uint32_t next_uidvalidity; /* out: epoch observed on this call (0 = not reported);
                               * captured by email_instrument's debug callback */
   bool stale;                /* out: the pinned epoch no longer matches the mailbox */
} email_imap_page_t;

/**
 * @brief Fetch recent emails from an IMAP folder, sorted newest-first.
 * @param folder       IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @param unread_only  If true, only fetch unread (UNSEEN) emails
 * @param page         Optional paging cursor (NULL = first page, no cursor out)
 * @param err          Why it failed (may be NULL)
 * @return 0 on success, 1 on failure (page->stale set when the cursor's epoch changed)
 */
int email_fetch_recent(const email_conn_t *conn,
                       const char *folder,
                       int count,
                       bool unread_only,
                       email_imap_page_t *page,
                       email_summary_t *out,
                       int max_out,
                       int *out_count,
                       email_err_t *err);

/**
 * @brief Read a message by UID from an IMAP folder (email_mime.h does the reading)
 *
 * Fetches at most opts->fetch_bytes of it (a bigger message reads as
 * truncated, never fails), or with opts->headers_only just From and Subject
 * (FETCH ENVELOPE, which leaves the message unread).  A full read marks the
 * message read on the server.
 *
 * @param folder IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @param err    Why it failed (may be NULL); EMAIL_ERR_NOT_FOUND for no such UID
 * @return 0, or 1 with no heap left in @p out; free a success with email_message_free()
 */
int email_read_message(const email_conn_t *conn,
                       const char *folder,
                       uint32_t uid,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       email_err_t *err);

/**
 * @brief Search emails by criteria in an IMAP folder.
 * @param folder  IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @param err     Why it failed (may be NULL): EMAIL_ERR_AUTH_FAILED for a refused
 *                login, EMAIL_ERR_TIMEOUT for a search that ran out of time
 *                (typically a large mailbox with no server-side full-text index,
 *                so the caller can suggest bounding it with a date), ...
 * @return 0 on success, 1 on failure
 */
int email_search(const email_conn_t *conn,
                 const char *folder,
                 const email_search_params_t *params,
                 email_imap_page_t *page,
                 email_summary_t *out,
                 int max_out,
                 int *out_count,
                 email_err_t *err);

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

/** Test IMAP connectivity (log in and look at INBOX); true if it worked. */
bool email_test_imap(const email_conn_t *conn);

/** Test SMTP connectivity (connect only); true if it worked. */
bool email_test_smtp(const email_conn_t *conn);

/* Trash / archive results (0 = done).  The values match the service's
 * EMAIL_RC_* codes of the same names so they pass through. */
#define EMAIL_CLIENT_RC_FAILURE 1         /* network, server, or bad input */
#define EMAIL_CLIENT_RC_NOT_FOUND 13      /* no message with that UID in the folder */
#define EMAIL_CLIENT_RC_NO_TRASH 16       /* the account has no Trash folder */
#define EMAIL_CLIENT_RC_FOLDER_MISSING 17 /* the account has no Archive folder */
#define EMAIL_CLIENT_RC_ALREADY_THERE 18  /* the message is already in that folder */
#define EMAIL_CLIENT_RC_LEFT_FLAGGED 19   /* copied + \Deleted; no MOVE or UIDPLUS to remove it */
#define EMAIL_CLIENT_RC_NOT_REMOVED 20    /* copied; removing the original failed */

/**
 * @brief Move a message to the account's Trash folder (email_imap_move.c)
 *
 * The Trash is the user's own folder the server marks \Trash, else one with
 * a known name at the top of their folders; with none the message stays
 * where it is (EMAIL_CLIENT_RC_NO_TRASH), never deleted.  Only this message
 * is touched: UID MOVE, or UID COPY + UID STORE \Deleted + UID EXPUNGE of its
 * UID; never a bare EXPUNGE.  A server with neither MOVE nor UIDPLUS gets the
 * copy and the \Deleted flag only (EMAIL_CLIENT_RC_LEFT_FLAGGED); a copy whose
 * original couldn't then be removed is EMAIL_CLIENT_RC_NOT_REMOVED.
 *
 * @param folder Source folder (e.g. "INBOX")
 * @param uid    Message UID
 * @return 0, EMAIL_CLIENT_RC_NOT_FOUND, _NO_TRASH, _ALREADY_THERE,
 *         _LEFT_FLAGGED, _NOT_REMOVED, or _FAILURE
 */
int email_trash_message(const email_conn_t *conn, const char *folder, uint32_t uid);

/**
 * @brief Move a message to the account's Archive folder (email_imap_move.c)
 *
 * The Archive is the user's own folder the server marks \Archive, else on
 * Gmail \All (All Mail), else one with a known name.  Moves as
 * email_trash_message does.
 *
 * @return 0, EMAIL_CLIENT_RC_NOT_FOUND, _FOLDER_MISSING, _ALREADY_THERE,
 *         _LEFT_FLAGGED, _NOT_REMOVED, or _FAILURE
 */
int email_archive_message(const email_conn_t *conn, const char *folder, uint32_t uid);

#endif /* EMAIL_CLIENT_H */
