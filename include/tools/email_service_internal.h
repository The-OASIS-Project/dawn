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
 * the project author(s).
 *
 * The email service's own helpers, shared by its modules (email_service.c,
 * email_service_read.c).  Not for use outside the service.
 */

#ifndef EMAIL_SERVICE_INTERNAL_H
#define EMAIL_SERVICE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/email_client.h"
#include "tools/email_db.h"
#include "tools/email_imap_roles.h"
#include "tools/email_service.h"
#include "tools/email_types.h"

/* email_svc_build_conn() outcomes. */
#define EMAIL_SVC_CONN_OK 0      /* success */
#define EMAIL_SVC_CONN_FAILURE 1 /* config/TLS refusal or internal — not a credential problem */
#define EMAIL_SVC_CONN_AUTH 2    /* OAuth token fetch or password decrypt failed (credentials) */
#define EMAIL_SVC_CONN_REVOKED 3 /* the OAuth grant was revoked at the provider */

/** An IMAP/SMTP connection for @p acct (credentials in it: sodium_memzero after use). */
int email_svc_build_conn(const email_account_t *acct, email_conn_t *conn);

/** The user's account named @p account_name (case-insensitive), or the first enabled one. */
int email_svc_find_account(int user_id, const char *account_name, email_account_t *out);

/** The email_err_t for an account lookup's EMAIL_RC_* (NO_ACCOUNTS, UNKNOWN_ACCOUNT, ...). */
email_err_t email_svc_account_err(int rc);

/** The email_err_t for a failed email_svc_build_conn() (EMAIL_SVC_CONN_*). */
email_err_t email_svc_conn_err(int conn_rc);

/**
 * @brief The account an operation runs on: @p target's id (the user's, enabled),
 *        else the one named @p account_name (email_svc_find_account)
 */
int email_svc_resolve(int user_id,
                      const char *account_name,
                      const email_target_t *target,
                      email_account_t *out);

/* An IMAP account's lease as one operation took it (or found it held). */
typedef struct {
   int64_t account_id;
   bool taken; /* this operation took it, so it releases it */
} email_svc_lease_t;

/**
 * @brief Take @p acct's lease for one operation, unless the account needs none
 *        (Gmail API) or @p target says the caller holds it (@c lease_held)
 *
 * Waits behind other callers up to @p wait_s seconds (0: only if free now),
 * giving up early when the thread's transfer cancel flag is set.  Taken only by
 * the public email_service_* entry points, never by the email_svc_* helpers
 * they call.
 *
 * @return EMAIL_RC_OK (end it with email_svc_lease_end), or EMAIL_RC_FAILURE with
 *         @p err EMAIL_ERR_BUSY (timed out, or too many accounts in use),
 *         EMAIL_ERR_CANCELLED or EMAIL_ERR_FAILED
 */
int email_svc_lease_begin(const email_account_t *acct,
                          const email_target_t *target,
                          int wait_s,
                          email_svc_lease_t *lease,
                          email_err_t *err);

/** Release what email_svc_lease_begin took (no-op when it took nothing). */
void email_svc_lease_end(email_svc_lease_t *lease);

/** Whether @p acct is read through the Gmail API (OAuth on gmail.com). */
bool email_svc_is_gmail_api(const email_account_t *acct);

/**
 * @brief A Gmail API access token for @p acct (refreshed as needed)
 * @param revoked Set when the failure was a revoked grant (may be NULL)
 */
int email_svc_gmail_token(const email_account_t *acct, char *token, size_t len, bool *revoked);

/**
 * @brief email_svc_gmail_token, with a failure as its email_err_t: on failure
 *        @p token is wiped and @p err (if given) is AUTH_REVOKED or AUTH_FAILED
 * @return 0, or 1
 */
int email_svc_gmail_token_err(const email_account_t *acct,
                              char *token,
                              size_t len,
                              email_err_t *err);

/**
 * @brief An IMAP message id ("folder:uid[.uidvalidity]") split and checked: the
 *        folder fits and passes the allow-list, the uid is valid.  Logged when it
 *        isn't (at debug during a fan-out, where a Gmail id is expected).
 * @param uidvalidity The id's pin, 0 when it has none (may be NULL)
 * @return true when @p folder and @p uid are filled
 */
bool email_svc_parse_imap_id(const char *message_id,
                             char *folder,
                             size_t folder_size,
                             uint32_t *uid,
                             uint32_t *uidvalidity,
                             bool fanout);

/* The most ids, and groups, one grouped IMAP call takes (read, flags and moves
 * share the grouping): the panel's flags and move limits. */
#define EMAIL_IMAP_GROUP_MAX_IDS EMAIL_FLAGS_MAX_IDS
#define EMAIL_IMAP_GROUP_MAX_FOLDERS EMAIL_FLAGS_MAX_FOLDERS

/* IMAP ids grouped for one call: one group per (folder, uidvalidity), so one
 * pinned SELECT each; every UID once per group. */
typedef struct {
   char folder[128];
   uint32_t uidvalidity; /* 0: these ids carry no pin */
   uint32_t uids[EMAIL_IMAP_GROUP_MAX_IDS];
   int count;
} email_imap_group_t;

/**
 * @brief Group IMAP ids by (folder, uidvalidity), at most EMAIL_IMAP_GROUP_MAX_FOLDERS
 *        groups (email_service_read.c)
 *
 * An unpinned id joins its folder's group when that folder has exactly one;
 * otherwise it goes in its own unpinned group (a stale epoch's group would
 * report it missing).  An id that doesn't parse is EMAIL_ERR_NOT_FOUND; one past
 * the group cap EMAIL_ERR_FAILED.
 *
 * @param at_group Per id: its group, or -1 (then @p errs says why)
 * @param at_pos   Per id: its UID's position in that group (duplicates share one)
 * @return the number of groups
 */
int email_svc_group_imap_ids(const char *const *ids,
                             int n,
                             email_imap_group_t *groups,
                             int *at_group,
                             int *at_pos,
                             email_err_t *errs);

/**
 * @brief Read @p message_id from one account (email_service_read without the fan-out)
 * @param fanout true while probing every account: an id of the other backend's
 *               shape is then expected, not an error
 * @return EMAIL_RC_OK, EMAIL_RC_NOT_FOUND or EMAIL_RC_FAILURE; @p err (may be NULL) says why
 */
int email_svc_read_single(const email_account_t *acct,
                          const char *message_id,
                          const email_read_opts_t *opts,
                          email_message_t *out,
                          email_err_t *err,
                          bool fanout);

/* =============================================================================
 * Account capabilities (email_service_move.c)
 * ============================================================================= */

/**
 * @brief The account's capability generation, taken before learning its roles
 *
 * A note made under an older generation (the account was edited or removed in
 * between) is dropped.
 */
uint64_t email_svc_caps_gen(int64_t account_id);

/** Record what @p roles say the account can move to, learned under @p gen. */
void email_svc_caps_from_roles(int64_t account_id, uint64_t gen, const email_imap_roles_t *roles);

/**
 * @brief Whether the list path may learn the account's roles now (not learned or
 *        tried within the hour); true also marks it tried
 */
bool email_svc_caps_probe_due(int64_t account_id);

/** The list path's probe failed: let the next page try again. */
void email_svc_caps_probe_failed(int64_t account_id);

#endif /* EMAIL_SERVICE_INTERNAL_H */
