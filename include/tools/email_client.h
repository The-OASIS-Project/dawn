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

#include "tools/email_parse.h" /* EMAIL_IMAP_FOLDER_URL_MAX */
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

/* Room for a mailbox URL: the server URL, '/', the longest encoded folder
 * (EMAIL_IMAP_FOLDER_URL_MAX) and ;UIDVALIDITY=<v>. */
#define EMAIL_IMAP_MAILBOX_URL_MAX \
   (EMAIL_IMAP_FOLDER_URL_MAX + sizeof(((email_conn_t *)0)->imap_url) + 32)

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
 * @param inbox_unseen Optional: the INBOX's unread count, asked on the same
 *                     connection before the folder is selected (-1 when the
 *                     server didn't say); NULL to skip it
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
                       int *inbox_unseen,
                       email_err_t *err);

/**
 * @brief Read a message by UID from an IMAP folder (email_mime.h does the reading)
 *
 * Fetches at most opts->fetch_bytes of it (a bigger message reads as
 * truncated, never fails), or with opts->headers_only just From and Subject
 * (FETCH ENVELOPE, which leaves the message unread).  A full read marks the
 * message read on the server.
 *
 * @param folder      IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail")
 * @param uidvalidity The mailbox epoch the id was issued under (0 = don't pin):
 *                    a mailbox rebuilt since then is NOT_FOUND, never another message
 * @param err         Why it failed (may be NULL); EMAIL_ERR_NOT_FOUND for no such UID
 * @return 0 (@p out->uidvalidity the epoch seen), or 1 with no heap left in @p out;
 *         free a success with email_message_free()
 */
int email_read_message(const email_conn_t *conn,
                       const char *folder,
                       uint32_t uid,
                       uint32_t uidvalidity,
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

/**
 * @brief The INBOX's unread count: STATUS INBOX (UNSEEN), one login
 *        (email_imap_flags.c)
 * @return 0 with @p unseen set, or 1 (@p err says why)
 */
int email_imap_inbox_unseen(const email_conn_t *conn, int *unseen, email_err_t *err);

/** One folder's share of an email_imap_set_seen call. */
typedef struct {
   const char *folder;
   uint32_t uidvalidity; /* the epoch the ids were issued under (0 = don't pin) */
   const uint32_t *uids; /* parsed UIDs, never text from a client */
   int n;
   bool *updated;   /* per UID: true when the message is there in the asked state */
   email_err_t err; /* EMAIL_ERR_NONE, or why this folder's change failed */
} email_imap_seen_batch_t;

/**
 * @brief Mark messages read or unread (\Seen only, nothing else), folder by
 *        folder, on one connection: one login for the whole call
 *
 * Per folder: UID STORE, then a UID FETCH (FLAGS) of the same set to see which
 * messages exist and now have the state asked for (a STORE of a UID that
 * doesn't exist is OK on the wire).  If the STORE went through but that check
 * failed, the folder's messages count as updated.  A folder the server won't
 * SELECT (deleted or renamed) is NOT_FOUND and the rest go on.  A cancel, a
 * lost connection or a refused login ends the call: the folders not reached
 * get the same error.
 * email_imap_flags.c
 *
 * @return 0 when every folder's commands went through (see each @p updated), or 1
 */
int email_imap_set_seen(const email_conn_t *conn,
                        email_imap_seen_batch_t *batches,
                        int nbatches,
                        bool seen);

/** Test IMAP connectivity (log in and look at INBOX); true if it worked. */
bool email_test_imap(const email_conn_t *conn);

/** Test SMTP connectivity (connect only); true if it worked. */
bool email_test_smtp(const email_conn_t *conn);

/** Most messages one folder's move carries. */
#define EMAIL_IMAP_MOVE_MAX 50

/** One folder's share of an email_imap_move_batch call. */
typedef struct {
   const char *folder;
   uint32_t uidvalidity;          /* in: the epoch the ids were issued under (0 = don't
                                   * pin); out: the epoch the server showed */
   const uint32_t *uids;          /* parsed UIDs, never text from a client */
   int n;                         /* at most EMAIL_IMAP_MOVE_MAX */
   email_move_outcome_t *outcome; /* per UID */
   email_err_t *errs;             /* per UID: why it wasn't moved (EMAIL_ERR_NONE when it was) */
   uint32_t *dest_uid;            /* per UID: where it landed (0 = unknown: no undo) */
   uint32_t dest_uidvalidity;     /* out: the destination's epoch (0 = unknown) */
} email_imap_move_group_t;

/**
 * @brief Move messages to the account's Trash or Archive, folder by folder,
 *        on one login (email_imap_batch.c)
 *
 * The Trash is the user's own folder the server marks \Trash, else one with a
 * known name at the top of their folders; the Archive likewise (\Archive, else
 * on Gmail \All).  With none, nothing moves (EMAIL_ERR_NO_TRASH /
 * EMAIL_ERR_FOLDER_MISSING), nothing is deleted.  Per folder: a UID FETCH of the
 * set (a missing message is NOT_FOUND; a mailbox rebuilt since the ids were
 * issued, or gone, makes all of its ids NOT_FOUND), then UID MOVE, or UID COPY +
 * UID STORE \Deleted + UID EXPUNGE of the copied UIDs; never a bare EXPUNGE.  A
 * server with neither MOVE nor UIDPLUS gets the copy and the flag only
 * (EMAIL_MOVE_LEFT_FLAGGED); a copy whose original couldn't be removed is
 * EMAIL_ERR_NOT_REMOVED.  The thread's cancel (email_transfer.h) is honoured
 * between folders only; the folders not reached are EMAIL_ERR_CANCELLED.
 *
 * @param dest_folder Out: the folder they went to ("" when none)
 * @param err         Out: an error that ended the whole call (refused login, no
 *                    such folder, lost connection), else EMAIL_ERR_NONE
 * @return 0 when the call ran (see each group's results), or 1
 */
int email_imap_move_batch(const email_conn_t *conn,
                          email_move_kind_t kind,
                          email_imap_move_group_t *groups,
                          int ngroups,
                          char *dest_folder,
                          size_t dest_size,
                          email_err_t *err);

/** One folder's share of an email_imap_move_back call. */
typedef struct {
   const char *dest_folder;   /* where the messages were moved to */
   uint32_t dest_uidvalidity; /* its epoch then (0 = don't pin) */
   const char *src_folder;    /* where they go back to */
   const uint32_t *uids;      /* their UIDs in dest_folder */
   int n;                     /* at most EMAIL_IMAP_MOVE_MAX */
   email_move_outcome_t *outcome;
   email_err_t *errs;
   uint32_t *new_uid;        /* per UID: its UID back in src_folder (0 = unknown) */
   uint32_t src_uidvalidity; /* out: src_folder's epoch, from COPYUID */
   email_summary_t *rows;    /* per UID: the restored message's row */
   bool *row_ok;             /* per UID: rows[i] was read */
} email_imap_undo_group_t;

/**
 * @brief Move messages back where they came from (undo), on one login
 *        (email_imap_batch.c)
 *
 * As email_imap_move_batch, towards each group's src_folder: a message no
 * longer in dest_folder (or a dest_folder rebuilt since) is NOT_FOUND; a
 * src_folder that is gone is EMAIL_ERR_FOLDER_MISSING.  Each restored message's
 * row is read back from src_folder.
 * @return 0 when the call ran (see each group's results), or 1 (@p err says why)
 */
int email_imap_move_back(const email_conn_t *conn,
                         email_imap_undo_group_t *groups,
                         int ngroups,
                         email_err_t *err);

/**
 * @brief The folder the server marks \All (all of the user's mail), from the
 *        cached folder roles (probed on a login of its own when not cached;
 *        a failed probe isn't retried for 5 minutes)
 * @return false when none is marked (or it couldn't be learned): @p out is ""
 */
bool email_imap_all_mail_folder(const email_conn_t *conn, char *out, size_t size);

#endif /* EMAIL_CLIENT_H */
