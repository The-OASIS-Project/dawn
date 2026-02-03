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
#include <string.h>
#include <strings.h>

void sanitize_utf8_for_json(char *str) {
   if (!str)
      return;

   unsigned char *src = (unsigned char *)str;
   unsigned char *dst = (unsigned char *)str;

   while (*src) {
      unsigned char c = *src;

      /* ASCII printable or allowed whitespace */
      if (c >= 32 && c < 127) {
         *dst++ = c;
         src++;
      } else if (c == '\n' || c == '\r' || c == '\t') {
         *dst++ = c;
         src++;
      } else if (c < 32) {
         /* Control character - skip */
         src++;
      } else if ((c & 0xE0) == 0xC0) {
         /* 2-byte UTF-8 sequence - check for truncation before accessing src[1] */
         if (src[1] != '\0' && (src[1] & 0xC0) == 0x80) {
            /* Valid 2-byte sequence - keep it */
            *dst++ = *src++;
            *dst++ = *src++;
         } else {
            /* Invalid/truncated sequence - replace with ? */
            *dst++ = '?';
            src++;
         }
      } else if ((c & 0xF0) == 0xE0) {
         /* 3-byte UTF-8 sequence - check for truncation before accessing src[1], src[2] */
         if (src[1] != '\0' && src[2] != '\0' && (src[1] & 0xC0) == 0x80 &&
             (src[2] & 0xC0) == 0x80) {
            /* Check for problematic codepoints (surrogates U+D800-U+DFFF) */
            unsigned int codepoint = ((c & 0x0F) << 12) | ((src[1] & 0x3F) << 6) | (src[2] & 0x3F);
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
               /* Surrogate - invalid in UTF-8 */
               *dst++ = '?';
               src += 3;
            } else {
               /* Valid 3-byte sequence - keep it */
               *dst++ = *src++;
               *dst++ = *src++;
               *dst++ = *src++;
            }
         } else {
            /* Invalid/truncated sequence - replace with ? */
            *dst++ = '?';
            src++;
         }
      } else if ((c & 0xF8) == 0xF0) {
         /* 4-byte UTF-8 sequence - check for truncation before accessing src[1-3] */
         if (src[1] != '\0' && src[2] != '\0' && src[3] != '\0' && (src[1] & 0xC0) == 0x80 &&
             (src[2] & 0xC0) == 0x80 && (src[3] & 0xC0) == 0x80) {
            /* Valid 4-byte sequence - keep it */
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
         } else {
            /* Invalid/truncated sequence - replace with ? */
            *dst++ = '?';
            src++;
         }
      } else {
         /* Invalid UTF-8 start byte (0x80-0xBF or 0xF8-0xFF) */
         *dst++ = '?';
         src++;
      }
   }
   *dst = '\0';
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
