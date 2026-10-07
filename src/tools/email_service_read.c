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
 * Reading a message for the email service: the account fan-out, the
 * per-account read options, and why a read failed.
 */

#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_parse.h"
#include "tools/email_service.h"
#include "tools/email_service_internal.h"
#include "tools/gmail_client.h"
#include "tools/oauth_client.h"

/* The options a read on @p acct runs with: the caller's, with the account's
 * own text cap when the caller left it to the account. */
static email_read_opts_t opts_for(const email_account_t *acct, const email_read_opts_t *opts) {
   email_read_opts_t o = *opts;
   if (o.max_text_chars <= 0)
      o.max_text_chars = acct->max_body_chars > 0 ? acct->max_body_chars : EMAIL_MAX_READ_BODY_LEN;
   if (o.fetch_bytes == 0)
      o.fetch_bytes = EMAIL_READ_FETCH_TOOL;
   return o;
}

static int read_gmail(const email_account_t *acct,
                      const char *message_id,
                      const email_read_opts_t *opts,
                      email_message_t *out,
                      email_err_t *err) {
   char token[OAUTH_TOKEN_BUF_SIZE];
   bool revoked = false;
   if (email_svc_gmail_token(acct, token, sizeof(token), &revoked) != 0) {
      sodium_memzero(token, sizeof(token));
      *err = revoked ? EMAIL_ERR_AUTH_REVOKED : EMAIL_ERR_AUTH_FAILED;
      return EMAIL_RC_FAILURE;
   }
   const int rc = gmail_read_message(token, message_id, opts, out, err);
   sodium_memzero(token, sizeof(token));
   if (rc == 0)
      return EMAIL_RC_OK;
   return *err == EMAIL_ERR_NOT_FOUND ? EMAIL_RC_NOT_FOUND : EMAIL_RC_FAILURE;
}

bool email_svc_parse_imap_id(const char *message_id,
                             char *folder,
                             size_t folder_size,
                             uint32_t *uid,
                             bool fanout) {
   if (!email_imap_id_parse(message_id, folder, folder_size, uid)) {
      /* During a no-account fan-out this is a Gmail id landing on an IMAP
       * account (backend mismatch): expected.  Otherwise a real error. */
      if (fanout)
         OLOG_DEBUG("email: message id '%s' is not an IMAP id (skipping IMAP account)", message_id);
      else
         OLOG_ERROR("email: invalid IMAP message id '%s'", message_id);
      return false;
   }
   if (!email_service_validate_folder_name(folder)) {
      OLOG_ERROR("email: invalid folder name in message_id '%s'", message_id);
      return false;
   }
   return true;
}

int email_svc_read_single(const email_account_t *acct,
                          const char *message_id,
                          const email_read_opts_t *opts,
                          email_message_t *out,
                          email_err_t *err,
                          bool fanout) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   const email_read_opts_t o = opts_for(acct, opts);

   if (email_svc_is_gmail_api(acct)) {
      /* An IMAP "folder:uid" id probed against a Gmail account during the
       * fan-out simply isn't there. */
      if (fanout && !gmail_message_id_valid(message_id)) {
         *err = EMAIL_ERR_NOT_FOUND;
         return EMAIL_RC_NOT_FOUND;
      }
      return read_gmail(acct, message_id, &o, out, err);
   }

   char folder[128];
   uint32_t uid = 0;
   if (!email_svc_parse_imap_id(message_id, folder, sizeof(folder), &uid, fanout)) {
      /* Not this backend's id shape: the message isn't in this account. */
      *err = fanout ? EMAIL_ERR_NOT_FOUND : EMAIL_ERR_FAILED;
      return fanout ? EMAIL_RC_NOT_FOUND : EMAIL_RC_FAILURE;
   }
   email_conn_t conn;
   const int crc = email_svc_build_conn(acct, &conn);
   if (crc != EMAIL_SVC_CONN_OK) {
      sodium_memzero(&conn, sizeof(conn));
      *err = email_svc_conn_err(crc);
      return EMAIL_RC_FAILURE;
   }
   const int rc = email_read_message(&conn, folder, uid, &o, out, err);
   sodium_memzero(&conn, sizeof(conn));
   if (rc != 0)
      return *err == EMAIL_ERR_NOT_FOUND ? EMAIL_RC_NOT_FOUND : EMAIL_RC_FAILURE;
   snprintf(out->message_id, sizeof(out->message_id), "%s", message_id);
   return EMAIL_RC_OK;
}

/* One account's read under its lease (taken here unless @p target). */
static int read_leased(const email_account_t *acct,
                       const char *message_id,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       const email_target_t *target,
                       email_err_t *err,
                       bool fanout) {
   /* A fan-out probing an IMAP account with a Gmail id: not here, and no reason
    * to wait for the account's lease to find that out. */
   if (fanout && !email_svc_is_gmail_api(acct)) {
      char folder[128];
      uint32_t uid = 0;
      if (!email_svc_parse_imap_id(message_id, folder, sizeof(folder), &uid, true)) {
         *err = EMAIL_ERR_NOT_FOUND;
         return EMAIL_RC_NOT_FOUND;
      }
   }
   /* A fan-out reports a busy account rather than wait it out: the accounts
    * after it shouldn't wait behind it one after another. */
   email_svc_lease_t lease;
   const int wait_s = fanout ? EMAIL_LEASE_FANOUT_WAIT_SEC : EMAIL_LEASE_WAIT_SEC;
   if (email_svc_lease_begin(acct, target, wait_s, &lease, err) != EMAIL_RC_OK)
      return EMAIL_RC_FAILURE;
   const int rc = email_svc_read_single(acct, message_id, opts, out, err, fanout);
   email_svc_lease_end(&lease);
   return rc;
}

int email_service_read(int user_id,
                       const char *account_name,
                       const char *message_id,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       const email_target_t *target,
                       email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!message_id || !message_id[0] || !opts)
      return EMAIL_RC_FAILURE;

   if (target || (account_name && account_name[0])) {
      email_account_t acct;
      const int find_rc = email_svc_resolve(user_id, account_name, target, &acct);
      if (find_rc != EMAIL_RC_OK) {
         *err = email_svc_account_err(find_rc);
         return find_rc;
      }
      const int rc = read_leased(&acct, message_id, opts, out, target, err, false);
      sodium_memzero(&acct, sizeof(acct));
      return rc;
   }

   /* No account named: try each enabled one until the message turns up.  A
    * failed attempt leaves no heap in out, so the next starts clean. */
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int acct_count = 0;
   email_db_account_list(user_id, accounts, EMAIL_MAX_ACCOUNTS, &acct_count);
   if (acct_count <= 0) {
      *err = EMAIL_ERR_NO_ACCOUNT;
      return EMAIL_RC_NO_ACCOUNTS;
   }
   bool enabled_seen = false;
   email_err_t first_real = EMAIL_ERR_NONE; /* the first failure that wasn't "not here" */
   for (int i = 0; i < acct_count; i++) {
      if (!accounts[i].enabled)
         continue;
      enabled_seen = true;
      email_err_t e = EMAIL_ERR_NONE;
      const int rc = read_leased(&accounts[i], message_id, opts, out, NULL, &e, true);
      if (rc == EMAIL_RC_OK) {
         sodium_memzero(accounts, sizeof(accounts));
         *err = EMAIL_ERR_NONE;
         return EMAIL_RC_OK;
      }
      if (e == EMAIL_ERR_CANCELLED) {
         sodium_memzero(accounts, sizeof(accounts));
         *err = e;
         return EMAIL_RC_FAILURE;
      }
      if (rc != EMAIL_RC_NOT_FOUND && first_real == EMAIL_ERR_NONE)
         first_real = e;
   }
   sodium_memzero(accounts, sizeof(accounts));
   /* "All disabled" (enable one) vs "in no mailbox" (a stale id) vs a real failure. */
   if (!enabled_seen) {
      *err = EMAIL_ERR_NO_ACCOUNT;
      return EMAIL_RC_NO_ACCOUNTS;
   }
   if (first_real == EMAIL_ERR_NONE) {
      *err = EMAIL_ERR_NOT_FOUND;
      return EMAIL_RC_NOT_FOUND;
   }
   *err = first_real;
   return EMAIL_RC_FAILURE;
}
