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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* =============================================================================
 * RFC 2047 encoded-word decoder (=?charset?Q?..?= / =?charset?B?..?=)
 *
 * Shared by both email backends for header display names / subjects.  NOTE: the
 * decoded bytes are emitted as-is — correct for UTF-8 (the modern norm), lossy
 * for legacy ISO-8859-x charsets (no transcoding).  Pre-existing behavior.
 * ============================================================================= */

/* Keep a decoded byte only if it is not a C0 control (tab excepted): an attacker
 * fully controls the decoded bytes via base64/QP, and an embedded NUL would
 * truncate the field while other control/CR-LF bytes would bleed into the
 * LLM/user context.  UTF-8 continuation bytes (>= 0x80) are preserved. */
static inline bool decoded_byte_ok(char c) {
   unsigned char u = (unsigned char)c;
   return u >= 0x20 || u == '\t';
}

/** Decode a single RFC 2047 quoted-printable encoded word.  Precondition:
 * dst_len >= 1 (guarded here defensively). */
static size_t decode_qp_word(const char *src, size_t src_len, char *dst, size_t dst_len) {
   if (dst_len == 0)
      return 0;
   size_t j = 0;
   for (size_t i = 0; i < src_len && j < dst_len - 1; i++) {
      char c;
      if (src[i] == '_') {
         c = ' ';
      } else if (src[i] == '=' && i + 2 < src_len && isxdigit((unsigned char)src[i + 1]) &&
                 isxdigit((unsigned char)src[i + 2])) {
         char hex[3] = { src[i + 1], src[i + 2], '\0' };
         c = (char)strtol(hex, NULL, 16);
         i += 2;
      } else {
         c = src[i];
      }
      if (decoded_byte_ok(c))
         dst[j++] = c;
   }
   dst[j] = '\0';
   return j;
}

/** Simple base64 decode (RFC 2045 alphabet).  Precondition: dst_len >= 1
 * (guarded here defensively). */
static size_t decode_b64_word(const char *src, size_t src_len, char *dst, size_t dst_len) {
   if (dst_len == 0)
      return 0;
   static const int8_t b64_table[256] = {
      [0 ... 255] = -1, ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,
      ['G'] = 6,        ['H'] = 7,  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12,
      ['N'] = 13,       ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
      ['U'] = 20,       ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25, ['a'] = 26,
      ['b'] = 27,       ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33,
      ['i'] = 34,       ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40,
      ['p'] = 41,       ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47,
      ['w'] = 48,       ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54,
      ['3'] = 55,       ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61,
      ['+'] = 62,       ['/'] = 63,
   };

   size_t j = 0;
   uint32_t accum = 0;
   int bits = 0;

   for (size_t i = 0; i < src_len && j < dst_len - 1; i++) {
      int8_t val = b64_table[(unsigned char)src[i]];
      if (val < 0)
         continue; /* skip padding and whitespace */
      accum = (accum << 6) | val;
      bits += 6;
      if (bits >= 8) {
         bits -= 8;
         char c = (char)((accum >> bits) & 0xFF);
         if (decoded_byte_ok(c))
            dst[j++] = c;
      }
   }
   dst[j] = '\0';
   return j;
}

void email_decode_rfc2047(const char *src, char *dst, size_t dst_len) {
   if (!dst || dst_len == 0)
      return;
   dst[0] = '\0';
   if (!src)
      return;

   size_t out = 0;
   const char *p = src;
   while (*p && out < dst_len - 1) {
      if (strncmp(p, "=?", 2) != 0) {
         dst[out++] = *p++;
         continue;
      }

      /* Parse =?charset?encoding?text?= */
      const char *charset_start = p + 2;
      const char *q1 = strchr(charset_start, '?');
      if (!q1 || !q1[1] || q1[2] != '?') {
         dst[out++] = *p++;
         continue;
      }

      char encoding = q1[1];
      const char *text_start = q1 + 3;
      const char *end = strstr(text_start, "?=");
      if (!end) {
         dst[out++] = *p++;
         continue;
      }

      size_t text_len = (size_t)(end - text_start);

      if (encoding == 'Q' || encoding == 'q') {
         out += decode_qp_word(text_start, text_len, dst + out, dst_len - out);
      } else if (encoding == 'B' || encoding == 'b') {
         out += decode_b64_word(text_start, text_len, dst + out, dst_len - out);
      } else {
         /* Unknown encoding, copy literally. */
         dst[out++] = *p++;
         continue;
      }

      p = end + 2;

      /* RFC 2047 §6.2: whitespace between adjacent encoded words is ignored. */
      const char *ws = p;
      while (*ws == ' ' || *ws == '\t')
         ws++;
      if (strncmp(ws, "=?", 2) == 0)
         p = ws;
   }
   dst[out] = '\0';
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
