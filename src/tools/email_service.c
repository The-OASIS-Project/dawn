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

#define _GNU_SOURCE /* strcasestr */

#include "tools/email_service.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <netdb.h>
#include <pthread.h>
#include <sodium.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/crypto_store.h"
#include "core/pending_slots.h" /* pending_slots_now: expiry on a clock that never steps back */
#include "logging.h"
#include "tools/calendar_db.h"
#include "tools/email_account_lease.h"
#include "tools/email_client.h"
#include "tools/email_db.h"
#include "tools/email_parse.h"
#include "tools/email_service_internal.h"
#include "tools/email_transfer.h"
#include "tools/gmail_client.h"
#include "tools/oauth_client.h"
#include "utils/string_utils.h"

/* Characters of a sender shown in a trash confirmation. */
#define EMAIL_TRASH_FROM_SHOWN 100

/* =============================================================================
 * Module State
 * ============================================================================= */

static struct {
   bool initialized;
   pthread_mutex_t draft_mutex;
   email_draft_t drafts[EMAIL_MAX_DRAFTS];

   pthread_mutex_t pending_trash_mutex;
   email_pending_trash_t pending_trash[EMAIL_MAX_PENDING_TRASH];

   /* Per-user confirm throttling (shared by confirm_send and confirm_trash) */
   struct {
      int user_id;
      int fail_count;
      time_t first_fail;
   } throttle[8];
   int throttle_count;
} s_email;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int email_service_init(void) {
   if (s_email.initialized)
      return 0;

   pthread_mutex_init(&s_email.draft_mutex, NULL);
   pthread_mutex_init(&s_email.pending_trash_mutex, NULL);
   memset(s_email.drafts, 0, sizeof(s_email.drafts));
   memset(s_email.pending_trash, 0, sizeof(s_email.pending_trash));
   s_email.initialized = true;

   OLOG_INFO("email_service: initialized");
   return 0;
}

void email_service_shutdown(void) {
   if (!s_email.initialized)
      return;

   /* Wipe all drafts */
   pthread_mutex_lock(&s_email.draft_mutex);
   for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
      sodium_memzero(&s_email.drafts[i], sizeof(email_draft_t));
   }
   pthread_mutex_unlock(&s_email.draft_mutex);

   /* Wipe pending trash */
   pthread_mutex_lock(&s_email.pending_trash_mutex);
   for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
      sodium_memzero(&s_email.pending_trash[i], sizeof(email_pending_trash_t));
   }
   pthread_mutex_unlock(&s_email.pending_trash_mutex);

   pthread_mutex_destroy(&s_email.draft_mutex);
   pthread_mutex_destroy(&s_email.pending_trash_mutex);
   s_email.initialized = false;
   OLOG_INFO("email_service: shutdown");
}

bool email_service_available(void) {
   return s_email.initialized;
}

/* =============================================================================
 * TLS Enforcement Helper
 *
 * Refuses plaintext connections to non-loopback servers.
 * ============================================================================= */

static bool is_loopback(const char *host) {
   if (!host)
      return false;
   if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0)
      return true;
   if (strcasecmp(host, "localhost") == 0)
      return true;
   return false;
}

/* =============================================================================
 * Connection Builder
 *
 * Builds email_conn_t from account, handling both app_password and OAuth.
 * TLS enforcement: refuse plaintext to non-loopback servers.
 * ============================================================================= */

/* email_svc_build_conn() outcomes (CONN_RC_*, email_service_internal.h).  Callers that
 * only need pass/fail keep
 * checking `!= EMAIL_SVC_CONN_OK`; the search path distinguishes EMAIL_SVC_CONN_AUTH
 * (a real credential problem worth telling the user about) from a config/TLS
 * refusal, so it doesn't mislabel a TLS misconfig as "login failed". */

int email_svc_build_conn(const email_account_t *acct, email_conn_t *conn) {
   memset(conn, 0, sizeof(*conn));

   /* Enforce TLS for non-loopback servers */
   if (!acct->imap_ssl && !is_loopback(acct->imap_server)) {
      OLOG_ERROR("email: refusing plaintext IMAP to non-loopback server %s", acct->imap_server);
      return EMAIL_SVC_CONN_FAILURE;
   }
   if (!acct->smtp_ssl && !is_loopback(acct->smtp_server)) {
      OLOG_ERROR("email: refusing plaintext SMTP to non-loopback server %s", acct->smtp_server);
      return EMAIL_SVC_CONN_FAILURE;
   }

   /* Build IMAP URL */
   const char *imap_scheme = acct->imap_ssl ? "imaps" : "imap";
   snprintf(conn->imap_url, sizeof(conn->imap_url), "%s://%s:%d", imap_scheme, acct->imap_server,
            acct->imap_port);

   /* Build SMTP URL */
   const char *smtp_scheme = acct->smtp_ssl ? "smtps" : "smtp";
   snprintf(conn->smtp_url, sizeof(conn->smtp_url), "%s://%s:%d", smtp_scheme, acct->smtp_server,
            acct->smtp_port);

   snprintf(conn->username, sizeof(conn->username), "%s", acct->username);
   snprintf(conn->display_name, sizeof(conn->display_name), "%s", acct->display_name);
   conn->max_body_chars = acct->max_body_chars > 0 ? acct->max_body_chars : EMAIL_MAX_READ_BODY_LEN;

   /* Auth dispatch */
   if (strcmp(acct->auth_type, "oauth") == 0) {
      oauth_provider_config_t google;
      if (oauth_build_google_provider(GOOGLE_EMAIL_SCOPE, &google) != 0) {
         OLOG_ERROR("email: failed to build Google OAuth provider");
         return EMAIL_SVC_CONN_FAILURE;
      }
      if (oauth_get_access_token(&google, acct->user_id, acct->oauth_account_key,
                                 conn->bearer_token, sizeof(conn->bearer_token)) != 0) {
         OLOG_ERROR("email: failed to get OAuth access token for %s", acct->name);
         /* oauth_get_access_token clears the flag on entry: it is this call's */
         return oauth_was_last_refresh_revoked(NULL, 0) ? EMAIL_SVC_CONN_REVOKED
                                                        : EMAIL_SVC_CONN_AUTH;
      }
   } else {
      /* App password — decrypt */
      if (email_decrypt_password(acct, conn->password, sizeof(conn->password)) != 0) {
         OLOG_ERROR("email: failed to decrypt password for %s", acct->name);
         return EMAIL_SVC_CONN_AUTH;
      }
   }

   return EMAIL_SVC_CONN_OK;
}

/* =============================================================================
 * Account Resolution
 *
 * Finds account by name (case-insensitive). NULL = first enabled account.
 * ============================================================================= */

email_err_t email_svc_account_err(int rc) {
   switch (rc) {
      case EMAIL_RC_OK:
         return EMAIL_ERR_NONE;
      case EMAIL_RC_NO_ACCOUNTS:
         return EMAIL_ERR_NO_ACCOUNT;
      case EMAIL_RC_UNKNOWN_ACCOUNT:
         return EMAIL_ERR_ACCOUNT_NOT_FOUND;
      case EMAIL_ACCT_RC_READONLY:
         return EMAIL_ERR_READ_ONLY;
      default:
         return EMAIL_ERR_FAILED;
   }
}

int email_svc_find_account(int user_id, const char *account_name, email_account_t *out) {
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int count = 0;
   email_db_account_list(user_id, accounts, EMAIL_MAX_ACCOUNTS, &count);
   if (count <= 0) {
      return EMAIL_RC_NO_ACCOUNTS;
   }

   int has_enabled = 0;
   int result = EMAIL_RC_UNKNOWN_ACCOUNT;
   for (int i = 0; i < count; i++) {
      if (!accounts[i].enabled)
         continue;
      has_enabled = 1;

      if (!account_name || !account_name[0]) {
         *out = accounts[i];
         result = EMAIL_RC_OK;
         break;
      }
      if (strcasecmp(accounts[i].name, account_name) == 0 ||
          strcasecmp(accounts[i].username, account_name) == 0) {
         *out = accounts[i];
         result = EMAIL_RC_OK;
         break;
      }
   }
   /* All accounts disabled is functionally the same as having none configured —
    * the LLM should tell the user to enable one, not search for matching names. */
   if (result == EMAIL_RC_UNKNOWN_ACCOUNT && !has_enabled) {
      result = EMAIL_RC_NO_ACCOUNTS;
   }

   sodium_memzero(accounts, sizeof(accounts));
   return result;
}

email_err_t email_svc_conn_err(int conn_rc) {
   switch (conn_rc) {
      case EMAIL_SVC_CONN_OK:
         return EMAIL_ERR_NONE;
      case EMAIL_SVC_CONN_REVOKED:
         return EMAIL_ERR_AUTH_REVOKED;
      case EMAIL_SVC_CONN_AUTH:
         return EMAIL_ERR_AUTH_FAILED;
      default:
         return EMAIL_ERR_FAILED;
   }
}

int email_service_find_account_by_id(int user_id,
                                     int64_t account_id,
                                     bool enabled_only,
                                     email_account_t *out) {
   if (!out || account_id <= 0)
      return EMAIL_RC_UNKNOWN_ACCOUNT;
   if (email_db_account_get(account_id, out) != 0)
      return EMAIL_RC_UNKNOWN_ACCOUNT;
   if (out->user_id != user_id || (enabled_only && !out->enabled)) {
      sodium_memzero(out, sizeof(*out));
      return EMAIL_RC_UNKNOWN_ACCOUNT;
   }
   return EMAIL_RC_OK;
}

int email_svc_resolve(int user_id,
                      const char *account_name,
                      const email_target_t *target,
                      email_account_t *out) {
   if (target)
      return email_service_find_account_by_id(user_id, target->account_id, true, out);
   return email_svc_find_account(user_id, account_name, out);
}

int email_svc_lease_begin(const email_account_t *acct,
                          const email_target_t *target,
                          int wait_s,
                          email_svc_lease_t *lease,
                          email_err_t *err) {
   lease->account_id = acct->id;
   lease->taken = false;
   if (email_svc_is_gmail_api(acct))
      return EMAIL_RC_OK;
   if (target && target->lease_held) {
      assert(email_lease_is_held(acct->id) && "lease_held claimed for an account not leased");
      return EMAIL_RC_OK;
   }
   const int rc = email_lease_acquire(acct->id, email_transfer_thread_cancel(), wait_s);
   if (rc == EMAIL_LEASE_OK) {
      lease->taken = true;
      return EMAIL_RC_OK;
   }
   if (err) {
      *err = rc == EMAIL_LEASE_CANCELLED                             ? EMAIL_ERR_CANCELLED
             : (rc == EMAIL_LEASE_TIMEOUT || rc == EMAIL_LEASE_FULL) ? EMAIL_ERR_BUSY
                                                                     : EMAIL_ERR_FAILED;
   }
   OLOG_WARNING("email: account %lld busy (lease rc=%d)", (long long)acct->id, rc);
   return EMAIL_RC_FAILURE;
}

void email_svc_lease_end(email_svc_lease_t *lease) {
   if (lease && lease->taken) {
      email_lease_release(lease->account_id);
      lease->taken = false;
   }
}

/* =============================================================================
 * Gmail API Detection
 *
 * Gmail API accounts: auth_type == "oauth" AND imap_server contains "gmail.com".
 * ============================================================================= */

bool email_svc_is_gmail_api(const email_account_t *acct) {
   return strcmp(acct->auth_type, "oauth") == 0 &&
          strcasestr(acct->imap_server, "gmail.com") != NULL;
}

bool email_service_is_gmail_account(const email_account_t *acct) {
   return acct && email_svc_is_gmail_api(acct);
}

/* =============================================================================
 * Folder Name Validation & Normalization
 * ============================================================================= */

/** Allow-list validation for folder names */
bool email_service_validate_folder_name(const char *folder) {
   if (!folder || !folder[0])
      return true; /* Empty = default inbox, valid */
   if (strlen(folder) > 127)
      return false;
   if (strstr(folder, ".."))
      return false; /* Path traversal */
   for (const char *p = folder; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' || c == '[' ||
          c == ']')
         continue;
      return false;
   }
   return true;
}

int email_svc_gmail_token(const email_account_t *acct, char *token, size_t len, bool *revoked) {
   if (revoked)
      *revoked = false;
   oauth_provider_config_t google;
   if (oauth_build_google_provider(GOOGLE_EMAIL_SCOPE, &google) != 0) {
      OLOG_ERROR("email: failed to build Google OAuth provider for Gmail API");
      return 1;
   }
   const int rc = oauth_get_access_token(&google, acct->user_id, acct->oauth_account_key, token,
                                         len);
   /* oauth_get_access_token clears the flag on entry: it is this call's */
   if (rc != 0 && revoked)
      *revoked = oauth_was_last_refresh_revoked(NULL, 0) != 0;
   return rc;
}

/* =============================================================================
 * Account Management (WebUI)
 * ============================================================================= */

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
                              const char *oauth_account_key) {
   bool is_oauth = auth_type && strcmp(auth_type, "oauth") == 0;
   if (!is_oauth && (!password || !password[0]))
      return 1;

   /* Duplicate check: same user + (OAuth key or username+server) */
   email_account_t existing[16];
   int count = 0;
   email_db_account_list(user_id, existing, 16, &count);
   for (int i = 0; i < count; i++) {
      if (is_oauth && oauth_account_key && oauth_account_key[0] &&
          strcmp(existing[i].oauth_account_key, oauth_account_key) == 0) {
         OLOG_INFO("email: account with OAuth key '%s' already exists, skipping",
                   oauth_account_key);
         return EMAIL_ADD_RC_DUPLICATE;
      }
      if (!is_oauth && username && imap_server && strcmp(existing[i].username, username) == 0 &&
          strcmp(existing[i].imap_server, imap_server) == 0) {
         OLOG_INFO("email: account '%s@%s' already exists, skipping", username, imap_server);
         return EMAIL_ADD_RC_DUPLICATE;
      }
   }

   email_account_t acct = { 0 };
   acct.user_id = user_id;
   acct.enabled = true;
   acct.read_only = read_only;
   acct.imap_port = imap_port > 0 ? imap_port : 993;
   acct.imap_ssl = imap_ssl;
   acct.smtp_port = smtp_port > 0 ? smtp_port : 465;
   acct.smtp_ssl = smtp_ssl;

   snprintf(acct.name, sizeof(acct.name), "%s", name ? name : "");
   snprintf(acct.imap_server, sizeof(acct.imap_server), "%s", imap_server ? imap_server : "");
   snprintf(acct.smtp_server, sizeof(acct.smtp_server), "%s", smtp_server ? smtp_server : "");
   snprintf(acct.username, sizeof(acct.username), "%s", username ? username : "");
   snprintf(acct.display_name, sizeof(acct.display_name), "%s", display_name ? display_name : "");

   if (is_oauth) {
      snprintf(acct.auth_type, sizeof(acct.auth_type), "oauth");
      snprintf(acct.oauth_account_key, sizeof(acct.oauth_account_key), "%s",
               oauth_account_key ? oauth_account_key : "");
   } else {
      snprintf(acct.auth_type, sizeof(acct.auth_type), "app_password");
      if (email_encrypt_password(password, &acct) != 0)
         return 1;
   }

   int64_t id = 0;
   return email_db_account_create(&acct, &id);
}

int email_service_remove_account(int64_t account_id) {
   email_account_t acct;
   if (email_db_account_get(account_id, &acct) != 0)
      return 1;

   /* If OAuth, revoke and delete tokens — but only if no other service shares them */
   if (strcmp(acct.auth_type, "oauth") == 0 && acct.oauth_account_key[0]) {
      bool calendar_uses_account = false;
      calendar_account_t cal_accts[CALENDAR_MAX_ACCOUNTS];
      int cal_count = 0;
      calendar_db_account_list(acct.user_id, cal_accts, CALENDAR_MAX_ACCOUNTS, &cal_count);
      for (int i = 0; i < cal_count; i++) {
         if (strcmp(cal_accts[i].auth_type, "oauth") == 0 &&
             strcmp(cal_accts[i].oauth_account_key, acct.oauth_account_key) == 0) {
            calendar_uses_account = true;
            break;
         }
      }

      if (calendar_uses_account) {
         OLOG_INFO("email: keeping OAuth token for '%s' (still used by calendar)",
                   acct.oauth_account_key);
      } else {
         oauth_provider_config_t google;
         if (oauth_build_google_provider(GOOGLE_EMAIL_SCOPE, &google) == 0) {
            int revoke_rc = oauth_revoke_and_delete(&google, acct.user_id, acct.oauth_account_key);
            if (revoke_rc != 0)
               OLOG_WARNING("email: OAuth revocation failed for '%s' (tokens deleted locally)",
                            acct.oauth_account_key);
         }
      }
   }

   return email_db_account_delete(account_id);
}

int email_service_test_connection(int user_id,
                                  int64_t account_id,
                                  const email_target_t *target,
                                  bool *imap_ok,
                                  bool *smtp_ok,
                                  email_err_t *err) {
   *imap_ok = false;
   *smtp_ok = false;
   if (err)
      *err = EMAIL_ERR_NONE;
   if (target && target->account_id != account_id)
      return EMAIL_RC_FAILURE;

   /* Account settings work on a disabled account too, so this one doesn't
    * require it enabled. */
   email_account_t acct;
   int rc = email_service_find_account_by_id(user_id, account_id, false, &acct);
   if (rc != EMAIL_RC_OK) {
      if (err)
         *err = EMAIL_ERR_ACCOUNT_NOT_FOUND;
      return rc;
   }

   /* Gmail API path — single API call covers both directions */
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      bool revoked = false;
      if (email_svc_gmail_token(&acct, token, sizeof(token), &revoked) != 0) {
         sodium_memzero(token, sizeof(token));
         sodium_memzero(&acct, sizeof(acct));
         if (err)
            *err = revoked ? EMAIL_ERR_AUTH_REVOKED : EMAIL_ERR_AUTH_FAILED;
         return EMAIL_RC_FAILURE;
      }
      char email[128];
      rc = gmail_test_connection(token, email, sizeof(email));
      sodium_memzero(token, sizeof(token));
      sodium_memzero(&acct, sizeof(acct));
      *imap_ok = (rc == 0);
      *smtp_ok = (rc == 0);
      if (rc != 0 && err)
         *err = EMAIL_ERR_FAILED;
      return rc == 0 ? EMAIL_RC_OK : EMAIL_RC_FAILURE;
   }

   email_conn_t conn;
   const int conn_rc = email_svc_build_conn(&acct, &conn);
   if (conn_rc != EMAIL_SVC_CONN_OK) {
      sodium_memzero(&conn, sizeof(conn));
      sodium_memzero(&acct, sizeof(acct));
      if (err)
         *err = email_svc_conn_err(conn_rc);
      return EMAIL_RC_FAILURE;
   }
   /* A test is a button press: it doesn't wait behind another caller using the
    * account, it says the account is busy.  The lease covers only the IMAP half. */
   email_svc_lease_t lease;
   email_err_t lease_err = EMAIL_ERR_NONE;
   if (email_svc_lease_begin(&acct, target, 0, &lease, &lease_err) == EMAIL_RC_OK) {
      *imap_ok = email_test_imap(&conn);
      email_svc_lease_end(&lease);
   }
   *smtp_ok = email_test_smtp(&conn); /* SMTP needs no lease */

   sodium_memzero(&conn, sizeof(conn));
   sodium_memzero(&acct, sizeof(acct));
   if (err && !(*imap_ok && *smtp_ok))
      *err = lease_err != EMAIL_ERR_NONE ? lease_err : EMAIL_ERR_FAILED;
   return (*imap_ok && *smtp_ok) ? EMAIL_RC_OK : EMAIL_RC_FAILURE;
}

int email_service_list_accounts(int user_id, email_account_t *out, int max) {
   int count = 0;
   email_db_account_list(user_id, out, max, &count);
   return count;
}

void email_service_fill_reply_states(int user_id, email_summary_t *rows, int nrows) {
   if (!rows || nrows <= 0)
      return;

   email_account_t accts[EMAIL_MAX_ACCOUNTS];
   int nacct = 0;
   email_db_account_list(user_id, accts, EMAIL_MAX_ACCOUNTS, &nacct);
   if (nacct <= 0)
      return;

   email_summary_t *sent = calloc(EMAIL_MAX_FETCH_RESULTS, sizeof(email_summary_t));
   if (!sent)
      return; /* leave all rows UNKNOWN */

   for (int a = 0; a < nacct; a++) {
      if (!accts[a].enabled || !email_svc_is_gmail_api(&accts[a]))
         continue;

      /* Find this account's enrichable rows and the oldest one's date — the
       * sent-search only needs to reach back that far (a reply is always later
       * than the message it answers). */
      int enrichable = 0;
      time_t oldest = 0;
      for (int r = 0; r < nrows; r++) {
         if (rows[r].thread_id[0] && !rows[r].from_me && rows[r].date > 0 &&
             strcmp(rows[r].account_addr, accts[a].username) == 0) {
            enrichable++;
            if (oldest == 0 || rows[r].date < oldest)
               oldest = rows[r].date;
         }
      }
      if (enrichable == 0)
         continue;

      /* Lower bound padded one day (Gmail after: is date-granular). */
      struct tm tmv;
      char since[16] = { 0 };
      time_t start = oldest - 86400;
      if (localtime_r(&start, &tmv))
         strftime(since, sizeof(since), "%Y-%m-%d", &tmv);

      email_search_params_t params = { 0 };
      snprintf(params.folder, sizeof(params.folder), "sent");
      snprintf(params.since, sizeof(params.since), "%s", since);
      int sent_count = 0;
      /* A non-empty next-page token means the sent set was truncated at the fetch
       * cap — there is sent mail we did NOT see, so a "no match" here cannot be
       * trusted as "not replied" (contract: over-budget → UNKNOWN, never NO). */
      char npt[256] = { 0 };
      /* Resolve by username: display names are not unique, so two "Gmail"
       * accounts must not share one sent-search or cross-tag each other's rows.
       * account_addr is stamped from acct->username, so it is the stable key. */
      int rc = email_service_search(user_id, accts[a].username, &params, sent,
                                    EMAIL_MAX_FETCH_RESULTS, &sent_count, npt, sizeof(npt), NULL, 0,
                                    NULL, NULL);
      if (rc != EMAIL_RC_OK) {
         OLOG_INFO("email_reply: acct='%s' enrichable=%d sent-search FAILED (rc=%d) — rows UNKNOWN",
                   accts[a].name, enrichable, rc);
         continue; /* rows for this account stay UNKNOWN */
      }
      bool sent_truncated = (npt[0] != '\0');

      int yes = 0, no = 0, unknown = 0;
      for (int r = 0; r < nrows; r++) {
         /* date <= 0 mirrors the oldest-date loop's `date > 0` filter, so an
          * unparseable/negative timestamp is treated identically in both
          * passes (matters only if fill is ever extended past Gmail). */
         if (!rows[r].thread_id[0] || rows[r].from_me || rows[r].date <= 0)
            continue;
         if (strcmp(rows[r].account_addr, accts[a].username) != 0)
            continue;
         email_reply_state_t state = EMAIL_REPLIED_NO;
         for (int s = 0; s < sent_count; s++) {
            if (sent[s].date > rows[r].date && strcmp(sent[s].thread_id, rows[r].thread_id) == 0) {
               state = EMAIL_REPLIED_YES;
               break;
            }
         }
         /* No match found, but the sent set was capped — we may have missed the
          * reply, so report UNKNOWN instead of asserting NO. */
         if (state == EMAIL_REPLIED_NO && sent_truncated)
            state = EMAIL_REPLIED_UNKNOWN;
         rows[r].replied = state;
         if (state == EMAIL_REPLIED_YES)
            yes++;
         else if (state == EMAIL_REPLIED_NO)
            no++;
         else
            unknown++;
      }
      OLOG_INFO("email_reply: acct='%s' since=%s enrichable=%d sent_found=%d truncated=%d -> "
                "yes=%d no=%d unknown=%d",
                accts[a].name, since, enrichable, sent_count, sent_truncated, yes, no, unknown);
   }

   /* Zero the scratch: it transiently held the owner's sent-mail metadata
    * (subjects/senders/previews), matching the token/conn wipe discipline. */
   sodium_memzero(sent, (size_t)EMAIL_MAX_FETCH_RESULTS * sizeof(*sent));
   free(sent);
}

/* =============================================================================
 * Draft Management
 * ============================================================================= */

/** Generate a hex draft ID from random bytes */
static void generate_draft_id(char *out, size_t out_len) {
   unsigned char bytes[7];
   randombytes_buf(bytes, sizeof(bytes));
   static const char hex[] = "0123456789abcdef";
   size_t i;
   for (i = 0; i < sizeof(bytes) && (i * 2 + 1) < out_len - 1; i++) {
      out[i * 2] = hex[bytes[i] >> 4];
      out[i * 2 + 1] = hex[bytes[i] & 0x0F];
   }
   out[i * 2] = '\0';
}

/** Expire old drafts (must hold draft_mutex) */
static void expire_drafts_locked(void) {
   time_t now = pending_slots_now();
   for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
      if (s_email.drafts[i].draft_id[0] && !s_email.drafts[i].used &&
          now - s_email.drafts[i].created_at > EMAIL_DRAFT_EXPIRY_SEC) {
         sodium_memzero(&s_email.drafts[i], sizeof(email_draft_t));
      }
   }
}

/* The slot to replace when a table of pending email items is full, or -1:
 * the caller's own session's oldest item when it holds one, else the oldest
 * item of a session that holds more than one.  A session's only item is never
 * pushed out (it may be what that user is about to confirm), and a session
 * that holds several gives one up to a session that holds none.  @p base points at items @p stride
 * bytes apart, each with its origin at @p origin_off, its user id at @p user_off and its time at @p
 * time_off. */
static int pick_victim(const void *base,
                       size_t stride,
                       int count,
                       size_t origin_off,
                       size_t user_off,
                       size_t time_off,
                       int user_id,
                       uint32_t session_id) {
#define ITEM(i) ((const char *)base + (size_t)(i)*stride)
#define SESSION(i) (((const turn_origin_t *)(ITEM(i) + origin_off))->session_id)
#define USER(i) (*(const int *)(ITEM(i) + user_off))
#define MADE(i) (*(const time_t *)(ITEM(i) + time_off))
   int own = -1, shared = -1;
   for (int i = 0; i < count; i++) {
      if (USER(i) == user_id && SESSION(i) == session_id) {
         if (own < 0 || MADE(i) < MADE(own))
            own = i;
         continue;
      }
      int same = 0;
      for (int j = 0; j < count; j++) {
         if (SESSION(j) == SESSION(i) && USER(j) == USER(i))
            same++;
      }
      if (same > 1 && (shared < 0 || MADE(i) < MADE(shared)))
         shared = i;
   }
#undef ITEM
#undef SESSION
#undef USER
#undef MADE
   return own >= 0 ? own : shared;
}

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
                               const turn_origin_t *origin) {
   if (!origin || origin->turn_token == 0)
      return EMAIL_RC_FAILURE; /* only a user's running turn prepares a draft */
   if (from_account_out && from_account_len > 0)
      from_account_out[0] = '\0';

   /* Validate field lengths */
   if (!body || strlen(body) > EMAIL_MAX_SEND_BODY_LEN) {
      OLOG_WARNING("email: draft body too long (%zu > %d)", body ? strlen(body) : 0,
                   EMAIL_MAX_SEND_BODY_LEN);
      return 1;
   }
   if (!subject || strlen(subject) > EMAIL_MAX_SUBJECT_LEN)
      return 1;

   /* Resolve the sending account — required, no implicit default.  Binding the
    * account here (and again verbatim at confirm time) is what prevents a reply
    * from silently going out from the wrong mailbox.  Not-found / no-accounts are
    * forwarded verbatim (as list_folders does) so the tool layer reuses the shared
    * account-error messages; only read-only is a draft-specific outcome. */
   if (!account_name || !account_name[0])
      return EMAIL_RC_FAILURE;

   email_account_t acct;
   int find_rc = email_svc_find_account(user_id, account_name, &acct);
   if (find_rc != EMAIL_RC_OK) {
      sodium_memzero(&acct, sizeof(acct));
      return find_rc;
   }
   if (acct.read_only) {
      sodium_memzero(&acct, sizeof(acct));
      return EMAIL_ACCT_RC_READONLY;
   }

   pthread_mutex_lock(&s_email.draft_mutex);
   expire_drafts_locked();

   /* Find free slot */
   int slot = -1;
   for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
      if (!s_email.drafts[i].draft_id[0]) {
         slot = i;
         break;
      }
   }

   if (slot < 0) {
      slot = pick_victim(s_email.drafts, sizeof(email_draft_t), EMAIL_MAX_DRAFTS,
                         offsetof(email_draft_t, origin), offsetof(email_draft_t, user_id),
                         offsetof(email_draft_t, created_at), user_id, origin->session_id);
      if (slot < 0) {
         pthread_mutex_unlock(&s_email.draft_mutex);
         sodium_memzero(&acct, sizeof(acct));
         return EMAIL_RC_PENDING_FULL;
      }
      sodium_memzero(&s_email.drafts[slot], sizeof(email_draft_t));
   }

   email_draft_t *d = &s_email.drafts[slot];
   d->user_id = user_id;
   d->origin = turn_origin_stored(origin);
   d->created_at = pending_slots_now();
   d->used = false;
   generate_draft_id(d->draft_id, sizeof(d->draft_id));

   snprintf(d->from_account, sizeof(d->from_account), "%s", acct.name);
   snprintf(d->to_address, sizeof(d->to_address), "%s", to_addr ? to_addr : "");
   snprintf(d->to_name, sizeof(d->to_name), "%s", to_name ? to_name : "");
   snprintf(d->subject, sizeof(d->subject), "%s", subject);
   snprintf(d->body, sizeof(d->body), "%s", body);

   snprintf(draft_id_out, draft_id_len, "%s", d->draft_id);
   if (from_account_out && from_account_len > 0)
      snprintf(from_account_out, from_account_len, "%s", d->from_account);

   pthread_mutex_unlock(&s_email.draft_mutex);
   sodium_memzero(&acct, sizeof(acct));
   return 0;
}

/* =============================================================================
 * Confirm Send Throttling
 * ============================================================================= */

static bool is_throttled(int user_id) {
   pthread_mutex_lock(&s_email.draft_mutex);
   time_t now = pending_slots_now();
   bool throttled = false;
   for (int i = 0; i < s_email.throttle_count; i++) {
      if (s_email.throttle[i].user_id == user_id) {
         /* Check if lockout has expired */
         if (now - s_email.throttle[i].first_fail > EMAIL_CONFIRM_LOCKOUT_SEC) {
            s_email.throttle[i].fail_count = 0;
         } else {
            throttled = s_email.throttle[i].fail_count >= EMAIL_CONFIRM_MAX_FAILURES;
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_email.draft_mutex);
   return throttled;
}

static void record_confirm_failure(int user_id) {
   pthread_mutex_lock(&s_email.draft_mutex);
   time_t now = pending_slots_now();
   for (int i = 0; i < s_email.throttle_count; i++) {
      if (s_email.throttle[i].user_id == user_id) {
         if (now - s_email.throttle[i].first_fail > EMAIL_CONFIRM_LOCKOUT_SEC) {
            s_email.throttle[i].fail_count = 1;
            s_email.throttle[i].first_fail = now;
         } else {
            s_email.throttle[i].fail_count++;
         }
         pthread_mutex_unlock(&s_email.draft_mutex);
         return;
      }
   }
   /* New user entry */
   if (s_email.throttle_count < 8) {
      s_email.throttle[s_email.throttle_count].user_id = user_id;
      s_email.throttle[s_email.throttle_count].fail_count = 1;
      s_email.throttle[s_email.throttle_count].first_fail = now;
      s_email.throttle_count++;
   }
   pthread_mutex_unlock(&s_email.draft_mutex);
}


static void expire_pending_trash_locked(void);

int email_service_describe_draft(int user_id,
                                 uint32_t session_id,
                                 const char *draft_id,
                                 char *out,
                                 size_t out_len,
                                 int *valid_for_sec) {
   if (!draft_id || !out || out_len == 0) {
      return EMAIL_RC_FAILURE;
   }
   int rc = EMAIL_RC_FAILURE;
   pthread_mutex_lock(&s_email.draft_mutex);
   expire_drafts_locked();
   for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
      const email_draft_t *d = &s_email.drafts[i];
      if (d->draft_id[0] && !d->used && d->user_id == user_id &&
          d->origin.session_id == session_id && strcmp(d->draft_id, draft_id) == 0) {
         char subject[160], body[320], name[96];
         str_excerpt_line(d->subject, 100, subject, sizeof(subject));
         str_excerpt_line(d->body, 200, body, sizeof(body));
         str_excerpt_line(d->to_name, 48, name, sizeof(name));
         /* The address first: a display name is the model's to choose. */
         const int n = snprintf(out, out_len,
                                "send email to %s%s%s%s from %s, subject \"%s\": "
                                "\"%s\"",
                                d->to_address, name[0] ? " (" : "", name, name[0] ? ")" : "",
                                d->from_account, subject, body);
         if (valid_for_sec) {
            *valid_for_sec = (int)(EMAIL_DRAFT_EXPIRY_SEC - (pending_slots_now() - d->created_at));
         }
         rc = (n > 0 && (size_t)n < out_len) ? EMAIL_RC_OK : EMAIL_RC_FAILURE;
         break;
      }
   }
   pthread_mutex_unlock(&s_email.draft_mutex);
   return rc;
}

int email_service_describe_pending_trash(int user_id,
                                         uint32_t session_id,
                                         const char *pending_id,
                                         char *out,
                                         size_t out_len,
                                         int *valid_for_sec) {
   if (!pending_id || !out || out_len == 0) {
      return EMAIL_RC_FAILURE;
   }
   int rc = EMAIL_RC_FAILURE;
   pthread_mutex_lock(&s_email.pending_trash_mutex);
   expire_pending_trash_locked();
   for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
      const email_pending_trash_t *t = &s_email.pending_trash[i];
      if (t->pending_id[0] && !t->used && t->user_id == user_id &&
          t->origin.session_id == session_id && strcmp(t->pending_id, pending_id) == 0) {
         char subject[160], from[160];
         str_excerpt_line(t->subject, 100, subject, sizeof(subject));
         str_excerpt_line(t->from, EMAIL_TRASH_FROM_SHOWN, from, sizeof(from));
         if (valid_for_sec) {
            *valid_for_sec = (int)(EMAIL_PENDING_TRASH_EXPIRY_SEC -
                                   (pending_slots_now() - t->created_at));
         }
         const int n = snprintf(out, out_len,
                                "move to Trash the email from %s, subject \"%s\" (account %s)",
                                from, subject, t->account_name);
         rc = (n > 0 && (size_t)n < out_len) ? EMAIL_RC_OK : EMAIL_RC_FAILURE;
         break;
      }
   }
   pthread_mutex_unlock(&s_email.pending_trash_mutex);
   return rc;
}

int email_service_confirm_send(int user_id, const char *draft_id, const turn_origin_t *origin) {
   if (!draft_id || !draft_id[0])
      return EMAIL_CONFIRM_RC_NOT_FOUND;

   /* Check throttle */
   if (is_throttled(user_id)) {
      OLOG_WARNING("email: confirm_send throttled for user %d", user_id);
      return EMAIL_CONFIRM_RC_THROTTLED;
   }

   pthread_mutex_lock(&s_email.draft_mutex);
   expire_drafts_locked();

   /* Find matching draft */
   email_draft_t *found = NULL;
   for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
      if (s_email.drafts[i].draft_id[0] && !s_email.drafts[i].used &&
          strcmp(s_email.drafts[i].draft_id, draft_id) == 0) {
         found = &s_email.drafts[i];
         break;
      }
   }

   if (!found) {
      pthread_mutex_unlock(&s_email.draft_mutex);
      record_confirm_failure(user_id);
      return EMAIL_CONFIRM_RC_NOT_FOUND;
   }

   /* Validate user_id match */
   if (found->user_id != user_id) {
      pthread_mutex_unlock(&s_email.draft_mutex);
      record_confirm_failure(user_id);
      OLOG_WARNING("email: confirm_send user mismatch (draft=%d, caller=%d)", found->user_id,
                   user_id);
      return EMAIL_CONFIRM_RC_NOT_FOUND;
   }

   /* The person's yes comes in a later turn of the session that drafted it.
    * Refused here, the draft stays (and the throttle isn't charged): the right
    * turn can still confirm it. */
   const turn_origin_rc_t orc = turn_origin_check(&found->origin, origin);
   if (orc != TURN_ORIGIN_OK) {
      pthread_mutex_unlock(&s_email.draft_mutex);
      OLOG_WARNING("email: confirm_send refused (%s)", turn_origin_refusal(orc));
      return email_confirm_rc(orc);
   }

   /* Mark as used before releasing mutex */
   found->used = true;

   /* Copy draft data to local before releasing mutex */
   char from_account[128], to_addr[256], to_name[64], subject[256], body[4096];
   snprintf(from_account, sizeof(from_account), "%s", found->from_account);
   snprintf(to_addr, sizeof(to_addr), "%s", found->to_address);
   snprintf(to_name, sizeof(to_name), "%s", found->to_name);
   snprintf(subject, sizeof(subject), "%s", found->subject);
   snprintf(body, sizeof(body), "%s", found->body);

   /* Clear the draft slot */
   sodium_memzero(found, sizeof(*found));
   pthread_mutex_unlock(&s_email.draft_mutex);

   /* Send from the account bound to the draft — re-resolved (not re-chosen) so a
    * send always leaves from the account the draft was prepared for.  If it was
    * disabled, deleted, or flipped read-only between draft and confirm, fail
    * rather than fall back to a different mailbox. */
   email_account_t acct;
   if (email_svc_find_account(user_id, from_account, &acct) != EMAIL_RC_OK || acct.read_only) {
      OLOG_WARNING("email: confirm_send account '%s' no longer available/writable", from_account);
      sodium_memzero(&acct, sizeof(acct));
      return EMAIL_CONFIRM_RC_ACCOUNT_GONE;
   }

   /* Gmail API path */
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(&acct, token, sizeof(token), NULL) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_send(token, acct.username, acct.display_name, to_addr, to_name, subject, body);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP/SMTP path */
   email_conn_t conn;
   int rc = email_svc_build_conn(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_send(&conn, to_addr, to_name, subject, body);
   sodium_memzero(&conn, sizeof(conn));
   return rc;
}

/* =============================================================================
 * List Folders / Labels
 * ============================================================================= */

int email_service_list_folders(int user_id,
                               const char *account_name,
                               char *out,
                               size_t out_len,
                               const email_target_t *target,
                               email_err_t *err) {
   if (err)
      *err = EMAIL_ERR_NONE;
   if (!out || out_len < 2) {
      if (err)
         *err = EMAIL_ERR_FAILED;
      return EMAIL_RC_FAILURE;
   }
   out[0] = '\0';

   email_account_t acct;
   int find_rc = email_svc_resolve(user_id, account_name, target, &acct);
   if (find_rc != EMAIL_RC_OK) {
      if (err)
         *err = email_svc_account_err(find_rc);
      return find_rc;
   }

   int rc;
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      bool revoked = false;
      if (email_svc_gmail_token(&acct, token, sizeof(token), &revoked) != 0) {
         if (err)
            *err = revoked ? EMAIL_ERR_AUTH_REVOKED : EMAIL_ERR_AUTH_FAILED;
         rc = EMAIL_RC_FAILURE;
      } else {
         rc = gmail_list_labels(token, out, out_len);
         if (rc != 0 && err)
            *err = EMAIL_ERR_FAILED;
      }
      sodium_memzero(token, sizeof(token));
      sodium_memzero(&acct, sizeof(acct));
      return rc;
   }

   email_svc_lease_t lease;
   if (email_svc_lease_begin(&acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK) {
      sodium_memzero(&acct, sizeof(acct));
      return EMAIL_RC_FAILURE;
   }
   email_conn_t conn;
   const int conn_rc = email_svc_build_conn(&acct, &conn);
   if (conn_rc == EMAIL_SVC_CONN_OK) {
      rc = email_list_folders(&conn, out, out_len);
      if (rc != 0 && err)
         *err = EMAIL_ERR_FAILED;
   } else {
      rc = EMAIL_RC_FAILURE;
      if (err)
         *err = email_svc_conn_err(conn_rc);
   }
   email_svc_lease_end(&lease);
   sodium_memzero(&conn, sizeof(conn));
   sodium_memzero(&acct, sizeof(acct));
   return rc;
}

/* =============================================================================
 * Pending Trash Management
 * ============================================================================= */

static void expire_pending_trash_locked(void) {
   time_t now = pending_slots_now();
   for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
      if (s_email.pending_trash[i].pending_id[0] && !s_email.pending_trash[i].used &&
          now - s_email.pending_trash[i].created_at > EMAIL_PENDING_TRASH_EXPIRY_SEC) {
         sodium_memzero(&s_email.pending_trash[i], sizeof(email_pending_trash_t));
      }
   }
}

int email_service_create_pending_trash(int user_id,
                                       const char *account_name,
                                       const char *message_id,
                                       char *pending_id_out,
                                       size_t pending_id_len,
                                       char *subject_out,
                                       size_t subject_len,
                                       char *from_out,
                                       size_t from_len,
                                       const turn_origin_t *origin) {
   if (!origin || origin->turn_token == 0)
      return EMAIL_RC_FAILURE; /* only a user's running turn prepares a trash */
   if (!message_id || !message_id[0])
      return 1;

   /* Find account — must not be read-only */
   email_account_t acct;
   if (account_name && account_name[0]) {
      if (email_svc_find_account(user_id, account_name, &acct) != 0) {
         sodium_memzero(&acct, sizeof(acct));
         return 1;
      }
   } else {
      /* Try to find account that can access this message */
      if (email_svc_find_account(user_id, NULL, &acct) != 0) {
         sodium_memzero(&acct, sizeof(acct));
         return 1;
      }
   }
   if (acct.read_only) {
      sodium_memzero(&acct, sizeof(acct));
      return EMAIL_ACCT_RC_READONLY;
   }

   /* From and Subject for the confirmation: headers only, no body fetched (and
    * on IMAP the message isn't marked read). */
   email_message_t msg = { 0 };
   const email_read_opts_t headers = { .headers_only = true };
   email_svc_lease_t lease;
   int read_rc = email_svc_lease_begin(&acct, NULL, EMAIL_LEASE_WAIT_SEC, &lease, NULL);
   if (read_rc == EMAIL_RC_OK) {
      read_rc = email_svc_read_single(&acct, message_id, &headers, &msg, NULL, false);
      email_svc_lease_end(&lease);
   }
   char fetched_subject[256] = "(unknown)";
   char fetched_from[128] = "(unknown)";
   if (read_rc == 0) {
      /* snprintf cuts bytes; a character cut in two would reach the
       * confirmation text as invalid UTF-8, so drop any such tail. */
      snprintf(fetched_subject, sizeof(fetched_subject), "%s", msg.subject);
      utf8_trim_incomplete(fetched_subject);
      char from[2 * sizeof(msg.from_name) + sizeof(msg.from_addr) + 8];
      email_display_mailbox(msg.from_name, msg.from_addr, from, sizeof(from));
      /* A long name would push the address out of the confirmation (excerpted
       * to EMAIL_TRASH_FROM_SHOWN); the address is the part that identifies. */
      if (strlen(from) > EMAIL_TRASH_FROM_SHOWN && msg.from_addr[0])
         email_display_mailbox(NULL, msg.from_addr, from, sizeof(from));
      safe_strncpy(fetched_from, from, sizeof(fetched_from));
      utf8_trim_incomplete(fetched_from);
      email_message_free(&msg);
   }

   if (subject_out && subject_len > 0)
      snprintf(subject_out, subject_len, "%s", fetched_subject);
   if (from_out && from_len > 0)
      snprintf(from_out, from_len, "%s", fetched_from);

   pthread_mutex_lock(&s_email.pending_trash_mutex);
   expire_pending_trash_locked();

   /* Find free slot */
   int slot = -1;
   for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
      if (!s_email.pending_trash[i].pending_id[0]) {
         slot = i;
         break;
      }
   }

   if (slot < 0) {
      slot = pick_victim(s_email.pending_trash, sizeof(email_pending_trash_t),
                         EMAIL_MAX_PENDING_TRASH, offsetof(email_pending_trash_t, origin),
                         offsetof(email_pending_trash_t, user_id),
                         offsetof(email_pending_trash_t, created_at), user_id, origin->session_id);
      if (slot < 0) {
         pthread_mutex_unlock(&s_email.pending_trash_mutex);
         sodium_memzero(&acct, sizeof(acct));
         return EMAIL_RC_PENDING_FULL;
      }
      sodium_memzero(&s_email.pending_trash[slot], sizeof(email_pending_trash_t));
   }

   email_pending_trash_t *pt = &s_email.pending_trash[slot];
   pt->user_id = user_id;
   pt->origin = turn_origin_stored(origin);
   pt->created_at = pending_slots_now();
   pt->used = false;
   generate_draft_id(pt->pending_id, sizeof(pt->pending_id));

   snprintf(pt->message_id, sizeof(pt->message_id), "%s", message_id);
   snprintf(pt->account_name, sizeof(pt->account_name), "%s", acct.name);
   snprintf(pt->subject, sizeof(pt->subject), "%s", fetched_subject);
   snprintf(pt->from, sizeof(pt->from), "%s", fetched_from);

   snprintf(pending_id_out, pending_id_len, "%s", pt->pending_id);

   pthread_mutex_unlock(&s_email.pending_trash_mutex);
   sodium_memzero(&acct, sizeof(acct));
   return 0;
}

/* The email_err_t for a move's EMAIL_RC_* (trash, archive). */
static email_err_t move_err(int rc) {
   switch (rc) {
      case EMAIL_RC_OK:
      case EMAIL_RC_ALREADY_THERE:
      case EMAIL_RC_LEFT_FLAGGED:
         return EMAIL_ERR_NONE;
      case EMAIL_RC_NOT_FOUND:
         return EMAIL_ERR_NOT_FOUND;
      case EMAIL_RC_NO_TRASH:
         return EMAIL_ERR_NO_TRASH;
      case EMAIL_RC_FOLDER_MISSING:
         return EMAIL_ERR_FOLDER_MISSING;
      default:
         return EMAIL_ERR_FAILED;
   }
}

/* Move one message to the account's Trash (@p trash) or Archive, on an account
 * already resolved and checked writable. */
static int execute_move(const email_account_t *acct,
                        const char *message_id,
                        bool trash,
                        const email_target_t *target,
                        email_err_t *err) {
   *err = EMAIL_ERR_FAILED;
   /* Gmail API path */
   if (email_svc_is_gmail_api(acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      bool revoked = false;
      if (email_svc_gmail_token(acct, token, sizeof(token), &revoked) != 0) {
         sodium_memzero(token, sizeof(token));
         *err = revoked ? EMAIL_ERR_AUTH_REVOKED : EMAIL_ERR_AUTH_FAILED;
         return 1;
      }
      int rc = trash ? gmail_trash_message(token, message_id)
                     : gmail_archive_message(token, message_id);
      sodium_memzero(token, sizeof(token));
      *err = rc == 0 ? EMAIL_ERR_NONE : EMAIL_ERR_FAILED;
      return rc;
   }

   char imap_folder[128];
   uint32_t uid_val = 0;
   if (!email_svc_parse_imap_id(message_id, imap_folder, sizeof(imap_folder), &uid_val, false))
      return 1;

   email_svc_lease_t lease;
   if (email_svc_lease_begin(acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK)
      return EMAIL_RC_FAILURE;
   email_conn_t conn;
   int rc = email_svc_build_conn(acct, &conn);
   if (rc != EMAIL_SVC_CONN_OK) {
      *err = email_svc_conn_err(rc);
      rc = 1;
   } else {
      rc = trash ? email_trash_message(&conn, imap_folder, uid_val)
                 : email_archive_message(&conn, imap_folder, uid_val);
      *err = move_err(rc);
   }
   email_svc_lease_end(&lease);
   sodium_memzero(&conn, sizeof(conn));
   return rc;
}

/* The pending trash @p pending_id of @p user_id (unused, unexpired); the caller
 * holds pending_trash_mutex. */
static email_pending_trash_t *find_pending_trash_locked(int user_id, const char *pending_id) {
   expire_pending_trash_locked();
   for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
      email_pending_trash_t *p = &s_email.pending_trash[i];
      if (p->pending_id[0] && !p->used && p->user_id == user_id &&
          strcmp(p->pending_id, pending_id) == 0)
         return p;
   }
   return NULL;
}

int email_service_confirm_trash(int user_id,
                                const char *pending_id,
                                const turn_origin_t *origin,
                                email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!pending_id || !pending_id[0])
      return EMAIL_CONFIRM_RC_NOT_FOUND;

   if (is_throttled(user_id)) {
      OLOG_WARNING("email: confirm_trash throttled for user %d", user_id);
      return EMAIL_CONFIRM_RC_THROTTLED;
   }

   /* Check the pending action, but don't consume it yet: the account's lease
    * may be busy, and a busy confirm must leave it staged for a retry. */
   pthread_mutex_lock(&s_email.pending_trash_mutex);
   email_pending_trash_t *found = find_pending_trash_locked(user_id, pending_id);
   if (!found) {
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      record_confirm_failure(user_id);
      return EMAIL_CONFIRM_RC_NOT_FOUND;
   }

   /* As for a draft: a later turn of the session that asked. */
   const turn_origin_rc_t orc = turn_origin_check(&found->origin, origin);
   if (orc != TURN_ORIGIN_OK) {
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      OLOG_WARNING("email: confirm_trash refused (%s)", turn_origin_refusal(orc));
      return email_confirm_rc(orc);
   }

   char message_id[192], account_name[128];
   snprintf(message_id, sizeof(message_id), "%s", found->message_id);
   snprintf(account_name, sizeof(account_name), "%s", found->account_name);
   pthread_mutex_unlock(&s_email.pending_trash_mutex);

   /* Re-resolve the account (not re-choose) and re-check writability — an account
    * flipped read-only, disabled, or deleted between 'trash' and 'confirm_trash'
    * must not still execute the delete (mirrors confirm_send).  That pending
    * action can never run, so it's consumed. */
   email_account_t acct;
   if (email_svc_find_account(user_id, account_name, &acct) != EMAIL_RC_OK || acct.read_only) {
      OLOG_WARNING("email: confirm_trash account '%s' no longer available/writable", account_name);
      sodium_memzero(&acct, sizeof(acct));
      pthread_mutex_lock(&s_email.pending_trash_mutex);
      found = find_pending_trash_locked(user_id, pending_id);
      if (found)
         sodium_memzero(found, sizeof(*found));
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      return EMAIL_CONFIRM_RC_ACCOUNT_GONE;
   }

   email_svc_lease_t lease;
   if (email_svc_lease_begin(&acct, NULL, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK) {
      sodium_memzero(&acct, sizeof(acct));
      /* Still staged.  The user's yes was valid; re-arm it for their next reply,
       * which a retry has to be (the same call again this turn is a duplicate). */
      pthread_mutex_lock(&s_email.pending_trash_mutex);
      found = find_pending_trash_locked(user_id, pending_id);
      if (found)
         found->origin = turn_origin_stored(origin);
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      return EMAIL_RC_FAILURE;
   }

   /* The wait can be long: check again that the account is still there and
    * writable before moving anything. */
   email_account_t now_acct;
   const bool still_writable = email_service_find_account_by_id(user_id, acct.id, true,
                                                                &now_acct) == EMAIL_RC_OK &&
                               !now_acct.read_only;
   sodium_memzero(&now_acct, sizeof(now_acct));
   if (!still_writable) {
      email_svc_lease_end(&lease);
      sodium_memzero(&acct, sizeof(acct));
      pthread_mutex_lock(&s_email.pending_trash_mutex);
      found = find_pending_trash_locked(user_id, pending_id);
      if (found)
         sodium_memzero(found, sizeof(*found));
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      return EMAIL_CONFIRM_RC_ACCOUNT_GONE;
   }

   /* Consume it now, only if it's still there: it may have expired, or another
    * confirm of it may have run, while this one waited. */
   pthread_mutex_lock(&s_email.pending_trash_mutex);
   found = find_pending_trash_locked(user_id, pending_id);
   const bool still_staged = found != NULL;
   if (found)
      sodium_memzero(found, sizeof(*found));
   pthread_mutex_unlock(&s_email.pending_trash_mutex);

   int rc;
   if (still_staged) {
      const email_target_t leased = { .account_id = acct.id, .lease_held = true };
      rc = execute_move(&acct, message_id, true, &leased, err);
   } else {
      rc = EMAIL_CONFIRM_RC_NOT_FOUND;
   }
   email_svc_lease_end(&lease);
   sodium_memzero(&acct, sizeof(acct));
   return rc;
}

/* =============================================================================
 * Archive (single-step, no confirmation)
 * ============================================================================= */

int email_service_archive(int user_id,
                          const char *account_name,
                          const char *message_id,
                          const email_target_t *target,
                          email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!message_id || !message_id[0])
      return 1;

   /* Resolve account */
   email_account_t acct;
   const int find_rc = email_svc_resolve(user_id, account_name, target, &acct);
   if (find_rc != EMAIL_RC_OK) {
      *err = email_svc_account_err(find_rc);
      return 1;
   }

   if (acct.read_only) {
      sodium_memzero(&acct, sizeof(acct));
      *err = EMAIL_ERR_READ_ONLY;
      return EMAIL_ACCT_RC_READONLY;
   }

   const int rc = execute_move(&acct, message_id, false, target, err);
   sodium_memzero(&acct, sizeof(acct));
   return rc;
}

/* =============================================================================
 * Access Summary
 * ============================================================================= */

int email_service_get_access_summary(int user_id,
                                     char *writable,
                                     size_t w_len,
                                     char *read_only_out,
                                     size_t r_len) {
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int count = 0;
   email_db_account_list(user_id, accounts, EMAIL_MAX_ACCOUNTS, &count);
   if (count <= 0)
      return EMAIL_ACCESS_RC_ERROR;

   writable[0] = '\0';
   read_only_out[0] = '\0';
   int w_pos = 0, r_pos = 0;
   int has_ro = 0;

   for (int i = 0; i < count; i++) {
      if (!accounts[i].enabled)
         continue;
      if (accounts[i].read_only) {
         has_ro = 1;
         if (r_pos > 0 && r_pos < (int)r_len - 2)
            r_pos += snprintf(read_only_out + r_pos, r_len - r_pos, ", ");
         r_pos += snprintf(read_only_out + r_pos, r_len - r_pos, "%s", accounts[i].name);
      } else {
         if (w_pos > 0 && w_pos < (int)w_len - 2)
            w_pos += snprintf(writable + w_pos, w_len - w_pos, ", ");
         w_pos += snprintf(writable + w_pos, w_len - w_pos, "%s", accounts[i].name);
      }
   }

   sodium_memzero(accounts, sizeof(accounts));
   return has_ro;
}
