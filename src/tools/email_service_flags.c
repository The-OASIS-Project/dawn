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
 * Email read state across backends: marking messages read or unread, and the
 * INBOX's unread count.  Both take an account by id (the WebUI mail panel).
 */

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_service.h"
#include "tools/email_service_internal.h"
#include "tools/gmail_client.h"
#include "tools/oauth_client.h"

_Static_assert(EMAIL_FLAGS_MAX_IDS <= GMAIL_FLAGS_MAX_IDS,
               "a set_flags call must fit one Gmail batchModify");

/* The account a panel call names, and a Gmail token when it's read through the API. */
static int resolve_target(int user_id,
                          const email_target_t *target,
                          email_account_t *acct,
                          email_err_t *err) {
   if (!target) {
      *err = EMAIL_ERR_ACCOUNT_NOT_FOUND;
      return EMAIL_RC_UNKNOWN_ACCOUNT;
   }
   const int rc = email_svc_resolve(user_id, NULL, target, acct);
   if (rc != EMAIL_RC_OK)
      *err = email_svc_account_err(rc);
   return rc;
}

static void set_flags_imap(const email_account_t *acct,
                           const char *const *ids,
                           int n,
                           bool unread,
                           email_flag_result_t *results) {
   email_imap_group_t *groups = calloc(EMAIL_IMAP_GROUP_MAX_FOLDERS, sizeof(*groups));
   if (!groups)
      return; /* every result stays FAILED */
   int at_group[EMAIL_IMAP_GROUP_MAX_IDS], at_pos[EMAIL_IMAP_GROUP_MAX_IDS];
   email_err_t errs[EMAIL_IMAP_GROUP_MAX_IDS];
   for (int i = 0; i < n; i++)
      errs[i] = EMAIL_ERR_FAILED;
   const int ngroups = email_svc_group_imap_ids(ids, n, groups, at_group, at_pos, errs);

   email_conn_t conn;
   const int crc = email_svc_build_conn(acct, &conn);
   email_imap_seen_batch_t batches[EMAIL_IMAP_GROUP_MAX_FOLDERS];
   bool updated[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_IMAP_GROUP_MAX_IDS];
   const email_err_t conn_err = crc == EMAIL_SVC_CONN_OK ? EMAIL_ERR_NONE : email_svc_conn_err(crc);
   if (crc == EMAIL_SVC_CONN_OK && ngroups > 0) {
      for (int g = 0; g < ngroups; g++) {
         batches[g] = (email_imap_seen_batch_t){ .folder = groups[g].folder,
                                                 .uidvalidity = groups[g].uidvalidity,
                                                 .uids = groups[g].uids,
                                                 .n = groups[g].count,
                                                 .updated = updated[g] };
      }
      email_imap_set_seen(&conn, batches, ngroups, !unread);
   }
   for (int i = 0; i < n; i++) {
      email_flag_result_t *r = &results[i];
      const int g = at_group[i];
      if (g < 0) {
         r->err = errs[i];
         continue;
      }
      if (conn_err != EMAIL_ERR_NONE) {
         r->err = conn_err;
         continue;
      }
      const bool ok = batches[g].err == EMAIL_ERR_NONE;
      r->updated = ok && updated[g][at_pos[i]];
      r->err = r->updated ? EMAIL_ERR_NONE : ok ? EMAIL_ERR_NOT_FOUND : batches[g].err;
   }
   sodium_memzero(&conn, sizeof(conn));
   free(groups);
}

int email_service_set_flags(int user_id,
                            const email_target_t *target,
                            const char *const *message_ids,
                            int n,
                            bool unread,
                            email_flag_result_t *results,
                            email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!message_ids || !results || n <= 0 || n > EMAIL_FLAGS_MAX_IDS)
      return EMAIL_RC_FAILURE;
   for (int i = 0; i < n; i++) {
      results[i].updated = false;
      results[i].err = EMAIL_ERR_FAILED;
   }

   email_account_t acct;
   int rc = resolve_target(user_id, target, &acct, err);
   if (rc != EMAIL_RC_OK)
      return rc;

   /* No read-only check: the read state is all this changes, as a read would. */
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token_err(&acct, token, sizeof(token), err) != 0) {
         sodium_memzero(token, sizeof(token));
         sodium_memzero(&acct, sizeof(acct));
         return EMAIL_RC_FAILURE;
      }
      bool updated[EMAIL_FLAGS_MAX_IDS];
      email_err_t errs[EMAIL_FLAGS_MAX_IDS];
      gmail_set_unread(token, message_ids, n, unread, updated, errs);
      sodium_memzero(token, sizeof(token));
      for (int i = 0; i < n; i++) {
         results[i].updated = updated[i];
         results[i].err = errs[i];
      }
   } else {
      email_svc_lease_t lease;
      if (email_svc_lease_begin(&acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK) {
         sodium_memzero(&acct, sizeof(acct));
         return EMAIL_RC_FAILURE;
      }
      set_flags_imap(&acct, message_ids, n, unread, results);
      email_svc_lease_end(&lease);
   }
   sodium_memzero(&acct, sizeof(acct));
   *err = EMAIL_ERR_NONE;
   return EMAIL_RC_OK;
}

int email_service_unread_count(int user_id,
                               const email_target_t *target,
                               int *inbox_unread,
                               email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!inbox_unread)
      return EMAIL_RC_FAILURE;
   *inbox_unread = -1;

   email_account_t acct;
   int rc = resolve_target(user_id, target, &acct, err);
   if (rc != EMAIL_RC_OK)
      return rc;

   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      rc = email_svc_gmail_token_err(&acct, token, sizeof(token), err) == 0 &&
                   gmail_inbox_unread(token, inbox_unread, err) == 0
               ? EMAIL_RC_OK
               : EMAIL_RC_FAILURE;
      sodium_memzero(token, sizeof(token));
   } else {
      email_svc_lease_t lease;
      if (email_svc_lease_begin(&acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK) {
         sodium_memzero(&acct, sizeof(acct));
         return EMAIL_RC_FAILURE;
      }
      email_conn_t conn;
      const int crc = email_svc_build_conn(&acct, &conn);
      if (crc != EMAIL_SVC_CONN_OK) {
         *err = email_svc_conn_err(crc);
         rc = EMAIL_RC_FAILURE;
      } else {
         rc = email_imap_inbox_unseen(&conn, inbox_unread, err) == 0 ? EMAIL_RC_OK
                                                                     : EMAIL_RC_FAILURE;
      }
      sodium_memzero(&conn, sizeof(conn));
      email_svc_lease_end(&lease);
   }
   sodium_memzero(&acct, sizeof(acct));
   return rc;
}
