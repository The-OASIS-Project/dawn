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
 * Flow: enumerate enabled accounts -> page each inbox newest-first through the
 * email_service layer (which stamps account name + address on each row) until the
 * window start, the end of the mailbox, or the account's digest depth -> keep
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
#include "tools/email_digest_internal.h"
#include "tools/email_service.h"
#include "tools/email_types.h"
#include "tools/tool_registry.h"

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

/* Growable array of in-window rows merged across accounts.  Worst case it holds
 * every scanned row: accounts x EMAIL_DIGEST_DEPTH_MAX x ~1.8KB (16 x 200 -> ~5.7MB,
 * briefly ~8.6MB while the last doubling copies).  Raising the depth ceiling
 * raises this bound. */
typedef struct {
   email_summary_t *v;
   int n;
   int cap;
} digest_rows_t;

static bool digest_rows_push(digest_rows_t *r, const email_summary_t *m) {
   if (r->n == r->cap) {
      int ncap = r->cap > 0 ? r->cap * 2 : EMAIL_MAX_FETCH_RESULTS;
      email_summary_t *nv = realloc(r->v, (size_t)ncap * sizeof(*nv));
      if (!nv)
         return false;
      r->v = nv;
      r->cap = ncap;
   }
   r->v[r->n++] = *m;
   return true;
}

/* Page one account's inbox newest-first until the window start, the end of the
 * mailbox, or the account's digest depth (see email_digest_next_step), keeping
 * in-window rows and appending one per-account status line.
 * @return true if the account was reachable (its first page fetched). */
static bool digest_fetch_account(int user_id,
                                 const email_account_t *acct,
                                 bool unread_only,
                                 time_t cutoff,
                                 email_summary_t *batch,
                                 digest_rows_t *rows,
                                 strbuf_t *status,
                                 int *unread_total) {
   /* The token sent and the token returned are kept apart so a cursor that
    * fails to advance (same token back) can be detected. */
   char tok_in[EMAIL_PAGE_TOKEN_LEN] = { 0 };
   char tok_out[EMAIL_PAGE_TOKEN_LEN] = { 0 };
   email_digest_page_state_t st = { 0 };
   st.depth = acct->digest_depth; /* clamped to [1, EMAIL_DIGEST_DEPTH_MAX] on DB load */
   email_digest_step_t step = EMAIL_DIGEST_MORE;
   int kept = 0;
   bool fetch_error = false;
   bool oom = false;

   while (step == EMAIL_DIGEST_MORE) {
      int want = st.depth - st.fetched;
      if (want > EMAIL_MAX_FETCH_RESULTS)
         want = EMAIL_MAX_FETCH_RESULTS;
      int out_count = 0;
      /* Resolve by username (the account's login/address), not the display name:
       * find_account matches name OR username first-wins, and display names are
       * not unique (two accounts may both be "Gmail").  Selecting by name would
       * fetch the first such account twice and omit the other's mail.  (Residual
       * edge: find_account checks name before username, so this still mis-resolves
       * if one account's display NAME equals another's username string — a
       * degenerate operator config.)  "inbox" is a portable folder token the
       * service layer normalizes per backend. */
      int rc = email_service_recent(user_id, acct->username, "inbox", want, unread_only,
                                    tok_in[0] ? tok_in : NULL, batch, EMAIL_MAX_FETCH_RESULTS,
                                    &out_count, tok_out, sizeof(tok_out));
      if (rc != EMAIL_RC_OK) {
         if (st.pages == 0) {
            strbuf_appendf(status, "  %s <%s>: unavailable (fetch error — check account/OAuth)\n",
                           acct->name, acct->username);
            return false;
         }
         fetch_error = true; /* keep what earlier pages found */
         break;
      }

      for (int i = 0; i < out_count; i++) {
         /* date == 0 means the backend could not parse a timestamp; keep it
          * rather than silently dropping (tri-state "unknown", not "old"). */
         if (batch[i].date > 0 && batch[i].date < cutoff)
            continue;
         if (!digest_rows_push(rows, &batch[i])) {
            OLOG_ERROR("email_digest: out of memory merging rows for %s", acct->username);
            oom = true;
            break;
         }
         kept++;
         if (batch[i].unread)
            (*unread_total)++;
      }
      if (oom)
         break;

      st.pages++;
      st.fetched += out_count;
      st.last_returned = out_count;
      st.reached_cutoff = email_digest_page_reached_cutoff(batch, out_count, cutoff);
      st.has_token = tok_out[0] != '\0';
      st.token_repeated = st.has_token && strcmp(tok_out, tok_in) == 0;
      step = email_digest_next_step(&st);
      if (step == EMAIL_DIGEST_MORE)
         memcpy(tok_in, tok_out, sizeof(tok_in));
   }

   strbuf_appendf(status, "  %s <%s>: %d in window", acct->name, acct->username, kept);
   if (oom)
      strbuf_appendf(status, " (out of memory; older in-window mail omitted)");
   else if (fetch_error)
      strbuf_appendf(status,
                     " (fetch error after %d messages; older in-window mail may be omitted)",
                     st.fetched);
   else if (step == EMAIL_DIGEST_STOP_DEPTH)
      strbuf_appendf(status,
                     " (digest depth %d reached; older in-window mail may be omitted — raise "
                     "\"Digest depth\" for this account in Settings -> Email)",
                     st.depth);
   else if (step == EMAIL_DIGEST_STOP_PAGE_LIMIT)
      strbuf_appendf(status, " (stopped after %d pages; older in-window mail may be omitted)",
                     st.pages);
   strbuf_appendf(status, "\n");
   return true;
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

   /* Heap scratch: one reusable one-page batch, and a growable merged buffer of
    * in-window rows.  Kept off the stack — email_summary_t is ~1.8KB and this
    * runs on the 512KB parallel tool thread. */
   email_summary_t *batch = calloc(EMAIL_MAX_FETCH_RESULTS, sizeof(email_summary_t));
   if (!batch)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   digest_rows_t rows = { 0 }; /* grown on demand by digest_rows_push */

   time_t now = time(NULL);
   time_t cutoff = now - window_sec;

   strbuf_t status;
   strbuf_init(&status, 256);

   int enabled_accounts = 0;
   int ok_accounts = 0;
   int total_unread = 0;

   for (int a = 0; a < n_acct; a++) {
      if (!accounts[a].enabled)
         continue;
      enabled_accounts++;
      if (digest_fetch_account(user_id, &accounts[a], opts->unread_only, cutoff, batch, &rows,
                               &status, &total_unread))
         ok_accounts++;
   }

   email_summary_t *merged = rows.v;
   int merged_n = rows.n;

   if (merged_n > 1)
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
