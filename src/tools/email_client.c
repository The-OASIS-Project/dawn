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
 * Email client — IMAP/SMTP operations via libcurl.
 *
 * Security notes:
 * - IMAP SEARCH terms use quoted-string syntax with control-char stripping and
 *   "/\ escaping (append_imap_quoted) to prevent command injection
 * - Date parameters are parsed via strptime() and reconstructed via strftime()
 * - TLS is enforced for non-loopback servers
 * - Credentials are wiped via sodium_memzero() on all code paths
 */

#define _GNU_SOURCE /* strptime, strcasestr */

#include "tools/email_client.h"

#include <ctype.h>
#include <curl/curl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/buf_printf.h"
#include "core/curl_buffer.h"
#include "logging.h"
#include "tools/email_client_internal.h"
#include "tools/email_display.h"
#include "tools/email_instrument.h"
#include "tools/email_mime.h"
#include "tools/email_parse.h"
#include "tools/email_transfer.h"
#include "tools/html_parser.h"

/* =============================================================================
 * Buffer for CURL responses
 *
 * Uses the shared curl_buffer_t (include/core/curl_buffer.h) with a 1 MB
 * cap on IMAP/SMTP responses.  The shared helper truncates with a flag
 * instead of aborting mid-transfer — callers MUST check `buf.truncated`
 * after curl_easy_perform and reject the response.
 * ============================================================================= */

#define EMAIL_MAX_RESPONSE_SIZE (1024 * 1024) /* 1 MB cap on IMAP/SMTP responses */

/* IMAP per-op timeout budget (TOTAL transfer time: connect + TLS + LOGIN + the
 * command).  The default covers the cheap ops (SEARCH ALL/UNSEEN, ENVELOPE
 * FETCH, LIST, a bounded single-message read).  Content SEARCH (TEXT/BODY) is
 * the outlier: on a server with no FTS index it brute-force-scans every message
 * body, and slow shared-hosting auth (measured ~2-5 s just for LOGIN) eats into
 * the budget too — so an explicit keyword search gets a much larger allowance.
 * A hung/pooled connection is bounded by CONNECT + these. */
#define EMAIL_IMAP_CONNECT_TIMEOUT_SEC 15L
#define EMAIL_IMAP_TIMEOUT_SEC 30L
#define EMAIL_IMAP_SEARCH_TIMEOUT_SEC 60L

/* =============================================================================
 * SMTP Upload Buffer for email_send
 * ============================================================================= */

typedef struct {
   const char *data;
   size_t len;
   size_t offset;
} smtp_upload_t;

static size_t smtp_read_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
   smtp_upload_t *upload = (smtp_upload_t *)userdata;
   size_t room = size * nitems;
   size_t remaining = upload->len - upload->offset;
   if (remaining == 0)
      return 0;
   size_t n = remaining < room ? remaining : room;
   memcpy(buffer, upload->data + upload->offset, n);
   upload->offset += n;
   return n;
}

/* =============================================================================
 * CURL Setup Helpers
 * ============================================================================= */

static void setup_auth(CURL *curl, const email_conn_t *conn) {
   /* Username is required for both auth methods — XOAUTH2 SASL includes it */
   curl_easy_setopt(curl, CURLOPT_USERNAME, conn->username);

   if (conn->bearer_token[0]) {
      curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, conn->bearer_token);
      curl_easy_setopt(curl, CURLOPT_LOGIN_OPTIONS, "AUTH=XOAUTH2");
   } else {
      curl_easy_setopt(curl, CURLOPT_PASSWORD, conn->password);
   }
}

/* IMAP receive buffer: bounds the longest server line libcurl reads intact.
 * ~2.8x a worst-case IMAP_SEARCH_WINDOW reply (every message matching, 10-digit
 * UIDs: ~45 KB). */
#define IMAP_RECV_BUFFER_SIZE (128 * 1024)

CURL *email_imap_handle_create(const email_conn_t *conn) {
   CURL *curl = curl_easy_init();
   if (!curl)
      return NULL;

   curl_easy_setopt(curl, CURLOPT_TIMEOUT, EMAIL_IMAP_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, EMAIL_IMAP_CONNECT_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
   /* libcurl's IMAP reader truncates any server response line longer than its
    * receive buffer (see "Windowed UID SEARCH" below).  The 16 KB default is
    * smaller than one full SEARCH window's reply can be, so size it well past it. */
   if (curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, (long)IMAP_RECV_BUFFER_SIZE) != CURLE_OK)
      OLOG_ERROR("email: libcurl refused the IMAP receive buffer size; long SEARCH replies "
                 "may be truncated");
   setup_auth(curl, conn);

   /* TLS certificate verification */
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

   /* SSRF protection: restrict to IMAP/IMAPS only */
   DAWN_CURL_SET_PROTOCOLS(curl, "imap,imaps");
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

   /* TLS settings — STARTTLS on port 143 */
   if (strstr(conn->imap_url, "imap://") != NULL) {
      curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
   }

   return curl;
}

static CURL *create_smtp_handle(const email_conn_t *conn) {
   CURL *curl = curl_easy_init();
   if (!curl)
      return NULL;

   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
   curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
   setup_auth(curl, conn);

   /* TLS certificate verification */
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

   /* SSRF protection: restrict to SMTP/SMTPS only */
   DAWN_CURL_SET_PROTOCOLS(curl, "smtp,smtps");
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

   /* TLS settings — STARTTLS on port 587 */
   if (strstr(conn->smtp_url, "smtp://") != NULL) {
      curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
   }

   return curl;
}

/* =============================================================================
 * IMAP Date Validation
 *
 * Parse ISO 8601 date via strptime(), reconstruct as IMAP date via strftime().
 * Never pass raw date strings into IMAP commands.
 * ============================================================================= */

static bool validate_imap_date(const char *iso_date, char *imap_out, size_t out_len) {
   if (!iso_date || !iso_date[0])
      return false;

   struct tm tm_info;
   memset(&tm_info, 0, sizeof(tm_info));

   /* Require the whole string to be a YYYY-MM-DD date (strptime tolerates
    * trailing text otherwise).  Matches the shared email_parse_valid_iso_date so
    * the IMAP and Gmail backends accept/reject the same dates. */
   const char *end = strptime(iso_date, "%Y-%m-%d", &tm_info);
   if (!end || *end != '\0')
      return false;

   /* Reconstruct in IMAP format: DD-Mon-YYYY */
   if (strftime(imap_out, out_len, "%d-%b-%Y", &tm_info) == 0)
      return false;

   return true;
}

bool email_search_date_valid(const char *iso) {
   /* email_client-layer accessor for the shared pure validator, so email_tool
    * (which doesn't include email_parse.h) can tell whether a search was
    * date-bounded without diverging from what the SEARCH builders emit. */
   return email_parse_valid_iso_date(iso);
}

/* The IMAP SEARCH quoted-string builders (email_imap_append_quoted /
 * email_imap_append_search_key) are pure string helpers and live in the
 * unit-tested email_parse.c — see the CRITICAL note there on curl's
 * CUSTOMREQUEST URL-decode (why '%' must be escaped). */

/* =============================================================================
 * UID page selection (shared by recent + search)
 * ============================================================================= */

/**
 * Pick one page of UIDs from a UID SEARCH response: the @p wanted newest, listed
 * newest-first in @p rev_uids for the FETCH command, and fill the paging cursor
 * from the SEARCH selection itself — never from fetched rows, since a FETCH can
 * drop a UID (expunged in between, unparseable envelope) and a row-derived cursor
 * would then skip or repeat mail.
 *
 * NOTE: the FETCH *response* comes back in whatever order the server chooses
 * (typically ascending), so the rows in out[] are NOT newest-first.  Callers that
 * care about order sort by date or UID themselves.
 *
 * @return number of UIDs written to @p rev_uids.
 */
static int select_uid_page(const char *response,
                           int wanted,
                           email_imap_page_t *page,
                           uint32_t *rev_uids) {
   uint32_t asc[EMAIL_MAX_FETCH_RESULTS];
   if (wanted > EMAIL_MAX_FETCH_RESULTS)
      wanted = EMAIL_MAX_FETCH_RESULTS;
   int total = 0;
   int n = email_imap_select_newest_uids(response, asc, wanted, &total);
   for (int i = 0; i < n; i++)
      rev_uids[i] = asc[n - 1 - i];
   if (page) {
      /* More (older) matches remain below the oldest UID on this page.  A cursor
       * of 1 would mean "below UID 1" = nothing, so it is never issued. */
      page->next_before_uid = (total > n && n > 0 && asc[0] > 1) ? asc[0] : 0;
   }
   return n;
}

/* Append " UID 1:<before-1>" so a continuation page only matches older mail.
 * The UID search key is core RFC 3501; the sequence window (see
 * imap_windowed_search) is what bounds the reply size. */
static void append_uid_range(char *cmd, size_t *pos, size_t *rem, const email_imap_page_t *page) {
   if (page && page->before_uid > 1)
      BUF_PRINTF(cmd, *pos, *rem, " UID 1:%u", page->before_uid - 1);
}

static void page_reset_outputs(email_imap_page_t *page) {
   if (!page)
      return;
   page->next_before_uid = 0;
   page->next_uidvalidity = 0;
   page->stale = false;
}

/* A pinned continuation page failed: decide whether it's because the mailbox's
 * UIDVALIDITY epoch changed (the cursor is stale) rather than some other SELECT
 * failure such as a missing folder, which reports the same curl code.  Stale only
 * when the server's SELECT reported an epoch AND it differs from the pinned one.
 * @return true (and logs) when the cursor is stale. */
static bool page_mark_stale(email_imap_page_t *page,
                            CURLcode res,
                            const email_instrument_ctx_t *dctx) {
   if (!page || page->uidvalidity == 0 || page->before_uid <= 1 ||
       res != CURLE_REMOTE_FILE_NOT_FOUND || dctx->uidvalidity == 0 ||
       dctx->uidvalidity == page->uidvalidity)
      return false;
   OLOG_WARNING("email: IMAP paging cursor is stale (mailbox UIDVALIDITY %u, cursor %u)",
                dctx->uidvalidity, page->uidvalidity);
   page->stale = true;
   return true;
}

/* Mailbox URL; a continuation page pins the UIDVALIDITY epoch its cursor came
 * from, so libcurl fails the SELECT (CURLE_REMOTE_FILE_NOT_FOUND) instead of
 * paging a rebuilt mailbox with stale UIDs. */
static void build_mailbox_url(const email_conn_t *conn,
                              const char *encoded_folder,
                              const email_imap_page_t *page,
                              char *url,
                              size_t url_len) {
   if (page && page->before_uid > 1 && page->uidvalidity > 0)
      snprintf(url, url_len, "%s/%s;UIDVALIDITY=%u", conn->imap_url, encoded_folder,
               page->uidvalidity);
   else
      snprintf(url, url_len, "%s/%s", conn->imap_url, encoded_folder);
}

/* =============================================================================
 * Parse email headers from FETCH response
 * ============================================================================= */

/* =============================================================================
 * Batch FETCH summaries for multiple UIDs
 *
 * One UID FETCH (FLAGS INTERNALDATE ENVELOPE) for the whole set.  All three
 * items are delivered INLINE (no IMAP literal), which matters twice over:
 *   1. libcurl's custom-command path (used for CUSTOMREQUEST) writes untagged
 *      response lines to us but DISCARDS literal octet blocks — so BODY[HEADER],
 *      which is a literal, can never be read this way.
 *   2. The only libcurl path that DOES stream a body literal is the URL form,
 *      but it issues plain BODY[...] (not BODY.PEEK), which marks messages
 *      \Seen — unacceptable for a digest that just lists mail.
 * ENVELOPE sidesteps both: it carries From/Subject/Date as an inline structure
 * and never touches the body, so nothing is marked read.  See email_parse.c
 * for the ENVELOPE + paren-matching parsers (unit-tested).
 * ============================================================================= */

/* Apply the per-message IMAP FETCH metadata — FLAGS and INTERNALDATE — onto a
 * summary.  `seg` is one message's full item-list text ("* N FETCH (UID .. FLAGS
 * (..) INTERNALDATE ".." ENVELOPE (..))").  FLAGS drives unread (\Seen absent)
 * and the tri-state replied (\Answered present -> YES, else NO); INTERNALDATE is
 * the reliable server receive time.  Absent tokens leave the caller's memset
 * defaults (unread false, replied UNKNOWN, date 0).
 *
 * SECURITY: the FLAGS/INTERNALDATE search is confined to the region BEFORE
 * "ENVELOPE".  Everything inside ENVELOPE (subject, sender name) is
 * sender-controlled, so without this bound a crafted subject like
 * `FLAGS (\Answered)` or `INTERNALDATE "01-Jan-1990..."` could forge the
 * read/reply state or receive date.  We request `(FLAGS INTERNALDATE ENVELOPE)`
 * in that order, so the genuine items always fall in this trusted prefix; a
 * match at or beyond ENVELOPE is ignored (fail-safe to the default).  Returns
 * true iff a FLAGS group was found in the prefix. */
static bool apply_fetch_metadata(const char *seg, email_summary_t *out) {
   /* Trusted prefix ends at the (sender-controlled) ENVELOPE. */
   const char *env = strcasestr(seg, "ENVELOPE");
   size_t prefix = env ? (size_t)(env - seg) : strlen(seg);

   bool flags_found = false;
   /* FLAGS (...) group — only if it lies in the trusted prefix. */
   const char *flags_kw = strcasestr(seg, "FLAGS");
   if (flags_kw && (size_t)(flags_kw - seg) < prefix) {
      const char *lp = strchr(flags_kw, '(');
      const char *rp = lp ? strchr(lp, ')') : NULL;
      if (lp && rp && rp > lp) {
         char group[256];
         size_t glen = (size_t)(rp - lp + 1);
         if (glen >= sizeof(group))
            glen = sizeof(group) - 1;
         memcpy(group, lp, glen);
         group[glen] = '\0';
         out->unread = !email_imap_flags_contains(group, "\\Seen");
         out->replied = email_imap_flags_contains(group, "\\Answered") ? EMAIL_REPLIED_YES
                                                                       : EMAIL_REPLIED_NO;
         flags_found = true;
      }
   }

   /* INTERNALDATE "dd-Mon-yyyy HH:MM:SS +ZZZZ" — reliable server receive time,
    * likewise only trusted from the pre-ENVELOPE prefix. */
   const char *idate_kw = strcasestr(seg, "INTERNALDATE");
   if (idate_kw && (size_t)(idate_kw - seg) < prefix) {
      const char *q1 = strchr(idate_kw, '"');
      const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
      if (q1 && q2 && q2 > q1 + 1) {
         char idate[64];
         size_t ilen = (size_t)(q2 - (q1 + 1));
         if (ilen >= sizeof(idate))
            ilen = sizeof(idate) - 1;
         memcpy(idate, q1 + 1, ilen);
         idate[ilen] = '\0';
         time_t t = email_parse_imap_internaldate(idate);
         if (t > 0)
            out->date = t;
      }
   }
   return flags_found;
}

static int batch_fetch_headers(CURL *curl,
                               const email_conn_t *conn,
                               const char *encoded_folder,
                               const uint32_t *uids,
                               int uid_count,
                               email_summary_t *out,
                               int max_out,
                               int *out_count) {
   if (uid_count <= 0)
      return 0;

   /* Build comma-separated UID list: "uid1,uid2,...,uidN" */
   char uid_list[1024];
   size_t upos = 0;
   size_t urem = sizeof(uid_list);
   for (int i = 0; i < uid_count && urem > 12; i++) {
      if (i > 0)
         BUF_PRINTF(uid_list, upos, urem, ",");
      BUF_PRINTF(uid_list, upos, urem, "%u", uids[i]);
   }

   char url[1024];
   snprintf(url, sizeof(url), "%s/%s", conn->imap_url, encoded_folder);
   curl_easy_setopt(curl, CURLOPT_URL, url);

   char fetch_cmd[1200];
   snprintf(fetch_cmd, sizeof(fetch_cmd), "UID FETCH %s (FLAGS INTERNALDATE ENVELOPE)", uid_list);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, fetch_cmd);

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      OLOG_WARNING("email: batch UID FETCH failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      return 1;
   }
   if (buf.truncated) {
      OLOG_WARNING("email: batch UID FETCH response exceeded %d byte cap; rejecting",
                   EMAIL_MAX_RESPONSE_SIZE);
      curl_buffer_free(&buf);
      return 1;
   }

   /* Parse the response, one "* <seq> FETCH (UID <uid> FLAGS (..) INTERNALDATE
    * ".." ENVELOPE (..))" untagged line per message.  email_imap_next_fetch
    * isolates each message (bounding it at its quote-aware matching ')', and
    * resyncing past a literal-truncated envelope so one bad message can't drop
    * the rest of the batch — see its contract). */
   const char *p = buf.data;
   const char *seg_start = NULL;
   size_t seg_len = 0;
   uint32_t uid = 0;
   while (*out_count < max_out &&
          (p = email_imap_next_fetch(p, &seg_start, &seg_len, &uid)) != NULL) {
      char *seg = malloc(seg_len + 1);
      if (!seg)
         break;
      memcpy(seg, seg_start, seg_len);
      seg[seg_len] = '\0';

      email_summary_t *s = &out[*out_count];
      memset(s, 0, sizeof(*s));
      s->uid = uid;

      /* FLAGS -> unread/replied, INTERNALDATE -> date (server receive time). */
      apply_fetch_metadata(seg, s);

      /* date_str is the human-readable date the recent/search formatter prints
       * (the digest sorts/filters on the epoch instead).  Derive it from the
       * reliable INTERNALDATE epoch rather than the sender's Date header. */
      if (s->date > 0) {
         struct tm tmv;
         if (localtime_r(&s->date, &tmv))
            strftime(s->date_str, sizeof(s->date_str), "%a, %d %b %Y %H:%M", &tmv);
      }

      /* ENVELOPE -> subject + first From (RAW envelope strings are RFC 2047-
       * encoded; decode them with the same word decoder the header path uses).
       * A message whose ENVELOPE contains an IMAP literal ({N}) in a string
       * field — a raw non-RFC2047 subject/name — loses that field (and the rest
       * of its envelope): libcurl's custom-command path discards literal octets.
       * Such a message keeps its date/flags/id and stays readable via `read`,
       * but its subject/sender degrade cleanly to blank — never a wrong value. */
      char subj_raw[256] = { 0 };
      char fname_raw[256] = { 0 };
      char faddr[256] = { 0 };
      if (email_parse_envelope(seg, subj_raw, sizeof(subj_raw), fname_raw, sizeof(fname_raw), faddr,
                               sizeof(faddr))) {
         /* As reading the message does: words decoded, invisible and
          * direction-changing characters dropped. */
         email_mime_header_text(subj_raw, s->subject, sizeof(s->subject));
         email_mime_header_text(fname_raw, s->from_name, sizeof(s->from_name));
         email_display_sanitize(faddr, strlen(faddr), s->from_addr, sizeof(s->from_addr), 0);
      }

      (*out_count)++;
      free(seg);
   }

   curl_buffer_free(&buf);
   return 0;
}

/* =============================================================================
 * URL-encode IMAP Folder Name
 *
 * Percent-encode folder name for use in IMAP curl URLs.
 * curl handles mUTF-7 decoding internally for IMAP URLs.
 * ============================================================================= */

static void url_encode_folder(const char *folder, char *out, size_t out_len) {
   size_t j = 0;
   for (size_t i = 0; folder[i] && j < out_len - 4; i++) {
      unsigned char c = (unsigned char)folder[i];
      if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-') {
         out[j++] = c;
      } else {
         j += snprintf(out + j, out_len - j, "%%%02X", c);
      }
   }
   out[j] = '\0';
}

/* =============================================================================
 * Windowed UID SEARCH
 *
 * libcurl truncates any IMAP server response line longer than its internal
 * buffer (curl known bug "IMAP SEARCH ALL truncated response", still open): it
 * keeps the first 40 bytes and splices on the tail, silently dropping UIDs and
 * gluing two partial numbers into a bogus one.  The limit is the handle's
 * receive buffer (CURLOPT_BUFFERSIZE, 16 KB by default — ~2k UIDs), so a big
 * mailbox breaks a single `UID SEARCH ALL` long before any response-size cap.
 * create_imap_handle raises the buffer; windowing keeps every reply far below it.
 *
 * So a search never asks for more than IMAP_SEARCH_WINDOW messages at once: it
 * walks down from the newest message in message-SEQUENCE-number windows
 * ("UID SEARCH <criteria> <lo>:<hi>").  Sequence numbers are dense (1..EXISTS)
 * and ascend in UID order, so a window of N returns at most N UIDs whatever the
 * UID gaps, and the walk is bounded by the mailbox size.  The paging cursor
 * stays a UID (" UID 1:<X-1>" AND'd onto every window), so it survives expunges.
 * ============================================================================= */

/* Messages per SEARCH window.  A reply grows with the number of MATCHES, so the
 * worst case is every message matching at 10-digit UIDs: ~45 KB, still ~2.8x
 * under IMAP_RECV_BUFFER_SIZE. */
#define IMAP_SEARCH_WINDOW 4096u
/* Windows per call before handing back a resume cursor (262k messages). */
#define IMAP_SEARCH_MAX_WINDOWS 64
/* Floor on a window's time slice, so a nearly spent budget still lets the first
 * window run rather than failing instantly. */
#define IMAP_MIN_WINDOW_MS 2000L

/* One custom command on the already-configured handle, reply into @p buf
 * (caller frees).  Uses the breaker-aware perform only for the op's first,
 * auth-bearing command. */
CURLcode email_imap_run_command(CURL *curl,
                                email_instrument_ctx_t *dctx,
                                const email_conn_t *conn,
                                const char *op,
                                const char *cmd,
                                bool first,
                                curl_buffer_t *buf) {
   curl_buffer_init_with_max(buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
   CURLcode res = first ? email_instrument_perform(curl, dctx, conn->username, op)
                        : curl_easy_perform(curl);
   if (res == CURLE_OK && buf->truncated) {
      OLOG_ERROR("email: IMAP %s response exceeded %d byte cap; rejecting", op,
                 EMAIL_MAX_RESPONSE_SIZE);
      res = CURLE_FILESIZE_EXCEEDED;
   }
   return res;
}

/* Legacy single-shot search, used only when the server's SELECT reported no
 * EXISTS count (RFC 3501 requires one) so windows can't be computed. */
static int imap_search_unwindowed(CURL *curl,
                                  email_instrument_ctx_t *dctx,
                                  const email_conn_t *conn,
                                  const char *op,
                                  const char *criteria,
                                  email_imap_page_t *page,
                                  int wanted,
                                  uint32_t *rev_uids,
                                  int *rev_count,
                                  CURLcode *res_out) {
   char cmd[2176];
   size_t pos = 0;
   size_t rem = sizeof(cmd);
   BUF_PRINTF(cmd, pos, rem, "UID SEARCH%s", criteria);
   append_uid_range(cmd, &pos, &rem, page);
   if (!criteria[0] && !(page && page->before_uid > 1))
      BUF_PRINTF(cmd, pos, rem, " ALL");
   if (rem <= 1) {
      OLOG_ERROR("email: IMAP SEARCH command too long; refusing to send it truncated");
      *res_out = CURLE_URL_MALFORMAT;
      return 1;
   }

   curl_buffer_t buf;
   CURLcode res = email_imap_run_command(curl, dctx, conn, op, cmd, false, &buf);
   if (res != CURLE_OK) {
      *res_out = res;
      OLOG_ERROR("email: IMAP SEARCH failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      return 1;
   }
   /* Unwindowed, one reply can outgrow libcurl's line buffer and be silently
    * spliced (see "Windowed UID SEARCH"); at least say when it's getting close. */
   if (buf.size > IMAP_RECV_BUFFER_SIZE * 3 / 4)
      OLOG_WARNING("email: unwindowed IMAP SEARCH reply is %zu bytes, near the %d-byte line "
                   "limit; results may be incomplete",
                   buf.size, IMAP_RECV_BUFFER_SIZE);
   *rev_count = select_uid_page(buf.data, wanted, page, rev_uids);
   curl_buffer_free(&buf);
   return 0;
}

static long long monotonic_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Run a tiny lookup whose "* SEARCH" reply is at most one number (a UID or a
 * sequence number).  @return true and sets @p out when one came back. */
static bool imap_lookup_one(CURL *curl,
                            email_instrument_ctx_t *dctx,
                            const email_conn_t *conn,
                            const char *op,
                            const char *cmd,
                            uint32_t *out) {
   curl_buffer_t buf;
   CURLcode res = email_imap_run_command(curl, dctx, conn, op, cmd, false, &buf);
   uint32_t v[1];
   int total = 0;
   int n = (res == CURLE_OK) ? email_imap_select_newest_uids(buf.data, v, 1, &total) : 0;
   curl_buffer_free(&buf);
   if (n != 1 || total != 1)
      return false;
   *out = v[0];
   return true;
}

/**
 * Select the mailbox and find the @p wanted newest matching UIDs (newest first
 * in @p rev_uids), filling the paging cursor in @p page.
 *
 * A continuation page starts its walk just below the cursor's own message
 * (looked up by UID), not at the top of the mailbox.  The window walk shares one
 * deadline of @p budget_sec (the SELECT before it and the header FETCH after it
 * have their own timeouts).  If the deadline or the window cap would be exceeded
 * with older messages still unscanned, the walk stops and the cursor resumes just
 * below the lowest message actually scanned, so a partial page never looks like
 * the end of the results.
 *
 * @param criteria  Search keys with a leading space each (" UNSEEN FROM \"x\""),
 *                  or "" for all messages.
 * @param res_out   Set to the failing CURLcode on error (for auth/timeout mapping).
 * @return 0 on success, 1 on failure (page->stale set if the cursor's epoch changed).
 */
static int imap_windowed_search(CURL *curl,
                                email_instrument_ctx_t *dctx,
                                const email_conn_t *conn,
                                const char *op,
                                const char *criteria,
                                email_imap_page_t *page,
                                int wanted,
                                long budget_sec,
                                uint32_t *rev_uids,
                                int *rev_count,
                                CURLcode *res_out) {
   *rev_count = 0;
   *res_out = CURLE_OK;
   if (wanted > EMAIL_MAX_FETCH_RESULTS)
      wanted = EMAIL_MAX_FETCH_RESULTS;
   const long long deadline_ms = monotonic_ms() + budget_sec * 1000;

   /* NOOP makes libcurl log in and SELECT the mailbox; the SELECT response
    * carries EXISTS + UIDVALIDITY, captured by the instrument's debug callback.
    * A pinned cursor whose epoch changed fails right here. */
   curl_buffer_t buf;
   CURLcode res = email_imap_run_command(curl, dctx, conn, op, "NOOP", true, &buf);
   curl_buffer_free(&buf);
   if (res != CURLE_OK) {
      *res_out = res;
      if (!page_mark_stale(page, res, dctx))
         OLOG_ERROR("email: IMAP SELECT failed: %s", curl_easy_strerror(res));
      return 1;
   }
   if (page)
      page->next_uidvalidity = dctx->uidvalidity;

   if (!dctx->exists_seen) {
      OLOG_WARNING("email: IMAP server sent no EXISTS count; searching unwindowed");
      return imap_search_unwindowed(curl, dctx, conn, op, criteria, page, wanted, rev_uids,
                                    rev_count, res_out);
   }

   char cmd[2176];
   _Static_assert(sizeof(cmd) >= 2048 + 64, "window command must fit the criteria + bounds");
   size_t pos;
   size_t rem;

   uint32_t seq_hi = dctx->exists;
   if (page && page->before_uid > 1 && seq_hi > 0) {
      /* Plain (non-UID) SEARCH answers with the sequence number.  Everything
       * below the cursor's message has a lower UID, so start right under it.
       * If it was expunged meanwhile, fall back to walking from the top. */
      uint32_t seq = 0;
      snprintf(cmd, sizeof(cmd), "SEARCH UID %u", page->before_uid);
      if (imap_lookup_one(curl, dctx, conn, op, cmd, &seq) && seq >= 1 && seq <= seq_hi)
         seq_hi = seq - 1;
   }

   /* 64-bit so a legitimate UID of UINT32_MAX still passes the ceiling check. */
   uint64_t upper = (page && page->before_uid > 1) ? page->before_uid : (uint64_t)UINT32_MAX + 1;
   bool more_below = false; /* a scanned window had more matches than we took */
   bool stopped_early = false;
   int windows = 0;
   long long slowest_ms = 0;
   uint32_t tmp[EMAIL_MAX_FETCH_RESULTS];

   while (seq_hi >= 1 && *rev_count < wanted) {
      long long left_ms = deadline_ms - monotonic_ms();
      /* Stop BEFORE a window that likely won't finish in time (judged by the
       * slowest one so far, plus a quarter), rather than letting curl kill it and
       * lose the pages already scanned. */
      if (windows > 0 && (windows == IMAP_SEARCH_MAX_WINDOWS || left_ms <= 0 ||
                          left_ms < slowest_ms + slowest_ms / 4)) {
         stopped_early = true;
         break;
      }
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       (long)(left_ms > IMAP_MIN_WINDOW_MS ? left_ms : IMAP_MIN_WINDOW_MS));
      uint32_t seq_lo = seq_hi > IMAP_SEARCH_WINDOW ? seq_hi - IMAP_SEARCH_WINDOW + 1 : 1;

      pos = 0;
      rem = sizeof(cmd);
      BUF_PRINTF(cmd, pos, rem, "UID SEARCH%s %u:%u", criteria, seq_lo, seq_hi);
      append_uid_range(cmd, &pos, &rem, page);
      if (rem <= 1) {
         OLOG_ERROR("email: IMAP SEARCH command too long; refusing to send it truncated");
         *res_out = CURLE_URL_MALFORMAT;
         return 1;
      }

      long long window_start_ms = monotonic_ms();
      res = email_imap_run_command(curl, dctx, conn, op, cmd, false, &buf);
      if (res == CURLE_OPERATION_TIMEDOUT && windows > 0) {
         /* Out of time mid-walk: keep what earlier windows found and hand back a
          * resume cursor (this window counts as unscanned; seq_hi is unchanged). */
         curl_buffer_free(&buf);
         stopped_early = true;
         break;
      }
      if (res != CURLE_OK) {
         *res_out = res;
         OLOG_ERROR("email: IMAP SEARCH failed: %s", curl_easy_strerror(res));
         curl_buffer_free(&buf);
         return 1;
      }
      long long took_ms = monotonic_ms() - window_start_ms;
      if (took_ms > slowest_ms)
         slowest_ms = took_ms;
      int need = wanted - *rev_count;
      int total = 0;
      int n = email_imap_select_newest_uids(buf.data, tmp, need, &total);
      curl_buffer_free(&buf);
      windows++;

      /* A window can't match more messages than it spans: anything else is a
       * mangled reply, so fail rather than show the wrong mail. */
      if ((uint32_t)total > seq_hi - seq_lo + 1) {
         OLOG_ERROR("email: IMAP SEARCH window %u:%u returned more matches than it spans", seq_lo,
                    seq_hi);
         *res_out = CURLE_RECV_ERROR;
         return 1;
      }
      /* Every UID must sit below the cursor and below what's already taken
       * (sequence order is UID order).  A concurrent expunge by another client
       * can renumber a message into this window a second time: drop it. */
      uint64_t ceiling = *rev_count > 0 ? rev_uids[*rev_count - 1] : upper;
      int keep = n;
      while (keep > 0 && tmp[keep - 1] >= ceiling)
         keep--;
      if (keep < n)
         OLOG_INFO("email: IMAP SEARCH window %u:%u: dropped %d already-seen UID(s)", seq_lo,
                   seq_hi, n - keep);
      for (int i = keep - 1; i >= 0; i--)
         rev_uids[(*rev_count)++] = tmp[i];

      if (total > n) {
         more_below = true; /* this window alone had more than we needed */
         break;
      }
      seq_hi = seq_lo - 1;
   }
   /* Leave the handle's own timeout for the header FETCH that follows. */
   curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, budget_sec);

   if (!page)
      return 0;
   uint32_t oldest = *rev_count > 0 ? rev_uids[*rev_count - 1] : 0;
   uint32_t next = 0;
   if (more_below) {
      next = oldest;
   } else if (stopped_early && seq_hi >= 1) {
      /* Resume just below the lowest message scanned (sequence seq_hi + 1). */
      uint32_t uid_at = 0;
      snprintf(cmd, sizeof(cmd), "UID SEARCH %u", seq_hi + 1);
      if (imap_lookup_one(curl, dctx, conn, op, cmd, &uid_at)) {
         next = uid_at;
      } else if (*rev_count > 0) {
         next = oldest; /* safe: at worst rescans a little */
      } else {
         /* No rows and no way to resume: an empty page without a cursor would
          * read as "no such mail", so report the search as failed instead. */
         OLOG_ERROR("email: IMAP search stopped early and could not resume");
         *res_out = CURLE_OPERATION_TIMEDOUT;
         return 1;
      }
      if (page->before_uid > 1 && next >= page->before_uid)
         OLOG_WARNING("email: IMAP resume cursor %u is not below the previous cursor %u; some "
                      "mail may be shown again",
                      next, page->before_uid);
      OLOG_INFO("email: IMAP search stopped after %d window(s); older mail remains", windows);
   } else if (*rev_count >= wanted && seq_hi >= 1) {
      next = oldest; /* filled up; unscanned older messages remain (may not match) */
   }
   page->next_before_uid = next > 1 ? next : 0;
   return 0;
}

/* =============================================================================
 * Public API: Fetch Recent
 * ============================================================================= */

int email_fetch_recent(const email_conn_t *conn,
                       const char *folder,
                       int count,
                       bool unread_only,
                       email_imap_page_t *page,
                       email_summary_t *out,
                       int max_out,
                       int *out_count) {
   *out_count = 0;
   page_reset_outputs(page);

   if (count > max_out)
      count = max_out;
   if (count > EMAIL_MAX_FETCH_RESULTS)
      count = EMAIL_MAX_FETCH_RESULTS;
   if (count <= 0)
      count = EMAIL_MAX_RECENT_DEFAULT;

   if (!folder || !folder[0])
      folder = "INBOX";

   char encoded_folder[256];
   url_encode_folder(folder, encoded_folder, sizeof(encoded_folder));

   CURL *curl = email_imap_handle_create(conn);
   if (!curl)
      return 1;

   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);

   char url[1024];
   build_mailbox_url(conn, encoded_folder, page, url, sizeof(url));
   curl_easy_setopt(curl, CURLOPT_URL, url);

   /* Step 1: the newest `count` matching UIDs, via windowed UID SEARCH.  UID
    * SEARCH (not plain SEARCH) so the ids feed the UID FETCH below: plain SEARCH
    * returns sequence numbers, which diverge from UIDs once anything has been
    * expunged. */
   uint32_t rev_uids[EMAIL_MAX_FETCH_RESULTS];
   int rev_count = 0;
   CURLcode res = CURLE_OK;
   if (imap_windowed_search(curl, &dctx, conn, "recent", unread_only ? " UNSEEN" : "", page, count,
                            EMAIL_IMAP_TIMEOUT_SEC, rev_uids, &rev_count, &res) != 0) {
      email_instrument_op_done(conn->username, "recent", curl, &dctx);
      curl_easy_cleanup(curl);
      return 1;
   }

   /* Step 2: batch-fetch headers (single round trip) */
   int fetch_rc = batch_fetch_headers(curl, conn, encoded_folder, rev_uids, rev_count, out, max_out,
                                      out_count);

   email_instrument_op_done(conn->username, "recent", curl, &dctx);
   curl_easy_cleanup(curl);
   /* Propagate the FETCH result: a failed/over-cap header fetch must surface as
    * an error, not a successful-looking empty mailbox (the digest would then
    * treat a broken account as healthy). */
   return fetch_rc;
}

/* =============================================================================
 * Public API: Read Message
 * ============================================================================= */

/* A read's sink: keeps at most `cap` bytes, then stops the transfer (a short
 * write makes curl end it with CURLE_WRITE_ERROR), so a message larger than
 * the read wants is never downloaded whole. */
typedef struct {
   curl_buffer_t buf;
   size_t cap;
   bool hit_cap;
} read_sink_t;

static size_t read_sink_write(void *data, size_t size, size_t nmemb, void *userp) {
   read_sink_t *s = userp;
   const size_t n = size * nmemb;
   const size_t room = s->cap > s->buf.size ? s->cap - s->buf.size : 0;
   if (n > room) {
      if (room > 0)
         curl_buffer_write_callback(data, 1, room, &s->buf);
      s->hit_cap = true;
      return 0;
   }
   return curl_buffer_write_callback(data, size, nmemb, &s->buf);
}

/* Subject and From without reading the body: FETCH (ENVELOPE) answers inline
 * (no literal for libcurl to drop) and, unlike a body fetch, leaves the
 * message unread. */
static int read_headers_only(CURL *curl,
                             email_instrument_ctx_t *dctx,
                             const email_conn_t *conn,
                             const char *folder_url,
                             uint32_t uid,
                             email_message_t *out,
                             email_err_t *err) {
   curl_easy_setopt(curl, CURLOPT_URL, folder_url);
   char cmd[64];
   snprintf(cmd, sizeof(cmd), "UID FETCH %u (ENVELOPE)", uid);
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "read", cmd, true, &buf);
   if (res != CURLE_OK || !buf.data) {
      *err = res == CURLE_OK ? EMAIL_ERR_NOT_FOUND : email_err_from_curl(res);
      curl_buffer_free(&buf);
      return 1;
   }
   char subject[512] = "";
   char from_name[256] = "";
   char from_addr[256] = "";
   const bool ok = email_parse_envelope(buf.data, subject, sizeof(subject), from_name,
                                        sizeof(from_name), from_addr, sizeof(from_addr));
   curl_buffer_free(&buf);
   if (!ok) {
      *err = EMAIL_ERR_NOT_FOUND;
      return 1;
   }
   email_mime_header_text(subject, out->subject, sizeof(out->subject));
   email_mime_header_text(from_name, out->from_name, sizeof(out->from_name));
   email_display_sanitize(from_addr, strlen(from_addr), out->from_addr, sizeof(out->from_addr), 0);
   return 0;
}

int email_read_message(const email_conn_t *conn,
                       const char *folder,
                       uint32_t uid,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_NONE;
   memset(out, 0, sizeof(*out));
   if (!conn || !opts || uid == 0) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   if (!folder || !folder[0])
      folder = "INBOX";

   char encoded_folder[256];
   url_encode_folder(folder, encoded_folder, sizeof(encoded_folder));
   CURL *curl = email_imap_handle_create(conn);
   if (!curl) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);
   email_transfer_set_cancel(curl, opts->cancel);
   char url[1024];
   int rc = 1;

   if (opts->headers_only) {
      snprintf(url, sizeof(url), "%s/%s", conn->imap_url, encoded_folder);
      rc = read_headers_only(curl, &dctx, conn, url, uid, out, err);
      email_instrument_op_done(conn->username, "read", curl, &dctx);
      curl_easy_cleanup(curl);
      if (rc == 0)
         out->uid = uid;
      return rc;
   }

   /* A bounded fetch (IMAP BODY[]<0.N>, curl ";PARTIAL="): a message bigger than
    * the read wants comes back cut, and is read as truncated.  PARTIAL is core
    * RFC 3501; a server that rejects it gets one plain fetch, whose sink stops
    * the transfer at the same size. */
   const size_t cap = opts->fetch_bytes > 0 ? opts->fetch_bytes : EMAIL_READ_FETCH_TOOL;
   read_sink_t sink;
   bool raw_cut = false;
   bool got = false;
   for (int attempt = 0; attempt < 2 && !got; attempt++) {
      const bool partial = attempt == 0;
      if (partial)
         snprintf(url, sizeof(url), "%s/%s/;UID=%u;PARTIAL=0.%zu", conn->imap_url, encoded_folder,
                  uid, cap);
      else
         snprintf(url, sizeof(url), "%s/%s/;UID=%u", conn->imap_url, encoded_folder, uid);
      curl_easy_setopt(curl, CURLOPT_URL, url);
      memset(&sink, 0, sizeof(sink));
      dctx.last_reject[0] = '\0'; /* a refused command shows up here */
      curl_buffer_init_with_max(&sink.buf, cap + 1);
      sink.cap = cap;
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, read_sink_write);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

      const CURLcode res = email_instrument_perform(curl, &dctx, conn->username, "read");
      if (res == CURLE_OK && sink.buf.size > 0) {
         raw_cut = partial && sink.buf.size >= cap;
         got = true;
      } else if (res == CURLE_WRITE_ERROR && sink.hit_cap && sink.buf.size > 0) {
         raw_cut = true;
         got = true;
      } else if (res == CURLE_OK) {
         /* The server answered and had no such message. */
         *err = EMAIL_ERR_NOT_FOUND;
         curl_buffer_free(&sink.buf);
         break;
      } else {
         curl_buffer_free(&sink.buf);
         *err = email_err_from_curl(res);
         /* A server that refused PARTIAL gets the plain fetch.  libcurl reports
          * that refusal as it reports a missing message (no FETCH answer); the
          * server's tagged NO/BAD tells them apart, so a missing message costs
          * one command.  Not a login denial (a second fetch would log in again,
          * the burst the instrumented perform guards against), not a cancel. */
         const bool refused = dctx.last_reject[0] != '\0';
         if (*err == EMAIL_ERR_AUTH_FAILED || *err == EMAIL_ERR_CANCELLED || !partial ||
             (*err == EMAIL_ERR_NOT_FOUND && !refused)) {
            OLOG_ERROR("email: IMAP FETCH uid=%u failed: %s", uid, curl_easy_strerror(res));
            break;
         }
         OLOG_WARNING("email: IMAP partial fetch uid=%u failed (%s); retrying full fetch", uid,
                      curl_easy_strerror(res));
      }
   }
   email_instrument_op_done(conn->username, "read", curl, &dctx);
   curl_easy_cleanup(curl);
   if (!got)
      return 1;

   rc = email_mime_parse_raw(sink.buf.data, sink.buf.size, raw_cut, opts, out);
   curl_buffer_free(&sink.buf);
   if (rc != 0) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   *err = EMAIL_ERR_NONE;
   out->uid = uid;
   return 0;
}

/* =============================================================================
 * Public API: Search
 * ============================================================================= */

int email_search(const email_conn_t *conn,
                 const char *folder,
                 const email_search_params_t *params,
                 email_imap_page_t *page,
                 email_summary_t *out,
                 int max_out,
                 int *out_count,
                 bool *auth_denied,
                 bool *timed_out) {
   *out_count = 0;
   if (auth_denied)
      *auth_denied = false;
   if (timed_out)
      *timed_out = false;
   page_reset_outputs(page);

   if (max_out > EMAIL_MAX_FETCH_RESULTS)
      max_out = EMAIL_MAX_FETCH_RESULTS;

   if (!folder || !folder[0])
      folder = "INBOX";

   char encoded_folder[256];
   url_encode_folder(folder, encoded_folder, sizeof(encoded_folder));

   CURL *curl = email_imap_handle_create(conn);
   if (!curl)
      return 1;

   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);

   /* Content SEARCH (TEXT/BODY) can brute-force-scan the whole mailbox on a
    * server without an FTS index — give it a larger budget than the cheap ops'
    * default (see EMAIL_IMAP_SEARCH_TIMEOUT_SEC).  imap_windowed_search spreads
    * this one budget across all of its windows. */
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, EMAIL_IMAP_SEARCH_TIMEOUT_SEC);

   /* Search keys; user-provided values go through email_imap_append_search_key
    * (quoted + sanitized).  The windowed search adds the "UID SEARCH" prefix,
    * the sequence window and the paging bound. */
   char search_cmd[2048] = "";
   size_t spos = 0;
   size_t srem = sizeof(search_cmd);

   if (params->unread_only) {
      BUF_PRINTF(search_cmd, spos, srem, " UNSEEN");
   }
   email_imap_append_search_key(search_cmd, &spos, &srem, "FROM", params->from);
   email_imap_append_search_key(search_cmd, &spos, &srem, "SUBJECT", params->subject);
   email_imap_append_search_key(search_cmd, &spos, &srem, "TEXT", params->text);

   /* Date parameters — validate via strptime/strftime (never raw) */
   if (params->since[0]) {
      char imap_date[32];
      if (validate_imap_date(params->since, imap_date, sizeof(imap_date))) {
         BUF_PRINTF(search_cmd, spos, srem, " SINCE %s", imap_date);
      }
   }
   if (params->before[0]) {
      char imap_date[32];
      if (validate_imap_date(params->before, imap_date, sizeof(imap_date))) {
         BUF_PRINTF(search_cmd, spos, srem, " BEFORE %s", imap_date);
      }
   }

   char url[1024];
   build_mailbox_url(conn, encoded_folder, page, url, sizeof(url));
   curl_easy_setopt(curl, CURLOPT_URL, url);

   /* The newest max_out matches, newest first, plus the cursor */
   uint32_t rev_uids[EMAIL_MAX_FETCH_RESULTS];
   int rev_count = 0;
   CURLcode res = CURLE_OK;
   if (spos >= sizeof(search_cmd) - 1) {
      OLOG_ERROR("email: IMAP search criteria too long; refusing to send them truncated");
      email_instrument_op_done(conn->username, "search", curl, &dctx);
      curl_easy_cleanup(curl);
      return 1;
   }
   if (imap_windowed_search(curl, &dctx, conn, "search", search_cmd, page, max_out,
                            EMAIL_IMAP_SEARCH_TIMEOUT_SEC, rev_uids, &rev_count, &res) != 0) {
      if (auth_denied && res == CURLE_LOGIN_DENIED)
         *auth_denied = true;
      if (timed_out && res == CURLE_OPERATION_TIMEDOUT)
         *timed_out = true;
      email_instrument_op_done(conn->username, "search", curl, &dctx);
      curl_easy_cleanup(curl);
      return 1;
   }

   int fetch_rc = batch_fetch_headers(curl, conn, encoded_folder, rev_uids, rev_count, out, max_out,
                                      out_count);

   email_instrument_op_done(conn->username, "search", curl, &dctx);
   curl_easy_cleanup(curl);
   /* Propagate the FETCH result: a failed/over-cap header fetch must surface as
    * an error, not a successful-looking empty mailbox (the digest would then
    * treat a broken account as healthy). */
   return fetch_rc;
}

/* =============================================================================
 * Public API: Send Email
 * ============================================================================= */

int email_send(const email_conn_t *conn,
               const char *to_addr,
               const char *to_name,
               const char *subject,
               const char *body) {
   if (!to_addr || !to_addr[0] || !subject || !body)
      return 1;

   /* Sanitize all header-injectable fields (CR/LF strip — shared helper). */
   char safe_subject[256], safe_to_name[64], safe_display_name[64], safe_to_addr[256];
   email_sanitize_header_value(subject, safe_subject, sizeof(safe_subject));
   email_sanitize_header_value(to_name ? to_name : "", safe_to_name, sizeof(safe_to_name));
   email_sanitize_header_value(conn->display_name, safe_display_name, sizeof(safe_display_name));
   email_sanitize_header_value(to_addr, safe_to_addr, sizeof(safe_to_addr));

   CURL *curl = create_smtp_handle(conn);
   if (!curl)
      return 1;

   curl_easy_setopt(curl, CURLOPT_URL, conn->smtp_url);

   /* From address — use username if no display name */
   char from_addr[512];
   email_format_mailbox(safe_display_name, conn->username, from_addr, sizeof(from_addr));
   curl_easy_setopt(curl, CURLOPT_MAIL_FROM, conn->username);

   /* Recipients */
   struct curl_slist *recipients = NULL;
   recipients = curl_slist_append(recipients, safe_to_addr);
   curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

   /* Build RFC 2822 message */
   char to_header[512];
   email_format_mailbox(safe_to_name, safe_to_addr, to_header, sizeof(to_header));

   char date_str[64];
   time_t now = time(NULL);
   struct tm tm_info;
   localtime_r(&now, &tm_info);
   strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S %z", &tm_info);

   size_t msg_size = strlen(body) + 1024;
   char *message = malloc(msg_size);
   if (!message) {
      curl_slist_free_all(recipients);
      curl_easy_cleanup(curl);
      return 1;
   }

   snprintf(message, msg_size,
            "Date: %s\r\n"
            "From: %s\r\n"
            "To: %s\r\n"
            "Subject: %s\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "MIME-Version: 1.0\r\n"
            "\r\n"
            "%s",
            date_str, from_addr, to_header, safe_subject, body);

   smtp_upload_t upload = { .data = message, .len = strlen(message), .offset = 0 };
   curl_easy_setopt(curl, CURLOPT_READFUNCTION, smtp_read_cb);
   curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
   curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

   CURLcode res = curl_easy_perform(curl);

   free(message);
   message = NULL;
   curl_slist_free_all(recipients);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      OLOG_ERROR("email: SMTP send failed: %s", curl_easy_strerror(res));
      return 1;
   }

   OLOG_INFO("email: sent to %s, subject '%s'", safe_to_addr, safe_subject);
   return 0;
}

/* =============================================================================
 * Public API: Test Connection
 * ============================================================================= */

int email_test_connection(const email_conn_t *conn, bool *imap_ok, bool *smtp_ok) {
   *imap_ok = false;
   *smtp_ok = false;

   /* Test IMAP: connect to INBOX */
   CURL *curl = email_imap_handle_create(conn);
   if (curl) {
      /* Instrument for the same on-wire-login count + rejection capture as the
       * real ops, so a passing test contrasts directly with a failing search in
       * the log — but the test BYPASSES the breaker (no retry): a diagnostic
       * button must return its verdict immediately. */
      email_instrument_ctx_t dctx;
      email_instrument_attach(curl, &dctx);

      char url[768];
      snprintf(url, sizeof(url), "%s/INBOX", conn->imap_url);
      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "SEARCH RECENT");

      curl_buffer_t buf;
      curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

      CURLcode res = curl_easy_perform(curl);
      *imap_ok = (res == CURLE_OK);
      if (!*imap_ok) {
         if (res == CURLE_LOGIN_DENIED)
            email_instrument_note_denied(conn->username, &dctx, "test-connection");
         else
            OLOG_WARNING("email: IMAP test failed: %s", curl_easy_strerror(res));
      }
      email_instrument_op_done(conn->username, "test-connection", curl, &dctx);
      curl_buffer_free(&buf);
      curl_easy_cleanup(curl);
   }

   /* Test SMTP: EHLO only (CURLOPT_CONNECT_ONLY) */
   curl = create_smtp_handle(conn);
   if (curl) {
      curl_easy_setopt(curl, CURLOPT_URL, conn->smtp_url);
      curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);

      CURLcode res = curl_easy_perform(curl);
      *smtp_ok = (res == CURLE_OK);
      if (!*smtp_ok) {
         OLOG_WARNING("email: SMTP test failed: %s", curl_easy_strerror(res));
      }
      curl_easy_cleanup(curl);
   }

   return (*imap_ok && *smtp_ok) ? 0 : 1;
}

/* =============================================================================
 * Public API: List Folders
 * ============================================================================= */

/** Allow-list validation for IMAP folder names */
static bool validate_imap_folder_char(const char *name) {
   if (!name || !name[0])
      return false;
   if (strlen(name) > 127)
      return false;
   /* Reject path traversal */
   if (strstr(name, ".."))
      return false;
   for (const char *p = name; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' || c == '[' ||
          c == ']')
         continue;
      return false;
   }
   return true;
}

/** Well-known IMAP system folder names (case-insensitive match) */
static const char *const imap_system_folders[] = {
   "INBOX", "Sent", "Drafts", "Trash", "Junk", "Spam", "Archive", "Flagged", NULL,
};

/** Gmail-specific system folders (matched by prefix) */
static const char *const gmail_system_prefix = "[Gmail]";

static bool is_system_folder(const char *name) {
   for (int i = 0; imap_system_folders[i]; i++) {
      if (strcasecmp(name, imap_system_folders[i]) == 0)
         return true;
   }
   /* Gmail system folders: [Gmail]/Sent Mail, [Gmail]/Trash, etc. */
   if (strncmp(name, gmail_system_prefix, 7) == 0)
      return true;
   return false;
}

/** Extract folder name from an IMAP LIST response line into fname[128] */
static bool parse_list_folder_name(const char *line, char *fname, size_t fname_len) {
   if (strncmp(line, "* LIST ", 7) != 0)
      return false;

   const char *p = line + 7;
   /* Skip flags (\Noselect \HasChildren ...) */
   const char *paren = strchr(p, ')');
   if (paren)
      p = paren + 1;
   while (*p == ' ')
      p++;
   /* Skip delimiter token (e.g. "/" or ".") — may be quoted */
   if (*p == '"') {
      p = strchr(p + 1, '"');
      if (p)
         p++;
   } else {
      while (*p && *p != ' ')
         p++;
   }
   while (*p == ' ')
      p++;

   if (*p == '"') {
      /* Quoted folder name */
      p++;
      const char *end_q = strchr(p, '"');
      if (!end_q)
         return false;
      size_t nlen = end_q - p;
      if (nlen == 0 || nlen >= fname_len)
         return false;
      memcpy(fname, p, nlen);
      fname[nlen] = '\0';
   } else if (*p) {
      /* Unquoted folder name */
      snprintf(fname, fname_len, "%s", p);
      size_t flen = strlen(fname);
      while (flen > 0 && (fname[flen - 1] == ' ' || fname[flen - 1] == '\r'))
         fname[--flen] = '\0';
      if (!flen)
         return false;
   } else {
      return false;
   }

   return validate_imap_folder_char(fname);
}

int email_list_folders(const email_conn_t *conn, char *out, size_t out_len) {
   if (!out || out_len < 2)
      return 1;
   out[0] = '\0';

   CURL *curl = email_imap_handle_create(conn);
   if (!curl)
      return 1;

   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);

   char url[768];
   snprintf(url, sizeof(url), "%s", conn->imap_url);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "LIST \"\" *");

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

   CURLcode res = email_instrument_perform(curl, &dctx, conn->username, "list");
   email_instrument_op_done(conn->username, "list", curl, &dctx);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK || !buf.data || buf.truncated) {
      OLOG_ERROR("email: IMAP LIST failed: %s%s", curl_easy_strerror(res),
                 buf.truncated ? " (response exceeded cap)" : "");
      curl_buffer_free(&buf);
      return 1;
   }

   /* Collect folder names into system and custom lists */
   char folders[64][128];
   int folder_count = 0;
   char *line = buf.data;
   while (line && *line && folder_count < 64) {
      char *eol = strstr(line, "\r\n");
      if (!eol)
         eol = strchr(line, '\n');
      if (eol)
         *eol = '\0';

      char fname[128];
      if (parse_list_folder_name(line, fname, sizeof(fname))) {
         snprintf(folders[folder_count], sizeof(folders[0]), "%s", fname);
         folder_count++;
      }

      if (!eol)
         break;
      line = eol + 1;
      if (*line == '\n')
         line++;
   }
   curl_buffer_free(&buf);

   /* Format output: System folders first, then custom */
   int pos = 0;
   int sys_count = 0;
   pos += snprintf(out + pos, out_len - pos, "System: ");
   int sys_start = pos;
   for (int i = 0; i < folder_count && pos < (int)out_len - 128; i++) {
      if (!is_system_folder(folders[i]))
         continue;
      if (sys_count > 0)
         pos += snprintf(out + pos, out_len - pos, ", ");
      pos += snprintf(out + pos, out_len - pos, "%s", folders[i]);
      sys_count++;
   }
   if (pos == sys_start)
      pos += snprintf(out + pos, out_len - pos, "(none)");

   int custom_count = 0;
   pos += snprintf(out + pos, out_len - pos, "\nFolders: ");
   int custom_start = pos;
   for (int i = 0; i < folder_count && pos < (int)out_len - 128; i++) {
      if (is_system_folder(folders[i]))
         continue;
      if (custom_count > 0)
         pos += snprintf(out + pos, out_len - pos, ", ");
      pos += snprintf(out + pos, out_len - pos, "%s", folders[i]);
      custom_count++;
   }
   if (pos == custom_start)
      pos += snprintf(out + pos, out_len - pos, "(none)");

   return 0;
}
