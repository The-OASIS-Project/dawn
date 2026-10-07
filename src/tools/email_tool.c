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
 * Email LLM tool — voice-controlled IMAP/SMTP email access.
 * Actions: recent, read, search, folders, send, confirm_send, trash, confirm_trash, archive,
 * accounts
 */

#include "tools/email_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/scheduled_context.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/contact_resolve.h"
#include "tools/email_digest.h"
#include "tools/email_display.h"
#include "tools/email_parse.h"
#include "tools/email_service.h"
#include "tools/oauth_client.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RESULT_BUF_SIZE 16384
#define MAX_EMAIL_RESULTS EMAIL_MAX_FETCH_RESULTS

/* An empty page that still carries a page_token: a large mailbox's search stopped
 * (time or window budget) before reaching older mail.  Say so, or the model reads
 * the empty page as "there is no such mail". */
#define EMAIL_PARTIAL_SCAN_NOTE                                                             \
   "No matches in the most recent part of this mailbox, but older mail has not been "       \
   "searched yet. This is NOT a confirmed \"no results\" — pass the page_token below to " \
   "continue searching older mail."

/* =============================================================================
 * Config (TOOL_CAP_DANGEROUS requires enabled = true as first field)
 * ============================================================================= */

typedef struct {
   bool enabled;
} email_tool_config_t;

static email_tool_config_t s_config;

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *email_tool_callback(const char *action, char *value, int *should_respond);
static int email_tool_init(void);
static void email_tool_cleanup(void);
static bool email_tool_available(void);

/* =============================================================================
 * JSON Helpers (same as calendar_tool.c)
 * ============================================================================= */

static const char *json_get_str(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_int(val);
}

/* =============================================================================
 * Access Summary Footer (tells LLM which accounts are read-only)
 * ============================================================================= */

static int append_access_summary(char *buf, int pos, size_t buf_len, int user_id) {
   char writable[512] = { 0 }, ro[512] = { 0 };
   if (email_service_get_access_summary(user_id, writable, sizeof(writable), ro, sizeof(ro)) !=
       EMAIL_ACCESS_ALL_WRITABLE) {
      pos += snprintf(buf + pos, buf_len - pos,
                      "\n\nWritable accounts: %s\nRead-only accounts (no sending): %s",
                      writable[0] ? writable : "(none)", ro);
   }
   return pos;
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_accounts(int user_id) {
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int count = email_service_list_accounts(user_id, accounts, EMAIL_MAX_ACCOUNTS);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");

   int pos = 0;
   if (count <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No email accounts configured.");
      return buf;
   }

   pos += snprintf(buf, RESULT_BUF_SIZE, "Email accounts (%d):\n", count);

   for (int i = 0; i < count && pos < RESULT_BUF_SIZE - 256; i++) {
      const char *status = accounts[i].enabled ? "" : " [DISABLED]";
      const char *ro = accounts[i].read_only ? " [READ-ONLY]" : "";
      const char *auth = strcmp(accounts[i].auth_type, "oauth") == 0 ? " (Google OAuth)" : "";

      pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "- %s%s%s%s (%s)\n", accounts[i].name, ro,
                      status, auth, accounts[i].username);
   }

   pos = append_access_summary(buf, pos, RESULT_BUF_SIZE, user_id);
   return buf;
}

/**
 * @brief Format an actionable error message for the LLM based on an
 *        email_service rc + (when relevant) the user-supplied account/folder
 *        names that failed validation.
 *
 * Caller frees.  Returns a heap-allocated string identical in lifecycle to
 * every other error return in this file (strdup'd or malloc'd, never NULL).
 *
 * Every caller of this helper is a genuine hard failure (unknown/absent
 * account, invalid folder, revoked OAuth, network/upstream error), so each
 * message carries TOOL_RESULT_ERROR_MARK to red the WebUI tool pill.  The mark
 * is stripped before the LLM reads the text (see TOOL_DEVELOPMENT_GUIDE.md
 * § Signaling a Failure).
 */
/* Room a row may need: the quoted sender, the fields as stored, and the fixed text. */
#define EMAIL_FROM_MAX \
   (2 * sizeof(((email_summary_t *)0)->from_name) + sizeof(((email_summary_t *)0)->from_addr) + 8)
#define EMAIL_ACCT_LABEL_MAX 300
#define EMAIL_LISTING_TAIL_ROOM 1024
#define EMAIL_ROW_MAX                                                                 \
   (EMAIL_FROM_MAX + sizeof(((email_summary_t *)0)->subject) + EMAIL_ACCT_LABEL_MAX + \
    sizeof(((email_summary_t *)0)->date_str) + sizeof(((email_summary_t *)0)->message_id) + 128)

/* Appends to @p buf (RESULT_BUF_SIZE) at @p pos; never moves pos past the buffer.
 * A text that doesn't fit whole is left out, so the result has no partial line. */
static int append_bounded(char *buf, int pos, const char *text) {
   size_t n = strlen(text);
   if (pos < 0 || (size_t)pos + n >= RESULT_BUF_SIZE)
      return pos;
   memcpy(buf + pos, text, n + 1);
   return pos + (int)n;
}

/* A listing's rows (recent and search), from @p pos in a RESULT_BUF_SIZE
 * buffer.  The sender's name is quoted, so it can't pass for an address.  A row
 * that doesn't fit whole ends the listing (senders control most of a row) and
 * sets @p cut: the page token would skip the rows left out, so the caller drops it. */
static int append_summary_rows(char *buf,
                               int pos,
                               const email_summary_t *emails,
                               int count,
                               bool *cut) {
   *cut = false;
   for (int i = 0; i < count; i++) {
      /* Prefer "name (address)" so a generic display name like "Gmail" still
       * identifies which inbox; collapse to one when name == address. */
      char acctlabel[EMAIL_ACCT_LABEL_MAX];
      const char *an = emails[i].account_name;
      const char *aa = emails[i].account_addr;
      if (aa[0] && an[0] && strcmp(an, aa) != 0)
         snprintf(acctlabel, sizeof(acctlabel), "%s (%s)", an, aa);
      else
         snprintf(acctlabel, sizeof(acctlabel), "%s", aa[0] ? aa : (an[0] ? an : "?"));
      char from[EMAIL_FROM_MAX];
      email_display_mailbox(emails[i].from_name, emails[i].from_addr, from, sizeof(from));
      char row[EMAIL_ROW_MAX];
      snprintf(row, sizeof(row),
               "\n%d. From: %s\n   Subject: %s%s%s\n   Account: %s | Date: %s\n   [ID: "
               "%s]\n",
               i + 1, from, emails[i].subject, emails[i].unread ? " [UNREAD]" : "",
               emails[i].replied == EMAIL_REPLIED_YES ? " [replied]" : "", acctlabel,
               emails[i].date_str, emails[i].message_id);
      /* Keep room for the page token and the note on unreachable accounts. */
      if ((size_t)pos + strlen(row) + EMAIL_LISTING_TAIL_ROOM >= RESULT_BUF_SIZE) {
         char note[160];
         snprintf(note, sizeof(note),
                  "\n[%d more not shown: the listing is full. Ask again with a smaller "
                  "count to see them.]\n",
                  count - i);
         *cut = true;
         return append_bounded(buf, pos, note);
      }
      pos = append_bounded(buf, pos, row);
   }
   return pos;
}

/* The page-token line after a listing, appended only whole. */
static int append_page_token(char *buf, int pos, const char *token) {
   char line[384];
   snprintf(line, sizeof(line),
            "\n[More results available. Use page_token: \"%s\" to fetch next page]", token);
   return append_bounded(buf, pos, line);
}

static char *email_rc_to_error(int rc, const char *op, const char *account, const char *folder) {
   char *msg = malloc(384);
   if (!msg)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   switch (rc) {
      case EMAIL_RC_UNKNOWN_ACCOUNT:
         if (account && account[0])
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: no email account matches '%s'. Call action='accounts' to see "
                     "configured account names; do not invent email addresses.",
                     account);
         else
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: no email account matches the request. Call action='accounts' to "
                     "enumerate.");
         break;
      case EMAIL_RC_NO_ACCOUNTS:
         snprintf(msg, 384,
                  TOOL_RESULT_ERROR_MARK
                  "Error: no email accounts configured (or all are disabled). Tell the user to "
                  "add or enable one via WebUI Settings -> Email.");
         break;
      case EMAIL_RC_INVALID_FOLDER:
         if (folder && folder[0])
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: invalid folder name '%s'. Valid: inbox, sent, drafts, trash, "
                     "spam, starred, important, all.",
                     folder);
         else
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: invalid folder name. Valid: inbox, sent, drafts, trash, spam, "
                     "starred, important, all.");
         break;
      case EMAIL_RC_NOT_FOUND:
         snprintf(msg, 384,
                  TOOL_RESULT_ERROR_MARK
                  "Error: no message with that id exists in %s. The id may be stale, mistyped, "
                  "or the message was moved/deleted. Do NOT retry the same id — get a fresh id "
                  "from 'recent', 'search', or 'digest' (copy it exactly).",
                  (account && account[0]) ? account : "any of your accounts");
         break;
      case EMAIL_RC_INVALID_PAGE_TOKEN:
         snprintf(msg, 384,
                  TOOL_RESULT_ERROR_MARK
                  "Error: page_token is not valid here (it belongs to a different kind of "
                  "account, is malformed, or the mailbox changed since it was issued). Do NOT "
                  "retry it — call '%s' without page_token to get a fresh first page and token. "
                  "Always reuse a page_token with the same account it came from.",
                  op);
         break;
      case EMAIL_RC_TIMEOUT:
         /* Generic fallback.  The 'search' path builds a date-aware message in
          * handle_search (it knows whether a date bound was already supplied),
          * so this branch only serves other ops. */
         snprintf(msg, 384,
                  TOOL_RESULT_ERROR_MARK
                  "Error: email %s timed out (the server was slow to respond). Retry once; if "
                  "persistent, the mailbox may be very large or the backend is slow.",
                  op);
         break;
      default: {
         /* If the underlying oauth_refresh detected invalid_grant, the
          * tokens were revoked at the provider — a generic "network
          * error" message is misleading and tells the LLM to retry
          * uselessly.  Substitute a "re-link this account" message. */
         char revoked_account[128] = { 0 };
         if (oauth_was_last_refresh_revoked(revoked_account, sizeof(revoked_account))) {
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: OAuth tokens for '%s' have been revoked at the provider. "
                     "Tell the user to re-link this email account in WebUI Settings -> "
                     "Email.  Do NOT retry — retrying with the same tokens will keep "
                     "failing until the user re-authorizes.",
                     revoked_account[0] ? revoked_account : "this account");
         } else {
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: email %s failed (network or upstream error). Retry once; if "
                     "persistent, the email backend may be unreachable.",
                     op);
         }
         break;
      }
   }
   return msg;
}

static bool json_get_bool(struct json_object *obj, const char *key, bool def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_boolean(val);
}

/* qsort comparators on email_summary_t.date (epoch seconds). */
static int cmp_summary_date_desc(const void *a, const void *b) {
   time_t da = ((const email_summary_t *)a)->date;
   time_t db = ((const email_summary_t *)b)->date;
   return (da < db) ? 1 : (da > db) ? -1 : 0;
}

static int cmp_summary_date_asc(const void *a, const void *b) {
   time_t da = ((const email_summary_t *)a)->date;
   time_t db = ((const email_summary_t *)b)->date;
   return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/* Sort summaries in place by date.  sort_mode "oldest" => ascending; anything
 * else (NULL or "newest") => newest-first.  Applied after the service layer
 * returns, so for a multi-account search the merged results interleave by date
 * (single-account recent is simply ordered). */
static void sort_summaries_by_date(email_summary_t *arr, int n, const char *sort_mode) {
   if (n < 2)
      return;
   bool oldest = (sort_mode && strcasecmp(sort_mode, "oldest") == 0);
   qsort(arr, (size_t)n, sizeof(*arr), oldest ? cmp_summary_date_asc : cmp_summary_date_desc);
}

static char *err_error(int rc,
                       email_err_t err,
                       const char *op,
                       const char *account,
                       const char *folder);

static char *handle_recent(struct json_object *details, int user_id) {
   /* 0 = not given: the service layer substitutes the account's max_recent. */
   int count = json_get_int(details, "count", 0);
   const char *account = json_get_str(details, "account");
   const char *folder = json_get_str(details, "folder");
   bool unread_only = json_get_bool(details, "unread_only", false);
   const char *page_token = json_get_str(details, "page_token");
   const char *sort = json_get_str(details, "sort");

   email_summary_t emails[MAX_EMAIL_RESULTS];
   int out_count = 0;
   char next_page_token[256] = { 0 };
   email_err_t err = EMAIL_ERR_NONE;
   int rc = email_service_recent(user_id, account, folder, count, unread_only, page_token, emails,
                                 MAX_EMAIL_RESULTS, &out_count, next_page_token,
                                 sizeof(next_page_token), NULL, NULL, &err);

   if (rc != EMAIL_RC_OK)
      return err_error(rc, err, "recent", account, folder);

   sort_summaries_by_date(emails, out_count, sort);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");

   int pos = 0;
   bool cut = false;
   if (out_count == 0 && next_page_token[0]) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "%s", EMAIL_PARTIAL_SCAN_NOTE);
   } else if (out_count == 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No recent emails found.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Recent emails (%d):\n", out_count);
      pos = append_summary_rows(buf, pos, emails, out_count, &cut);
   }

   if (next_page_token[0] && !cut)
      pos = append_page_token(buf, pos, next_page_token);

   return buf;
}

/* Why an operation failed, when the cause is known; otherwise the generic text for rc. */
static char *err_error(int rc,
                       email_err_t err,
                       const char *op,
                       const char *account,
                       const char *folder) {
   if (rc == EMAIL_RC_FAILURE) {
      switch (err) {
         case EMAIL_ERR_BUSY:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: this mailbox is busy with another request (one connection per "
                          "account at a time). Retry in a moment.");
         case EMAIL_ERR_UNSUPPORTED_QUERY:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: this account's mail server can only search ASCII text. "
                          "Search with ASCII words (drop accented or non-Latin characters).");
         case EMAIL_ERR_CANCELLED:
            return strdup(TOOL_RESULT_ERROR_MARK "Error: the request was stopped.");
         case EMAIL_ERR_AUTH_FAILED:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: the mail server refused the login. The account's password or "
                          "app password may have changed; tell the user to check it in WebUI "
                          "Settings -> Email.");
         case EMAIL_ERR_AUTH_REVOKED:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: access to this mailbox was revoked at the provider. Tell the "
                          "user to reconnect the account in WebUI Settings -> Email.");
         case EMAIL_ERR_UNREACHABLE:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: couldn't reach the mail server (network, DNS or TLS). Retry "
                          "once; if it persists, the server is down or unreachable from here.");
         case EMAIL_ERR_TIMEOUT:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: the mail server didn't answer in time. Retry once.");
         case EMAIL_ERR_RATE_LIMITED:
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: the mail provider asked us to slow down. Wait a minute "
                          "before trying again.");
         default:
            break;
      }
   }
   return email_rc_to_error(rc, op, account, folder);
}

/* @p bytes as "512 B", "12 KB", "3.4 MB". */
static void format_size(size_t bytes, char *out, size_t size) {
   if (bytes < 1024)
      snprintf(out, size, "%zu B", bytes);
   else if (bytes < 1024 * 1024)
      snprintf(out, size, "%zu KB", (bytes + 512) / 1024);
   else
      snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

static void append_addrs(strbuf_t *sb,
                         const char *label,
                         const email_addr_t *list,
                         int count,
                         int total) {
   if (count <= 0)
      return;
   strbuf_appendf(sb, "%s: ", label);
   for (int i = 0; i < count; i++) {
      /* A name that is just the address again isn't shown twice. */
      if (list[i].name[0] && strcasecmp(list[i].name, list[i].addr) != 0)
         strbuf_appendf(sb, "%s%s <%s>", i ? ", " : "", list[i].name, list[i].addr);
      else
         strbuf_appendf(sb, "%s%s", i ? ", " : "", list[i].addr);
   }
   if (total > count)
      strbuf_appendf(sb, " (and %d more)", total - count);
   strbuf_append(sb, "\n");
}

static char *handle_read(struct json_object *details, int user_id) {
   /* Accept message_id (string) or fall back to uid (int) for backward compat */
   const char *mid = json_get_str(details, "message_id");
   char message_id[192] = "";
   if (mid && mid[0]) {
      snprintf(message_id, sizeof(message_id), "%s", mid);
   } else {
      int uid_val = json_get_int(details, "uid", 0);
      if (uid_val <= 0)
         return strdup(
             "Error: 'message_id' is required (get IDs from 'recent' or 'search' results)");
      snprintf(message_id, sizeof(message_id), "%u", (uint32_t)uid_val);
   }

   const char *account = json_get_str(details, "account");

   /* The account's own body cap; never HTML (the model reads text). */
   const email_read_opts_t opts = { .fetch_bytes = EMAIL_READ_FETCH_TOOL };
   email_message_t msg = { 0 };
   email_err_t err = EMAIL_ERR_NONE;
   int rc = email_service_read(user_id, account, message_id, &opts, &msg, NULL, &err);
   if (rc != EMAIL_RC_OK)
      return err_error(rc, err, "read", account, NULL);

   /* Every field here is the sender's text, already made safe to show. */
   /* Headers, 32+32 addresses and 16 attachment lines fit well within 64 KB. */
   strbuf_t sb;
   const size_t body = msg.body_len > 0 ? (size_t)msg.body_len : 0;
   strbuf_init_with_max(&sb, body + 2048, body + 65536);
   char from[2 * sizeof(msg.from_name) + sizeof(msg.from_addr) + 8];
   email_display_mailbox(msg.from_name, msg.from_addr, from, sizeof(from));
   strbuf_appendf(&sb, "From: %s\n", from);
   append_addrs(&sb, "To", msg.to_list, msg.to_count, msg.to_total);
   append_addrs(&sb, "Cc", msg.cc_list, msg.cc_count, msg.cc_total);
   if (msg.reply_to.addr[0] && strcasecmp(msg.reply_to.addr, msg.from_addr) != 0)
      strbuf_appendf(&sb, "Reply-To: %s\n", msg.reply_to.addr);
   strbuf_appendf(&sb, "Subject: %s\nDate: %s\n", msg.subject, msg.date_str);
   if (msg.attachment_count > 0) {
      strbuf_append(&sb, "Attachments:\n");
      for (int i = 0; i < msg.attachment_count; i++) {
         const email_attachment_t *a = &msg.attachments[i];
         char size[32];
         format_size(a->size, size, sizeof(size));
         strbuf_appendf(&sb, "  %d. %s (%s, %s%s)\n", i + 1,
                        a->filename[0] ? a->filename : "(unnamed)", a->mime, size,
                        a->is_inline ? ", inline" : "");
      }
      if (msg.attachments_truncated)
         strbuf_append(&sb, "  (more attachments not listed)\n");
   }
   strbuf_appendf(&sb, "\n%s", msg.body && msg.body[0] ? msg.body : "(No body)");
   if (msg.text_truncated)
      strbuf_append(&sb, "\n[Message truncated]");
   email_message_free(&msg);

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   }
   return strbuf_steal(&sb);
}

static char *handle_search(struct json_object *details, int user_id) {
   const char *account = json_get_str(details, "account");

   email_search_params_t params = { 0 };

   const char *folder = json_get_str(details, "folder");
   if (folder)
      snprintf(params.folder, sizeof(params.folder), "%s", folder);

   const char *from = json_get_str(details, "from");
   if (from)
      snprintf(params.from, sizeof(params.from), "%s", from);

   const char *subject = json_get_str(details, "subject");
   if (subject)
      snprintf(params.subject, sizeof(params.subject), "%s", subject);

   const char *text = json_get_str(details, "text");
   if (text)
      snprintf(params.text, sizeof(params.text), "%s", text);

   const char *since = json_get_str(details, "since");
   if (since)
      snprintf(params.since, sizeof(params.since), "%s", since);

   const char *before = json_get_str(details, "before");
   if (before)
      snprintf(params.before, sizeof(params.before), "%s", before);

   const char *page_token = json_get_str(details, "page_token");
   if (page_token)
      snprintf(params.page_token, sizeof(params.page_token), "%s", page_token);

   params.unread_only = json_get_bool(details, "unread_only", false);

   const char *sort = json_get_str(details, "sort");

   email_summary_t emails[MAX_EMAIL_RESULTS];
   int out_count = 0;
   char next_page_token[256] = { 0 };
   char warn[256] = { 0 }; /* accounts that couldn't be searched (multi-account merge) */
   email_search_report_t report;
   int rc = email_service_search(user_id, account, &params, emails, MAX_EMAIL_RESULTS, &out_count,
                                 next_page_token, sizeof(next_page_token), warn, sizeof(warn), NULL,
                                 &report);

   if (rc != EMAIL_RC_OK) {
      /* Every enabled account failed.  Name them (with 'login failed' where known)
       * instead of the generic backend-error message, so the user gets an
       * actionable report rather than a silent/opaque failure. */
      if (warn[0]) {
         char *msg = malloc(512);
         if (!msg)
            return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
         snprintf(msg, 512,
                  TOOL_RESULT_ERROR_MARK
                  "Error: could not search %s. Tell the user; an account marked 'login failed' "
                  "has bad or expired credentials and should be re-checked in WebUI Settings -> "
                  "Email.",
                  warn);
         return msg;
      }
      if (rc == EMAIL_RC_FAILURE && report.err != EMAIL_ERR_FAILED)
         return err_error(rc, report.err, "search", account, params.folder);
      if (rc == EMAIL_RC_TIMEOUT) {
         /* Date-aware hint: if the search was already date-bounded, telling the
          * LLM to "add a since date" is wrong — the range is just still too big,
          * so advise narrowing.  Otherwise advise bounding with a date.  Keyed on
          * date VALIDITY (what the SEARCH actually emitted), not raw presence: a
          * malformed date is silently dropped, so treating it as "bounded" would
          * tell the LLM to tighten a bound that was never applied. */
         bool bounded = email_search_date_valid(params.since) ||
                        email_search_date_valid(params.before);
         char *msg = malloc(384);
         if (!msg)
            return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
         if (bounded)
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: email search timed out even with a date range — the range is still "
                     "too large for this mailbox (the server has no fast full-text index). Narrow "
                     "it: use a tighter 'since', add a 'before', or a specific 'from'/'subject'.");
         else
            snprintf(msg, 384,
                     TOOL_RESULT_ERROR_MARK
                     "Error: email search timed out — the mailbox is large and the server has no "
                     "fast full-text index, so an unbounded search is slow. Retry with a 'since' "
                     "date (e.g. the last 12 months) to bound it; add 'before' or a specific "
                     "'from'/'subject' to narrow further.");
         return msg;
      }
      return email_rc_to_error(rc, "search", account, folder);
   }

   sort_summaries_by_date(emails, out_count, sort);

   /* Enrich reply status so a "did I reply to Fred?" search shows [replied].
    * One in:sent lookup per Gmail account represented in the results. */
   email_service_fill_reply_states(user_id, emails, out_count);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");

   int pos = 0;
   bool cut = false;
   if (out_count == 0 && next_page_token[0]) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "%s", EMAIL_PARTIAL_SCAN_NOTE);
   } else if (out_count == 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No emails matching your search criteria.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Search results (%d):\n", out_count);
      pos = append_summary_rows(buf, pos, emails, out_count, &cut);
   }

   if (next_page_token[0] && !cut)
      pos = append_page_token(buf, pos, next_page_token);

   /* Partial result: some accounts succeeded, others couldn't be reached.  Surface
    * it so the LLM tells the user rather than silently presenting incomplete results
    * (or a bare "No emails" that's actually a down account, not a true no-match). */
   /* Size the guard to the actual note length (fixed text + warn) so the advisory
    * never truncates mid-sentence when several accounts failed. */
   if (warn[0] && pos + (int)strlen(warn) + 256 < RESULT_BUF_SIZE)
      snprintf(buf + pos, RESULT_BUF_SIZE - pos,
               "\n\nNote: could not search %s — those results are NOT included. Tell the user; "
               "an account marked 'login failed' has bad or expired credentials and should be "
               "re-checked in WebUI Settings -> Email.",
               warn);

   return buf;
}

#define EMAIL_PENDING_FULL_ERR                                                                 \
   TOOL_RESULT_ERROR_MARK "Error: too many emails are waiting for a confirm. Confirm one, or " \
                          "try again in a few minutes."

static char *handle_send(struct json_object *details, int user_id, const turn_origin_t *origin) {
   const char *account = json_get_str(details, "account");
   const char *to = json_get_str(details, "to");
   const char *subject = json_get_str(details, "subject");
   const char *body = json_get_str(details, "body");

   if (!account || !account[0])
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: 'account' is required — specify which configured account to send "
                    "FROM (the sender). Call action='accounts' to list them, and never invent "
                    "one. When replying, use the account the original message arrived on.");
   if (!to || !to[0])
      return strdup("Error: 'to' is required (email address or contact name)");
   if (!subject || !subject[0])
      return strdup("Error: 'subject' is required");
   if (!body || !body[0])
      return strdup("Error: 'body' is required");

   /* Validate field lengths */
   if (strlen(body) > EMAIL_MAX_SEND_BODY_LEN) {
      char err[128];
      snprintf(err, sizeof(err), "Error: email body too long (%zu chars, max %d)", strlen(body),
               EMAIL_MAX_SEND_BODY_LEN);
      return strdup(err);
   }
   if (strlen(subject) > EMAIL_MAX_SUBJECT_LEN) {
      return strdup("Error: subject too long (max 250 characters)");
   }

   /* Who it goes to, without guessing (contact_resolve.h): one address, or a
    * contact certain enough to draft to.  One to confirm is still drafted,
    * since a draft is itself a question to the user: its preview says why. */
   char resolved_addr[256] = "";
   char resolved_name[64] = "";
   char confirm_note[512] = "";
   {
      char *words = tool_user_words_dup();
      const contact_resolve_opts_t ropts = {
         .field = CONTACT_FIELD_EMAIL,
         .spoken = tool_turn_spoken(),
         .user_words = words,
      };
      contact_resolve_t r;
      contact_resolve(user_id, to, &ropts, &r);
      free(words);
      if (r.kind != CONTACT_RESOLVE_LITERAL && r.kind != CONTACT_RESOLVE_UNIQUE &&
          r.kind != CONTACT_RESOLVE_CONFIRM) {
         char question[1024];
         contact_resolve_question(to, CONTACT_FIELD_EMAIL, &r, question, sizeof(question));
         return strdup(question);
      }
      if (r.kind == CONTACT_RESOLVE_CONFIRM) {
         contact_resolve_question(to, CONTACT_FIELD_EMAIL, &r, confirm_note, sizeof(confirm_note));
      }
      snprintf(resolved_addr, sizeof(resolved_addr), "%s", r.value);
      snprintf(resolved_name, sizeof(resolved_name), "%s", r.name);
   }

   /* Create draft (two-step send) */
   char draft_id[16];
   char from_account[128] = "";
   int rc = email_service_create_draft(user_id, account, resolved_addr, resolved_name, subject,
                                       body, draft_id, sizeof(draft_id), from_account,
                                       sizeof(from_account), origin);
   if (rc == EMAIL_ACCT_RC_READONLY)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: that account is read-only and cannot send. Choose a writable "
                    "account (call action='accounts'), or enable write access in WebUI "
                    "Settings -> Email.");
   /* Unknown-account / no-accounts are forwarded from find_account — reuse the
    * shared account-error messages instead of restating them here. */
   if (rc == EMAIL_RC_PENDING_FULL)
      return strdup(EMAIL_PENDING_FULL_ERR);
   if (rc != EMAIL_RC_OK)
      return email_rc_to_error(rc, "send draft", account, NULL);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");

   /* The line the model says as written: one line, nothing in it that could
    * read as another line or an instruction. */
   char say_to[256];
   char say_subject[256];
   email_display_sanitize(resolved_addr, strlen(resolved_addr), say_to, sizeof(say_to), 0);
   email_display_sanitize(subject, strlen(subject), say_subject, sizeof(say_subject), 0);

   snprintf(buf, RESULT_BUF_SIZE,
            "Draft email prepared:\n"
            "  From account: %s\n"
            "  To: %s%s%s%s\n"
            "  Subject: %s\n"
            "  Body: %s\n\n"
            "%s%s%s"
            "Read this back to the user (including which account it will send FROM), and say this "
            "line exactly as written, so the user hears where it really goes:\n"
            "  Sending to %s, from %s, subject: %s\n"
            "Then ask for confirmation, and call confirm_send with draft_id '%s' only if the "
            "user's very next message says yes; a confirm in this turn, or any later one, "
            "is refused.",
            from_account, resolved_name[0] ? resolved_name : "", resolved_name[0] ? " <" : "",
            resolved_addr, resolved_name[0] ? ">" : "", subject, body,
            confirm_note[0] ? "First check the recipient: " : "", confirm_note,
            confirm_note[0] ? "\n" : "", say_to, from_account, say_subject, draft_id);

   return buf;
}

static char *handle_confirm_send(struct json_object *details,
                                 int user_id,
                                 const turn_origin_t *origin) {
   const char *draft_id = json_get_str(details, "draft_id");
   if (!draft_id || !draft_id[0])
      return strdup("Error: 'draft_id' is required");

   int rc = email_service_confirm_send(user_id, draft_id, origin);
   switch (rc) {
      case EMAIL_RC_OK:
         return strdup("Email sent successfully.");
      case EMAIL_CONFIRM_RC_SAME_TURN:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. The user has to answer yes in a new message after "
                       "seeing what this does; a confirm in the same turn that prepared it is "
                       "refused. Ask the user, and confirm when they reply.");
      case EMAIL_CONFIRM_RC_NOT_NEXT:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. A confirm counts only in the user's reply right "
                       "after the read-back; the conversation has moved on since. Prepare it "
                       "again and read it back if the user still wants it.");
      case EMAIL_CONFIRM_RC_FROM_VISUAL:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. This turn came from a rendered visual, not the "
                       "user, and a visual can't approve anything. Prepare it again, read it "
                       "back, and confirm only when the user replies themselves.");
      case EMAIL_CONFIRM_RC_OTHER_SESSION:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. This was prepared in another session (another "
                       "browser tab, device or channel), and only that one can confirm it. "
                       "Prepare it again here if the user wants it.");
      case EMAIL_CONFIRM_RC_NOT_FOUND:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: draft not found or expired. The draft may have timed out "
                       "(5-minute limit). Please use 'send' to create a new draft.");
      case EMAIL_CONFIRM_RC_THROTTLED:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: too many failed confirmation attempts. Please wait 60 seconds "
                       "before trying again.");
      case EMAIL_CONFIRM_RC_ACCOUNT_GONE:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: the account this draft was prepared to send from is no longer "
                       "available or has become read-only. Call action='accounts', then use "
                       "'send' to prepare a new draft from a writable account.");
      default:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: failed to send email (network or upstream error). Retry once; "
                       "if persistent, the SMTP server may be unreachable.");
   }
}

static char *handle_folders(struct json_object *details, int user_id) {
   const char *account = json_get_str(details, "account");

   char buf[4096];
   email_err_t err = EMAIL_ERR_NONE;
   int rc = email_service_list_folders(user_id, account, buf, sizeof(buf), NULL, &err);
   if (rc != EMAIL_RC_OK)
      return err_error(rc, err, "folder list", account, NULL);

   if (!buf[0])
      return strdup("No folders found.");

   char *result = malloc(RESULT_BUF_SIZE);
   if (!result)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");

   if (account && account[0])
      snprintf(result, RESULT_BUF_SIZE, "Folders for '%s':\n%s", account, buf);
   else
      snprintf(result, RESULT_BUF_SIZE, "Available folders/labels:\n%s", buf);
   return result;
}

/* =============================================================================
 * Trash / Archive Handlers
 * ============================================================================= */

static char *handle_trash(struct json_object *details, int user_id, const turn_origin_t *origin) {
   const char *mid = json_get_str(details, "message_id");
   if (!mid || !mid[0])
      return strdup("Error: 'message_id' is required (get IDs from 'recent' or 'search' results)");

   const char *account = json_get_str(details, "account");

   char pending_id[16];
   char subject[256] = { 0 };
   char from[128] = { 0 };
   int rc = email_service_create_pending_trash(user_id, account, mid, pending_id,
                                               sizeof(pending_id), subject, sizeof(subject), from,
                                               sizeof(from), origin);
   if (rc == EMAIL_ACCT_RC_READONLY)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: email account is read-only. Cannot trash emails. Tell the user "
                    "to enable write access for this account in WebUI Settings -> Email.");
   if (rc == EMAIL_RC_PENDING_FULL)
      return strdup(EMAIL_PENDING_FULL_ERR);
   if (rc != EMAIL_RC_OK)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: failed to prepare trash action. The message_id may be invalid "
                    "(get fresh IDs from 'recent' or 'search') or the account may be "
                    "unreachable — retry once.");

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");

   snprintf(buf, RESULT_BUF_SIZE,
            "Pending trash:\n"
            "  From: %s\n"
            "  Subject: %s\n\n"
            "Confirm with the user before proceeding, and call confirm_trash with pending_id "
            "'%s' only if the user's very next message says yes; a confirm in this turn, or "
            "any later one, is refused.",
            from, subject, pending_id);

   return buf;
}

static char *handle_confirm_trash(struct json_object *details,
                                  int user_id,
                                  const turn_origin_t *origin) {
   const char *pending_id = json_get_str(details, "pending_id");
   if (!pending_id || !pending_id[0])
      return strdup("Error: 'pending_id' is required");

   email_err_t err = EMAIL_ERR_NONE;
   int rc = email_service_confirm_trash(user_id, pending_id, origin, &err);
   switch (rc) {
      case EMAIL_RC_OK:
         return strdup("Email moved to Trash.");
      case EMAIL_CONFIRM_RC_SAME_TURN:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. The user has to answer yes in a new message after "
                       "seeing what this does; a confirm in the same turn that prepared it is "
                       "refused. Ask the user, and confirm when they reply.");
      case EMAIL_CONFIRM_RC_NOT_NEXT:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. A confirm counts only in the user's reply right "
                       "after the read-back; the conversation has moved on since. Prepare it "
                       "again and read it back if the user still wants it.");
      case EMAIL_CONFIRM_RC_FROM_VISUAL:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. This turn came from a rendered visual, not the "
                       "user, and a visual can't approve anything. Prepare it again, read it "
                       "back, and confirm only when the user replies themselves.");
      case EMAIL_CONFIRM_RC_OTHER_SESSION:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: not confirmed. This was prepared in another session (another "
                       "browser tab, device or channel), and only that one can confirm it. "
                       "Prepare it again here if the user wants it.");
      case EMAIL_RC_ALREADY_THERE:
         return strdup("That email is already in Trash; nothing changed (DAWN never deletes "
                       "mail permanently).");
      case EMAIL_RC_LEFT_FLAGGED:
         return strdup("Email copied to Trash and marked deleted, but it is still in its folder: "
                       "this server can't remove one message without also purging others. Tell "
                       "the user their mail app can remove it there.");
      case EMAIL_RC_NOT_REMOVED:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: the email was copied to Trash, but removing the original failed "
                       "(network or server error), so it is still in its folder too. Nothing was "
                       "lost. Tell the user; a retry may leave a second copy in Trash.");
      case EMAIL_RC_NOT_FOUND:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: that message is no longer in its folder (moved or deleted "
                       "elsewhere). Nothing was changed.");
      case EMAIL_CONFIRM_RC_NOT_FOUND:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: pending trash not found or expired. The request may have timed out "
                       "(5-minute limit). Please use 'trash' to create a new request.");
      case EMAIL_CONFIRM_RC_THROTTLED:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: too many failed confirmation attempts. Please wait 60 seconds "
                       "before trying again.");
      case EMAIL_CONFIRM_RC_ACCOUNT_GONE:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: the account this message belongs to is no longer available or "
                       "has become read-only. Nothing was deleted. Call action='accounts' to "
                       "check, then retry from a writable account.");
      case EMAIL_RC_NO_TRASH:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: this account has no Trash folder, so the message was left where "
                       "it is (DAWN never deletes mail permanently). Tell the user; they can "
                       "create a Trash folder in their mail app.");
      default:
         /* A busy or stopped confirm happens before anything is consumed: the
          * trash is still staged under the same id. */
         if (err == EMAIL_ERR_BUSY)
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: this mailbox is busy with another request (one connection per "
                          "account at a time). Nothing was trashed and the trash is still staged. "
                          "Tell the user; if they say yes again, confirm with the same pending "
                          "id.");
         if (err == EMAIL_ERR_CANCELLED)
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: the request was stopped before anything was trashed; the trash "
                          "is still staged. If the user says yes again, confirm with the same "
                          "pending id.");
         if (err == EMAIL_ERR_OUTCOME_UNKNOWN)
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: the mail server didn't confirm the trash, so the message may "
                          "or may not have been moved. Don't retry; tell the user to check the "
                          "folder.");
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: failed to trash email (network or upstream error). Retry "
                       "once; if persistent, the email backend may be unreachable.");
   }
}

static char *handle_archive(struct json_object *details, int user_id) {
   const char *mid = json_get_str(details, "message_id");
   if (!mid || !mid[0])
      return strdup("Error: 'message_id' is required (get IDs from 'recent' or 'search' results)");

   const char *account = json_get_str(details, "account");

   email_err_t err = EMAIL_ERR_NONE;
   int rc = email_service_archive(user_id, account, mid, NULL, &err);
   switch (rc) {
      case EMAIL_RC_OK:
         return strdup("Email archived (moved out of its folder to the account's archive).");
      case EMAIL_RC_ALREADY_THERE:
         return strdup("That email is already in the archive; nothing changed.");
      case EMAIL_RC_LEFT_FLAGGED:
         return strdup("Email copied to the archive and marked deleted, but it is still in its "
                       "folder: this server can't remove one message without also purging "
                       "others. Tell the user their mail app can remove it there.");
      case EMAIL_RC_NOT_REMOVED:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: the email was copied to the archive, but removing the original "
                       "failed (network or server error), so it is still in its folder too. "
                       "Nothing was lost. Tell the user; a retry may leave a second copy.");
      case EMAIL_RC_NOT_FOUND:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: that message is no longer in its folder (moved or deleted "
                       "elsewhere). Get fresh IDs from 'recent'.");
      case EMAIL_ACCT_RC_READONLY:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: email account is read-only. Cannot archive emails. Tell the "
                       "user to enable write access for this account in WebUI Settings -> "
                       "Email.");
      case EMAIL_RC_FOLDER_MISSING:
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: this account has no Archive folder, so the message was left "
                       "where it is. Tell the user; they can create an Archive folder in their "
                       "mail app.");
      default:
         if (err == EMAIL_ERR_BUSY)
            return err_error(EMAIL_RC_FAILURE, err, "archive", account, NULL);
         if (err == EMAIL_ERR_IN_TRASH)
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: that message is in Trash or Spam, so it wasn't archived. Tell "
                          "the user; it has to be restored to the inbox first.");
         if (err == EMAIL_ERR_OUTCOME_UNKNOWN)
            return strdup(TOOL_RESULT_ERROR_MARK
                          "Error: the mail server didn't confirm the archive, so the message may "
                          "or may not have been moved. Don't retry; tell the user to check the "
                          "folder.");
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: failed to archive email (network or upstream error). The "
                       "message_id may be invalid (get fresh IDs from 'recent') or the "
                       "account may be unreachable — retry once.");
   }
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

/* All-inbox briefing digest.  Parses window/max/unread_only, delegates the
 * aggregation to email_digest.c.  Read-only, schedulable. */
static char *handle_digest(struct json_object *details, int user_id) {
   email_digest_opts_t opts = { 0 };
   opts.window_seconds = EMAIL_DIGEST_DEFAULT_WINDOW_SEC;
   opts.max = EMAIL_DIGEST_DEFAULT_MAX;
   opts.unread_only = json_get_bool(details, "unread_only", false);

   /* window: "<n>h" (hours, default) or "<n>d" (days), e.g. "24h", "2d".  Clamp n
    * to the max window (in the chosen unit) BEFORE multiplying, so a huge/garbage
    * value can't overflow the int math; email_digest_build re-clamps in seconds. */
   const char *w = json_get_str(details, "window");
   if (w && w[0]) {
      char *end = NULL;
      long n = strtol(w, &end, 10);
      char unit = (end && *end) ? *end : 'h';
      if (n > 0) {
         if (unit == 'd' || unit == 'D') {
            long max_days = EMAIL_DIGEST_MAX_WINDOW_SEC / (24 * 3600);
            if (n > max_days)
               n = max_days;
            opts.window_seconds = (int)(n * 24 * 3600);
         } else {
            long max_hours = EMAIL_DIGEST_MAX_WINDOW_SEC / 3600;
            if (n > max_hours)
               n = max_hours;
            opts.window_seconds = (int)(n * 3600);
         }
      }
   }

   /* max is passed through unclamped; email_digest_build owns the row ceiling so
    * every caller of the module inherits the same bound. */
   opts.max = json_get_int(details, "max", 0);

   return email_digest_build(user_id, &opts);
}

/* Single source of truth for which email actions may run unattended (from a
 * scheduled task or briefing).  Only read-only actions qualify; anything that
 * sends, trashes, or archives requires a live conversation (a human in the
 * loop).  Used by BOTH the create-time gate (email_validate_schedulable_action,
 * reached via tool_registry) and the fire-time gate in email_tool_callback.
 * Unknown/new actions are NOT schedulable by default (fail closed). */
static bool email_action_is_schedulable(const char *action) {
   /* Keep this list in sync with the read-only actions the callback dispatches so
    * a scheduled step never passes the gate only to fail "unknown action" at fire
    * time.  'digest' is the daily-briefing aggregator (email_digest.c). */
   return action && (strcmp(action, "accounts") == 0 || strcmp(action, "recent") == 0 ||
                     strcmp(action, "search") == 0 || strcmp(action, "folders") == 0 ||
                     strcmp(action, "read") == 0 || strcmp(action, "digest") == 0);
}

#define EMAIL_SCHEDULABLE_ERR                                                             \
   "only read-only email actions (accounts / recent / search / folders / read / digest) " \
   "may run from a schedule; send, trash, and archive require a live conversation."

/* Actions whose `arguments` fields are ALL optional.  A reasoning model may
 * narrate prose instead of an args object while still meaning "use the
 * defaults", so tool_parse_details degrades a non-JSON value to an empty object
 * for these.  NOTE: this is a superset of the no-argument case ('accounts') —
 * 'read' is excluded because it requires message_id, and the mutations
 * (send/confirm_send/trash/confirm_trash/archive) stay strict. */
static bool email_action_no_required_fields(const char *action) {
   return action && (strcmp(action, "accounts") == 0 || strcmp(action, "recent") == 0 ||
                     strcmp(action, "search") == 0 || strcmp(action, "folders") == 0 ||
                     strcmp(action, "digest") == 0);
}

/* Per-action schedulability gate registered in email_metadata.  The email tool
 * is TOOL_CAP_DANGEROUS (send/trash/archive) yet TOOL_CAP_SCHEDULABLE (so the
 * read-only digest can run in a daily briefing); tool_registry_validate_schedulable
 * does NOT reject DANGEROUS tools, so without this gate a scheduled step could
 * send or trash mail.  Rejects non-read actions at scheduler CREATE time so the
 * LLM is told up front.  Mirrors the fire-time gate in the callback. */
static int email_validate_schedulable_action(const char *action,
                                             char *err_buf,
                                             size_t err_buf_size) {
   if (email_action_is_schedulable(action))
      return SUCCESS;
   if (err_buf && err_buf_size)
      snprintf(err_buf, err_buf_size, EMAIL_SCHEDULABLE_ERR);
   return FAILURE;
}

static const tool_action_kind_entry_t s_email_action_kinds[] = {
   { "recent", TOOL_KIND_READ, NULL },
   { "read", TOOL_KIND_READ, NULL },
   { "search", TOOL_KIND_READ, NULL },
   { "folders", TOOL_KIND_READ, NULL },
   { "digest", TOOL_KIND_READ, NULL },
   { "accounts", TOOL_KIND_READ, NULL },
   { "send", TOOL_KIND_PREPARE, "confirm_send" },
   { "trash", TOOL_KIND_PREPARE, "confirm_trash" },
   { "confirm_send", TOOL_KIND_ACT, NULL },
   { "confirm_trash", TOOL_KIND_ACT, NULL },
};

/* The actions that send, delete or move mail: every listed action that isn't
 * a read, and archive (unlisted, so it acts). */
static bool email_action_acts(const char *action) {
   for (int i = 0; i < TOOL_KIND_COUNT(s_email_action_kinds); i++) {
      if (strcmp(action, s_email_action_kinds[i].action) == 0)
         return s_email_action_kinds[i].kind != TOOL_KIND_READ;
   }
   return strcmp(action, "archive") == 0;
}

/* What a call that waits for the user's reply code does (tool_metadata_t
 * describe_call): from the draft or pending item itself, never the model's
 * words. */
static int email_describe_call(const char *action,
                               const char *value,
                               char *out,
                               size_t out_len,
                               int *valid_for_sec) {
   struct json_object *details = tool_parse_details(value, false);
   if (!details)
      return FAILURE;
   const int user_id = tool_get_current_user_id();
   int rc = FAILURE;
   /* Only this session's draft or pending trash: its confirm is refused
    * anywhere else. */
   turn_origin_t origin;
   const bool live = turn_origin_capture(&origin);
   if (strcmp(action, "confirm_send") == 0) {
      rc = live && email_service_describe_draft(user_id, origin.session_id,
                                                json_get_str(details, "draft_id"), out, out_len,
                                                valid_for_sec) == EMAIL_RC_OK
               ? SUCCESS
               : FAILURE;
   } else if (strcmp(action, "confirm_trash") == 0) {
      rc = live && email_service_describe_pending_trash(user_id, origin.session_id,
                                                        json_get_str(details, "pending_id"), out,
                                                        out_len, valid_for_sec) == EMAIL_RC_OK
               ? SUCCESS
               : FAILURE;
   } else if (strcmp(action, "archive") == 0) {
      const char *mid = json_get_str(details, "message_id");
      const char *account = json_get_str(details, "account");
      if (!mid || !mid[0]) {
         snprintf(out, out_len, "it doesn't name the email to archive");
      } else {
         char shown_mid[160], shown_account[160];
         str_excerpt_line(mid, 100, shown_mid, sizeof(shown_mid));
         str_excerpt_line(account ? account : "", 100, shown_account, sizeof(shown_account));
         const int n = snprintf(out, out_len, "archive email %s%s%s", shown_mid,
                                shown_account[0] ? " in " : "", shown_account);
         rc = (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
      }
   } else {
      rc = TOOL_DESCRIBE_DEFAULT;
   }
   json_object_put(details);
   return rc;
}

static char *email_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!action || !action[0])
      return strdup("Error: action is required");

   /* Fire-time schedulability gate.  Keyed on the scheduled-origin context, NOT
    * "no session" — the identity fallback in tool_get_current_user_id resolves a
    * scheduled owner, so "no session" alone no longer distinguishes scheduled
    * from interactive.  Defense in depth with the create-time gate above; this
    * also catches legacy briefing rows that predate the gate (the single-tool
    * briefing path skips tool_registry_validate_schedulable). */
   if (scheduled_context_get(NULL) && !email_action_is_schedulable(action))
      return strdup(TOOL_RESULT_ERROR_MARK "Error: " EMAIL_SCHEDULABLE_ERR);

   /* Actions with only-optional fields tolerate a prose `arguments` value (a
    * model may still narrate despite the schema); read + the mutations require
    * fields, where a non-JSON value stays a hard error. */
   struct json_object *details = tool_parse_details(value, email_action_no_required_fields(action));
   if (!details)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: invalid JSON in details parameter");

   int user_id = tool_get_current_user_id();
   if (user_id <= 0) {
      json_object_put(details);
      return strdup(TOOL_GUEST_REFUSAL);
   }

   /* What changes or sends mail needs a person in a live conversation: not a
    * background job, a re-engaged background turn or an MQTT message, where
    * the request may come from content the model read rather than from the
    * user.  And a confirm must come from the same session, in a later turn
    * (turn_origin_t): the user's answer, not the model's own next step. */
   turn_origin_t origin = { 0 };
   if (email_action_acts(action) && !turn_origin_capture(&origin)) {
      json_object_put(details);
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: sending, trashing and archiving email need the user in a live "
                    "conversation, and this request came from a background job or an automated "
                    "turn. Tell the user what you would do, and let them ask for it.");
   }

   char *result = NULL;

   if (strcmp(action, "accounts") == 0) {
      result = handle_accounts(user_id);
   } else if (strcmp(action, "recent") == 0) {
      result = handle_recent(details, user_id);
   } else if (strcmp(action, "read") == 0) {
      result = handle_read(details, user_id);
   } else if (strcmp(action, "search") == 0) {
      result = handle_search(details, user_id);
   } else if (strcmp(action, "folders") == 0) {
      result = handle_folders(details, user_id);
   } else if (strcmp(action, "send") == 0) {
      result = handle_send(details, user_id, &origin);
   } else if (strcmp(action, "confirm_send") == 0) {
      result = handle_confirm_send(details, user_id, &origin);
   } else if (strcmp(action, "trash") == 0) {
      result = handle_trash(details, user_id, &origin);
   } else if (strcmp(action, "confirm_trash") == 0) {
      result = handle_confirm_trash(details, user_id, &origin);
   } else if (strcmp(action, "archive") == 0) {
      result = handle_archive(details, user_id);
   } else if (strcmp(action, "digest") == 0) {
      result = handle_digest(details, user_id);
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               TOOL_RESULT_ERROR_MARK
               "Error: unknown action '%s'. Valid: accounts, recent, read, search, folders, "
               "digest, send, confirm_send, trash, confirm_trash, archive",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

static int email_tool_init(void) {
   return email_service_init();
}

static void email_tool_cleanup(void) {
   email_service_shutdown();
}

static bool email_tool_available(void) {
   return s_config.enabled && email_service_available();
}

/* =============================================================================
 * Config Parser
 * ============================================================================= */

static void email_tool_config_parse(toml_table_t *table, void *config) {
   email_tool_config_t *cfg = (email_tool_config_t *)config;

   if (!table)
      return;

   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok)
      cfg->enabled = enabled.u.b;
}

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

static const treg_param_t email_params[] = {
   {
       .name = "action",
       .description = "Email action: 'accounts' (list configured accounts), "
                      "'recent' (fetch recent emails), 'read' (read full email by message_id), "
                      "'search' (search by from/subject/text/date), "
                      "'folders' (list available folders/labels), "
                      "'digest' (read-only briefing summary of recent inbox mail across ALL "
                      "accounts, grouped by importance/category), "
                      "'send' (compose draft for user confirmation), "
                      "'confirm_send' (send confirmed draft), "
                      "'trash' (move email to trash — requires confirmation), "
                      "'confirm_trash' (confirm trash action), "
                      "'archive' (remove from inbox, keep in All Mail)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "recent", "read", "search", "folders", "digest", "send", "confirm_send",
                        "accounts", "trash", "confirm_trash", "archive" },
       .enum_count = 11,
   },
   {
       .name = "arguments",
       .description =
           "JSON object of the action's arguments, passed as a JSON-encoded string.  "
           "Omit for an action that takes no arguments; never fill it with a description "
           "or rationale.  Shapes: "
           "recent {count? (default: the account's setting; up to 50), folder?, unread_only?, "
           "account?, page_token?, sort?}, "
           "read {message_id, account?}, "
           "search {from?, subject?, text?, since?, before?, folder?, unread_only?, "
           "account?, page_token?, sort?} (dates: YYYY-MM-DD, UTC, since=inclusive, "
           "before=exclusive), "
           "folders {account?}, "
           "digest {window? ('24h' default, or '2d'/'7d'), unread_only?, max? (default 50)} "
           "(a briefing-style summary of recent inbox mail across ALL accounts, grouped by "
           "importance/category with an [E-NN] label per message; act on one via its [ID]), "
           "send {account, to, subject, body} (account: REQUIRED — the configured "
           "account to send FROM; to: email or contact name), "
           "confirm_send {draft_id}, "
           "trash {message_id, account?} (creates pending — ask user to confirm), "
           "confirm_trash {pending_id}, "
           "archive {message_id, account?} (removes from inbox, no confirmation needed).\n"
           "  account?: the CONFIGURED account name OR username/email from the 'accounts' "
           "action — must already exist. Do NOT invent an email address; if uncertain, "
           "call action='accounts' first to enumerate. 'send' REQUIRES account (the sender — "
           "there is no default; when replying, use the account the original arrived on). "
           "When omitted: 'search' merges ALL "
           "enabled accounts and 'digest' always spans every account; 'read' searches every "
           "enabled account for the message id; but 'recent'/'folders' use only the FIRST "
           "enabled account — name the account explicitly, or use 'digest', to cover every "
           "inbox with those two.\n"
           "  folder?: defaults to inbox; valid values listed in the top-level tool "
           "description.\n"
           "  sort?: \"newest\" (default) or \"oldest\" — orders results by date.\n"
           "  page_token: opaque cursor from a previous response; pass back verbatim, do "
           "not parse or invent.\n"
           "Results include a page_token when more results are available — pass it in the "
           "next call to fetch the next page.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t email_metadata = {
   .name = "email",
   .action_kinds = s_email_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_email_action_kinds),
   .describe_call = email_describe_call,
   .device_string = "email",
   .topic = "dawn",
   .aliases = { "mail", "inbox", "gmail" },
   .alias_count = 3,

   .description = "Read, send, trash, and archive emails. "
                  "Use 'accounts' to see configured email accounts. "
                  "Use 'folders' to list available folders/labels for an account. "
                  "Use 'recent' to fetch recent emails sorted newest-first "
                  "(set unread_only:true for unread only, folder to specify folder/label). "
                  "Use 'read' to read a full email by message_id (from recent/search results). "
                  "Use 'search' to find emails by from, subject, text, or date range "
                  "(results sorted newest-first, set unread_only:true to filter unread). "
                  "The 'folder' parameter (on recent/search) selects which folder/label: "
                  "inbox, sent, trash, spam, drafts, starred, important, all, "
                  "or a custom label name. Default is inbox. "
                  "Use 'send' to compose a draft (reads back to user for confirmation). "
                  "'send' REQUIRES an 'account' argument naming which configured account to "
                  "send FROM (call 'accounts' to list them; when replying, use the account the "
                  "original message arrived on). "
                  "Use 'confirm_send' with the draft_id to actually send, only if the user's "
                  "very next message says yes (a confirm in the turn that drafted it, or any "
                  "later one, is refused). "
                  "Use 'trash' to move an email to trash (two-step: creates pending action, "
                  "then 'confirm_trash' if the user's very next message says yes). "
                  "Use 'archive' to move an email out of its folder to the account's archive "
                  "(no confirmation needed). "
                  "send, trash and archive work only in a live conversation with the user, "
                  "not from a background job. "
                  "For 'send', the 'to' field can be a contact name (resolved via contacts; "
                  "pass the name as the user said it, never a guess; for a relationship such as "
                  "'my wife', the name of the person you know it means) or one email address.",
   .params = email_params,
   .param_count = TOOL_PARAM_COUNT(email_params),

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_DANGEROUS | TOOL_CAP_SCHEDULABLE,
   /* Scheduled read steps (digest/recent/search) carry [E-NN]/[ID] rows the user
    * may follow up on ("open E-03") — persist the raw result into the briefing
    * conversation so those references resolve on a later turn. */
   .persist_scheduled_output = true,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = email_tool_config_parse,
   .config_section = "email",

   .is_available = email_tool_available,
   .validate_schedulable_action = email_validate_schedulable_action,
   .init = email_tool_init,
   .cleanup = email_tool_cleanup,
   .callback = email_tool_callback,
};

int email_tool_register(void) {
   return tool_registry_register(&email_metadata);
}
