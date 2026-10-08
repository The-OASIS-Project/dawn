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
 * Email service: listing and searching a folder (email_service_recent,
 * email_service_search), with folder-name normalization and page cursors.
 */

#define _GNU_SOURCE /* strcasestr */

#include <sodium.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_client_internal.h"
#include "tools/email_db.h"
#include "tools/email_fanout.h"
#include "tools/email_parse.h"
#include "tools/email_service.h"
#include "tools/email_service_internal.h"
#include "tools/email_transfer.h"
#include "tools/gmail_client.h"
#include "tools/oauth_client.h"

/** IMAP server is Gmail (regardless of auth type) */
static bool is_gmail_imap_server(const email_account_t *acct) {
   return strcasestr(acct->imap_server, "gmail.com") != NULL;
}

/* The folder-name check (email_service.c), under a shorter local name. */
#define validate_folder_name email_service_validate_folder_name

/** Folder normalization result */
typedef struct {
   char gmail_query[256]; /* Gmail search fragment (e.g. "in:sent", "label:\"Receipts\"") */
   char imap_folder[128]; /* IMAP folder name (e.g. "INBOX", "[Gmail]/Sent Mail") */
   bool imap_all;         /* "all" on a generic IMAP server: the folder it marks \All, if any */
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
            /* Not named on generic IMAP: INBOX, unless (for "all") the server
             * marks a folder \All, looked up once connected (imap_all_folder). */
            snprintf(out->imap_folder, sizeof(out->imap_folder), "INBOX");
            out->imap_all = strcasecmp(folder_map[i].user_name, "all") == 0;
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

/* "all" on a generic IMAP server: the folder it marks \All (RFC 6154), when it
 * marks one; else INBOX stays. */
static void imap_all_folder(const email_conn_t *conn, folder_norm_t *norm) {
   if (!norm->imap_all)
      return;
   char all[sizeof(norm->imap_folder)];
   if (email_imap_all_mail_folder(conn, all, sizeof(all)) && validate_folder_name(all))
      snprintf(norm->imap_folder, sizeof(norm->imap_folder), "%s", all);
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
/* Why a transfer failed, given the backend said only "failed": a transfer the
 * caller stopped (its cancel flag set) fails the same way, and is a cancel. */
static email_err_t failed_or_stopped(void) {
   const atomic_bool *cancel = email_transfer_thread_cancel();
   return cancel && atomic_load(cancel) ? EMAIL_ERR_CANCELLED : EMAIL_ERR_FAILED;
}

static void stamp_account(email_summary_t *out, int n, const email_account_t *acct) {
   for (int i = 0; i < n; i++) {
      snprintf(out[i].account_name, sizeof(out[i].account_name), "%s", acct->name);
      snprintf(out[i].account_addr, sizeof(out[i].account_addr), "%s", acct->username);
   }
}

/* IMAP rows' ids as DAWN issues them ("folder:uid.uidvalidity"); a row whose id
 * wouldn't fit keeps none rather than a cut one. */
static void imap_row_ids(email_summary_t *out, int n, const char *folder, uint32_t uidvalidity) {
   for (int i = 0; i < n; i++) {
      if (!email_imap_id_format(folder, out[i].uid, uidvalidity, out[i].message_id,
                                sizeof(out[i].message_id)))
         OLOG_WARNING("email: an IMAP id didn't fit (folder name of %zu bytes)", strlen(folder));
   }
}

static int recent_on_account(const email_account_t *acct,
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
                         email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (ext) {
      ext->inbox_unread = -1;
      ext->uidvalidity = 0;
      ext->rows_missing = 0;
      ext->imap_folder[0] = '\0';
   }

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
   int find_rc = email_svc_resolve(user_id, account_name, target, &acct);
   if (find_rc != EMAIL_RC_OK) {
      *err = email_svc_account_err(find_rc);
      return find_rc;
   }

   if (count <= 0)
      count = acct.max_recent > 0 ? acct.max_recent : EMAIL_MAX_RECENT_DEFAULT;
   const int rc = recent_on_account(&acct, folder, count, unread_only, page_token, out, max,
                                    out_count, next_page_token, npt_len, ext, target, err);
   sodium_memzero(&acct, sizeof(acct));
   return rc;
}

/* email_service_recent on a resolved account. */
static int recent_on_account(const email_account_t *acct,
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
                             email_err_t *err) {
   folder_norm_t norm;
   normalize_folder(folder, acct, &norm);
   /* The unread count comes with the inbox's first page only: decided on the
    * folder asked for, since other folders can fall back to INBOX on IMAP. */
   const bool asked_inbox = !folder || !folder[0] || strcasecmp(folder, "inbox") == 0;
   const bool want_unread = ext && ext->want_inbox_unread && asked_inbox &&
                            !(page_token && page_token[0]) && !ext->at_or_before;
   int *inbox_unread = ext ? &ext->inbox_unread : NULL;

   /* Gmail API path */
   if (email_svc_is_gmail_api(acct)) {
      if (is_imap_page_token(page_token)) {
         *err = EMAIL_ERR_CURSOR_STALE;
         return EMAIL_RC_INVALID_PAGE_TOKEN;
      }
      char token[OAUTH_TOKEN_BUF_SIZE];
      if (email_svc_gmail_token_err(acct, token, sizeof(token), err) != 0)
         return 1;
      int rc = gmail_fetch_recent(token, norm.gmail_query, count, unread_only,
                                  ext ? ext->at_or_before : 0, page_token, out, max, out_count,
                                  next_page_token, npt_len, want_unread ? inbox_unread : NULL,
                                  ext ? &ext->rows_missing : NULL);
      sodium_memzero(token, sizeof(token));
      stamp_account(out, *out_count, acct);
      *err = rc == 0 ? EMAIL_ERR_NONE : failed_or_stopped();
      return rc;
   }

   /* IMAP path: UID cursor paging */
   email_imap_page_t page;
   if (imap_page_from_token(page_token, &page) != EMAIL_RC_OK) {
      *err = EMAIL_ERR_CURSOR_STALE;
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   }

   email_svc_lease_t lease;
   if (email_svc_lease_begin(acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK)
      return EMAIL_RC_FAILURE;
   /* Taken before connecting: an account edit while this list runs makes
    * what the probe below learns stale, and it's dropped. */
   const uint64_t caps_gen = want_unread ? email_svc_caps_gen(acct->id) : 0;
   email_conn_t conn;
   int rc = email_svc_build_conn(acct, &conn);
   if (rc != EMAIL_SVC_CONN_OK) {
      *err = email_svc_conn_err(rc);
      rc = 1;
   } else {
      imap_all_folder(&conn, &norm);
      rc = email_fetch_recent(&conn, norm.imap_folder, count, unread_only, &page, out, max,
                              out_count, want_unread ? inbox_unread : NULL, err);
      /* The panel's first inbox page learns what the account can move to, while
       * the lease is held (cached roles cost nothing; else one probe an hour). */
      if (rc == 0 && want_unread && email_svc_caps_probe_due(acct->id)) {
         /* Not the list's to cancel: a newer list replacing this one mustn't
          * leave the account's buttons unknown for an hour. */
         const atomic_bool *saved = email_transfer_scope_cancel(NULL);
         email_imap_roles_t roles;
         if (email_imap_roles_get(&conn, true, &roles))
            email_svc_caps_from_roles(acct->id, caps_gen, &roles);
         else
            email_svc_caps_probe_failed(acct->id);
         email_transfer_scope_cancel(saved);
      }
   }
   email_svc_lease_end(&lease);
   sodium_memzero(&conn, sizeof(conn));
   if (rc != 0 && page.stale) {
      *err = EMAIL_ERR_CURSOR_STALE;
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   }
   if (rc == 0)
      imap_page_to_token(&page, next_page_token, npt_len);
   if (ext) {
      ext->uidvalidity = page.next_uidvalidity ? page.next_uidvalidity : page.uidvalidity;
      snprintf(ext->imap_folder, sizeof(ext->imap_folder), "%s", norm.imap_folder);
   }

   /* Each row's id, pinned to the epoch this listing saw, then the account. */
   imap_row_ids(out, *out_count, norm.imap_folder,
                page.next_uidvalidity ? page.next_uidvalidity : page.uidvalidity);
   stamp_account(out, *out_count, acct);

   return rc;
}

/* An IMAP search key is sent as a quoted string, which servers take as ASCII:
 * non-ASCII text would match nothing (or fail) rather than what was asked. */
static bool imap_query_ascii(const email_search_params_t *params) {
   const char *keys[] = { params->from, params->subject, params->text };
   for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
      for (const unsigned char *p = (const unsigned char *)keys[k]; *p; p++) {
         if (*p >= 0x80)
            return false;
      }
   }
   return true;
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
                                 const email_target_t *target,
                                 int lease_wait_s,
                                 uint32_t *uidvalidity,
                                 char *imap_folder,
                                 email_err_t *err,
                                 int *missing) {
   /* Work on a copy so the caller may hand back the previous page's token buffer
    * as next_page_token (see email_service_recent). */
   email_search_params_t local_params = *params;
   params = &local_params;

   *out_count = 0;
   *err = EMAIL_ERR_FAILED;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   folder_norm_t norm;
   normalize_folder(params->folder, acct, &norm);

   if (email_svc_is_gmail_api(acct)) {
      if (is_imap_page_token(params->page_token)) {
         *err = EMAIL_ERR_CURSOR_STALE;
         return EMAIL_RC_INVALID_PAGE_TOKEN;
      }
      email_search_params_t gmail_params = *params;
      snprintf(gmail_params.folder, sizeof(gmail_params.folder), "%s", norm.gmail_query);

      char token[OAUTH_TOKEN_BUF_SIZE];
      /* A failed token = the account can't authenticate (revoked/expired). */
      if (email_svc_gmail_token_err(acct, token, sizeof(token), err) != 0)
         return 1;
      int rc = gmail_search(token, &gmail_params, max, out, max, out_count, next_page_token,
                            npt_len, missing);
      sodium_memzero(token, sizeof(token));
      stamp_account(out, *out_count, acct);
      *err = rc == 0 ? EMAIL_ERR_NONE : failed_or_stopped();
      return rc;
   }

   if (!imap_query_ascii(params)) {
      *err = EMAIL_ERR_UNSUPPORTED_QUERY;
      return EMAIL_RC_FAILURE;
   }

   /* IMAP path: UID cursor paging */
   email_imap_page_t page;
   if (imap_page_from_token(params->page_token, &page) != EMAIL_RC_OK) {
      *err = EMAIL_ERR_CURSOR_STALE;
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   }

   email_svc_lease_t lease;
   if (email_svc_lease_begin(acct, target, lease_wait_s, &lease, err) != EMAIL_RC_OK)
      return EMAIL_RC_FAILURE;
   email_conn_t conn;
   int rc = email_svc_build_conn(acct, &conn);
   if (rc != EMAIL_SVC_CONN_OK) {
      /* Only a credential failure (bad token / undecryptable password) is a "login"
       * problem; a TLS/config refusal must NOT be mislabeled as bad credentials. */
      *err = email_svc_conn_err(rc);
      rc = 1;
   } else {
      imap_all_folder(&conn, &norm);
      rc = email_search(&conn, norm.imap_folder, params, &page, out, max, out_count, err);
   }
   email_svc_lease_end(&lease);
   sodium_memzero(&conn, sizeof(conn));
   if (rc != 0 && page.stale) {
      *err = EMAIL_ERR_CURSOR_STALE;
      return EMAIL_RC_INVALID_PAGE_TOKEN;
   }
   if (rc == 0)
      imap_page_to_token(&page, next_page_token, npt_len);
   if (uidvalidity)
      *uidvalidity = page.next_uidvalidity ? page.next_uidvalidity : page.uidvalidity;
   if (imap_folder) /* the folder searched, as resolved ("all" may name the server's own) */
      snprintf(imap_folder, sizeof(((email_search_report_t *)0)->imap_folder), "%s",
               norm.imap_folder);

   imap_row_ids(out, *out_count, norm.imap_folder,
                page.next_uidvalidity ? page.next_uidvalidity : page.uidvalidity);
   stamp_account(out, *out_count, acct);

   /* Surface a timeout as a distinct code so the tool layer can hint the LLM to
    * bound the search with a date (large mailbox / no server FTS index). */
   if (rc != 0 && *err == EMAIL_ERR_FAILED)
      *err = failed_or_stopped();
   if (rc != 0 && *err == EMAIL_ERR_TIMEOUT)
      return EMAIL_RC_TIMEOUT;
   return rc;
}

static const char *search_failure_reason(email_err_t err) {
   switch (err) {
      case EMAIL_ERR_AUTH_FAILED:
      case EMAIL_ERR_AUTH_REVOKED:
         return "login failed";
      case EMAIL_ERR_TIMEOUT:
         return "timed out — narrow with a date range";
      case EMAIL_ERR_UNSUPPORTED_QUERY:
         return "can't search non-ASCII text";
      case EMAIL_ERR_BUSY:
         return "busy with another request";
      default:
         return "unreachable";
   }
}

/* One account of a search across all of them (email_fanout_run). */
typedef struct {
   email_account_t *accounts;
   const email_search_params_t *params;
} fanout_search_t;

static void fanout_search_one(void *ctx, int index, email_fanout_slot_t *slot, int max) {
   fanout_search_t *fs = ctx;
   /* A busy account is reported, not waited out. */
   slot->rc = search_single_account(&fs->accounts[index], fs->params, slot->rows, max, &slot->count,
                                    NULL, 0, NULL, EMAIL_LEASE_FANOUT_WAIT_SEC, NULL, NULL,
                                    &slot->err, &slot->missing);
}

/* The accounts a search across all of them couldn't search (email_fanout_merge). */
typedef struct {
   const email_account_t *accounts;
   char *warn_out;
   size_t warn_len;
   email_search_report_t *report;
   int any_error;
   int any_timeout;
} fanout_fail_t;

static void fanout_failed(void *ctx, int index, const email_fanout_slot_t *slot) {
   fanout_fail_t *ff = ctx;
   const email_account_t *acct = &ff->accounts[index];
   /* Logged, and in warn_out so the model can tell the user the results are
    * partial: an auth failure would otherwise be invisible when other accounts
    * return matches. */
   ff->any_error = 1;
   if (slot->rc == EMAIL_RC_TIMEOUT)
      ff->any_timeout = 1;
   const char *reason = search_failure_reason(slot->err);
   OLOG_WARNING("email: search failed for account '%s' (rc=%d, %s)", acct->name, slot->rc, reason);
   if (ff->warn_out && ff->warn_len > 0) {
      size_t used = strlen(ff->warn_out);
      snprintf(ff->warn_out + used, ff->warn_len - used, "%s%s (%s)", used > 0 ? ", " : "",
               acct->name, reason);
   }
   if (ff->report->failed_count < EMAIL_MAX_ACCOUNTS) {
      ff->report->failed[ff->report->failed_count].account_id = acct->id;
      ff->report->failed[ff->report->failed_count].err = slot->err;
      ff->report->failed_count++;
   }
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
                         size_t warn_len,
                         const email_target_t *target,
                         email_search_report_t *report) {
   email_search_report_t report_local;
   if (!report)
      report = &report_local;
   memset(report, 0, sizeof(*report));
   report->err = EMAIL_ERR_FAILED;

   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';
   if (warn_out && warn_len > 0)
      warn_out[0] = '\0';

   if (!validate_folder_name(params->folder))
      return EMAIL_RC_INVALID_FOLDER;
   if (max <= 0)
      return EMAIL_RC_FAILURE;

   /* One account (by id, or named) — search just that one */
   if (target || (account_name && account_name[0])) {
      email_account_t acct;
      int find_rc = email_svc_resolve(user_id, account_name, target, &acct);
      if (find_rc != EMAIL_RC_OK) {
         report->err = email_svc_account_err(find_rc);
         return find_rc;
      }
      const int rc = search_single_account(&acct, params, out, max, out_count, next_page_token,
                                           npt_len, target, EMAIL_LEASE_WAIT_SEC,
                                           &report->uidvalidity, report->imap_folder, &report->err,
                                           &report->rows_missing);
      sodium_memzero(&acct, sizeof(acct));
      return rc;
   }

   /* No account specified — search ALL enabled accounts and merge results.
    * Pagination only applies to single-account searches, so drop any page_token:
    * a cursor belongs to one account and would 400 the others (Gmail) or be
    * rejected as foreign (IMAP), turning a fresh search into spurious errors. */
   email_search_params_t first_page = *params;
   first_page.page_token[0] = '\0';
   params = &first_page;
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int acct_count = 0;
   email_db_account_list(user_id, accounts, EMAIL_MAX_ACCOUNTS, &acct_count);
   if (acct_count <= 0) {
      report->err = EMAIL_ERR_NO_ACCOUNT;
      return EMAIL_RC_NO_ACCOUNTS;
   }
   /* The enabled ones, in list order (the order a date tie is merged in). */
   int enabled = 0;
   for (int i = 0; i < acct_count; i++) {
      if (!accounts[i].enabled)
         continue;
      if (enabled != i)
         accounts[enabled] = accounts[i];
      enabled++;
   }
   if (enabled == 0) {
      sodium_memzero(accounts, sizeof(accounts));
      report->err = EMAIL_ERR_NO_ACCOUNT;
      return EMAIL_RC_NO_ACCOUNTS;
   }

   /* Every account at once: the search takes as long as the slowest one, not
    * all of them added up.  The rows are then merged newest first across the
    * accounts, so the limit keeps the newest mail wherever it is. */
   fanout_search_t fs = { .accounts = accounts, .params = params };
   email_fanout_slot_t slots[EMAIL_MAX_ACCOUNTS];
   if (email_fanout_run(enabled, max, fanout_search_one, &fs, slots) != 0) {
      sodium_memzero(accounts, sizeof(accounts));
      return EMAIL_RC_FAILURE;
   }
   for (int i = 0; i < enabled; i++) {
      if (slots[i].rc == 0)
         OLOG_INFO("email: search '%s' %lld ms, %d results", accounts[i].name,
                   (long long)slots[i].ms, slots[i].count);
      else
         OLOG_INFO("email: search '%s' %lld ms, failed (%s)", accounts[i].name,
                   (long long)slots[i].ms, search_failure_reason(slots[i].err));
   }

   fanout_fail_t ff = { .accounts = accounts,
                        .warn_out = warn_out,
                        .warn_len = warn_len,
                        .report = report };
   int total = 0;
   const bool whole = email_fanout_merge(slots, enabled, max, out, &total, fanout_failed, &ff);
   for (int i = 0; i < enabled; i++) {
      if (slots[i].rc == 0)
         report->rows_missing += slots[i].missing;
   }
   email_fanout_free(slots, enabled);
   sodium_memzero(accounts, sizeof(accounts));
   if (!whole) {
      /* Stopped: what was found is kept. */
      *out_count = total;
      report->err = EMAIL_ERR_CANCELLED;
      return EMAIL_RC_FAILURE;
   }
   const int any_error = ff.any_error;
   const int any_timeout = ff.any_timeout;

   *out_count = total;
   if (total > 0) {
      report->err = EMAIL_ERR_NONE;
      return EMAIL_RC_OK;
   }
   /* Zero results across all enabled accounts: distinguish a genuine no-match
    * (every account searched OK, just nothing matched) from a real failure (at
    * least one account errored).  Reporting no-match as FAILURE makes the LLM
    * believe email is down and abandon the search instead of broadening it. */
   if (!any_error) {
      report->err = EMAIL_ERR_NONE;
      return EMAIL_RC_OK;
   }
   report->err = report->failed[0].err;
   return any_timeout ? EMAIL_RC_TIMEOUT : EMAIL_RC_FAILURE;
}
