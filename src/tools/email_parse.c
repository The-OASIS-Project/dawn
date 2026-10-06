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
 * Pure parsing helpers for the IMAP email backend — see email_parse.h.
 */

#define _GNU_SOURCE /* strptime, timegm, struct tm.tm_gmtoff */

#include "tools/email_parse.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/buf_printf.h"

/* Convert a strptime-filled tm that carries a zone offset (%z -> tm_gmtoff)
 * into a UTC epoch.  timegm() reads the broken-down time AS UTC, so subtract
 * the parsed offset to recover the true instant.  Returns 0 on overflow/failure
 * so a bad parse reads as "unknown" rather than a 1970 timestamp. */
static time_t tm_with_offset_to_utc(struct tm *tm) {
   long off = tm->tm_gmtoff;
   time_t utc = timegm(tm);
   if (utc <= 0)
      return 0;
   utc -= off;
   return utc > 0 ? utc : 0;
}

/** Strip CR/LF from a header value to prevent SMTP header injection.  Shared by
 * both backends' send paths. */
void email_sanitize_header_value(const char *src, char *dst, size_t dst_len) {
   if (!dst || dst_len == 0)
      return;
   size_t j = 0;
   if (src) {
      for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
         if (src[i] != '\r' && src[i] != '\n')
            dst[j++] = src[i];
      }
   }
   dst[j] = '\0';
}

void email_format_mailbox(const char *name, const char *addr, char *dst, size_t dst_len) {
   if (!dst || dst_len == 0)
      return;
   if (!name || !name[0]) {
      snprintf(dst, dst_len, "%s", addr ? addr : "");
      return;
   }
   char quoted[192];
   size_t j = 0;
   size_t i = 0;
   quoted[j++] = '"';
   for (; name[i] && j < sizeof(quoted) - 3; i++) {
      if (name[i] == '"' || name[i] == '\\') {
         if (j >= sizeof(quoted) - 4)
            break;
         quoted[j++] = '\\';
      }
      quoted[j++] = name[i];
   }
   if (name[i]) {
      /* Cut short: never end inside a UTF-8 character. */
      size_t start = j;
      while (start > 1 && ((unsigned char)quoted[start - 1] & 0xC0) == 0x80)
         start--;
      if (start > 1 && ((unsigned char)quoted[start - 1] & 0xC0) == 0xC0) {
         const unsigned char lead = (unsigned char)quoted[start - 1];
         const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
         if (j - (start - 1) < want)
            j = start - 1;
      }
   }
   quoted[j++] = '"';
   quoted[j] = '\0';
   if (addr && addr[0])
      snprintf(dst, dst_len, "%s <%s>", quoted, addr);
   else
      snprintf(dst, dst_len, "%s", quoted); /* a From with no address */
}

void email_display_mailbox(const char *name, const char *addr, char *dst, size_t dst_len) {
   if (!dst || dst_len == 0)
      return;
   const bool has_addr = addr && addr[0];
   /* A name that is itself an address shows as the address it claims to be;
    * keep only the real one (or say there is none). */
   /* '@' or a lookalike: fullwidth U+FF20, small U+FE6B. */
   const bool name_is_addr = name &&
                             (strchr(name, '@') || strstr(name, "\xEF\xBC\xA0") ||
                              strstr(name, "\xEF\xB9\xAB")) &&
                             !(has_addr && strcasecmp(name, addr) == 0);
   if (name_is_addr)
      name = NULL;
   if (has_addr) {
      email_format_mailbox(name, addr, dst, dst_len);
   } else if (name && name[0]) {
      char quoted[200];
      email_format_mailbox(name, NULL, quoted, sizeof(quoted));
      snprintf(dst, dst_len, "%s (no address)", quoted);
   } else {
      snprintf(dst, dst_len, "(no address)");
   }
}

time_t email_parse_rfc822_date(const char *date_str) {
   if (!date_str || !date_str[0])
      return 0;

   struct tm tm_info;

   /* Preferred: forms carrying a numeric zone offset, so the result is a true
    * UTC instant independent of the sender's timezone. */
   static const char *const tz_fmts[] = {
      "%a, %d %b %Y %H:%M:%S %z",
      "%d %b %Y %H:%M:%S %z",
   };
   for (size_t i = 0; i < sizeof(tz_fmts) / sizeof(tz_fmts[0]); i++) {
      memset(&tm_info, 0, sizeof(tm_info));
      if (strptime(date_str, tz_fmts[i], &tm_info))
         return tm_with_offset_to_utc(&tm_info);
   }

   /* Fallback: no zone offset present.  Assume local time (best effort — the
    * historical behavior).  Returns 0 on total parse failure. */
   static const char *const notz_fmts[] = {
      "%a, %d %b %Y %H:%M:%S",
      "%d %b %Y %H:%M:%S",
   };
   for (size_t i = 0; i < sizeof(notz_fmts) / sizeof(notz_fmts[0]); i++) {
      memset(&tm_info, 0, sizeof(tm_info));
      if (strptime(date_str, notz_fmts[i], &tm_info)) {
         tm_info.tm_isdst = -1;
         time_t t = mktime(&tm_info);
         return t > 0 ? t : 0;
      }
   }
   return 0;
}

time_t email_parse_imap_internaldate(const char *idate) {
   if (!idate || !idate[0])
      return 0;

   /* Skip a leading quote if the caller passed the value with its quotes. */
   if (*idate == '"')
      idate++;

   struct tm tm_info;
   memset(&tm_info, 0, sizeof(tm_info));
   /* RFC 3501 date-time: "10-Sep-2026 15:45:00 +0000".  A single leading space
    * for one-digit days ("%e"-style) is tolerated by strptime's %d. */
   if (strptime(idate, "%d-%b-%Y %H:%M:%S %z", &tm_info))
      return tm_with_offset_to_utc(&tm_info);
   return 0;
}

bool email_parse_valid_iso_date(const char *iso) {
   if (!iso || !iso[0])
      return false;
   struct tm tm_info;
   memset(&tm_info, 0, sizeof(tm_info));
   /* Exactly a YYYY-MM-DD calendar date (the search since/before format).
    * strptime tolerates trailing text, so require it to consume the WHOLE string
    * — otherwise "2025-01-01garbage" would validate.  Shared authority: both the
    * IMAP (validate_imap_date) and Gmail (build_search_query) backends gate their
    * date emission on this, so an invalid date is dropped identically. */
   const char *end = strptime(iso, "%Y-%m-%d", &tm_info);
   return end != NULL && *end == '\0';
}

/* =============================================================================
 * IMAP SEARCH quoted-string builders (pure; unit-tested in test_email_parse.c)
 *
 * Appends `value` as an IMAP quoted string ("...") for safe interpolation into a
 * SEARCH command: control characters (CR/LF/NUL/DEL, everything < 0x20 and 0x7f)
 * are dropped, the two quoted-string metacharacters (" and \) are backslash-
 * escaped, and '%' is emitted as '%25'.
 *
 * CRITICAL — the built command is URL-DECODED AGAIN by libcurl: the SEARCH is
 * carried via CURLOPT_CUSTOMREQUEST, and libcurl's IMAP layer runs
 * Curl_urldecode() (with control-char rejection) over it before it hits the wire.
 * So sanitizing the pre-decode form is not enough — a percent-encoded
 * metacharacter (%22 -> ", %5C -> \) would survive this function and
 * re-materialize on the wire, breaking out of the quotes.  Escaping every literal
 * '%' as '%25' makes the user's bytes round-trip to themselves after curl's
 * decode (and lets a genuine term like "50% off" through, which would otherwise
 * fail as CURLE_URL_MALFORMAT).  curl's REJECT_CTRL still blocks a decoded
 * CR/LF/NUL, so no command chaining.  This is why the earlier IMAP *literal* form
 * ({N}\r\n<data>) could not work — its embedded CR/LF hit REJECT_CTRL.  (High-bit
 * UTF-8 octets pass through unchanged, matching the prior no-CHARSET behavior.)
 * ============================================================================= */
void email_imap_append_quoted(char *buf, size_t *off, size_t *rem, const char *value) {
   BUF_PRINTF(buf, *off, *rem, "\"");
   for (const char *p = value ? value : ""; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 || c == 0x7f)
         continue; /* control chars are not permitted in an IMAP quoted string */
      if (c == '%') {
         /* Escape for curl's CUSTOMREQUEST URL-decode (see the note above). */
         BUF_PRINTF(buf, *off, *rem, "%%25");
         continue;
      }
      /* Emit the escaping backslash (for " and \) and the char in one append.  NOTE:
       * this does not make truncation safe — BUF_PRINTF keeps snprintf's partial
       * output, so a nearly full buffer can still end in a lone backslash (or a cut
       * "%25").  Callers size the buffer for the worst case and refuse to send a
       * command that filled it (see email_search / imap_windowed_search). */
      BUF_PRINTF(buf, *off, *rem, (c == '"' || c == '\\') ? "\\%c" : "%c", (char)c);
   }
   BUF_PRINTF(buf, *off, *rem, "\"");
}

void email_imap_append_search_key(char *buf,
                                  size_t *off,
                                  size_t *rem,
                                  const char *key,
                                  const char *value) {
   /* Skip the key entirely when `value` is NULL/empty or reduces to empty after
    * control-char stripping — emitting KEY "" matches the empty substring (every
    * message), silently turning a narrowing filter into match-all. */
   bool has_content = false;
   for (const char *p = value ? value : ""; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (!(c < 0x20 || c == 0x7f)) {
         has_content = true;
         break;
      }
   }
   if (!has_content)
      return;
   BUF_PRINTF(buf, *off, *rem, " %s ", key);
   email_imap_append_quoted(buf, off, rem, value);
}

bool email_imap_flags_contains(const char *flags_group, const char *flag) {
   if (!flags_group || !flag || !flag[0])
      return false;

   size_t flen = strlen(flag);
   const char *p = flags_group;
   while ((p = strcasestr(p, flag)) != NULL) {
      /* Left boundary: start-of-string, whitespace, or '('. */
      bool left_ok = (p == flags_group);
      if (!left_ok) {
         char prev = p[-1];
         left_ok = (prev == ' ' || prev == '\t' || prev == '(');
      }
      /* Right boundary: end-of-string, whitespace, or ')'. */
      char next = p[flen];
      bool right_ok = (next == '\0' || next == ' ' || next == '\t' || next == ')');

      if (left_ok && right_ok)
         return true;
      p += 1; /* overlapping search — advance one char and retry */
   }
   return false;
}

const char *email_imap_match_paren(const char *open) {
   if (!open || *open != '(')
      return NULL;
   int depth = 0;
   bool in_quote = false;
   for (const char *p = open; *p; p++) {
      if (in_quote) {
         if (*p == '\\' && p[1])
            p++; /* skip the escaped char */
         else if (*p == '"')
            in_quote = false;
         continue;
      }
      if (*p == '"')
         in_quote = true;
      else if (*p == '(')
         depth++;
      else if (*p == ')') {
         depth--;
         if (depth == 0)
            return p;
      }
   }
   return NULL;
}

/* Skip ASCII whitespace within [p, end). */
static const char *env_skip_ws(const char *p, const char *end) {
   while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
   return p;
}

/* Read one IMAP nstring (quoted-string or NIL) at p.  Copies the unescaped
 * contents into out (out="" for NIL).  Returns the pointer just past the token,
 * or NULL on a parse error / an IMAP literal ({N}, which the caller's transport
 * discards → treat as unparseable). `out` may be NULL to skip a field. */
static const char *env_read_nstring(const char *p, const char *end, char *out, size_t outsz) {
   if (out && outsz)
      out[0] = '\0';
   p = env_skip_ws(p, end);
   if (p >= end)
      return NULL;

   if (*p == '"') {
      p++;
      size_t o = 0;
      while (p < end && *p != '"') {
         if (*p == '\\' && p + 1 < end)
            p++; /* escaped char: take the next byte literally */
         if (out && o + 1 < outsz)
            out[o++] = *p;
         p++;
      }
      if (p >= end)
         return NULL; /* unterminated quoted string */
      if (out && outsz)
         out[o] = '\0';
      return p + 1; /* past closing quote */
   }

   if ((size_t)(end - p) >= 3 && strncasecmp(p, "NIL", 3) == 0) {
      const char *q = p + 3;
      /* NIL must be a whole token (bounded by whitespace, paren, or end). */
      if (q == end || *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' || *q == '(' || *q == ')')
         return q;
      return NULL;
   }

   if (*p == '{') {
      /* IMAP literal {N}.  libcurl's custom-command path discards the literal's
       * octet data but leaves the "{N}" marker, immediately followed by the next
       * token.  We can't recover the bytes, so treat this field as unavailable
       * (empty) and resume AFTER the marker so the remaining fields still parse
       * (e.g. a raw-8-bit subject shouldn't cost us the From). */
      const char *rb = (const char *)memchr(p, '}', (size_t)(end - p));
      if (!rb)
         return NULL;
      return rb + 1;
   }

   return NULL; /* unexpected token */
}

bool email_parse_envelope(const char *seg,
                          char *subject,
                          size_t subject_sz,
                          char *from_name,
                          size_t from_name_sz,
                          char *from_addr,
                          size_t from_addr_sz) {
   if (subject && subject_sz)
      subject[0] = '\0';
   if (from_name && from_name_sz)
      from_name[0] = '\0';
   if (from_addr && from_addr_sz)
      from_addr[0] = '\0';
   if (!seg)
      return false;

   static const char ENVELOPE_KW[] = "ENVELOPE";
   const char *env = strcasestr(seg, ENVELOPE_KW);
   if (!env)
      return false;
   const char *end = seg + strlen(seg);
   const char *p = env_skip_ws(env + (sizeof(ENVELOPE_KW) - 1), end);
   if (p >= end || *p != '(')
      return false;
   p++; /* into the ENVELOPE (...) group */

   /* Field 0: date (skip). */
   p = env_read_nstring(p, end, NULL, 0);
   if (!p)
      return false;
   /* Field 1: subject. */
   p = env_read_nstring(p, end, subject, subject_sz);
   if (!p)
      return false;

   /* Field 2: from = "(" 1*address ")" / NIL.  We take the first address only. */
   p = env_skip_ws(p, end);
   if (p >= end)
      return true; /* subject captured; no from */
   if ((size_t)(end - p) >= 3 && strncasecmp(p, "NIL", 3) == 0)
      return true; /* from is NIL */
   if (*p != '(')
      return true;
   p++; /* into the address-list */
   p = env_skip_ws(p, end);
   if (p >= end || *p != '(')
      return true; /* empty / malformed list */
   p++;            /* into the first address "(name adl mailbox host)" */

   char name[256], mailbox[256], host[256];
   p = env_read_nstring(p, end, name, sizeof(name));
   if (!p)
      return true;
   p = env_read_nstring(p, end, NULL, 0); /* addr-adl (at-domain-list) — discarded */
   if (!p)
      return true;
   p = env_read_nstring(p, end, mailbox, sizeof(mailbox));
   if (!p)
      return true;
   p = env_read_nstring(p, end, host, sizeof(host));
   if (!p)
      return true;

   if (from_name && from_name_sz)
      snprintf(from_name, from_name_sz, "%s", name);
   if (from_addr && from_addr_sz) {
      if (mailbox[0] && host[0])
         snprintf(from_addr, from_addr_sz, "%s@%s", mailbox, host);
      else if (mailbox[0])
         snprintf(from_addr, from_addr_sz, "%s", mailbox);
   }
   return true;
}

/* Scan from `from` for the next untagged response that begins a real FETCH
 * ("* <seq> FETCH ... UID ...").  Used to resynchronize after the transport
 * discards an IMAP literal ({N}) and truncates a FETCH line: it anchors on an
 * actual FETCH boundary — validated with the same FETCH-within-64 / UID-within
 * checks the main loop trusts — rather than any bare "* " (which can appear
 * inside a quoted subject/name) or any "{" (a quoted brace is not a literal
 * marker).  Returns NULL if no further FETCH boundary exists. */
static const char *next_fetch_boundary(const char *from) {
   const char *q = from;
   while ((q = strstr(q, "* ")) != NULL) {
      const char *kw = strcasestr(q, "FETCH");
      if (kw && kw <= q + 64) {
         const char *uid = strstr(q, "UID ");
         if (uid && uid <= kw + 256)
            return q;
      }
      q += 2;
   }
   return NULL;
}

const char *email_imap_next_fetch(const char *p,
                                  const char **out_seg,
                                  size_t *out_seg_len,
                                  uint32_t *out_uid) {
   if (out_seg)
      *out_seg = NULL;
   if (out_seg_len)
      *out_seg_len = 0;
   if (out_uid)
      *out_uid = 0;
   if (!p)
      return NULL;

   while (*p) {
      const char *fetch = strstr(p, "* ");
      if (!fetch)
         return NULL;

      /* "FETCH" follows "* <seq> " within a few bytes; bound the scan so a
       * hostile server can't force O(n^2) rescans on a flood of "* " tokens. */
      const char *fetch_kw = strcasestr(fetch, "FETCH");
      if (!fetch_kw || fetch_kw > fetch + 64) {
         p = fetch + 2;
         continue;
      }
      const char *uid_str = strstr(fetch, "UID ");
      if (!uid_str || uid_str > fetch_kw + 256) {
         p = fetch_kw + 5;
         continue;
      }
      uint32_t uid = (uint32_t)strtoul(uid_str + 4, NULL, 10);

      const char *open = strchr(fetch_kw, '(');
      if (!open) {
         p = fetch_kw + 5;
         continue;
      }

      const char *close = email_imap_match_paren(open);
      const char *seg_end; /* one past this message's text */
      const char *resume;
      if (close) {
         seg_end = close + 1;
         resume = close + 1;
      } else {
         /* Unclosed item list: the ENVELOPE carried an IMAP literal ({N}) the
          * transport discarded, dropping the rest of the line and jumping to the
          * next "* <seq> FETCH".  Resync to the next VALIDATED FETCH boundary
          * (scanned after this message's own FETCH keyword) so a quoted "* " or
          * a quoted "{" in the truncated envelope can't misdirect the resync and
          * make this message swallow the rest of the batch. */
         const char *nxt = next_fetch_boundary(fetch_kw + 5);
         if (nxt) {
            seg_end = nxt;
            resume = nxt;
         } else {
            seg_end = fetch + strlen(fetch); /* nothing after: last, truncated */
            resume = seg_end;
         }
      }

      if (out_seg)
         *out_seg = fetch;
      if (out_seg_len)
         *out_seg_len = (size_t)(seg_end - fetch);
      if (out_uid)
         *out_uid = uid;
      return resume;
   }
   return NULL;
}

/* =============================================================================
 * IMAP UID selection + paging cursor (pure; unit-tested in test_email_parse.c)
 * ============================================================================= */

/* Restore the min-heap property below index i (heap[0] is the smallest kept UID). */
static void uid_heap_sift_down(uint32_t *heap, int n, int i) {
   for (;;) {
      int smallest = i;
      int l = 2 * i + 1;
      int r = l + 1;
      if (l < n && heap[l] < heap[smallest])
         smallest = l;
      if (r < n && heap[r] < heap[smallest])
         smallest = r;
      if (smallest == i)
         return;
      uint32_t t = heap[i];
      heap[i] = heap[smallest];
      heap[smallest] = t;
      i = smallest;
   }
}

static void uid_heap_sift_up(uint32_t *heap, int i) {
   while (i > 0) {
      int parent = (i - 1) / 2;
      if (heap[parent] <= heap[i])
         return;
      uint32_t t = heap[i];
      heap[i] = heap[parent];
      heap[parent] = t;
      i = parent;
   }
}

static int cmp_uid_asc(const void *a, const void *b) {
   uint32_t x = *(const uint32_t *)a;
   uint32_t y = *(const uint32_t *)b;
   return (x > y) - (x < y);
}

/* Case-insensitive "* SEARCH" at the start of a line. */
static bool line_is_search(const char *line) {
   static const char kSearch[] = "* SEARCH";
   for (size_t i = 0; i < sizeof(kSearch) - 1; i++) {
      if (toupper((unsigned char)line[i]) != kSearch[i])
         return false;
   }
   return true;
}

int email_imap_select_newest_uids(const char *response, uint32_t *out, int wanted, int *total_out) {
   int total = 0;
   int n = 0;
   if (total_out)
      *total_out = 0;
   if (!response || !out)
      return 0;

   const char *line = response;
   while (*line) {
      const char *eol = line;
      while (*eol && *eol != '\r' && *eol != '\n')
         eol++;

      if (line_is_search(line)) {
         const char *p = line + 8;
         while (p < eol) {
            while (p < eol && *p == ' ')
               p++;
            if (p >= eol || !isdigit((unsigned char)*p))
               break; /* end of the number list (or a trailing non-numeric token) */
            uint64_t v = 0;
            bool overflow = false;
            while (p < eol && isdigit((unsigned char)*p)) {
               v = v * 10 + (uint64_t)(*p - '0');
               if (v > UINT32_MAX)
                  overflow = true;
               p++;
            }
            if (overflow || v == 0)
               continue; /* not a valid UID; skip, don't count */
            total++;
            if (wanted <= 0)
               continue;
            if (n < wanted) {
               out[n] = (uint32_t)v;
               uid_heap_sift_up(out, n);
               n++;
            } else if ((uint32_t)v > out[0]) {
               out[0] = (uint32_t)v;
               uid_heap_sift_down(out, n, 0);
            }
         }
      }

      line = eol;
      while (*line == '\r' || *line == '\n')
         line++;
   }

   if (n > 1)
      qsort(out, (size_t)n, sizeof(*out), cmp_uid_asc);
   /* A buggy or hostile server may repeat UIDs; FETCHing one twice would show the
    * message twice, so collapse duplicates (the list is sorted). */
   int w = 0;
   for (int i = 0; i < n; i++) {
      if (w == 0 || out[i] != out[w - 1])
         out[w++] = out[i];
   }
   total -= n - w; /* repeats aren't more mail; don't let them imply another page */
   n = w;
   if (total_out)
      *total_out = total;
   return n;
}

bool email_imap_page_token_format(uint32_t before_uid,
                                  uint32_t uidvalidity,
                                  char *out,
                                  size_t out_len) {
   if (!out || out_len == 0 || before_uid < 2)
      return false;
   int w = uidvalidity ? snprintf(out, out_len, "u%u.%u", before_uid, uidvalidity)
                       : snprintf(out, out_len, "u%u", before_uid);
   if (w < 0 || (size_t)w >= out_len) {
      out[0] = '\0';
      return false;
   }
   return true;
}

/* Parse 1-10 digits with no leading zero into a uint32; advances *pp. */
static bool parse_u32_strict(const char **pp, uint32_t *out) {
   const char *p = *pp;
   if (!isdigit((unsigned char)*p) || *p == '0')
      return false;
   uint64_t v = 0;
   int digits = 0;
   while (isdigit((unsigned char)*p)) {
      if (++digits > 10)
         return false;
      v = v * 10 + (uint64_t)(*p - '0');
      p++;
   }
   if (v > UINT32_MAX)
      return false;
   *out = (uint32_t)v;
   *pp = p;
   return true;
}

bool email_imap_page_token_parse(const char *tok, uint32_t *before_uid, uint32_t *uidvalidity) {
   if (!tok || tok[0] != 'u' || !before_uid || !uidvalidity)
      return false;
   const char *p = tok + 1;
   uint32_t uid = 0;
   uint32_t v = 0;
   if (!parse_u32_strict(&p, &uid) || uid < 2)
      return false;
   if (*p == '.') {
      p++;
      if (!parse_u32_strict(&p, &v))
         return false;
   }
   if (*p != '\0')
      return false;
   *before_uid = uid;
   *uidvalidity = v;
   return true;
}

bool email_imap_parse_uidvalidity(const char *line, size_t len, uint32_t *out) {
   static const char kPrefix[] = "* OK [UIDVALIDITY ";
   const size_t plen = sizeof(kPrefix) - 1;
   if (!line || !out || len <= plen)
      return false;
   for (size_t i = 0; i < plen; i++) {
      if (toupper((unsigned char)line[i]) != kPrefix[i])
         return false;
   }
   size_t i = plen;
   uint64_t v = 0;
   int digits = 0;
   while (i < len && isdigit((unsigned char)line[i])) {
      if (++digits > 10)
         return false;
      v = v * 10 + (uint64_t)(line[i] - '0');
      i++;
   }
   if (digits == 0 || v == 0 || v > UINT32_MAX || i >= len || line[i] != ']')
      return false;
   *out = (uint32_t)v;
   return true;
}

bool email_imap_parse_exists(const char *line, size_t len, uint32_t *out) {
   static const char kSuffix[] = " EXISTS";
   const size_t slen = sizeof(kSuffix) - 1;
   if (!line || !out || len < 3 || line[0] != '*' || line[1] != ' ')
      return false;
   size_t i = 2;
   uint64_t v = 0;
   int digits = 0;
   while (i < len && isdigit((unsigned char)line[i])) {
      if (++digits > 10)
         return false;
      v = v * 10 + (uint64_t)(line[i] - '0');
      i++;
   }
   if (digits == 0 || v > UINT32_MAX || len - i < slen)
      return false;
   for (size_t k = 0; k < slen; k++) {
      if (toupper((unsigned char)line[i + k]) != kSuffix[k])
         return false;
   }
   i += slen;
   if (i < len && line[i] != '\r' && line[i] != '\n')
      return false;
   *out = (uint32_t)v;
   return true;
}

bool email_imap_id_parse(const char *message_id, char *folder, size_t folder_size, uint32_t *uid) {
   if (!message_id || !folder || folder_size == 0 || !uid)
      return false;
   const char *uid_str = message_id;
   const char *last_colon = strrchr(message_id, ':');
   if (last_colon && last_colon > message_id) {
      const size_t len = (size_t)(last_colon - message_id);
      if (len >= folder_size)
         return false;
      memcpy(folder, message_id, len);
      folder[len] = '\0';
      uid_str = last_colon + 1;
   } else {
      if (snprintf(folder, folder_size, "INBOX") >= (int)folder_size)
         return false;
      if (last_colon)
         uid_str = last_colon + 1;
   }
   if (!isdigit((unsigned char)uid_str[0]))
      return false;
   char *end = NULL;
   const unsigned long v = strtoul(uid_str, &end, 10);
   if (!end || *end != '\0' || v == 0 || v > UINT32_MAX)
      return false;
   *uid = (uint32_t)v;
   return true;
}
