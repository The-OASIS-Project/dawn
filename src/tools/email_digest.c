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
 * Email daily-briefing digest — aggregates recent inbox mail across every
 * enabled account into one categorized, briefing-ready summary.
 *
 * Flow: enumerate enabled accounts -> fetch recent inbox per account through the
 * email_service layer (which stamps account name + address on each row) -> keep
 * rows inside the rolling window (in-memory filter on the parsed epoch) -> merge
 * + date-sort across accounts -> cap -> render Important/Primary/Other sections
 * with display-only E-NN labels and the real [ID] for follow-up actions.
 *
 * All of this is read-only; the tool's schedulability gate (email_tool.c) allows
 * it in a scheduled briefing.  Per-account fetch failures degrade to a status
 * line rather than failing the whole digest.
 */

#include "tools/email_digest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/strbuf.h"
#include "logging.h"
#include "tools/email_db.h"
#include "tools/email_service.h"
#include "tools/email_types.h"
#include "tools/tool_registry.h"

/* Per-account fetch depth.  recent() has no time filter, so we pull this many
 * newest inbox messages and window-filter in memory.  A busier-than-this day in
 * one inbox trips the cap note (oldest-in-window may be omitted). */
#define DIGEST_FETCH_PER_ACCT EMAIL_MAX_FETCH_RESULTS

static int cmp_summary_date_desc(const void *a, const void *b) {
   time_t da = ((const email_summary_t *)a)->date;
   time_t db = ((const email_summary_t *)b)->date;
   return (da < db) ? 1 : (da > db) ? -1 : 0;
}

static const char *category_label(email_category_t c) {
   switch (c) {
      case EMAIL_CAT_SOCIAL:
         return "Social";
      case EMAIL_CAT_PROMOTIONS:
         return "Promotions";
      case EMAIL_CAT_UPDATES:
         return "Updates";
      case EMAIL_CAT_FORUMS:
         return "Forums";
      case EMAIL_CAT_PRIMARY:
      default:
         return "Primary";
   }
}

/* Human window label: days when a whole number of days, else whole hours, else
 * minutes.  Echoes back input like "7d"/"24h" rather than "168h". */
static void format_window(int sec, char *out, size_t out_len) {
   if (sec > 0 && sec % 86400 == 0)
      snprintf(out, out_len, "%dd", sec / 86400);
   else if (sec > 0 && sec % 3600 == 0)
      snprintf(out, out_len, "%dh", sec / 3600);
   else
      snprintf(out, out_len, "%dm", sec / 60 > 0 ? sec / 60 : 1);
}

/* "name (address)", or just one when they are equal / one is empty. */
static void format_acct_label(const email_summary_t *m, char *out, size_t out_len) {
   const char *nm = m->account_name;
   const char *ad = m->account_addr;
   if (ad[0] && nm[0] && strcmp(nm, ad) != 0)
      snprintf(out, out_len, "%s (%s)", nm, ad);
   else
      snprintf(out, out_len, "%s", ad[0] ? ad : (nm[0] ? nm : "?"));
}

/* Section membership: importance wins over category. */
typedef enum {
   SEC_IMPORTANT,
   SEC_PRIMARY,
   SEC_OTHER
} digest_section_t;

static digest_section_t message_section(const email_summary_t *m) {
   if (m->important)
      return SEC_IMPORTANT;
   if (m->category == EMAIL_CAT_PRIMARY)
      return SEC_PRIMARY;
   return SEC_OTHER;
}

/* Emit one message row.  *enn is the running display counter (E-NN). */
static void emit_row(strbuf_t *sb, const email_summary_t *m, int *enn, bool show_category) {
   char acct[300];
   format_acct_label(m, acct, sizeof(acct));

   char ts[32];
   time_t d = m->date;
   struct tm tmv;
   /* strftime returns 0 (and leaves ts indeterminate) if the result won't fit —
    * reachable via a crafted far-future Date: header — so gate every use of ts on
    * its success and fall back rather than %s-printing an uninitialized buffer. */
   if (!(d > 0 && localtime_r(&d, &tmv) && strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tmv)))
      snprintf(ts, sizeof(ts), "(no date)");

   strbuf_appendf(sb, "[E-%02d] From: %s%s%s | Subject: %s%s%s", *enn, m->from_name,
                  m->from_name[0] ? " " : "", m->from_addr, m->subject,
                  m->unread ? " [UNREAD]" : "",
                  (m->replied == EMAIL_REPLIED_YES) ? " [replied]" : "");
   if (show_category)
      strbuf_appendf(sb, " (%s)", category_label(m->category));
   strbuf_appendf(sb, "\n       Account: %s | %s\n       [ID: %s]\n", acct, ts, m->message_id);
   (*enn)++;
}

/* Emit a titled section over the messages in [0,n) matching `want`. */
static void emit_section(strbuf_t *sb,
                         const email_summary_t *msgs,
                         int n,
                         digest_section_t want,
                         const char *title,
                         int *enn) {
   int count = 0;
   for (int i = 0; i < n; i++) {
      if (message_section(&msgs[i]) == want)
         count++;
   }
   if (count == 0)
      return;
   strbuf_appendf(sb, "\n### %s (%d)\n", title, count);
   for (int i = 0; i < n; i++) {
      if (message_section(&msgs[i]) == want)
         emit_row(sb, &msgs[i], enn, want == SEC_OTHER);
   }
}

char *email_digest_build(int user_id, const email_digest_opts_t *opts) {
   if (!opts)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: internal error (no digest options)");

   int window_sec = opts->window_seconds > 0 ? opts->window_seconds
                                             : EMAIL_DIGEST_DEFAULT_WINDOW_SEC;
   if (window_sec > EMAIL_DIGEST_MAX_WINDOW_SEC)
      window_sec = EMAIL_DIGEST_MAX_WINDOW_SEC;
   /* Row ceiling is authoritative here so every caller (tool or direct) inherits
    * it, alongside the window clamp above. */
   int cap = opts->max > 0 ? opts->max : EMAIL_DIGEST_DEFAULT_MAX;
   if (cap > EMAIL_DIGEST_MAX_ROWS)
      cap = EMAIL_DIGEST_MAX_ROWS;

   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int n_acct = email_service_list_accounts(user_id, accounts, EMAIL_MAX_ACCOUNTS);
   if (n_acct <= 0)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: no email accounts configured. Add one in WebUI Settings -> Email.");

   /* n_acct includes disabled accounts.  If every configured account is disabled
    * the fetch loop below would skip them all and emit a misleading "0 of 0
    * inboxes" success — surface the enable-account guidance instead. */
   int enabled_pre = 0;
   for (int a = 0; a < n_acct; a++)
      if (accounts[a].enabled)
         enabled_pre++;
   if (enabled_pre == 0)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: no email accounts enabled. Enable one in WebUI Settings -> Email.");

   /* Heap scratch: one reusable per-account batch, and a merged buffer sized to
    * the worst case (every account returns a full batch).  Kept off the stack —
    * email_summary_t is ~1.8KB and this runs on the 512KB parallel tool thread. */
   email_summary_t *batch = calloc(DIGEST_FETCH_PER_ACCT, sizeof(email_summary_t));
   email_summary_t *merged = calloc((size_t)n_acct * DIGEST_FETCH_PER_ACCT,
                                    sizeof(email_summary_t));
   if (!batch || !merged) {
      free(batch);
      free(merged);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   }

   time_t now = time(NULL);
   time_t cutoff = now - window_sec;

   strbuf_t status;
   strbuf_init(&status, 256);

   int merged_n = 0;
   int enabled_accounts = 0;
   int ok_accounts = 0;
   int total_unread = 0;

   for (int a = 0; a < n_acct; a++) {
      if (!accounts[a].enabled)
         continue;
      enabled_accounts++;

      /* "inbox" is a portable folder token: the service layer normalizes it to
       * the Gmail INBOX label / IMAP "INBOX" per backend (normalize_folder). */
      int out_count = 0;
      char npt[256] = { 0 };
      /* Resolve by username (the account's login/address), not the display name:
       * find_account matches name OR username first-wins, and display names are
       * not unique (two accounts may both be "Gmail").  Selecting by name would
       * fetch the first such account twice and omit the other's mail.  (Residual
       * edge: find_account checks name before username, so this still mis-resolves
       * if one account's display NAME equals another's username string — a
       * degenerate operator config; the clean fix is threading the DB id through
       * email_summary_t, deferred as disproportionate for these fixes.) */
      int rc = email_service_recent(user_id, accounts[a].username, "inbox", DIGEST_FETCH_PER_ACCT,
                                    opts->unread_only, NULL, batch, DIGEST_FETCH_PER_ACCT,
                                    &out_count, npt, sizeof(npt));
      if (rc != 0) {
         strbuf_appendf(&status, "  %s <%s>: unavailable (fetch error — check account/OAuth)\n",
                        accounts[a].name, accounts[a].username);
         continue;
      }
      ok_accounts++;
      /* unread is reliable on both backends now: Gmail via the UNREAD label,
       * IMAP via the \Seen flag parsed at fetch time. */

      int kept = 0;
      for (int i = 0; i < out_count && merged_n < n_acct * DIGEST_FETCH_PER_ACCT; i++) {
         /* date == 0 means the backend could not parse a timestamp; keep it
          * rather than silently dropping (tri-state "unknown", not "old"). */
         if (batch[i].date > 0 && batch[i].date < cutoff)
            continue;
         merged[merged_n++] = batch[i];
         kept++;
         if (batch[i].unread)
            total_unread++;
      }
      bool cap_hit = (out_count >= DIGEST_FETCH_PER_ACCT);
      strbuf_appendf(&status, "  %s <%s>: %d in window%s\n", accounts[a].name, accounts[a].username,
                     kept,
                     cap_hit ? " (fetch cap reached; older in-window mail may be omitted)" : "");
   }

   qsort(merged, merged_n, sizeof(email_summary_t), cmp_summary_date_desc);

   int shown = merged_n < cap ? merged_n : cap;
   int omitted = merged_n - shown;

   /* Best-effort reply enrichment: Gmail rows are filled here via one in:sent
    * search per account (bounded to the shown/capped rows to limit network
    * cost); IMAP rows already carry their replied state from the \Answered flag
    * parsed at fetch time, and fill_reply_states leaves them untouched.  emit_row
    * renders [replied] for EMAIL_REPLIED_YES and stays silent for NO/UNKNOWN
    * (never asserts "not replied" on a skipped/failed lookup). */
   email_service_fill_reply_states(user_id, merged, shown);

   char wlabel[16];
   format_window(window_sec, wlabel, sizeof(wlabel));
   strbuf_t sb;
   strbuf_init(&sb, 4096);
   strbuf_appendf(&sb,
                  "Email digest — %d message%s across %d of %d inbox%s, %d unread, window=%s.\n",
                  merged_n, merged_n == 1 ? "" : "s", ok_accounts, enabled_accounts,
                  enabled_accounts == 1 ? "" : "es", total_unread, wlabel);
   if (status.len > 0)
      strbuf_appendf(&sb, "Per-account:\n%s", status.buf);
   if (merged_n == 0)
      strbuf_appendf(&sb, "\nNo messages in the last %s.\n", wlabel);

   int enn = 1;
   emit_section(&sb, merged, shown, SEC_IMPORTANT, "Important", &enn);
   emit_section(&sb, merged, shown, SEC_PRIMARY, "Primary", &enn);
   emit_section(&sb, merged, shown, SEC_OTHER, "Updates / Promotions / Social / Forums", &enn);

   if (omitted > 0)
      strbuf_appendf(&sb, "\n(+%d more in window, not shown — raise max or narrow the window.)\n",
                     omitted);

   strbuf_free(&status);
   free(batch);
   free(merged);

   char *result = sb.buf ? strdup(sb.buf) : NULL;
   bool oom = strbuf_oom(&sb) || (sb.buf && !result);
   strbuf_free(&sb);
   if (!result)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   if (oom)
      OLOG_WARNING("email_digest: output truncated (strbuf hit its cap)");
   return result;
}
