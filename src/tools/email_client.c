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
 * - IMAP SEARCH commands use literal syntax {N}\r\n<data> to prevent injection
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
#include "tools/email_parse.h"
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

/* Byte ceiling for a single-message read fetch (IMAP BODY[]<0.N>).  We only need
 * headers + the first ~50 KB of decoded text body — reading a message must not
 * pull the entire raw MIME (base64 attachments can be many MB and would blow the
 * 1 MB response cap, failing the read outright).  512 KB covers headers plus a
 * large text/HTML part for essentially all real mail; a bigger message degrades
 * to a truncated body (out->truncated), never a failed read.  Attachments are a
 * separate feature (see EMAIL_ATTACHMENT_DOWNLOAD_DESIGN.md). */
#define EMAIL_MAX_READ_FETCH_BYTES (512 * 1024)

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

static CURL *create_imap_handle(const email_conn_t *conn) {
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

   if (!strptime(iso_date, "%Y-%m-%d", &tm_info))
      return false;

   /* Reconstruct in IMAP format: DD-Mon-YYYY */
   if (strftime(imap_out, out_len, "%d-%b-%Y", &tm_info) == 0)
      return false;

   return true;
}

/* =============================================================================
 * IMAP Literal Builder
 *
 * Builds IMAP literal syntax {N}\r\n<data> for safe string interpolation.
 * Literals are length-prefixed and cannot be escaped out of.
 * ============================================================================= */

static void append_imap_literal(char *buf, size_t *off, size_t *rem, const char *value) {
   size_t val_len = strlen(value);
   BUF_PRINTF(buf, *off, *rem, "{%zu}\r\n%s", val_len, value);
}

/* RFC 2047 encoded-word decoding (email_decode_rfc2047) lives in email_parse.c
 * so both the IMAP and Gmail backends share one implementation — see the note
 * there.  copy_header_value below wraps it after unfolding. */

/* =============================================================================
 * Header Parsing Helpers
 * ============================================================================= */

/** Extract a header value from raw email headers */
static const char *find_header(const char *headers, const char *name) {
   const char *p = headers;
   size_t name_len = strlen(name);
   while (p && *p) {
      if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
         p += name_len + 1;
         while (*p == ' ' || *p == '\t')
            p++;
         return p;
      }
      /* Skip to next line */
      p = strchr(p, '\n');
      if (p)
         p++;
   }
   return NULL;
}

/** Copy a header value with unfolding (joins continuation lines) and RFC 2047 decoding */
static void copy_header_value(const char *start, char *out, size_t out_len) {
   if (!start) {
      out[0] = '\0';
      return;
   }

   /* Step 1: Unfold — join continuation lines (lines starting with space/tab) */
   char raw[512];
   size_t j = 0;
   const char *p = start;
   while (*p && j < sizeof(raw) - 1) {
      if (*p == '\r') {
         p++;
         continue;
      }
      if (*p == '\n') {
         /* Check for continuation (next line starts with space/tab) */
         if (p[1] == ' ' || p[1] == '\t') {
            if (j > 0 && raw[j - 1] != ' ')
               raw[j++] = ' ';
            p++; /* skip \n */
            while (*p == ' ' || *p == '\t')
               p++; /* skip leading whitespace */
            continue;
         }
         break; /* End of header */
      }
      raw[j++] = *p++;
   }
   raw[j] = '\0';

   /* Step 2: Decode RFC 2047 encoded words */
   email_decode_rfc2047(raw, out, out_len);
}

/** Parse "Display Name <email@example.com>" from a decoded From header value */
static void parse_from_value(const char *decoded,
                             char *name,
                             size_t name_len,
                             char *addr,
                             size_t addr_len) {
   if (!decoded || !decoded[0]) {
      name[0] = '\0';
      addr[0] = '\0';
      return;
   }

   const char *lt = strchr(decoded, '<');
   const char *gt = lt ? strchr(lt, '>') : NULL;

   if (lt && gt && gt > lt + 1) {
      /* Copy display name (before <) */
      size_t n = lt - decoded;
      while (n > 0 && (decoded[n - 1] == ' ' || decoded[n - 1] == '"'))
         n--;
      const char *ns = decoded;
      while (n > 0 && (*ns == ' ' || *ns == '"'))
         ns++, n--;
      if (n > name_len - 1)
         n = name_len - 1;
      memcpy(name, ns, n);
      name[n] = '\0';

      /* Copy email address */
      size_t a = gt - (lt + 1);
      if (a > addr_len - 1)
         a = addr_len - 1;
      memcpy(addr, lt + 1, a);
      addr[a] = '\0';
   } else {
      /* No angle brackets — entire thing is the address */
      name[0] = '\0';
      snprintf(addr, addr_len, "%s", decoded);
   }
}

/* =============================================================================
 * Extract plain text body from email content
 * ============================================================================= */

/** Check if content looks like HTML (starts with tag or doctype) */
static bool looks_like_html(const char *text) {
   /* Skip leading whitespace */
   while (*text && isspace((unsigned char)*text))
      text++;
   if (strncasecmp(text, "<!doctype", 9) == 0)
      return true;
   if (strncasecmp(text, "<html", 5) == 0)
      return true;
   /* Check Content-Type header above the body for text/html */
   return false;
}

/* Extract the plain-text body, capped at max_chars.  Sets *out_truncated to
 * reflect whether the EXTRACTED text (not the raw MIME) was clipped, so the
 * caller's truncation flag matches the Gmail backend.  No "[truncated]" marker
 * is appended here — the tool layer renders a single "[Message truncated]". */
static char *extract_plain_body(const char *raw, int max_chars, bool *out_truncated) {
   if (out_truncated)
      *out_truncated = false;

   /* Check Content-Type header for HTML before splitting */
   const char *ct = find_header(raw, "Content-Type");
   bool is_html = (ct && strcasestr(ct, "text/html"));

   /* Find body start (after blank line separating headers from body) */
   const char *body = strstr(raw, "\r\n\r\n");
   if (!body)
      body = strstr(raw, "\n\n");
   if (!body)
      return strdup("(No body)");

   body += (body[0] == '\r') ? 4 : 2;

   /* Heuristic fallback: detect HTML even without Content-Type header */
   if (!is_html)
      is_html = looks_like_html(body);

   /* If body is HTML, convert to plain text via html_parser */
   if (is_html) {
      size_t body_len = strlen(body);
      char *extracted = NULL;
      if (html_extract_text_plain(body, body_len, &extracted) == HTML_PARSE_SUCCESS && extracted) {
         size_t ext_len = strlen(extracted);
         if (max_chars > 0 && (int)ext_len > max_chars) {
            ext_len = max_chars;
            if (out_truncated)
               *out_truncated = true;
         }
         char *out = malloc(ext_len + 1);
         if (!out) {
            free(extracted);
            return NULL;
         }
         memcpy(out, extracted, ext_len);
         out[ext_len] = '\0';
         free(extracted);
         return out;
      }
      /* Fall through to raw extraction if html_extract_text fails */
   }

   /* Plain text: take text up to max_chars */
   size_t len = strlen(body);
   if (max_chars > 0 && (int)len > max_chars) {
      len = max_chars;
      if (out_truncated)
         *out_truncated = true;
   }

   char *out = malloc(len + 1);
   if (!out)
      return NULL;

   memcpy(out, body, len);
   out[len] = '\0';
   return out;
}

/* =============================================================================
 * Parse UID list from IMAP SEARCH response
 * ============================================================================= */

/**
 * Parse the last N UIDs from an IMAP UID SEARCH response.
 *
 * UID SEARCH returns UIDs in ascending order (a plain SEARCH would return
 * sequence numbers — callers MUST issue UID SEARCH so these ids match the
 * subsequent UID FETCH). For "recent" email, we want the highest UIDs (newest
 * messages). This uses a circular buffer to capture only the last `wanted` UIDs
 * from arbitrarily large inboxes without excessive memory.
 *
 * @param response  Raw IMAP UID SEARCH response
 * @param uids      Output array (must hold at least `wanted` elements)
 * @param wanted    Max UIDs to return (from the tail of the list)
 * @return Number of UIDs written to uids[], in ascending order
 */
static int parse_tail_uids(const char *response, uint32_t *uids, int wanted) {
   /* Guard against a server that returns no body (curl leaves buf.data NULL) and
    * against a zero `wanted` (the `total % wanted` below would divide by zero). */
   if (!response || wanted <= 0)
      return 0;
   const char *p = strstr(response, "* SEARCH");
   if (!p)
      return 0;
   p += 8;

   int total = 0;
   while (*p) {
      while (*p == ' ')
         p++;
      if (*p == '\r' || *p == '\n' || *p == '\0')
         break;

      char *end;
      unsigned long uid = strtoul(p, &end, 10);
      if (end == p)
         break;

      /* Circular buffer: overwrite oldest when full */
      uids[total % wanted] = (uint32_t)uid;
      total++;
      p = end;
   }

   if (total <= wanted) {
      /* All fit — already in ascending order */
      return total;
   }

   /* Rearrange circular buffer to sequential ascending order */
   uint32_t tmp[EMAIL_MAX_FETCH_RESULTS];
   int start = total % wanted;
   for (int i = 0; i < wanted; i++) {
      tmp[i] = uids[(start + i) % wanted];
   }
   memcpy(uids, tmp, wanted * sizeof(uint32_t));
   return wanted;
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
         email_decode_rfc2047(subj_raw, s->subject, sizeof(s->subject));
         if (fname_raw[0])
            email_decode_rfc2047(fname_raw, s->from_name, sizeof(s->from_name));
         snprintf(s->from_addr, sizeof(s->from_addr), "%s", faddr);
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
 * Public API: Fetch Recent
 * ============================================================================= */

int email_fetch_recent(const email_conn_t *conn,
                       const char *folder,
                       int count,
                       bool unread_only,
                       email_summary_t *out,
                       int max_out,
                       int *out_count) {
   *out_count = 0;

   if (count > max_out)
      count = max_out;
   if (count > EMAIL_MAX_FETCH_RESULTS)
      count = EMAIL_MAX_FETCH_RESULTS;
   if (count <= 0)
      count = 10;

   if (!folder || !folder[0])
      folder = "INBOX";

   char encoded_folder[256];
   url_encode_folder(folder, encoded_folder, sizeof(encoded_folder));

   CURL *curl = create_imap_handle(conn);
   if (!curl)
      return 1;

   /* Step 1: SEARCH for UIDs, optionally unread only */
   char url[1024];
   snprintf(url, sizeof(url), "%s/%s", conn->imap_url, encoded_folder);
   curl_easy_setopt(curl, CURLOPT_URL, url);

   /* UID SEARCH (not plain SEARCH): the results are fed to UID FETCH below, so
    * both must speak UIDs.  Plain SEARCH returns message SEQUENCE numbers, which
    * only coincide with UIDs in a mailbox that has never had a message expunged
    * — on any established mailbox the two diverge and UID-FETCHing sequence
    * numbers returns nothing ("No recent emails found"). */
   char search_cmd[128];
   snprintf(search_cmd, sizeof(search_cmd), "UID SEARCH %s", unread_only ? "UNSEEN" : "ALL");
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, search_cmd);

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      OLOG_ERROR("email: IMAP SEARCH failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      curl_easy_cleanup(curl);
      return 1;
   }
   if (buf.truncated) {
      OLOG_ERROR("email: IMAP SEARCH response exceeded %d byte cap; rejecting",
                 EMAIL_MAX_RESPONSE_SIZE);
      curl_buffer_free(&buf);
      curl_easy_cleanup(curl);
      return 1;
   }

   /* Parse last N UIDs (highest = newest) from SEARCH response */
   uint32_t tail_uids[EMAIL_MAX_FETCH_RESULTS];
   int total = parse_tail_uids(buf.data, tail_uids, count);
   curl_buffer_free(&buf);

   /* Step 2: Batch-fetch headers (single round trip) in reverse order (newest first) */
   uint32_t rev_uids[EMAIL_MAX_FETCH_RESULTS];
   int rev_count = 0;
   for (int i = total - 1; i >= 0 && rev_count < max_out; i--)
      rev_uids[rev_count++] = tail_uids[i];

   batch_fetch_headers(curl, conn, encoded_folder, rev_uids, rev_count, out, max_out, out_count);

   curl_easy_cleanup(curl);
   return 0;
}

/* =============================================================================
 * Public API: Read Message
 * ============================================================================= */

int email_read_message(const email_conn_t *conn,
                       const char *folder,
                       uint32_t uid,
                       email_message_t *out) {
   memset(out, 0, sizeof(*out));

   if (!folder || !folder[0])
      folder = "INBOX";

   char encoded_folder[256];
   url_encode_folder(folder, encoded_folder, sizeof(encoded_folder));

   CURL *curl = create_imap_handle(conn);
   if (!curl)
      return 1;

   /* Fetch the message by UID.  First try a bounded partial fetch (IMAP
    * BODY[]<0.N>, curl ";PARTIAL=") so a huge message (big attachments) can't
    * blow the response cap and fail the read outright.  PARTIAL is core RFC 3501,
    * but if a non-conforming server rejects it, fall back once to a full fetch
    * (still capped at EMAIL_MAX_RESPONSE_SIZE) so reads keep working. */
   char url[1024];
   curl_buffer_t buf;
   CURLcode res = CURLE_OK;
   bool used_partial = true;

   for (int attempt = 0; attempt < 2; attempt++) {
      if (used_partial)
         snprintf(url, sizeof(url), "%s/%s/;UID=%u;PARTIAL=0.%d", conn->imap_url, encoded_folder,
                  uid, EMAIL_MAX_READ_FETCH_BYTES);
      else
         snprintf(url, sizeof(url), "%s/%s/;UID=%u", conn->imap_url, encoded_folder, uid);
      curl_easy_setopt(curl, CURLOPT_URL, url);

      curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

      res = curl_easy_perform(curl);
      if (res == CURLE_OK && buf.data && !buf.truncated)
         break; /* success */

      bool was_truncated = buf.truncated;
      curl_buffer_free(&buf);
      if (used_partial) {
         OLOG_WARNING("email: IMAP partial fetch uid=%u failed (%s); retrying full fetch", uid,
                      curl_easy_strerror(res));
         used_partial = false;
         continue; /* retry without PARTIAL */
      }
      OLOG_ERROR("email: IMAP FETCH uid=%u failed: %s%s", uid, curl_easy_strerror(res),
                 was_truncated ? " (response exceeded cap)" : "");
      curl_easy_cleanup(curl);
      return 1;
   }
   curl_easy_cleanup(curl);

   out->uid = uid;

   /* Parse headers (unfold + RFC 2047 decode) */
   char from_decoded[256];
   const char *from_val = find_header(buf.data, "From");
   copy_header_value(from_val, from_decoded, sizeof(from_decoded));
   parse_from_value(from_decoded, out->from_name, sizeof(out->from_name), out->from_addr,
                    sizeof(out->from_addr));

   const char *to_val = find_header(buf.data, "To");
   copy_header_value(to_val, out->to, sizeof(out->to));

   const char *subj_val = find_header(buf.data, "Subject");
   copy_header_value(subj_val, out->subject, sizeof(out->subject));

   const char *date_val = find_header(buf.data, "Date");
   copy_header_value(date_val, out->date_str, sizeof(out->date_str));

   /* The partial fetch caps the RAW MIME at EMAIL_MAX_READ_FETCH_BYTES; when it
    * returns exactly the cap the message was cut, so the body may be incomplete
    * even if the text extractor didn't hit its own max_chars limit (e.g. a large
    * leading HTML/image part pushed the text/plain past the cut).  Fold that into
    * the truncation flag so a clipped body is never reported as complete.  Only
    * applies to the partial fetch — a full-fetch fallback got the whole message
    * (a real overflow there would have set buf.truncated and failed above). */
   bool raw_truncated = used_partial && (buf.size >= (size_t)EMAIL_MAX_READ_FETCH_BYTES);

   /* Extract plain text body (defensive fallback; service layer always supplies
    * a positive cap via build_conn_for_account) */
   int max_chars = conn->max_body_chars > 0 ? conn->max_body_chars : EMAIL_MAX_READ_BODY_LEN;
   bool body_truncated = false;
   out->body = extract_plain_body(buf.data, max_chars, &body_truncated);
   if (out->body) {
      out->body_len = strlen(out->body);
      out->truncated = body_truncated || raw_truncated;
   }

   /* Count attachments (rough heuristic — count Content-Disposition: attachment) */
   const char *p = buf.data;
   while ((p = strcasestr(p, "Content-Disposition: attachment")) != NULL) {
      out->attachment_count++;
      p += 30;
   }

   curl_buffer_free(&buf);
   return 0;
}

/* =============================================================================
 * Public API: Search
 * ============================================================================= */

int email_search(const email_conn_t *conn,
                 const char *folder,
                 const email_search_params_t *params,
                 email_summary_t *out,
                 int max_out,
                 int *out_count) {
   *out_count = 0;

   if (max_out > EMAIL_MAX_FETCH_RESULTS)
      max_out = EMAIL_MAX_FETCH_RESULTS;

   if (!folder || !folder[0])
      folder = "INBOX";

   char encoded_folder[256];
   url_encode_folder(folder, encoded_folder, sizeof(encoded_folder));

   CURL *curl = create_imap_handle(conn);
   if (!curl)
      return 1;

   /* Build IMAP UID SEARCH command with literal syntax for user-provided values.
    * UID SEARCH (not plain SEARCH) so the returned ids are UIDs, matching the
    * UID FETCH that consumes them — see the note in email_fetch_recent. */
   char search_cmd[2048];
   size_t spos = 0;
   size_t srem = sizeof(search_cmd);
   BUF_PRINTF(search_cmd, spos, srem, "UID SEARCH");

   if (params->unread_only) {
      BUF_PRINTF(search_cmd, spos, srem, " UNSEEN");
   }
   if (params->from[0]) {
      BUF_PRINTF(search_cmd, spos, srem, " FROM ");
      append_imap_literal(search_cmd, &spos, &srem, params->from);
   }
   if (params->subject[0]) {
      BUF_PRINTF(search_cmd, spos, srem, " SUBJECT ");
      append_imap_literal(search_cmd, &spos, &srem, params->subject);
   }
   if (params->text[0]) {
      BUF_PRINTF(search_cmd, spos, srem, " TEXT ");
      append_imap_literal(search_cmd, &spos, &srem, params->text);
   }

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

   /* Need at least one search key */
   if (!params->unread_only && !params->from[0] && !params->subject[0] && !params->text[0] &&
       !params->since[0] && !params->before[0]) {
      BUF_PRINTF(search_cmd, spos, srem, " ALL");
   }

   char url[1024];
   snprintf(url, sizeof(url), "%s/%s", conn->imap_url, encoded_folder);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, search_cmd);

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      OLOG_ERROR("email: IMAP SEARCH failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      curl_easy_cleanup(curl);
      return 1;
   }
   if (buf.truncated) {
      OLOG_ERROR("email: IMAP SEARCH response exceeded %d byte cap; rejecting",
                 EMAIL_MAX_RESPONSE_SIZE);
      curl_buffer_free(&buf);
      curl_easy_cleanup(curl);
      return 1;
   }

   uint32_t tail_uids[EMAIL_MAX_FETCH_RESULTS];
   int total = parse_tail_uids(buf.data, tail_uids, max_out);
   curl_buffer_free(&buf);

   /* Batch-fetch headers (single round trip) in reverse order (newest first) */
   uint32_t rev_uids[EMAIL_MAX_FETCH_RESULTS];
   int rev_count = 0;
   for (int i = total - 1; i >= 0 && rev_count < max_out; i--)
      rev_uids[rev_count++] = tail_uids[i];

   batch_fetch_headers(curl, conn, encoded_folder, rev_uids, rev_count, out, max_out, out_count);

   curl_easy_cleanup(curl);
   return 0;
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
   char from_addr[320];
   if (safe_display_name[0]) {
      snprintf(from_addr, sizeof(from_addr), "%s <%s>", safe_display_name, conn->username);
   } else {
      snprintf(from_addr, sizeof(from_addr), "%s", conn->username);
   }
   curl_easy_setopt(curl, CURLOPT_MAIL_FROM, conn->username);

   /* Recipients */
   struct curl_slist *recipients = NULL;
   recipients = curl_slist_append(recipients, safe_to_addr);
   curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);

   /* Build RFC 2822 message */
   char to_header[384];
   if (safe_to_name[0]) {
      snprintf(to_header, sizeof(to_header), "%s <%s>", safe_to_name, safe_to_addr);
   } else {
      snprintf(to_header, sizeof(to_header), "%s", safe_to_addr);
   }

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
   CURL *curl = create_imap_handle(conn);
   if (curl) {
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
         OLOG_WARNING("email: IMAP test failed: %s", curl_easy_strerror(res));
      }
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

   CURL *curl = create_imap_handle(conn);
   if (!curl)
      return 1;

   char url[768];
   snprintf(url, sizeof(url), "%s", conn->imap_url);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "LIST \"\" *");

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

   CURLcode res = curl_easy_perform(curl);
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

/* =============================================================================
 * Public API: Free Message
 * ============================================================================= */

void email_message_free(email_message_t *msg) {
   if (msg) {
      free(msg->body);
      msg->body = NULL;
      msg->body_len = 0;
   }
}

/* =============================================================================
 * Trash / Archive via IMAP
 *
 * Pattern: COPY to destination folder → STORE \Deleted → EXPUNGE.
 * Each IMAP command requires a separate curl_easy_perform().
 * ============================================================================= */

/**
 * @brief Move a message by UID from one folder to another via IMAP.
 * Steps: SELECT source → COPY to dest → STORE \Deleted → EXPUNGE.
 */
static int imap_move_message(const email_conn_t *conn,
                             const char *folder,
                             uint32_t uid,
                             const char *dest_folder) {
   if (!conn || !folder || !folder[0] || !dest_folder || !dest_folder[0] || uid == 0)
      return 1;

   CURL *curl = create_imap_handle(conn);
   if (!curl)
      return 1;

   /* URL-encode the source folder */
   char *encoded_folder = curl_easy_escape(curl, folder, 0);
   if (!encoded_folder) {
      curl_easy_cleanup(curl);
      return 1;
   }

   char url[768];
   snprintf(url, sizeof(url), "%s/%s", conn->imap_url, encoded_folder);
   curl_easy_setopt(curl, CURLOPT_URL, url);

   curl_buffer_t buf;
   int rc = 1;

   /* Step 1: COPY to destination */
   char copy_cmd[384];
   snprintf(copy_cmd, sizeof(copy_cmd), "UID COPY %u \"%s\"", uid, dest_folder);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, copy_cmd);

   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
   CURLcode res = curl_easy_perform(curl);
   curl_buffer_free(&buf);

   if (res != CURLE_OK) {
      OLOG_ERROR("email_imap: COPY failed for UID %u: %s", uid, curl_easy_strerror(res));
      goto cleanup;
   }

   /* Step 2: STORE \Deleted flag */
   char store_cmd[64];
   snprintf(store_cmd, sizeof(store_cmd), "UID STORE %u +FLAGS (\\Deleted)", uid);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, store_cmd);

   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
   res = curl_easy_perform(curl);
   curl_buffer_free(&buf);

   if (res != CURLE_OK) {
      OLOG_WARNING("email_imap: STORE \\Deleted failed for UID %u (message copied to %s): %s", uid,
                   dest_folder, curl_easy_strerror(res));
      /* COPY succeeded, so this is a partial success — message exists in both folders */
      rc = 0;
      goto cleanup;
   }

   /* Step 3: EXPUNGE to remove from source */
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "EXPUNGE");

   curl_buffer_init_with_max(&buf, EMAIL_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
   res = curl_easy_perform(curl);
   curl_buffer_free(&buf);

   if (res != CURLE_OK) {
      OLOG_WARNING("email_imap: EXPUNGE failed for UID %u (flagged but not removed): %s", uid,
                   curl_easy_strerror(res));
   }

   rc = 0;

cleanup:
   curl_free(encoded_folder);
   curl_easy_cleanup(curl);
   return rc;
}

int email_trash_message(const email_conn_t *conn, const char *folder, uint32_t uid, bool is_gmail) {
   const char *trash_folder = is_gmail ? "[Gmail]/Trash" : "Trash";
   OLOG_INFO("email_imap: trashing UID %u from %s to %s", uid, folder, trash_folder);
   return imap_move_message(conn, folder, uid, trash_folder);
}

int email_archive_message(const email_conn_t *conn,
                          const char *folder,
                          uint32_t uid,
                          bool is_gmail) {
   const char *archive_folder = is_gmail ? "[Gmail]/All Mail" : "Archive";
   OLOG_INFO("email_imap: archiving UID %u from %s to %s", uid, folder, archive_folder);
   return imap_move_message(conn, folder, uid, archive_folder);
}
