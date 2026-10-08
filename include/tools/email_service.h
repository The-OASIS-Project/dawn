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

#include "core/turn_origin.h"
#include "tools/email_client.h"
#include "tools/email_db.h"
#include "tools/email_undo.h"

/* Draft / pending action limits */
#define EMAIL_MAX_DRAFTS 16
#define EMAIL_DRAFT_EXPIRY_SEC 300
/* Outbound (send/draft) body cap — bounded by the fixed email_draft_t.body[4096]
 * buffer; must stay well under 4096 to avoid silent truncation on send.
 * The inbound (read) cap is EMAIL_MAX_READ_BODY_LEN in email_types.h. */
#define EMAIL_MAX_SEND_BODY_LEN 4000
#define EMAIL_MAX_SUBJECT_LEN 250
#define EMAIL_CONFIRM_MAX_FAILURES 3
#define EMAIL_CONFIRM_LOCKOUT_SEC 60
#define EMAIL_MAX_PENDING_TRASH 16
#define EMAIL_PENDING_TRASH_EXPIRY_SEC 300
/* Longest wait for an IMAP account's lease (the longest operation another
 * caller may hold it for: a search's connect plus its budget, with margin). */
#define EMAIL_LEASE_WAIT_SEC 90

/* How long a multi-account search or read waits for each IMAP account's lease
 * before reporting that account busy and moving on. */
#define EMAIL_LEASE_FANOUT_WAIT_SEC 10

/**
 * The account an operation runs on, chosen by id (checked against the user; it
 * must be enabled).  NULL instead means the operation resolves the account by
 * name.  The operation takes the account's IMAP lease itself unless
 * @c lease_held says the caller already holds it; a Gmail account needs none.
 */
typedef struct {
   int64_t account_id;
   bool lease_held; /* the caller holds this IMAP account's lease */
} email_target_t;

/* An account that failed in a multi-account search, and why. */
typedef struct {
   int64_t account_id;
   email_err_t err;
} email_acct_failure_t;

/* What went wrong in a search (email_service_search). */
typedef struct {
   email_err_t err;                                 /* why it failed; NONE on success */
   email_acct_failure_t failed[EMAIL_MAX_ACCOUNTS]; /* accounts a fan-out couldn't search */
   int failed_count;
   uint32_t uidvalidity;  /* one IMAP account: the mailbox epoch seen (0 = not seen) */
   char imap_folder[128]; /* one IMAP account: the folder searched ("" = not reached) */
   int rows_missing;      /* Gmail: matching rows left out (Gmail kept refusing them) */
} email_search_report_t;

/* What a paging caller (the mail panel) wants with a page of email_service_recent
 * beyond the rows; pass NULL for none. */
typedef struct {
   bool want_inbox_unread; /* in: the INBOX's unread count (inbox, first page only) */
   time_t at_or_before;    /* in, Gmail: only rows dated at or before this (0 = no bound) */
   int inbox_unread;       /* out: -1 when unknown or not asked */
   uint32_t uidvalidity;   /* out, IMAP: the mailbox epoch seen (0 = not seen) */
   char imap_folder[128];  /* out, IMAP: the folder listed ("" = not reached) */
   int rows_missing;       /* out, Gmail: listed rows left out (Gmail kept refusing them) */
} email_page_ext_t;


typedef struct {
   char draft_id[16]; /* hex-encoded randombytes_buf() */
   int user_id;
   char from_account[128]; /* canonical sending-account name, resolved at draft time */
   char to_address[256];
   char to_name[64];
   char subject[256];
   char body[4096];
   turn_origin_t origin; /* where it was made (turn_origin_check) */
   time_t created_at;    /* pending_slots_now(), not wall time */
   bool used;
} email_draft_t;

typedef struct {
   char pending_id[16]; /* hex-encoded randombytes_buf() */
   int user_id;
   char message_id[192];   /* Gmail hex ID or IMAP folder:uid composite */
   char account_name[128]; /* Resolved account name for confirm step */
   char subject[256];      /* For confirmation display */
   char from[128];         /* For confirmation display */
   turn_origin_t origin;   /* where it was made (turn_origin_check) */
   time_t created_at;      /* pending_slots_now(), not wall time */
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
#define EMAIL_RC_TIMEOUT 14 /* op timed out (large mailbox / slow server); hint to bound it */
#define EMAIL_RC_INVALID_PAGE_TOKEN 15 /* page_token malformed, from another account, or stale */
#define EMAIL_RC_NO_TRASH 16 /* the account has no Trash folder: nothing was moved or deleted */
#define EMAIL_RC_FOLDER_MISSING 17 /* the account has no Archive folder: nothing was moved */
#define EMAIL_RC_ALREADY_THERE 18  /* trash/archive: the message is already in that folder */
/* trash/archive: copied and marked \Deleted, left in its folder (the server has
 * neither MOVE nor UIDPLUS, so removing it would purge other mail too) */
#define EMAIL_RC_LEFT_FLAGGED 19
/* trash/archive: copied, but removing the original failed (a transient error;
 * the original is still in its folder, and a retry may copy it again) */
#define EMAIL_RC_NOT_REMOVED 20
/* send / trash: every slot holds another session's live item (none is pushed out) */
#define EMAIL_RC_PENDING_FULL 22

/* Two-step action codes (compose/send + trash prep/confirm; archive shares the
 * account-resolution set).  0/1 reuse EMAIL_RC_OK / EMAIL_RC_FAILURE above, and
 * account-not-found / no-accounts are forwarded verbatim as the shared
 * EMAIL_RC_UNKNOWN_ACCOUNT (10) / EMAIL_RC_NO_ACCOUNTS (11) — so the only codes
 * defined here are the genuinely per-function outcomes, and each has a DISJOINT
 * value (2-7): no literal ever means two different things. */
/* account read-only (create_draft / create_pending_trash / archive) */
#define EMAIL_ACCT_RC_READONLY 2

#define EMAIL_CONFIRM_RC_NOT_FOUND 3     /* draft/pending not found or expired */
#define EMAIL_CONFIRM_RC_THROTTLED 4     /* too many failed confirmations */
#define EMAIL_CONFIRM_RC_ACCOUNT_GONE 5  /* account gone/read-only at confirm time */
#define EMAIL_CONFIRM_RC_OTHER_SESSION 8 /* confirmed from another session than the draft's */
#define EMAIL_CONFIRM_RC_SAME_TURN 9     /* confirmed in the turn that prepared it */
#define EMAIL_CONFIRM_RC_NOT_NEXT 21     /* confirmed later than the turn right after it */
#define EMAIL_CONFIRM_RC_FROM_VISUAL 23  /* confirmed in a turn a rendered visual started */

/** A refused turn_origin_check as this module's confirm code. */
static inline int email_confirm_rc(turn_origin_rc_t rc) {
   switch (rc) {
      case TURN_ORIGIN_OK:
         return EMAIL_RC_OK;
      case TURN_ORIGIN_SAME_TURN:
         return EMAIL_CONFIRM_RC_SAME_TURN;
      case TURN_ORIGIN_NOT_NEXT:
         return EMAIL_CONFIRM_RC_NOT_NEXT;
      case TURN_ORIGIN_FROM_VISUAL:
         return EMAIL_CONFIRM_RC_FROM_VISUAL;
      default:
         return EMAIL_CONFIRM_RC_OTHER_SESSION;
   }
}

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

/**
 * @brief Test @p account_id's IMAP and SMTP (one API call for Gmail)
 *
 * Doesn't wait for an IMAP account in use by another caller: the IMAP half
 * reports EMAIL_ERR_BUSY instead (the SMTP half is still tested).  Works on a
 * disabled account (account settings do).
 *
 * @param user_id The account's owner (checked)
 * @param target  The same account (see email_target_t), or NULL
 * @param err     Why it failed (may be NULL)
 * @return EMAIL_RC_OK if both halves worked, EMAIL_RC_UNKNOWN_ACCOUNT, or EMAIL_RC_FAILURE
 */
int email_service_test_connection(int user_id,
                                  int64_t account_id,
                                  const email_target_t *target,
                                  bool *imap_ok,
                                  bool *smtp_ok,
                                  email_err_t *err);
int email_service_list_accounts(int user_id, email_account_t *out, int max);

/** Whether @p acct's operations take its lease (IMAP: one connection per account). */
bool email_service_account_uses_lease(const email_account_t *acct);

/**
 * @brief The user's account @p account_id
 * @param enabled_only Refuse a disabled account (operations do; account settings don't)
 * @return EMAIL_RC_OK, EMAIL_RC_UNKNOWN_ACCOUNT when it isn't the user's (or is disabled),
 *         or EMAIL_RC_FAILURE when the database couldn't answer (try again)
 */
int email_service_find_account_by_id(int user_id,
                                     int64_t account_id,
                                     bool enabled_only,
                                     email_account_t *out);

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

/**
 * @brief Newest-first messages from one account's folder.
 * @param count            Rows wanted; <= 0 uses the account's max_recent setting.
 * @param page_token       Continuation cursor from a previous call's @p next_page_token
 *                         (NULL/empty = first page).  Opaque and backend-specific: a Gmail
 *                         token on an IMAP account (or a stale IMAP cursor) returns
 *                         EMAIL_RC_INVALID_PAGE_TOKEN.
 * @param next_page_token  Filled when more (older) messages remain; empty otherwise.
 * @param ext              Optional paging extras (email_page_ext_t), or NULL
 * @param target             The account by id, or NULL to resolve by name (see email_target_t)
 * @param err              Why it failed (may be NULL)
 * @note @p page_token and @p next_page_token may be the same buffer (the input is
 *       copied before the output is cleared).
 */
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
                         size_t npt_len,
                         email_page_ext_t *ext,
                         const email_target_t *target,
                         email_err_t *err);

/**
 * @brief Read a message (email_service_read.c)
 *
 * With no account named, each enabled account is tried until one has it.
 * opts->max_text_chars <= 0 takes the account's own body cap.
 *
 * @param target The account by id, or NULL to resolve by name (see email_target_t)
 * @param err  Why it failed (may be NULL)
 * @return EMAIL_RC_OK, EMAIL_RC_NOT_FOUND, EMAIL_RC_NO_ACCOUNTS, an account-lookup
 *         code, or EMAIL_RC_FAILURE; free a success with email_message_free()
 */
int email_service_read(int user_id,
                       const char *account_name,
                       const char *message_id,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       const email_target_t *target,
                       email_err_t *err);

/* Most messages one email_service_set_flags call changes, and the most folders
 * they may span (an IMAP login each). */
#define EMAIL_FLAGS_MAX_IDS 50
#define EMAIL_FLAGS_MAX_FOLDERS 8

/* One message's outcome in email_service_set_flags. */
typedef struct {
   bool updated;    /* the message is there in the state asked for */
   email_err_t err; /* why not: NOT_FOUND (no such message), FAILED (past the folder cap), ... */
} email_flag_result_t;

/**
 * @brief Mark messages read or unread (email_service_flags.c)
 *
 * Only the read state is ever changed (\Seen, Gmail's UNREAD), so a read-only
 * account allows it, as reading a message would.  IMAP ids are parsed into
 * folder and UID before anything is sent, one login per folder.
 *
 * @param target  The account by id (required)
 * @param n       At most EMAIL_FLAGS_MAX_IDS
 * @param results Per id, in order
 * @return EMAIL_RC_OK (see @p results), an account-lookup code, or
 *         EMAIL_RC_FAILURE with @p err set (nothing could be asked)
 */
int email_service_set_flags(int user_id,
                            const email_target_t *target,
                            const char *const *message_ids,
                            int n,
                            bool unread,
                            email_flag_result_t *results,
                            email_err_t *err);

/**
 * @brief The INBOX's unread count (email_service_flags.c)
 * @param target The account by id (required)
 * @return EMAIL_RC_OK with @p inbox_unread set, an account-lookup code, or
 *         EMAIL_RC_FAILURE with @p err set
 */
int email_service_unread_count(int user_id,
                               const email_target_t *target,
                               int *inbox_unread,
                               email_err_t *err);

/**
 * @brief Search email across one or (when account_name is NULL) all enabled accounts.
 * @param warn_out  Optional out (may be NULL): on a multi-account search where some
 *                  accounts failed but others succeeded, this is filled with a
 *                  human-readable note naming the unreachable accounts (auth failures
 *                  flagged distinctly) so the caller can tell the user results are
 *                  partial instead of the failure being silent. Empty when all
 *                  searched accounts were reached.
 * @param target   One account by id (a paged single-account search), or NULL
 *                 (see email_target_t)
 * @param report   Optional out (may be NULL): why it failed, and which accounts a
 *                 multi-account search couldn't reach
 * @note Paging (params->page_token / next_page_token) applies to a single-account
 *       search only; a multi-account search searches every account at once,
 *       returns the newest @p max rows across them (each account's newest), and
 *       reports an account in use elsewhere for EMAIL_LEASE_FANOUT_WAIT_SEC as
 *       EMAIL_ERR_BUSY in @p report rather than waiting it out.  On an
 *       IMAP account a query with non-ASCII text fails as EMAIL_ERR_UNSUPPORTED_QUERY
 *       (the server's quoted strings are ASCII).
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
                         size_t warn_len,
                         const email_target_t *target,
                         email_search_report_t *report);

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
 *         from account resolution (no name match / no configured accounts);
 *         EMAIL_RC_FAILURE too when @p origin is NULL or has no turn (only a running
 *         turn the user started prepares a draft)
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
                               size_t from_account_len,
                               const turn_origin_t *origin);

/**
 * @brief Confirm and send a draft.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on send failure
 *         (network/upstream), EMAIL_CONFIRM_RC_NOT_FOUND if the draft is not
 *         found/expired, EMAIL_CONFIRM_RC_THROTTLED if throttled,
 *         EMAIL_CONFIRM_RC_ACCOUNT_GONE if the draft's bound sending account is
 *         no longer available or writable (deleted/disabled/read-only since the
 *         draft was prepared — prepare a new draft), EMAIL_CONFIRM_RC_OTHER_SESSION
 *         / _SAME_TURN / _NOT_NEXT when @p origin isn't the turn right after the
 *         draft's, in its session (turn_origin_check; the draft stays)
 */
int email_service_confirm_send(int user_id, const char *draft_id, const turn_origin_t *origin);

/**
 * @brief What confirming a draft sends, in one line for a text asking the
 *        user to approve it: the recipient's address, the account, the
 *        subject and the start of the body; @p valid_for_sec receives how
 *        long the draft has left (may be NULL)
 * @param session_id The session asking: only a draft made in it is described
 *                   (its confirm is refused anywhere else)
 * @return EMAIL_RC_OK, or EMAIL_RC_FAILURE when the session has no such draft
 */
int email_service_describe_draft(int user_id,
                                 uint32_t session_id,
                                 const char *draft_id,
                                 char *out,
                                 size_t out_len,
                                 int *valid_for_sec);

/**
 * @brief What confirming a pending trash moves, in one line (only one made
 *        in @p session_id)
 * @return EMAIL_RC_OK, or EMAIL_RC_FAILURE when the session has no such pending trash
 */
int email_service_describe_pending_trash(int user_id,
                                         uint32_t session_id,
                                         const char *pending_id,
                                         char *out,
                                         size_t out_len,
                                         int *valid_for_sec);

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
 * @param target        The account by id, or NULL to resolve by name (see email_target_t)
 * @param err           Why it failed (may be NULL)
 * @return 0 on success, 1 on failure
 */
int email_service_list_folders(int user_id,
                               const char *account_name,
                               char *out,
                               size_t out_len,
                               const email_target_t *target,
                               email_err_t *err);

/**
 * @brief Create a pending trash action for two-step delete.
 * Fetches message metadata for confirmation display.
 * @param pending_id_out  Output: hex pending ID string
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure (also for a NULL
 *         or turn-less @p origin), EMAIL_ACCT_RC_READONLY if the account is read-only
 */
int email_service_create_pending_trash(int user_id,
                                       const char *account_name,
                                       const char *message_id,
                                       char *pending_id_out,
                                       size_t pending_id_len,
                                       char *subject_out,
                                       size_t subject_len,
                                       char *from_out,
                                       size_t from_len,
                                       const turn_origin_t *origin);

/**
 * @brief Confirm and execute a pending trash action.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure,
 *         EMAIL_RC_NOT_FOUND (the message is gone), EMAIL_RC_NO_TRASH,
 *         EMAIL_RC_ALREADY_THERE, EMAIL_RC_LEFT_FLAGGED, EMAIL_RC_NOT_REMOVED
 *         (IMAP; see email_imap_move_batch),
 *         EMAIL_CONFIRM_RC_NOT_FOUND if not found/expired,
 *         EMAIL_CONFIRM_RC_THROTTLED if throttled,
 *         EMAIL_CONFIRM_RC_ACCOUNT_GONE if the account is no longer available or
 *         writable since the pending action was prepared, EMAIL_CONFIRM_RC_OTHER_SESSION
 *         / _SAME_TURN / _NOT_NEXT as for email_service_confirm_send
 * @param err Why the move failed (may be NULL)
 */
int email_service_confirm_trash(int user_id,
                                const char *pending_id,
                                const turn_origin_t *origin,
                                email_err_t *err);

/**
 * @brief Archive a message: move it to the account's archive folder (Gmail
 * API: remove the INBOX label).  Single-step operation — no confirmation needed.
 * @return EMAIL_RC_OK on success, EMAIL_RC_FAILURE on failure,
 *         EMAIL_ACCT_RC_READONLY if the account is read-only, EMAIL_RC_NOT_FOUND,
 *         EMAIL_RC_FOLDER_MISSING, EMAIL_RC_ALREADY_THERE, EMAIL_RC_LEFT_FLAGGED,
 *         EMAIL_RC_NOT_REMOVED (IMAP; see email_imap_move_batch)
 * @param target The account by id, or NULL to resolve by name (see email_target_t)
 * @param err  Why it failed (may be NULL)
 */
int email_service_archive(int user_id,
                          const char *account_name,
                          const char *message_id,
                          const email_target_t *target,
                          email_err_t *err);

/* =============================================================================
 * Trash, archive and undo, several messages at a time (email_service_move.c)
 * ============================================================================= */

/** Most messages one move or undo takes. */
#define EMAIL_MOVE_MAX_IDS 50

/** One message's share of email_service_move. */
typedef struct {
   email_move_outcome_t outcome;
   email_err_t err;                     /* EMAIL_ERR_NONE unless outcome is EMAIL_MOVE_FAILED */
   char message_id[192];                /* the id as the server knows it now (IMAP: pinned) */
   char undo[EMAIL_UNDO_TOKEN_LEN + 1]; /* "" = can't be undone */
} email_move_result_t;

/**
 * @brief Move messages of one account to its Trash or Archive
 *
 * IMAP: one lease and one login; per (folder, epoch) group a check that the
 * messages exist, then the move (email_imap_move_batch).  Gmail: per message, its
 * labels read first (already in Trash, or not in the inbox for an archive, is
 * EMAIL_MOVE_ALREADY_THERE; an archive of a message in Trash or Spam is
 * EMAIL_ERR_IN_TRASH), then the move, paced per account; an archive without
 * @p want_undo skips the read.  With
 * @p want_undo a moved message gets an undo token (none when the server can't
 * say where it went, or the token store is full).  Tells the WebUI what left its
 * folder (email_changed_notify), whatever else happened.
 *
 * @param target The account (see email_target_t); its lease is taken unless held
 * @param ids    At most EMAIL_MOVE_MAX_IDS
 * @param results One per id, in order.  Callers heap-allocate a full batch
 *                (EMAIL_MOVE_MAX_IDS results are ~12 KB)
 * @param err    Out: what ended the whole call, EMAIL_ERR_NONE when it ran
 * @return EMAIL_RC_OK (see @p results), EMAIL_ACCT_RC_READONLY, an account-lookup
 *         code, or EMAIL_RC_FAILURE (@p err says why; @p results carry it too)
 */
int email_service_move(int user_id,
                       const email_target_t *target,
                       const char *const *ids,
                       int n,
                       email_move_kind_t kind,
                       bool want_undo,
                       email_move_result_t *results,
                       email_err_t *err);

/**
 * @brief Whether the account has a Trash and an Archive to move to, once known
 *        (learned on a worker: a move, the panel's first inbox page).  A Gmail
 *        API account (@p gmail_api) has both.  Reads no credentials and does no
 *        I/O: safe on any thread.
 * @return false when not yet known (both outs false)
 */
bool email_service_account_caps(int64_t account_id,
                                bool gmail_api,
                                bool *can_trash,
                                bool *can_archive);

/** Forget what email_service_account_caps knows of an account (its server or
 *  login changed, or it's gone). */
void email_service_account_caps_forget(int64_t account_id);

/** One undo record's result in email_service_undo. */
typedef struct {
   email_err_t err;     /* EMAIL_ERR_NONE when it's back */
   bool retry;          /* it couldn't run this time: release its token, don't finish it */
   bool row_ok;         /* row holds the message as it is back */
   email_summary_t row; /* its id pinned to where it is now */
} email_undo_result_t;

/**
 * @brief Move messages back where a move took them (records claimed with
 *        email_undo_claim)
 *
 * Under the account's lease the account is resolved again: one no longer the
 * user's, enabled, writable, or with another server or login is
 * EMAIL_ERR_UNDO_EXPIRED.  A message no longer where the move put it (Trash
 * emptied, moved on) is EMAIL_ERR_NOT_FOUND; a folder it came from that is gone,
 * EMAIL_ERR_FOLDER_MISSING.  Tells the WebUI the restored rows and the ids that
 * left Trash or Archive (email_changed_notify).
 *
 * @param n       At most EMAIL_MOVE_MAX_IDS
 * @param results results[i] is recs[i]'s: finish its token (email_undo_finish)
 *                unless retry is set, then release it (email_undo_release).  On
 *                an early EMAIL_RC_FAILURE with every result's retry set (a
 *                transient lookup, the lease busy) release them all.  Callers
 *                heap-allocate a full batch (EMAIL_MOVE_MAX_IDS results are
 *                ~95 KB, as many records ~55 KB): too big for a tool thread's stack
 * @return EMAIL_RC_OK (see @p results), or EMAIL_RC_FAILURE (@p err says why;
 *         EMAIL_ERR_UNDO_EXPIRED when the account changed under every record)
 */
int email_service_undo(int user_id,
                       const email_target_t *target,
                       const email_undo_rec_t *recs,
                       int n,
                       email_undo_result_t *results,
                       email_err_t *err);

/**
 * @brief What a move or undo changed on @p account_id, for the user's open mail
 *        panels: rows that appeared, ids that left their folder, and whether
 *        the panel should reload (a row couldn't be read, or a message's move
 *        may have happened without its answer)
 *
 * @p gmail_api says which backend the ids are from (a Gmail row carries flags).
 * @p kind and @p undo say what moved, because a Gmail id is the same in every
 * view.  A move's @p destroyed ids left INBOX (archive) or everything but
 * Trash (trash); an IMAP id names its folder, so it left just that one.  An
 * undo's @p created rows come back (a Gmail row replaces the one with its id,
 * which leaves the Trash view for a trash undo); its @p destroyed ids are the
 * IMAP copies that left Trash or Archive.
 *
 * Called with no mutex held (the account's lease may be).  A no-op unless the
 * WebUI overrides it; the override only queues.
 */
void email_changed_notify(int user_id,
                          int64_t account_id,
                          bool gmail_api,
                          email_move_kind_t kind,
                          bool undo,
                          const email_summary_t *created,
                          int nc,
                          const char *const *destroyed,
                          int nd,
                          bool refresh);

#endif /* EMAIL_SERVICE_H */
