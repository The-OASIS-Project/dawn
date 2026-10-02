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
 * Email text made safe to show (email_display.h).
 */

#include "tools/email_display.h"

#include <string.h>

#include "utils/string_utils.h"

/* Tag characters (U+E0000-E007F): invisible on screen, but a model reads
 * them as text, so they can carry instructions no person sees. */
static bool tag_char(unsigned cp) {
   return cp >= 0xE0000 && cp <= 0xE007F;
}

/* Whether the code point is one that changes how text reads without being
 * seen: bidi controls and marks, zero-width and other blank characters. */
static bool invisible(unsigned cp) {
   return (cp >= 0x200B && cp <= 0x200F) || /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
          (cp >= 0x202A && cp <= 0x202E) || /* LRE, RLE, PDF, LRO, RLO */
          (cp >= 0x2060 && cp <= 0x2064) || /* word joiner, invisible operators */
          (cp >= 0x2066 && cp <= 0x206F) || /* LRI..PDI, deprecated format controls */
          (cp >= 0xFE00 && cp <= 0xFE0F) || /* variation selectors */
          (cp >= 0xFFF9 && cp <= 0xFFFB) || /* interlinear annotation */
          cp == 0x061C ||                   /* Arabic letter mark */
          cp == 0x00AD ||                   /* soft hyphen */
          cp == 0x034F ||                   /* combining grapheme joiner */
          cp == 0x180E ||                   /* Mongolian vowel separator */
          cp == 0x115F || cp == 0x1160 ||   /* Hangul fillers */
          cp == 0x3164 || cp == 0xFFA0 ||   /*   (render blank) */
          cp == 0xFEFF ||                   /* BOM / zero-width no-break space */
          tag_char(cp);
}

/* Line and paragraph separators: a line break to a text renderer. */
static bool line_separator(unsigned cp) {
   return cp == 0x2028 || cp == 0x2029;
}

static unsigned decode(const unsigned char *s, size_t n) {
   if (n == 2)
      return ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
   if (n == 3)
      return ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
   return ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
}

/* The well-formed sequence at s within len bytes, or 0. */
static size_t seq_len(const unsigned char *s, size_t len) {
   char tmp[5] = { 0 };
   const size_t take = len < 4 ? len : 4;
   memcpy(tmp, s, take);
   return utf8_valid_seq_len(tmp);
}

size_t email_display_sanitize(const char *src,
                              size_t src_len,
                              char *out,
                              size_t out_size,
                              unsigned flags) {
   if (!out || out_size == 0)
      return 0;
   size_t o = 0;
   const unsigned char *s = (const unsigned char *)(src ? src : "");
   if (!src)
      src_len = 0;
   for (size_t i = 0; i < src_len && s[i];) {
      const unsigned char c = s[i];
      const unsigned char *piece = &c;
      size_t plen = 1;
      unsigned char repl;
      if (c < 0x80) {
         if (c == '\r' || c == '\n' || c == '\t') {
            repl = ' ';
            piece = &repl;
         } else if (c < 0x20 || c == 0x7F) {
            i++;
            continue;
         } else if ((flags & EMAIL_DISPLAY_FILENAME) && (c == '/' || c == '\\')) {
            repl = '_';
            piece = &repl;
         }
         i++;
      } else {
         const size_t n = seq_len(s + i, src_len - i);
         if (!n) {
            repl = '?';
            piece = &repl;
            i++;
         } else {
            const unsigned cp = decode(s + i, n);
            if ((cp >= 0x80 && cp <= 0x9F) || invisible(cp)) {
               i += n;
               continue;
            }
            if (line_separator(cp)) {
               repl = ' '; /* one line, as for CR and LF */
               piece = &repl;
            } else {
               piece = s + i;
               plen = n;
            }
            i += n;
         }
      }
      /* No leading space, and no run of spaces. */
      if (plen == 1 && *piece == ' ' && (o == 0 || out[o - 1] == ' '))
         continue;
      if (o + plen >= out_size)
         break;
      memcpy(out + o, piece, plen);
      o += plen;
   }
   while (o > 0 && out[o - 1] == ' ')
      o--;
   out[o] = '\0';
   return o;
}

/* The Content-ID characters real mail uses ("part1.0609@example.com",
 * "image001.png@01D2..."): no character with a meaning in a URL or HTML. */
static bool cid_char(unsigned char c) {
   if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      return true;
   return c != '\0' && strchr("._@$+=-", c) != NULL;
}

bool email_display_content_id(const char *src, size_t src_len, char *out, size_t out_size) {
   if (!out || out_size == 0)
      return false;
   out[0] = '\0';
   if (!src)
      return false;
   while (src_len > 0 && (*src == ' ' || *src == '\t')) {
      src++;
      src_len--;
   }
   while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\t'))
      src_len--;
   if (src_len >= 2 && src[0] == '<' && src[src_len - 1] == '>') {
      src++;
      src_len -= 2;
   }
   if (src_len == 0 || src_len >= out_size)
      return false;
   for (size_t i = 0; i < src_len; i++) {
      if (!cid_char((unsigned char)src[i]))
         return false;
   }
   memcpy(out, src, src_len);
   out[src_len] = '\0';
   return true;
}

size_t email_display_body_clean(char *s) {
   if (!s)
      return 0;
   size_t o = 0;
   for (size_t i = 0; s[i];) {
      const unsigned char c = (unsigned char)s[i];
      const size_t n = c < 0x80 ? 0 : utf8_valid_seq_len(s + i);
      if (n) {
         const unsigned cp = decode((const unsigned char *)s + i, n);
         if (tag_char(cp)) {
            i += n;
            continue;
         }
         if (line_separator(cp)) {
            s[o++] = '\n';
            i += n;
            continue;
         }
         memmove(s + o, s + i, n);
         o += n;
         i += n;
      } else {
         s[o++] = s[i++];
      }
   }
   s[o] = '\0';
   return o;
}
