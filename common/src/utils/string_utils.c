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
 * String Utilities - Common string functions shared across tools
 */

#include "utils/string_utils.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Whether code point @p cp is one a reader can't see: controls, the soft
 * hyphen, bidi and zero-width marks and overrides, invisible fillers,
 * variation selectors, interlinear annotations, tag characters. */
static bool excerpt_invisible(unsigned cp) {
   return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD || cp == 0x061C || cp == 0x115F ||
          cp == 0x1160 || cp == 0x180E || (cp >= 0x200B && cp <= 0x200F) ||
          (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || cp == 0x3164 ||
          (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
          (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
          (cp >= 0xE0000 && cp <= 0xE0FFF);
}

/* The length of the UTF-8 character at @p p (1 for an invalid byte), and
 * whether a reader can't see it (excerpt_invisible).  Only well-formed UTF-8
 * is a character: an overlong form, a surrogate, a code point past U+10FFFF
 * or a cut sequence is one hidden byte. */
static size_t excerpt_char(const unsigned char *p, bool *hidden) {
   *hidden = true;
   unsigned cp;
   size_t n;
   unsigned min;
   if (p[0] < 0x80) {
      *hidden = excerpt_invisible(p[0]);
      return 1;
   } else if ((p[0] & 0xE0) == 0xC0) {
      cp = p[0] & 0x1Fu;
      n = 2;
      min = 0x80;
   } else if ((p[0] & 0xF0) == 0xE0) {
      cp = p[0] & 0x0Fu;
      n = 3;
      min = 0x800;
   } else if ((p[0] & 0xF8) == 0xF0) {
      cp = p[0] & 0x07u;
      n = 4;
      min = 0x10000;
   } else {
      return 1;
   }
   for (size_t i = 1; i < n; i++) {
      if ((p[i] & 0xC0) != 0x80) {
         return 1; /* stops at a NUL too: never reads past the end */
      }
      cp = (cp << 6) | (p[i] & 0x3Fu);
   }
   if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return 1;
   }
   *hidden = excerpt_invisible(cp);
   return n;
}

void str_excerpt_line(const char *in, size_t max_bytes, char *out, size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   out[0] = '\0';
   const unsigned char *p = (const unsigned char *)(in ? in : "");
   /* Room for the longest cut note: "... (4294967295 more characters)". */
   const size_t note = 40;
   const size_t room = out_len > note + 1 ? out_len - note - 1 : 0;
   size_t w = 0, read = 0;
   bool space = false;
   while (*p) {
      bool hidden;
      const size_t n = excerpt_char(p, &hidden);
      const bool blank = hidden || *p == ' ';
      if (blank) {
         if (w > 0 && !space) {
            if (w + 1 > room || read + 1 > max_bytes) {
               break;
            }
            out[w++] = ' ';
            read++;
         }
         space = true;
      } else {
         if (w + n > room || read + n > max_bytes) {
            break;
         }
         memcpy(out + w, p, n);
         w += n;
         read += n;
         space = false;
      }
      p += n;
   }
   while (w > 0 && out[w - 1] == ' ') {
      w--;
   }
   out[w] = '\0';
   /* What was left out, in characters a reader would see. */
   size_t left = 0;
   while (*p) {
      bool hidden;
      const size_t n = excerpt_char(p, &hidden);
      left += !hidden && *p != ' ';
      p += n;
   }
   if (left > 0) {
      snprintf(out + w, out_len - w, "... (%zu more characters)", left);
   }
}

void utf8_trim_incomplete(char *s) {
   if (!s)
      return;
   size_t len = strlen(s);
   size_t i = len;
   while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
      i--; /* walk back over continuation bytes (10xxxxxx) */
   }
   if (i == 0) {
      return;
   }
   unsigned char lead = (unsigned char)s[i - 1];
   size_t seq_len = 1;
   if ((lead & 0xE0) == 0xC0) {
      seq_len = 2;
   } else if ((lead & 0xF0) == 0xE0) {
      seq_len = 3;
   } else if ((lead & 0xF8) == 0xF0) {
      seq_len = 4;
   }
   if (seq_len > (len - (i - 1))) {
      s[i - 1] = '\0'; /* incomplete trailing sequence — truncate at the lead byte */
   }
}

void utf8_truncate(char *str, size_t max_bytes) {
   if (!str || strlen(str) <= max_bytes)
      return;
   str[max_bytes] = '\0';
   utf8_trim_incomplete(str);
}

size_t utf8_valid_seq_len(const char *str) {
   const unsigned char *s = (const unsigned char *)str;
   const unsigned char c = s[0];
   size_t n;
   unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the second byte */
   if (c >= 0xC2 && c <= 0xDF) {
      n = 2;
   } else if (c >= 0xE0 && c <= 0xEF) {
      n = 3;
      if (c == 0xE0)
         lo = 0xA0; /* overlong */
      else if (c == 0xED)
         hi = 0x9F; /* surrogates */
   } else if (c >= 0xF0 && c <= 0xF4) {
      n = 4;
      if (c == 0xF0)
         lo = 0x90; /* overlong */
      else if (c == 0xF4)
         hi = 0x8F; /* past U+10FFFF */
   } else {
      return 0;
   }
   if (s[1] < lo || s[1] > hi)
      return 0;
   for (size_t i = 2; i < n; i++) {
      if ((s[i] & 0xC0) != 0x80)
         return 0;
   }
   return n;
}

void sanitize_utf8_for_json(char *str) {
   if (!str)
      return;

   unsigned char *src = (unsigned char *)str;
   unsigned char *dst = (unsigned char *)str;

   while (*src) {
      unsigned char c = *src;

      if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t') {
         *dst++ = c;
         src++;
      } else if (c < 32 || c == 127) {
         /* Control character - skip */
         src++;
      } else {
         const size_t n = utf8_valid_seq_len((const char *)src);
         if (n) {
            memmove(dst, src, n);
            dst += n;
            src += n;
         } else {
            /* Invalid, overlong or truncated - replace the lead byte */
            *dst++ = '?';
            src++;
         }
      }
   }
   *dst = '\0';
}

void extract_url_host(const char *url, char *out, size_t out_size) {
   if (!url || !out || out_size == 0) {
      if (out && out_size > 0) {
         out[0] = '\0';
      }
      return;
   }

   /* Skip protocol (http:// or https://) */
   const char *p = strstr(url, "://");
   p = p ? p + 3 : url;

   /* Find end of hostname (stop at /, :, ?, or end) */
   const char *end = p;
   while (*end && *end != '/' && *end != ':' && *end != '?') {
      end++;
   }

   size_t len = (size_t)(end - p);
   if (len >= out_size) {
      len = out_size - 1;
   }
   memcpy(out, p, len);
   out[len] = '\0';
}

const char *strcasestr_portable(const char *haystack, const char *needle) {
   if (!haystack || !needle)
      return NULL;
   size_t needle_len = strlen(needle);
   if (needle_len == 0)
      return haystack;

   for (; *haystack; haystack++) {
      if (strncasecmp(haystack, needle, needle_len) == 0) {
         return haystack;
      }
   }
   return NULL;
}

/* =============================================================================
 * Sentence Boundary Detection
 * ============================================================================= */

/* Common abbreviations that end with a period but aren't sentence endings */
static const char *ABBREVIATIONS[] = {
   /* Titles */
   "Mr", "Mrs", "Ms", "Dr", "Prof", "Sr", "Jr", "Rev", "Gen", "Col", "Lt", "Sgt", "Capt",
   /* Geographic */
   "U.S", "U.K", "E.U", "St", "Mt", "Ave", "Blvd", "Rd",
   /* Time/Date */
   "Jan", "Feb", "Mar", "Apr", "Jun", "Jul", "Aug", "Sep", "Sept", "Oct", "Nov", "Dec", "Mon",
   "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
   /* Common */
   "vs", "etc", "e.g", "i.e", "al", "approx", "govt", "dept", "est", "inc", "corp", "ltd", "no",
   "nos", "vol", "pp", "fig", "ch", "sec", "pt", NULL /* Sentinel */
};

bool str_is_abbreviation(const char *text, const char *period_pos) {
   if (!text || !period_pos || period_pos <= text) {
      return false;
   }

   /* Find start of word before the period */
   const char *word_end = period_pos;
   const char *word_start = period_pos;

   /* Skip back over any periods (handles U.S., e.g., etc.) */
   while (word_start > text) {
      const char *prev = word_start - 1;
      if (isalpha((unsigned char)*prev)) {
         word_start = prev;
      } else if (*prev == '.' && word_start - 1 > text &&
                 isalpha((unsigned char)*(word_start - 2))) {
         /* Skip embedded period (like in U.S.) */
         word_start = prev;
      } else {
         break;
      }
   }

   if (word_start >= word_end) {
      return false;
   }

   /* Extract the word (without trailing period) */
   size_t word_len = word_end - word_start;
   if (word_len == 0 || word_len > 10) {
      return false;
   }

   char word[12];
   size_t j = 0;
   for (const char *p = word_start; p < word_end && j < sizeof(word) - 1; p++) {
      if (isalpha((unsigned char)*p)) {
         word[j++] = *p;
      }
   }
   word[j] = '\0';

   /* Check against abbreviation list (case-insensitive) */
   for (int i = 0; ABBREVIATIONS[i] != NULL; i++) {
      if (strcasecmp(word, ABBREVIATIONS[i]) == 0) {
         return true;
      }
   }

   /* Also check for single capital letter (like middle initials: John F. Kennedy) */
   if (j == 1 && isupper((unsigned char)word[0])) {
      return true;
   }

   return false;
}

bool str_is_sentence_terminator(char c) {
   /* Note: Colon excluded - causes over-segmentation on times (3:00) and lists */
   return (c == '.' || c == '!' || c == '?');
}

bool str_is_sentence_boundary(const char *text, const char *pos) {
   if (!text || !pos || !*pos) {
      return false;
   }

   /* Must be a sentence terminator */
   if (!str_is_sentence_terminator(*pos)) {
      return false;
   }

   /* Look ahead - skip closing quotes/parens */
   const char *next = pos + 1;
   while (*next && (*next == '"' || *next == '\'' || *next == ')')) {
      next++;
   }

   /* Must be followed by whitespace or end of string */
   if (*next && !isspace((unsigned char)*next)) {
      return false;
   }

   /* For periods, check if this is an abbreviation */
   if (*pos == '.' && str_is_abbreviation(text, pos)) {
      return false;
   }

   return true;
}
