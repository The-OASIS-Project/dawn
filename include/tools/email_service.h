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
 * Email service — multi-account routing, auth dispatch, draft management.
 */

#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "tools/email_client.h"
#include "tools/email_db.h"

/* Draft / pending action limits */
#define EMAIL_MAX_DRAFTS 4
#define EMAIL_DRAFT_EXPIRY_SEC 300
/* Outbound (send/draft) body cap — bounded by the fixed email_draft_t.body[4096]
 * buffer; must stay well under 4096 to avoid silent truncation on send.
 * The inbound (read) cap is EMAIL_MAX_READ_BODY_LEN in email_types.h. */
#define EMAIL_MAX_SEND_BODY_LEN 4000
#define EMAIL_MAX_SUBJECT_LEN 250
#define EMAIL_CONFIRM_MAX_FAILURES 3
#define EMAIL_CONFIRM_LOCKOUT_SEC 60
#define EMAIL_MAX_PENDING_TRASH 10
#define EMAIL_PENDING_TRASH_EXPIRY_SEC 300

typedef struct {
   char draft_id[16]; /* hex-encoded randombytes_buf() */
   int user_id;
   char from_account[128]; /* canonical sending-account name, resolved at draft time */
   char to_address[256];
   char to_name[64];
   char subject[256];
   char body[4096];
   time_t created_at;
   bool used;
} email_draft_t;

typedef struct {
   char pending_id[16]; /* hex-encoded randombytes_buf() */
   int user_id;
   char message_id[192];   /* Gmail hex ID or IMAP folder:uid composite */
   char account_name[128]; /* Resolved account name for confirm step */
   char subject[256];      /* For confirmation display */
   char from[128];         /* For confirmation display */
   time_t created_at;
   bool used;
} email_pending_trash_t;

/* =============================================================================
 * Return codes (0 = success).  Specific codes below are shared across the
 * read paths (recent / read / search / list_folders) so the email_tool
 * wrapper can surface actionable messages instead of a generic
 * "search failed".  Other functions document their own per-function codes
 * (draft creation = 1/2/3/4, confirm = 1/2/3/4, etc.) reserved in the low
 * single digits — see each function's doc comment for its exact contract.
 * ============================================================================= */
#define EMAIL_RC_OK 0
#define EMAIL_RC_FAILURE 1          /* generic — network, upstream, or unmapped */
#define EMAIL_RC_UNKNOWN_ACCOUNT 10 /* account_name didn't match any configured account */
#define EMAIL_RC_NO_ACCOUNTS 11     /* user has no enabled email accounts */
#define EMAIL_RC_INVALID_FOLDER 12  /* folder name failed validation */
#define EMAIL_RC_NOT_FOUND 13       /* message id not found in the mailbox (stale/wrong/deleted) */

/* Two-step action codes (compose/send + trash prep/confirm; archive shares the
 * account-resolution set).  0/1 reuse EMAIL_RC_OK / EMAIL_RC_FAILURE above, and
 * account-not-found / no-accounts are forwarded verbatim as the shared
 * EMAIL_RC_UNKNOWN_ACCOUNT (10) / EMAIL_RC_NO_ACCOUNTS (11) — so the only codes
 * defined here are the genuinely per-function outcomes, and each has a DISJOINT
 * value (2-7): no literal ever means two different things. */
/* account read-only (create_draft / create_pending_trash / archive) */
#define EMAIL_ACCT_RC_READONLY 2

#define EMAIL_CONFIRM_RC_NOT_FOUND 3    /* draft/pending not found or expired */
#define EMAIL_CONFIRM_RC_THROTTLED 4    /* too many failed confirmations */
#define EMAIL_CONFIRM_RC_ACCOUNT_GONE 5 /* account gone/read-only at confirm time */

/* add_account: an account with this identity already exists */
#define EMAIL_ADD_RC_DUPLICATE 6

/* Access-summary status predicate (get_access_summary): this one is NOT an
 * OK/FAILURE result — 0 and 1 are a boolean, EMAIL_ACCESS_RC_ERROR is the
 * error sentinel. */
#define EMAIL_ACCESS_ALL_WRITABLE 0 /* no read-only accounts */
#define EMAIL_ACCESS_HAS_READONLY 1 /* at least one read-only account */
#define EMAIL_ACCESS_RC_ERROR 7     /* no accounts configured / lookup failed */

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int email_service_init(void);
void email_service_shutdown(void);
bool email_service_available(void);

/**
 * @brief Validate an IMAP/Gmail folder name (pre-flight check exposed to the
 *        tool layer so it can surface the offending folder string).
 *
 * Empty input is valid (defaults to inbox).  Rejects strings > 127 chars,
 * path-traversal (".."), and any character outside [A-Za-z0-9 _.\-/\[\]].
 *
 * @return true if folder is valid, false otherwise.
 */
bool email_service_validate_folder_name(const char *folder);

/* =============================================================================
 * Account Management (WebUI)
 * ============================================================================= */

/**
 * @brief Add (provision) an email account for a user.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure,
 *         EMAIL_ADD_RC_DUPLICATE if an account with this identity already exists
 */
int email_service_add_account(int user_id,
                              const char *name,
                              const char *imap_server,
                              int imap_port,
                              bool imap_ssl,
                              const char *smtp_server,
                              int smtp_port,
                              bool smtp_ssl,
                              const char *username,
                              const char *display_name,
                              const char *password,
                              bool read_only,
                              const char *auth_type,
                              const char *oauth_account_key);

int email_service_remove_account(int64_t account_id);
int email_service_test_connection(int64_t account_id, bool *imap_ok, bool *smtp_ok);
int email_service_list_accounts(int user_id, email_account_t *out, int max);

/* Best-effort fill of each row's `replied` tri-state (email_summary_t.replied):
 * for each Gmail account represented in @p rows, one `in:sent` search — bounded
 * to the oldest enrichable row's date for that account (a reply is always later
 * than the message it answers) — builds the set of threads the user sent into,
 * and a row is marked EMAIL_REPLIED_YES iff its thread has a SENT message dated
 * later than the row (the correspondent-answered-my-thread false positive is
 * thus excluded).  Rows with no thread_id (IMAP), self-sent rows (from_me),
 * unparseable-date rows, rows whose account's sent-search fails, and — when the
 * sent set truncates at the fetch cap — still-unmatched rows are left
 * EMAIL_REPLIED_UNKNOWN (never a false "not replied").  Network I/O: one search
 * per Gmail account with enrichable rows; call on the shown/capped rows only. */
void email_service_fill_reply_states(int user_id, email_summary_t *rows, int nrows);

/* True if this account is served by the Gmail REST backend (OAuth + gmail.com).
 * email_summary_t.unread is populated on BOTH backends (Gmail via the UNREAD
 * label, IMAP via the \Seen flag parsed at fetch time), so this predicate is
 * purely a backend selector, not an unread-reliability signal. */
bool email_service_is_gmail_account(const email_account_t *acct);

/* =============================================================================
 * Operations (used by email_tool.c)
 * account_name: string match against account.name, NULL = first enabled
 *
 * CONTRACT: these service-layer entry points stamp email_summary_t.account_name
 * on every returned row (both backends).  The lower-level fetch primitives
 * (gmail_fetch_recent / email_fetch_recent / gmail_search / email_search) do NOT
 * — they don't know the account name.  So any new caller (e.g. email_digest.c's
 * multi-account loop) MUST go through these service functions to inherit the
 * account label, or stamp account_name itself.
 * ============================================================================= */

int email_service_recent(int user_id,
                         const char *account_name,
                         const char *folder,
                         int count,
                         bool unread_only,
                         const char *page_token,
                         email_summary_t *out,
                         int max,
                         int *out_count,
                         char *next_page_token,
                         size_t npt_len);

int email_service_read(int user_id,
                       const char *account_name,
                       const char *message_id,
                       email_message_t *out);

/**
 * @brief Search email across one or (when account_name is NULL) all enabled accounts.
 * @param warn_out  Optional out (may be NULL): on a multi-account search where some
 *                  accounts failed but others succeeded, this is filled with a
 *                  human-readable note naming the unreachable accounts (auth failures
 *                  flagged distinctly) so the caller can tell the user results are
 *                  partial instead of the failure being silent. Empty when all
 *                  searched accounts were reached.
 */
int email_service_search(int user_id,
                         const char *account_name,
                         const email_search_params_t *params,
                         email_summary_t *out,
                         int max,
                         int *out_count,
                         char *next_page_token,
                         size_t npt_len,
                         char *warn_out,
                         size_t warn_len);

/**
 * @brief Create a draft email for two-step send.
 * @param account_name  Required: which configured account to send FROM (canonical
 *                      name or username). There is no implicit default — the caller
 *                      must name the sender so a reply can never go out from the
 *                      wrong mailbox. The resolved account is bound to the draft and
 *                      used verbatim at confirm time.
 * @param draft_id_out  Output: hex draft ID string
 * @param from_account_out  Optional output (may be NULL): the resolved CANONICAL
 *                      account name bound to the draft — differs from @p account_name
 *                      when the caller passed a username/email alias. Print this,
 *                      not the raw input, so the confirmation readback names the
 *                      account the send is actually bound to.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on generic failure (bad input
 *         / empty account), EMAIL_ACCT_RC_READONLY if the named account is
 *         read-only, or a forwarded EMAIL_RC_UNKNOWN_ACCOUNT / EMAIL_RC_NO_ACCOUNTS
 *         from account resolution (no name match / no configured accounts)
 */
int email_service_create_draft(int user_id,
                               const char *account_name,
                               const char *to_addr,
                               const char *to_name,
                               const char *subject,
                               const char *body,
                               char *draft_id_out,
                               size_t draft_id_len,
                               char *from_account_out,
                               size_t from_account_len);

/**
 * @brief Confirm and send a draft.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on send failure
 *         (network/upstream), EMAIL_CONFIRM_RC_NOT_FOUND if the draft is not
 *         found/expired, EMAIL_CONFIRM_RC_THROTTLED if throttled,
 *         EMAIL_CONFIRM_RC_ACCOUNT_GONE if the draft's bound sending account is
 *         no longer available or writable (deleted/disabled/read-only since the
 *         draft was prepared — prepare a new draft)
 */
int email_service_confirm_send(int user_id, const char *draft_id);

/**
 * @brief Get access summary for read-only indication.
 * @return EMAIL_ACCESS_HAS_READONLY if any read-only accounts exist,
 *         EMAIL_ACCESS_ALL_WRITABLE if all writable, EMAIL_ACCESS_RC_ERROR on error
 */
int email_service_get_access_summary(int user_id,
                                     char *writable,
                                     size_t w_len,
                                     char *read_only_out,
                                     size_t r_len);

/**
 * @brief List available folders/labels for an email account.
 * @param account_name  Account name, or NULL for first enabled
 * @param out           Output buffer for formatted folder list
 * @param out_len       Size of output buffer
 * @return 0 on success, 1 on failure
 */
int email_service_list_folders(int user_id, const char *account_name, char *out, size_t out_len);

/**
 * @brief Create a pending trash action for two-step delete.
 * Fetches message metadata for confirmation display.
 * @param pending_id_out  Output: hex pending ID string
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure,
 *         EMAIL_ACCT_RC_READONLY if the account is read-only
 */
int email_service_create_pending_trash(int user_id,
                                       const char *account_name,
                                       const char *message_id,
                                       char *pending_id_out,
                                       size_t pending_id_len,
                                       char *subject_out,
                                       size_t subject_len,
                                       char *from_out,
                                       size_t from_len);

/**
 * @brief Confirm and execute a pending trash action.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure,
 *         EMAIL_CONFIRM_RC_NOT_FOUND if not found/expired,
 *         EMAIL_CONFIRM_RC_THROTTLED if throttled,
 *         EMAIL_CONFIRM_RC_ACCOUNT_GONE if the account is no longer available or
 *         writable since the pending action was prepared
 */
int email_service_confirm_trash(int user_id, const char *pending_id);

/**
 * @brief Archive a message (remove from inbox, keep in All Mail/Archive).
 * Single-step operation — no confirmation needed.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure,
 *         EMAIL_ACCT_RC_READONLY if the account is read-only
 */
int email_service_archive(int user_id, const char *account_name, const char *message_id);

#endif /* EMAIL_SERVICE_H */
