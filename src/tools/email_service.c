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
#include <ctype.h>
#include <netdb.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/crypto_store.h"
#include "logging.h"
#include "tools/calendar_db.h"
#include "tools/email_client.h"
#include "tools/email_db.h"
#include "tools/gmail_client.h"
#include "tools/oauth_client.h"

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

static int build_conn_for_account(const email_account_t *acct, email_conn_t *conn) {
   memset(conn, 0, sizeof(*conn));

   /* Enforce TLS for non-loopback servers */
   if (!acct->imap_ssl && !is_loopback(acct->imap_server)) {
      OLOG_ERROR("email: refusing plaintext IMAP to non-loopback server %s", acct->imap_server);
      return 1;
   }
   if (!acct->smtp_ssl && !is_loopback(acct->smtp_server)) {
      OLOG_ERROR("email: refusing plaintext SMTP to non-loopback server %s", acct->smtp_server);
      return 1;
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
         return 1;
      }
      if (oauth_get_access_token(&google, acct->user_id, acct->oauth_account_key,
                                 conn->bearer_token, sizeof(conn->bearer_token)) != 0) {
         OLOG_ERROR("email: failed to get OAuth access token for %s", acct->name);
         return 1;
      }
   } else {
      /* App password — decrypt */
      if (email_decrypt_password(acct, conn->password, sizeof(conn->password)) != 0) {
         OLOG_ERROR("email: failed to decrypt password for %s", acct->name);
         return 1;
      }
   }

   return 0;
}

/* =============================================================================
 * Account Resolution
 *
 * Finds account by name (case-insensitive). NULL = first enabled account.
 * ============================================================================= */

static int find_account(int user_id, const char *account_name, email_account_t *out) {
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

static int find_writable_account(int user_id, email_account_t *out) {
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int count = 0;
   email_db_account_list(user_id, accounts, EMAIL_MAX_ACCOUNTS, &count);

   int result = 1;
   for (int i = 0; i < count; i++) {
      if (accounts[i].enabled && !accounts[i].read_only) {
         *out = accounts[i];
         result = 0;
         break;
      }
   }
   sodium_memzero(accounts, sizeof(accounts));
   return result;
}

/* =============================================================================
 * Gmail API Detection
 *
 * Gmail API accounts: auth_type == "oauth" AND imap_server contains "gmail.com".
 * ============================================================================= */

static bool is_gmail_api_account(const email_account_t *acct) {
   return strcmp(acct->auth_type, "oauth") == 0 &&
          strcasestr(acct->imap_server, "gmail.com") != NULL;
}

/** IMAP server is Gmail (regardless of auth type) */
static bool is_gmail_imap_server(const email_account_t *acct) {
   return strcasestr(acct->imap_server, "gmail.com") != NULL;
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

/* Internal alias matching the previous static name so the rest of this file
 * doesn't need a sed.  The external name carries the email_service_ prefix
 * because it lives in the public header. */
#define validate_folder_name email_service_validate_folder_name

/** Folder normalization result */
typedef struct {
   char gmail_query[256]; /* Gmail search fragment (e.g. "in:sent", "label:\"Receipts\"") */
   char imap_folder[128]; /* IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail") */
} folder_norm_t;

/** Normalization map entry */
typedef struct {
   const char *user_name;
   const char *gmail_query;
   const char *imap_gmail;   /* IMAP folder on Gmail servers */
   const char *imap_generic; /* IMAP folder on non-Gmail servers (NULL = unsupported) */
} folder_map_entry_t;

static const folder_map_entry_t folder_map[] = {
   { "inbox", "in:inbox", "INBOX", "INBOX" },
   { "sent", "in:sent", "[Gmail]/Sent Mail", "Sent" },
   { "trash", "in:trash", "[Gmail]/Trash", "Trash" },
   { "spam", "in:spam", "[Gmail]/Spam", "Spam" },
   { "drafts", "in:drafts", "[Gmail]/Drafts", "Drafts" },
   { "starred", "is:starred", "[Gmail]/Starred", NULL },
   { "important", "is:important", "[Gmail]/Important", NULL },
   { "all", "in:all", "[Gmail]/All Mail", NULL },
};
#define FOLDER_MAP_COUNT (sizeof(folder_map) / sizeof(folder_map[0]))

/** Strip double quotes from a folder name for safe Gmail query interpolation */
static void strip_folder_quotes(const char *src, char *dst, size_t dst_len) {
   size_t j = 0;
   for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
      if (src[i] != '"')
         dst[j++] = src[i];
   }
   dst[j] = '\0';
}

static void normalize_folder(const char *folder, const email_account_t *acct, folder_norm_t *out) {
   memset(out, 0, sizeof(*out));

   /* Empty/NULL = default inbox */
   if (!folder || !folder[0]) {
      snprintf(out->gmail_query, sizeof(out->gmail_query), "in:inbox");
      snprintf(out->imap_folder, sizeof(out->imap_folder), "INBOX");
      return;
   }

   /* Check normalization map */
   for (size_t i = 0; i < FOLDER_MAP_COUNT; i++) {
      if (strcasecmp(folder, folder_map[i].user_name) == 0) {
         snprintf(out->gmail_query, sizeof(out->gmail_query), "%s", folder_map[i].gmail_query);
         if (is_gmail_imap_server(acct)) {
            snprintf(out->imap_folder, sizeof(out->imap_folder), "%s", folder_map[i].imap_gmail);
         } else if (folder_map[i].imap_generic) {
            snprintf(out->imap_folder, sizeof(out->imap_folder), "%s", folder_map[i].imap_generic);
         } else {
            /* Unsupported on generic IMAP — fall back to INBOX */
            snprintf(out->imap_folder, sizeof(out->imap_folder), "INBOX");
         }
         return;
      }
   }

   /* Custom folder/label — pass through with quote stripping for Gmail.
    * Quoted to handle multi-word labels (e.g. label:"My Label"). */
   char safe[128];
   strip_folder_quotes(folder, safe, sizeof(safe));
   snprintf(out->gmail_query, sizeof(out->gmail_query), "label:\"%s\"", safe);
   snprintf(out->imap_folder, sizeof(out->imap_folder), "%s", folder);
}

static int get_gmail_token(const email_account_t *acct, char *token, size_t len) {
   oauth_provider_config_t google;
   if (oauth_build_google_provider(GOOGLE_EMAIL_SCOPE, &google) != 0) {
      OLOG_ERROR("email: failed to build Google OAuth provider for Gmail API");
      return 1;
   }
   return oauth_get_access_token(&google, acct->user_id, acct->oauth_account_key, token, len);
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
         return 2; /* Duplicate */
      }
      if (!is_oauth && username && imap_server && strcmp(existing[i].username, username) == 0 &&
          strcmp(existing[i].imap_server, imap_server) == 0) {
         OLOG_INFO("email: account '%s@%s' already exists, skipping", username, imap_server);
         return 2; /* Duplicate */
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

int email_service_test_connection(int64_t account_id, bool *imap_ok, bool *smtp_ok) {
   *imap_ok = false;
   *smtp_ok = false;

   email_account_t acct;
   if (email_db_account_get(account_id, &acct) != 0)
      return 1;

   /* Gmail API path — single API call covers both directions */
   if (is_gmail_api_account(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(&acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      char email[128];
      int rc = gmail_test_connection(token, email, sizeof(email));
      sodium_memzero(token, sizeof(token));
      *imap_ok = (rc == 0);
      *smtp_ok = (rc == 0);
      return rc;
   }

   email_conn_t conn;
   int rc = build_conn_for_account(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_test_connection(&conn, imap_ok, smtp_ok);

   /* Wipe credentials */
   sodium_memzero(&conn, sizeof(conn));
   return rc;
}

int email_service_list_accounts(int user_id, email_account_t *out, int max) {
   int count = 0;
   email_db_account_list(user_id, out, max, &count);
   return count;
}

/* =============================================================================
 * Operations (Tool Layer)
 * ============================================================================= */

/* Stamp the owning account's display name + address onto each returned row.
 * The lower-level fetch primitives don't know the account, so the service layer
 * is the single point that labels every row (contract in email_service.h).  The
 * address (username) is the unambiguous inbox identifier — the display name may
 * be a generic label like "Gmail" that doesn't say which account it is. */
static void stamp_account(email_summary_t *out, int n, const email_account_t *acct) {
   for (int i = 0; i < n; i++) {
      snprintf(out[i].account_name, sizeof(out[i].account_name), "%s", acct->name);
      snprintf(out[i].account_addr, sizeof(out[i].account_addr), "%s", acct->username);
   }
}

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
                         size_t npt_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   if (!validate_folder_name(folder))
      return EMAIL_RC_INVALID_FOLDER;

   email_account_t acct;
   int find_rc = find_account(user_id, account_name, &acct);
   if (find_rc != EMAIL_RC_OK)
      return find_rc;

   if (count <= 0)
      count = acct.max_recent > 0 ? acct.max_recent : 10;

   folder_norm_t norm;
   normalize_folder(folder, &acct, &norm);

   /* Gmail API path */
   if (is_gmail_api_account(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(&acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_fetch_recent(token, norm.gmail_query, count, unread_only, page_token, out, max,
                                  out_count, next_page_token, npt_len);
      sodium_memzero(token, sizeof(token));
      stamp_account(out, *out_count, &acct);
      return rc;
   }

   /* IMAP path (no pagination support) */
   email_conn_t conn;
   int rc = build_conn_for_account(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_fetch_recent(&conn, norm.imap_folder, count, unread_only, out, max, out_count);
   sodium_memzero(&conn, sizeof(conn));

   /* Populate message_id (folder:uid) for IMAP results, then stamp the account. */
   for (int i = 0; i < *out_count; i++)
      snprintf(out[i].message_id, sizeof(out[i].message_id), "%s:%u", norm.imap_folder, out[i].uid);
   stamp_account(out, *out_count, &acct);

   return rc;
}

/** Read from a single account */
static int read_single_account(email_account_t *acct, const char *message_id, email_message_t *out);

int email_service_read(int user_id,
                       const char *account_name,
                       const char *message_id,
                       email_message_t *out) {
   if (!message_id || !message_id[0])
      return 1;

   /* Specific account requested */
   if (account_name && account_name[0]) {
      email_account_t acct;
      int find_rc = find_account(user_id, account_name, &acct);
      if (find_rc != EMAIL_RC_OK)
         return find_rc;
      return read_single_account(&acct, message_id, out);
   }

   /* No account specified — try all enabled accounts until one succeeds */
   email_account_t accounts[16];
   int acct_count = 0;
   email_db_account_list(user_id, accounts, 16, &acct_count);
   if (acct_count <= 0)
      return EMAIL_RC_NO_ACCOUNTS;
   int enabled_seen = 0;
   for (int i = 0; i < acct_count; i++) {
      if (!accounts[i].enabled)
         continue;
      enabled_seen = 1;
      if (read_single_account(&accounts[i], message_id, out) == 0)
         return EMAIL_RC_OK;
   }
   /* Distinguish "you have accounts but they're all disabled" from generic
    * fetch failure — the LLM should tell the user to enable one. */
   return enabled_seen ? EMAIL_RC_FAILURE : EMAIL_RC_NO_ACCOUNTS;
}

static int read_single_account(email_account_t *acct,
                               const char *message_id,
                               email_message_t *out) {
   /* Gmail API path — hex IDs contain no colons */
   if (is_gmail_api_account(acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int max_chars = acct->max_body_chars > 0 ? acct->max_body_chars : EMAIL_MAX_READ_BODY_LEN;
      int rc = gmail_read_message(token, message_id, max_chars, out);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP path — parse composite "folder:uid" using strrchr (last colon).
    * Folder names can contain colons but UIDs are always pure decimal. */
   char imap_folder[128] = "INBOX";
   const char *uid_str = message_id;

   const char *last_colon = strrchr(message_id, ':');
   if (last_colon) {
      size_t folder_len = last_colon - message_id;
      if (folder_len > 0 && folder_len < sizeof(imap_folder)) {
         memcpy(imap_folder, message_id, folder_len);
         imap_folder[folder_len] = '\0';
      }
      uid_str = last_colon + 1;
   }

   if (!validate_folder_name(imap_folder)) {
      OLOG_ERROR("email: invalid folder name in message_id '%s'", message_id);
      return 1;
   }

   char *endptr = NULL;
   unsigned long uid_val = strtoul(uid_str, &endptr, 10);
   if (!endptr || *endptr != '\0' || uid_val == 0 || uid_val > UINT32_MAX) {
      OLOG_ERROR("email: invalid IMAP UID '%s'", uid_str);
      return 1;
   }

   email_conn_t conn;
   int rc = build_conn_for_account(acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_read_message(&conn, imap_folder, (uint32_t)uid_val, out);
   sodium_memzero(&conn, sizeof(conn));

   /* Populate message_id for IMAP results */
   if (rc == 0)
      snprintf(out->message_id, sizeof(out->message_id), "%s", message_id);

   return rc;
}

/** Search a single account. Used by email_service_search for both
 *  targeted and multi-account searches. */
static int search_single_account(email_account_t *acct,
                                 const email_search_params_t *params,
                                 email_summary_t *out,
                                 int max,
                                 int *out_count,
                                 char *next_page_token,
                                 size_t npt_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   folder_norm_t norm;
   normalize_folder(params->folder, acct, &norm);

   if (is_gmail_api_account(acct)) {
      email_search_params_t gmail_params = *params;
      snprintf(gmail_params.folder, sizeof(gmail_params.folder), "%s", norm.gmail_query);

      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_search(token, &gmail_params, max, out, max, out_count, next_page_token,
                            npt_len);
      sodium_memzero(token, sizeof(token));
      stamp_account(out, *out_count, acct);
      return rc;
   }

   /* IMAP path (no pagination support) */
   email_conn_t conn;
   int rc = build_conn_for_account(acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_search(&conn, norm.imap_folder, params, out, max, out_count);
   sodium_memzero(&conn, sizeof(conn));

   for (int i = 0; i < *out_count; i++)
      snprintf(out[i].message_id, sizeof(out[i].message_id), "%s:%u", norm.imap_folder, out[i].uid);
   stamp_account(out, *out_count, acct);

   return rc;
}

int email_service_search(int user_id,
                         const char *account_name,
                         const email_search_params_t *params,
                         email_summary_t *out,
                         int max,
                         int *out_count,
                         char *next_page_token,
                         size_t npt_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   if (!validate_folder_name(params->folder))
      return EMAIL_RC_INVALID_FOLDER;

   /* Specific account requested — search just that one */
   if (account_name && account_name[0]) {
      email_account_t acct;
      int find_rc = find_account(user_id, account_name, &acct);
      if (find_rc != EMAIL_RC_OK)
         return find_rc;
      return search_single_account(&acct, params, out, max, out_count, next_page_token, npt_len);
   }

   /* No account specified — search ALL enabled accounts and merge results.
    * Pagination only applies to single-account searches. */
   email_account_t accounts[16];
   int acct_count = 0;
   email_db_account_list(user_id, accounts, 16, &acct_count);
   if (acct_count <= 0)
      return EMAIL_RC_NO_ACCOUNTS;
   int enabled_seen = 0;
   int any_error = 0;
   int total = 0;
   for (int i = 0; i < acct_count && total < max; i++) {
      if (!accounts[i].enabled)
         continue;
      enabled_seen = 1;

      int this_count = 0;
      int remaining = max - total;
      int rc = search_single_account(&accounts[i], params, out + total, remaining, &this_count,
                                     NULL, 0);
      if (rc == 0) {
         total += this_count;
      } else {
         /* Per-account transport/upstream failure.  Logged (not silent) so a
          * genuine backend problem is diagnosable from the log; the search still
          * continues across the remaining accounts. */
         any_error = 1;
         OLOG_WARNING("email: search failed for account '%s' (rc=%d)", accounts[i].name, rc);
      }
   }

   *out_count = total;
   if (total > 0)
      return EMAIL_RC_OK;
   if (!enabled_seen)
      return EMAIL_RC_NO_ACCOUNTS;
   /* Zero results across all enabled accounts: distinguish a genuine no-match
    * (every account searched OK, just nothing matched) from a real failure (at
    * least one account errored).  Reporting no-match as FAILURE makes the LLM
    * believe email is down and abandon the search instead of broadening it. */
   return any_error ? EMAIL_RC_FAILURE : EMAIL_RC_OK;
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
   time_t now = time(NULL);
   for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
      if (s_email.drafts[i].draft_id[0] && !s_email.drafts[i].used &&
          now - s_email.drafts[i].created_at > EMAIL_DRAFT_EXPIRY_SEC) {
         sodium_memzero(&s_email.drafts[i], sizeof(email_draft_t));
      }
   }
}

int email_service_create_draft(int user_id,
                               const char *to_addr,
                               const char *to_name,
                               const char *subject,
                               const char *body,
                               char *draft_id_out,
                               size_t draft_id_len) {
   /* Validate field lengths */
   if (!body || strlen(body) > EMAIL_MAX_SEND_BODY_LEN) {
      OLOG_WARNING("email: draft body too long (%zu > %d)", body ? strlen(body) : 0,
                   EMAIL_MAX_SEND_BODY_LEN);
      return 1;
   }
   if (!subject || strlen(subject) > EMAIL_MAX_SUBJECT_LEN)
      return 1;

   /* Check for writable accounts */
   email_account_t acct;
   if (find_writable_account(user_id, &acct) != 0)
      return 2; /* All accounts read-only */

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
      /* Evict oldest */
      time_t oldest = 0;
      for (int i = 0; i < EMAIL_MAX_DRAFTS; i++) {
         if (oldest == 0 || s_email.drafts[i].created_at < oldest) {
            oldest = s_email.drafts[i].created_at;
            slot = i;
         }
      }
      sodium_memzero(&s_email.drafts[slot], sizeof(email_draft_t));
   }

   email_draft_t *d = &s_email.drafts[slot];
   d->user_id = user_id;
   d->created_at = time(NULL);
   d->used = false;
   generate_draft_id(d->draft_id, sizeof(d->draft_id));

   snprintf(d->to_address, sizeof(d->to_address), "%s", to_addr ? to_addr : "");
   snprintf(d->to_name, sizeof(d->to_name), "%s", to_name ? to_name : "");
   snprintf(d->subject, sizeof(d->subject), "%s", subject);
   snprintf(d->body, sizeof(d->body), "%s", body);

   snprintf(draft_id_out, draft_id_len, "%s", d->draft_id);

   pthread_mutex_unlock(&s_email.draft_mutex);
   return 0;
}

/* =============================================================================
 * Confirm Send Throttling
 * ============================================================================= */

static bool is_throttled(int user_id) {
   pthread_mutex_lock(&s_email.draft_mutex);
   time_t now = time(NULL);
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
   time_t now = time(NULL);
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

int email_service_confirm_send(int user_id, const char *draft_id) {
   if (!draft_id || !draft_id[0])
      return 2;

   /* Check throttle */
   if (is_throttled(user_id)) {
      OLOG_WARNING("email: confirm_send throttled for user %d", user_id);
      return 3;
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
      return 2; /* Not found / expired */
   }

   /* Validate user_id match */
   if (found->user_id != user_id) {
      pthread_mutex_unlock(&s_email.draft_mutex);
      record_confirm_failure(user_id);
      OLOG_WARNING("email: confirm_send user mismatch (draft=%d, caller=%d)", found->user_id,
                   user_id);
      return 2;
   }

   /* Mark as used before releasing mutex */
   found->used = true;

   /* Copy draft data to local before releasing mutex */
   char to_addr[256], to_name[64], subject[256], body[4096];
   snprintf(to_addr, sizeof(to_addr), "%s", found->to_address);
   snprintf(to_name, sizeof(to_name), "%s", found->to_name);
   snprintf(subject, sizeof(subject), "%s", found->subject);
   snprintf(body, sizeof(body), "%s", found->body);

   /* Clear the draft slot */
   sodium_memzero(found, sizeof(*found));
   pthread_mutex_unlock(&s_email.draft_mutex);

   /* Find writable account and send */
   email_account_t acct;
   if (find_writable_account(user_id, &acct) != 0)
      return 1;

   /* Gmail API path */
   if (is_gmail_api_account(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(&acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_send(token, acct.username, acct.display_name, to_addr, to_name, subject, body);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP/SMTP path */
   email_conn_t conn;
   int rc = build_conn_for_account(&acct, &conn);
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

int email_service_list_folders(int user_id, const char *account_name, char *out, size_t out_len) {
   if (!out || out_len < 2)
      return EMAIL_RC_FAILURE;
   out[0] = '\0';

   email_account_t acct;
   int find_rc = find_account(user_id, account_name, &acct);
   if (find_rc != EMAIL_RC_OK)
      return find_rc;

   /* Gmail API path */
   if (is_gmail_api_account(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(&acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_list_labels(token, out, out_len);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP path */
   email_conn_t conn;
   int rc = build_conn_for_account(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_list_folders(&conn, out, out_len);
   sodium_memzero(&conn, sizeof(conn));
   return rc;
}

/* =============================================================================
 * Pending Trash Management
 * ============================================================================= */

static void expire_pending_trash_locked(void) {
   time_t now = time(NULL);
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
                                       size_t from_len) {
   if (!message_id || !message_id[0])
      return 1;

   /* Find account — must not be read-only */
   email_account_t acct;
   if (account_name && account_name[0]) {
      if (find_account(user_id, account_name, &acct) != 0)
         return 1;
   } else {
      /* Try to find account that can access this message */
      if (find_account(user_id, NULL, &acct) != 0)
         return 1;
   }
   if (acct.read_only)
      return 2;

   /* Fetch message metadata for confirmation display */
   email_message_t msg = { 0 };
   int read_rc = read_single_account(&acct, message_id, &msg);
   char fetched_subject[256] = "(unknown)";
   char fetched_from[128] = "(unknown)";
   if (read_rc == 0) {
      snprintf(fetched_subject, sizeof(fetched_subject), "%s", msg.subject);
      snprintf(fetched_from, sizeof(fetched_from), "%.63s%s%.63s", msg.from_name,
               msg.from_name[0] ? " " : "", msg.from_addr);
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
      /* Evict oldest */
      time_t oldest = 0;
      for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
         if (oldest == 0 || s_email.pending_trash[i].created_at < oldest) {
            oldest = s_email.pending_trash[i].created_at;
            slot = i;
         }
      }
      sodium_memzero(&s_email.pending_trash[slot], sizeof(email_pending_trash_t));
   }

   email_pending_trash_t *pt = &s_email.pending_trash[slot];
   pt->user_id = user_id;
   pt->created_at = time(NULL);
   pt->used = false;
   generate_draft_id(pt->pending_id, sizeof(pt->pending_id));

   snprintf(pt->message_id, sizeof(pt->message_id), "%s", message_id);
   snprintf(pt->account_name, sizeof(pt->account_name), "%s", acct.name);
   snprintf(pt->subject, sizeof(pt->subject), "%s", fetched_subject);
   snprintf(pt->from, sizeof(pt->from), "%s", fetched_from);

   snprintf(pending_id_out, pending_id_len, "%s", pt->pending_id);

   pthread_mutex_unlock(&s_email.pending_trash_mutex);
   return 0;
}

/** Execute trash on a resolved account + message_id */
static int execute_trash(email_account_t *acct, const char *message_id) {
   /* Gmail API path */
   if (is_gmail_api_account(acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_trash_message(token, message_id);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP path — parse composite "folder:uid" */
   char imap_folder[128] = "INBOX";
   const char *uid_str = message_id;

   const char *last_colon = strrchr(message_id, ':');
   if (last_colon) {
      size_t folder_len = last_colon - message_id;
      if (folder_len > 0 && folder_len < sizeof(imap_folder)) {
         memcpy(imap_folder, message_id, folder_len);
         imap_folder[folder_len] = '\0';
      }
      uid_str = last_colon + 1;
   }

   if (!validate_folder_name(imap_folder))
      return 1;

   char *endptr = NULL;
   unsigned long uid_val = strtoul(uid_str, &endptr, 10);
   if (!endptr || *endptr != '\0' || uid_val == 0 || uid_val > UINT32_MAX)
      return 1;

   email_conn_t conn;
   int rc = build_conn_for_account(acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_trash_message(&conn, imap_folder, (uint32_t)uid_val, is_gmail_imap_server(acct));
   sodium_memzero(&conn, sizeof(conn));
   return rc;
}

int email_service_confirm_trash(int user_id, const char *pending_id) {
   if (!pending_id || !pending_id[0])
      return 2;

   if (is_throttled(user_id)) {
      OLOG_WARNING("email: confirm_trash throttled for user %d", user_id);
      return 3;
   }

   pthread_mutex_lock(&s_email.pending_trash_mutex);
   expire_pending_trash_locked();

   /* Find matching pending trash */
   email_pending_trash_t *found = NULL;
   for (int i = 0; i < EMAIL_MAX_PENDING_TRASH; i++) {
      if (s_email.pending_trash[i].pending_id[0] && !s_email.pending_trash[i].used &&
          strcmp(s_email.pending_trash[i].pending_id, pending_id) == 0) {
         found = &s_email.pending_trash[i];
         break;
      }
   }

   if (!found) {
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      record_confirm_failure(user_id);
      return 2;
   }

   if (found->user_id != user_id) {
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      record_confirm_failure(user_id);
      OLOG_WARNING("email: confirm_trash user mismatch (pending=%d, caller=%d)", found->user_id,
                   user_id);
      return 2;
   }

   found->used = true;

   /* Copy data locally before releasing mutex */
   char message_id[192], account_name[128];
   snprintf(message_id, sizeof(message_id), "%s", found->message_id);
   snprintf(account_name, sizeof(account_name), "%s", found->account_name);

   sodium_memzero(found, sizeof(*found));
   pthread_mutex_unlock(&s_email.pending_trash_mutex);

   /* Resolve account and execute trash */
   email_account_t acct;
   if (find_account(user_id, account_name, &acct) != 0)
      return 1;

   return execute_trash(&acct, message_id);
}

/* =============================================================================
 * Archive (single-step, no confirmation)
 * ============================================================================= */

int email_service_archive(int user_id, const char *account_name, const char *message_id) {
   if (!message_id || !message_id[0])
      return 1;

   /* Resolve account */
   email_account_t acct;
   if (account_name && account_name[0]) {
      if (find_account(user_id, account_name, &acct) != 0)
         return 1;
   } else {
      if (find_account(user_id, NULL, &acct) != 0)
         return 1;
   }

   if (acct.read_only)
      return 2;

   /* Gmail API path */
   if (is_gmail_api_account(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (get_gmail_token(&acct, token, sizeof(token)) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_archive_message(token, message_id);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP path — parse composite "folder:uid" */
   char imap_folder[128] = "INBOX";
   const char *uid_str = message_id;

   const char *last_colon = strrchr(message_id, ':');
   if (last_colon) {
      size_t folder_len = last_colon - message_id;
      if (folder_len > 0 && folder_len < sizeof(imap_folder)) {
         memcpy(imap_folder, message_id, folder_len);
         imap_folder[folder_len] = '\0';
      }
      uid_str = last_colon + 1;
   }

   if (!validate_folder_name(imap_folder))
      return 1;

   char *endptr = NULL;
   unsigned long uid_val = strtoul(uid_str, &endptr, 10);
   if (!endptr || *endptr != '\0' || uid_val == 0 || uid_val > UINT32_MAX)
      return 1;

   email_conn_t conn;
   int rc = build_conn_for_account(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_archive_message(&conn, imap_folder, (uint32_t)uid_val, is_gmail_imap_server(&acct));
   sodium_memzero(&conn, sizeof(conn));
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
      return 2;

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
