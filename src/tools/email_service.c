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
#include "tools/email_client.h"
#include "tools/email_db.h"
#include "tools/email_parse.h"
#include "tools/email_service_internal.h"
#include "tools/gmail_client.h"
#include "tools/oauth_client.h"
#include "utils/string_utils.h"

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

int email_service_test_connection(int64_t account_id, bool *imap_ok, bool *smtp_ok) {
   *imap_ok = false;
   *smtp_ok = false;

   email_account_t acct;
   if (email_db_account_get(account_id, &acct) != 0)
      return 1;

   /* Gmail API path — single API call covers both directions */
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(&acct, token, sizeof(token), NULL) != 0) {
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
   int rc = email_svc_build_conn(&acct, &conn);
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

/* IMAP page_token <-> cursor.  An empty token is the first page; anything that
 * isn't a well-formed IMAP cursor (e.g. a Gmail token) is rejected rather than
 * silently restarting from page one. */
static int imap_page_from_token(const char *page_token, email_imap_page_t *page) {
   memset(page, 0, sizeof(*page));
   if (!page_token || !page_token[0])
      return EMAIL_RC_OK;
   if (!email_imap_page_token_parse(page_token, &page->before_uid, &page->uidvalidity))
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   return EMAIL_RC_OK;
}

static void imap_page_to_token(const email_imap_page_t *page, char *npt, size_t npt_len) {
   if (!npt || npt_len == 0 || page->next_before_uid == 0)
      return;
   /* Pin the epoch this page was read under; fall back to the one the incoming
    * cursor carried if the server's SELECT line wasn't observed. */
   uint32_t v = page->next_uidvalidity ? page->next_uidvalidity : page->uidvalidity;
   email_imap_page_token_format(page->next_before_uid, v, npt, npt_len);
}

/* A Gmail account handed an IMAP cursor: reject it the same way instead of
 * letting the Gmail API 400 into a generic "network error". */
static bool is_imap_page_token(const char *page_token) {
   uint32_t uid = 0, v = 0;
   return page_token && page_token[0] && email_imap_page_token_parse(page_token, &uid, &v);
}

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
   /* Copy the incoming cursor before clearing the outgoing one, so a caller may
    * pass the same buffer for both (page N's token in, page N+1's out). */
   char tok_in[EMAIL_PAGE_TOKEN_LEN];
   snprintf(tok_in, sizeof(tok_in), "%s", page_token ? page_token : "");
   page_token = tok_in;

   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   if (!validate_folder_name(folder))
      return EMAIL_RC_INVALID_FOLDER;

   email_account_t acct;
   int find_rc = email_svc_find_account(user_id, account_name, &acct);
   if (find_rc != EMAIL_RC_OK)
      return find_rc;

   if (count <= 0)
      count = acct.max_recent > 0 ? acct.max_recent : EMAIL_MAX_RECENT_DEFAULT;

   folder_norm_t norm;
   normalize_folder(folder, &acct, &norm);

   /* Gmail API path */
   if (email_svc_is_gmail_api(&acct)) {
      if (is_imap_page_token(page_token))
         return EMAIL_RC_INVALID_PAGE_TOKEN;
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(&acct, token, sizeof(token), NULL) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_fetch_recent(token, norm.gmail_query, count, unread_only, page_token, out, max,
                                  out_count, next_page_token, npt_len);
      sodium_memzero(token, sizeof(token));
      stamp_account(out, *out_count, &acct);
      return rc;
   }

   /* IMAP path: UID cursor paging */
   email_imap_page_t page;
   if (imap_page_from_token(page_token, &page) != EMAIL_RC_OK)
      return EMAIL_RC_INVALID_PAGE_TOKEN;

   email_conn_t conn;
   int rc = email_svc_build_conn(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_fetch_recent(&conn, norm.imap_folder, count, unread_only, &page, out, max, out_count);
   sodium_memzero(&conn, sizeof(conn));
   if (rc != 0 && page.stale)
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   if (rc == 0)
      imap_page_to_token(&page, next_page_token, npt_len);

   /* Populate message_id (folder:uid) for IMAP results, then stamp the account. */
   for (int i = 0; i < *out_count; i++)
      snprintf(out[i].message_id, sizeof(out[i].message_id), "%s:%u", norm.imap_folder, out[i].uid);
   stamp_account(out, *out_count, &acct);

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
                                 size_t npt_len,
                                 bool *auth_error) {
   /* Work on a copy so the caller may hand back the previous page's token buffer
    * as next_page_token (see email_service_recent). */
   email_search_params_t local_params = *params;
   params = &local_params;

   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';
   if (auth_error)
      *auth_error = false;

   folder_norm_t norm;
   normalize_folder(params->folder, acct, &norm);

   if (email_svc_is_gmail_api(acct)) {
      if (is_imap_page_token(params->page_token))
         return EMAIL_RC_INVALID_PAGE_TOKEN;
      email_search_params_t gmail_params = *params;
      snprintf(gmail_params.folder, sizeof(gmail_params.folder), "%s", norm.gmail_query);

      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(acct, token, sizeof(token), NULL) != 0) {
         /* Token fetch failed = the account can't authenticate (revoked/expired). */
         if (auth_error)
            *auth_error = true;
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_search(token, &gmail_params, max, out, max, out_count, next_page_token,
                            npt_len);
      sodium_memzero(token, sizeof(token));
      stamp_account(out, *out_count, acct);
      return rc;
   }

   /* IMAP path: UID cursor paging */
   email_imap_page_t page;
   if (imap_page_from_token(params->page_token, &page) != EMAIL_RC_OK)
      return EMAIL_RC_INVALID_PAGE_TOKEN;

   email_conn_t conn;
   int rc = email_svc_build_conn(acct, &conn);
   if (rc != EMAIL_SVC_CONN_OK) {
      /* Only a credential failure (bad token / undecryptable password) is a "login"
       * problem; a TLS/config refusal must NOT be mislabeled as bad credentials. */
      if (auth_error)
         *auth_error = (rc == EMAIL_SVC_CONN_AUTH);
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   bool imap_auth_denied = false;
   bool imap_timed_out = false;
   rc = email_search(&conn, norm.imap_folder, params, &page, out, max, out_count, &imap_auth_denied,
                     &imap_timed_out);
   if (rc != 0 && auth_error)
      *auth_error = imap_auth_denied;
   sodium_memzero(&conn, sizeof(conn));
   if (rc != 0 && page.stale)
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   if (rc == 0)
      imap_page_to_token(&page, next_page_token, npt_len);

   for (int i = 0; i < *out_count; i++)
      snprintf(out[i].message_id, sizeof(out[i].message_id), "%s:%u", norm.imap_folder, out[i].uid);
   stamp_account(out, *out_count, acct);

   /* Surface a timeout as a distinct code so the tool layer can hint the LLM to
    * bound the search with a date (large mailbox / no server FTS index). */
   if (rc != 0 && imap_timed_out)
      return EMAIL_RC_TIMEOUT;
   return rc;
}

int email_service_search(int user_id,
                         const char *account_name,
                         const email_search_params_t *params,
                         email_summary_t *out,
                         int max,
                         int *out_count,
                         char *next_page_token,
                         size_t npt_len,
                         char *warn_out,
                         size_t warn_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';
   if (warn_out && warn_len > 0)
      warn_out[0] = '\0';

   if (!validate_folder_name(params->folder))
      return EMAIL_RC_INVALID_FOLDER;

   /* Specific account requested — search just that one */
   if (account_name && account_name[0]) {
      email_account_t acct;
      int find_rc = email_svc_find_account(user_id, account_name, &acct);
      if (find_rc != EMAIL_RC_OK)
         return find_rc;
      return search_single_account(&acct, params, out, max, out_count, next_page_token, npt_len,
                                   NULL);
   }

   /* No account specified — search ALL enabled accounts and merge results.
    * Pagination only applies to single-account searches, so drop any page_token:
    * a cursor belongs to one account and would 400 the others (Gmail) or be
    * rejected as foreign (IMAP), turning a fresh search into spurious errors. */
   email_search_params_t first_page = *params;
   first_page.page_token[0] = '\0';
   params = &first_page;
   email_account_t accounts[16];
   int acct_count = 0;
   email_db_account_list(user_id, accounts, 16, &acct_count);
   if (acct_count <= 0)
      return EMAIL_RC_NO_ACCOUNTS;
   int enabled_seen = 0;
   int any_error = 0;
   int any_timeout = 0;
   int total = 0;
   for (int i = 0; i < acct_count && total < max; i++) {
      if (!accounts[i].enabled)
         continue;
      enabled_seen = 1;

      int this_count = 0;
      int remaining = max - total;
      bool acct_auth = false;
      int rc = search_single_account(&accounts[i], params, out + total, remaining, &this_count,
                                     NULL, 0, &acct_auth);
      if (rc == 0) {
         total += this_count;
      } else {
         /* Per-account transport/upstream failure.  Logged (not silent) so a
          * genuine backend problem is diagnosable from the log; the search still
          * continues across the remaining accounts.  Also surfaced to warn_out so
          * the LLM can tell the user results are partial — an auth failure would
          * otherwise be completely invisible when other accounts return matches. */
         any_error = 1;
         bool acct_timeout = (rc == EMAIL_RC_TIMEOUT);
         if (acct_timeout)
            any_timeout = 1;
         const char *reason = acct_auth      ? "login failed"
                              : acct_timeout ? "timed out — narrow with a date range"
                                             : "unreachable";
         OLOG_WARNING("email: search failed for account '%s' (rc=%d, %s)", accounts[i].name, rc,
                      reason);
         if (warn_out && warn_len > 0) {
            size_t used = strlen(warn_out);
            snprintf(warn_out + used, warn_len - used, "%s%s (%s)", used > 0 ? ", " : "",
                     accounts[i].name, reason);
         }
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
   if (!any_error)
      return EMAIL_RC_OK;
   return any_timeout ? EMAIL_RC_TIMEOUT : EMAIL_RC_FAILURE;
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
                                    EMAIL_MAX_FETCH_RESULTS, &sent_count, npt, sizeof(npt), NULL,
                                    0);
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
         str_excerpt_line(t->from, 100, from, sizeof(from));
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

int email_service_list_folders(int user_id, const char *account_name, char *out, size_t out_len) {
   if (!out || out_len < 2)
      return EMAIL_RC_FAILURE;
   out[0] = '\0';

   email_account_t acct;
   int find_rc = email_svc_find_account(user_id, account_name, &acct);
   if (find_rc != EMAIL_RC_OK)
      return find_rc;

   /* Gmail API path */
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(&acct, token, sizeof(token), NULL) != 0) {
         sodium_memzero(token, sizeof(token));
         return 1;
      }
      int rc = gmail_list_labels(token, out, out_len);
      sodium_memzero(token, sizeof(token));
      return rc;
   }

   /* IMAP path */
   email_conn_t conn;
   int rc = email_svc_build_conn(&acct, &conn);
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
   int read_rc = email_svc_read_single(&acct, message_id, &headers, &msg, NULL, false);
   char fetched_subject[256] = "(unknown)";
   char fetched_from[128] = "(unknown)";
   if (read_rc == 0) {
      /* snprintf cuts bytes; a character cut in two would reach the
       * confirmation text as invalid UTF-8, so drop any such tail. */
      snprintf(fetched_subject, sizeof(fetched_subject), "%s", msg.subject);
      utf8_trim_incomplete(fetched_subject);
      char from[sizeof(msg.from_name) + 1 + sizeof(msg.from_addr)];
      snprintf(from, sizeof(from), "%s%s%s", msg.from_name, msg.from_name[0] ? " " : "",
               msg.from_addr);
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

/** Execute trash on a resolved account + message_id */
static int execute_trash(email_account_t *acct, const char *message_id) {
   /* Gmail API path */
   if (email_svc_is_gmail_api(acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(acct, token, sizeof(token), NULL) != 0) {
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
   int rc = email_svc_build_conn(acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_trash_message(&conn, imap_folder, (uint32_t)uid_val);
   sodium_memzero(&conn, sizeof(conn));
   return rc;
}

int email_service_confirm_trash(int user_id, const char *pending_id, const turn_origin_t *origin) {
   if (!pending_id || !pending_id[0])
      return EMAIL_CONFIRM_RC_NOT_FOUND;

   if (is_throttled(user_id)) {
      OLOG_WARNING("email: confirm_trash throttled for user %d", user_id);
      return EMAIL_CONFIRM_RC_THROTTLED;
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
      return EMAIL_CONFIRM_RC_NOT_FOUND;
   }

   if (found->user_id != user_id) {
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      record_confirm_failure(user_id);
      OLOG_WARNING("email: confirm_trash user mismatch (pending=%d, caller=%d)", found->user_id,
                   user_id);
      return EMAIL_CONFIRM_RC_NOT_FOUND;
   }

   /* As for a draft: a later turn of the session that asked. */
   const turn_origin_rc_t orc = turn_origin_check(&found->origin, origin);
   if (orc != TURN_ORIGIN_OK) {
      pthread_mutex_unlock(&s_email.pending_trash_mutex);
      OLOG_WARNING("email: confirm_trash refused (%s)", turn_origin_refusal(orc));
      return email_confirm_rc(orc);
   }

   found->used = true;

   /* Copy data locally before releasing mutex */
   char message_id[192], account_name[128];
   snprintf(message_id, sizeof(message_id), "%s", found->message_id);
   snprintf(account_name, sizeof(account_name), "%s", found->account_name);

   sodium_memzero(found, sizeof(*found));
   pthread_mutex_unlock(&s_email.pending_trash_mutex);

   /* Re-resolve the account (not re-choose) and re-check writability — an account
    * flipped read-only, disabled, or deleted between 'trash' and 'confirm_trash'
    * must not still execute the delete (mirrors confirm_send). */
   email_account_t acct;
   if (email_svc_find_account(user_id, account_name, &acct) != EMAIL_RC_OK || acct.read_only) {
      OLOG_WARNING("email: confirm_trash account '%s' no longer available/writable", account_name);
      sodium_memzero(&acct, sizeof(acct));
      return EMAIL_CONFIRM_RC_ACCOUNT_GONE;
   }

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
      if (email_svc_find_account(user_id, account_name, &acct) != 0)
         return 1;
   } else {
      if (email_svc_find_account(user_id, NULL, &acct) != 0)
         return 1;
   }

   if (acct.read_only)
      return EMAIL_ACCT_RC_READONLY;

   /* Gmail API path */
   if (email_svc_is_gmail_api(&acct)) {
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token(&acct, token, sizeof(token), NULL) != 0) {
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
   int rc = email_svc_build_conn(&acct, &conn);
   if (rc != 0) {
      sodium_memzero(&conn, sizeof(conn));
      return 1;
   }

   rc = email_archive_message(&conn, imap_folder, (uint32_t)uid_val);
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
