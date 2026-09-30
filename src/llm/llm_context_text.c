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
 * The text of what DAWN adds to a request (llm_context_text.h).
 */

#include "llm/llm_context_text.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *llm_context_with_tag(const char *text, const char *tag) {
   if (!text) {
      return NULL;
   }
   const char *t = tag ? tag : "";
   const size_t ph = strlen(LLM_CONTEXT_TAG_PLACEHOLDER);
   size_t count = 0;
   for (const char *p = strstr(text, LLM_CONTEXT_TAG_PLACEHOLDER); p;
        p = strstr(p + ph, LLM_CONTEXT_TAG_PLACEHOLDER)) {
      count++;
   }
   const size_t len = strlen(text) + count * strlen(t) + 1;
   char *out = malloc(len);
   if (!out) {
      return NULL;
   }
   size_t off = 0;
   const char *from = text;
   for (const char *p = strstr(from, LLM_CONTEXT_TAG_PLACEHOLDER); p;
        p = strstr(from, LLM_CONTEXT_TAG_PLACEHOLDER)) {
      memcpy(out + off, from, (size_t)(p - from));
      off += (size_t)(p - from);
      memcpy(out + off, t, strlen(t));
      off += strlen(t);
      from = p + ph;
   }
   memcpy(out + off, from, strlen(from) + 1);
   return out;
}

void llm_operator_note_label(const char *tag, char *buf, size_t size) {
   if (!buf || size == 0) {
      return;
   }
   if (tag && tag[0]) {
      snprintf(buf, size, "[Operator note %s] ", tag);
   } else {
      snprintf(buf, size, "[Operator note] ");
   }
}

/* ---- Neutralizing untrusted text ---------------------------------------
 *
 * Matching runs on a shadow of the text: characters that render as nothing
 * (zero-width, bidi and tag characters, combining marks, variation selectors,
 * fillers) are left out of it, and fullwidth ASCII, dash and bracket variants
 * and odd spaces appear as ASCII.  An imitation of a DAWN marker is found in
 * the shadow whatever separates its words (whitespace, newlines, "-_.:*"),
 * whatever opens it, and whatever letters spell them (a Cyrillic "О", a
 * mathematical "𝐓" match their Latin letter); so is a tag-shaped string
 * ("dawn-ctx-" and 8 hex digits, its hyphens in any spelling).  Only those
 * spans of the original are rewritten: every other byte passes as it was
 * (a tab, an em dash, a joiner in an emoji or a Persian word stays).  Each
 * shadow position knows where its run of separators and of rule characters
 * ends, so matching is linear in the text's length. */

/* The code point at @p s (UTF-8) in @p cp; its length in bytes (1 for a byte
 * that starts no valid sequence, taken as itself). */
static size_t utf8_at(const unsigned char *s, unsigned *cp) {
   if (s[0] < 0x80) {
      *cp = s[0];
      return 1;
   }
   size_t n = 0;
   unsigned c = 0;
   if ((s[0] & 0xE0) == 0xC0) {
      n = 2;
      c = s[0] & 0x1F;
   } else if ((s[0] & 0xF0) == 0xE0) {
      n = 3;
      c = s[0] & 0x0F;
   } else if ((s[0] & 0xF8) == 0xF0) {
      n = 4;
      c = s[0] & 0x07;
   } else {
      *cp = s[0];
      return 1;
   }
   for (size_t i = 1; i < n; i++) {
      if ((s[i] & 0xC0) != 0x80) {
         *cp = s[0];
         return 1;
      }
      c = (c << 6) | (s[i] & 0x3F);
   }
   *cp = c;
   return n;
}

/* What the shadow makes of code point @p cp: 0 to leave it out, an ASCII
 * byte to show it as, or -1 to show it as it is. */
static int shadowed(unsigned cp) {
   if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
       (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF ||
       cp == 0x00AD || cp == 0x180E || cp == 0x034F || (cp >= 0x0300 && cp <= 0x036F) ||
       (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
       (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0000 && cp <= 0xE007F) ||
       (cp >= 0xE0100 && cp <= 0xE01EF) || cp == 0x115F || cp == 0x1160 || cp == 0x3164 ||
       cp == 0xFFA0) {
      return 0;
   }
   if ((cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212 || cp == 0xFE58 || cp == 0xFE63 ||
       cp == 0x2500 || cp == 0x2501 || cp == 0x2E3A || cp == 0x2E3B) {
      return '-';
   }
   if (cp >= 0xFF01 && cp <= 0xFF5E) {
      return (int)(cp - 0xFEE0); /* fullwidth ASCII */
   }
   if (cp == 0xFE5D || cp == 0x3010 || cp == 0x3008 || cp == 0x27E8 || cp == 0x300C ||
       cp == 0x27E6 || cp == 0x3014 || cp == 0x00AB || cp == 0x2039 || cp == 0x300A) {
      return '[';
   }
   if (cp == 0xFE5E || cp == 0x3011 || cp == 0x3009 || cp == 0x27E9 || cp == 0x300D ||
       cp == 0x27E7 || cp == 0x3015 || cp == 0x00BB || cp == 0x203A || cp == 0x300B) {
      return ']';
   }
   if (cp == 0x00B7 || cp == 0x2022 || cp == 0x2027 || cp == 0x30FB) {
      return '.';
   }
   if (cp == 0x2028 || cp == 0x2029 || cp == 0x0085) {
      return '\n';
   }
   if (cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F ||
       cp == 0x3000 || cp == '\t') {
      return ' ';
   }
   return -1;
}

/* The ASCII letter or digit @p cp reads as (lowercase), or 0: Latin itself,
 * and its lookalikes in Cyrillic, Greek and the mathematical alphanumerics. */
static char folded(unsigned cp) {
   if (cp < 0x80) {
      return (cp >= 'A' && cp <= 'Z')                                 ? (char)(cp - 'A' + 'a')
             : ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) ? (char)cp
                                                                      : 0;
   }
   if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
       (cp >= 0xFF41 && cp <= 0xFF5A)) {
      return folded(cp - 0xFEE0); /* fullwidth */
   }
   if (cp >= 0x24B6 && cp <= 0x24CF) {
      return (char)('a' + (cp - 0x24B6)); /* circled capital */
   }
   if (cp >= 0x24D0 && cp <= 0x24E9) {
      return (char)('a' + (cp - 0x24D0)); /* circled small */
   }
   if (cp >= 0x2460 && cp <= 0x2468) {
      return (char)('1' + (cp - 0x2460)); /* circled digit */
   }
   if (cp == 0x24EA || cp == 0x2070 || cp == 0x2080) {
      return '0';
   }
   if (cp == 0x00B9) {
      return '1';
   }
   if (cp == 0x00B2 || cp == 0x00B3) {
      return (char)('2' + (cp - 0x00B2));
   }
   if ((cp >= 0x2074 && cp <= 0x2079) || (cp >= 0x2084 && cp <= 0x2089)) {
      return (char)('4' + (cp - (cp >= 0x2080 ? 0x2084 : 0x2074))); /* super/subscript */
   }
   if (cp >= 0x2081 && cp <= 0x2083) {
      return (char)('1' + (cp - 0x2081));
   }
   if (cp >= 0x1D400 && cp <= 0x1D6A3) {
      const unsigned k = (cp - 0x1D400) % 52;
      return (char)(k < 26 ? 'a' + k : 'a' + (k - 26));
   }
   if (cp >= 0x1D7CE && cp <= 0x1D7FF) {
      return (char)('0' + (cp - 0x1D7CE) % 10);
   }
   static const struct {
      unsigned cp;
      char c;
   } k_confusable[] = {
      /* Cyrillic */
      { 0x0410, 'a' },
      { 0x0430, 'a' },
      { 0x0412, 'b' },
      { 0x0415, 'e' },
      { 0x0435, 'e' },
      { 0x041A, 'k' },
      { 0x043A, 'k' },
      { 0x041C, 'm' },
      { 0x043C, 'm' },
      { 0x041D, 'h' },
      { 0x043D, 'h' },
      { 0x041E, 'o' },
      { 0x043E, 'o' },
      { 0x0420, 'p' },
      { 0x0440, 'p' },
      { 0x0421, 'c' },
      { 0x0441, 'c' },
      { 0x0422, 't' },
      { 0x0442, 't' },
      { 0x0425, 'x' },
      { 0x0445, 'x' },
      { 0x0423, 'y' },
      { 0x0443, 'y' },
      { 0x0406, 'i' },
      { 0x0456, 'i' },
      { 0x0408, 'j' },
      { 0x0458, 'j' },
      { 0x0405, 's' },
      { 0x0455, 's' },
      { 0x0501, 'd' },
      { 0x051B, 'q' },
      { 0x051D, 'w' },
      { 0x0475, 'v' },
      { 0x0474, 'v' },
      { 0x0418, 'n' },
      { 0x0438, 'n' },
      { 0x041F, 'n' },
      { 0x043F, 'n' },
      { 0x0417, '3' },
      { 0x0427, '4' },
      /* Greek */
      { 0x0391, 'a' },
      { 0x03B1, 'a' },
      { 0x0392, 'b' },
      { 0x0395, 'e' },
      { 0x03B5, 'e' },
      { 0x0396, 'z' },
      { 0x0397, 'h' },
      { 0x0399, 'i' },
      { 0x03B9, 'i' },
      { 0x039A, 'k' },
      { 0x03BA, 'k' },
      { 0x039C, 'm' },
      { 0x039D, 'n' },
      { 0x03B7, 'n' },
      { 0x039F, 'o' },
      { 0x03BF, 'o' },
      { 0x03A1, 'p' },
      { 0x03C1, 'p' },
      { 0x03A4, 't' },
      { 0x03C4, 't' },
      { 0x03A5, 'y' },
      { 0x03C5, 'u' },
      { 0x03A7, 'x' },
      { 0x03C7, 'x' },
      { 0x03BD, 'v' },
      { 0x03C9, 'w' },
      /* Latin letters that read as others */
      { 0x0131, 'i' },
      { 0x0269, 'i' },
      { 0x0261, 'g' },
      { 0x1D00, 'a' },
      { 0x0280, 'r' },
      { 0x0274, 'n' },
      { 0x1D1B, 't' },
      { 0x1D1C, 'u' },
   };
   for (size_t i = 0; i < sizeof(k_confusable) / sizeof(k_confusable[0]); i++) {
      if (k_confusable[i].cp == cp) {
         return k_confusable[i].c;
      }
   }
   return 0;
}

static bool is_ws(char c) {
   return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f';
}

/* A separator between a marker's words: whitespace, or a hyphen,
 * underscore, period, colon, asterisk, slash or comma. */
static bool is_sep(char c) {
   return is_ws(c) || c == '-' || c == '_' || c == '.' || c == ':' || c == '*' || c == '/' ||
          c == ',';
}

/* Characters a framed block's opening run is made of. */
static bool is_rule(char c) {
   return c == '-' || c == '=' || c == '*' || c == '#' || c == '~' || c == '_';
}

/* The shadow of a text, and where each of its bytes came from. */
typedef struct {
   char *s;          /* the shadow, NUL-terminated */
   uint32_t *src;    /* src[i]: the original offset shadow byte i came from; src[len]: its end */
   uint32_t *sep_to; /* sep_to[i]: where the run of separators at i ends (i: none) */
   uint32_t *rule_to;
   size_t len;
} shadow_t;

static void shadow_free(shadow_t *sh) {
   free(sh->s);
   free(sh->src);
   free(sh->sep_to);
   free(sh->rule_to);
   memset(sh, 0, sizeof(*sh));
}

/* HTML named character references a reader decodes to ASCII punctuation (the
 * characters DAWN's framing is made of, or what the shadow folds them to) or
 * a space.  A legacy one is also read in capitals and without its ';', as
 * browsers do. */
static const struct {
   const char *name; /* between '&' and ';' */
   char c;
   bool legacy;
} k_named_refs[] = {
   { "lt", '<', true },      { "gt", '>', true },        { "amp", '&', true },
   { "quot", '"', true },    { "nbsp", ' ', true },      { "lsqb", '[', false },
   { "lbrack", '[', false }, { "rsqb", ']', false },     { "rbrack", ']', false },
   { "lpar", '(', false },   { "rpar", ')', false },     { "lcub", '{', false },
   { "lbrace", '{', false }, { "rcub", '}', false },     { "rbrace", '}', false },
   { "vert", '|', false },   { "verbar", '|', false },   { "colon", ':', false },
   { "sol", '/', false },    { "period", '.', false },   { "comma", ',', false },
   { "lowbar", '_', false }, { "ast", '*', false },      { "midast", '*', false },
   { "equals", '=', false }, { "num", '#', false },      { "tilde", '~', false },
   { "hyphen", '-', false }, { "dash", '-', false },     { "minus", '-', false },
   { "ndash", '-', false },  { "mdash", '-', false },    { "horbar", '-', false },
   { "boxh", '-', false },   { "laquo", '[', false },    { "lsaquo", '[', false },
   { "lang", '[', false },   { "raquo", ']', false },    { "rsaquo", ']', false },
   { "rang", ']', false },   { "excl", '!', false },     { "apos", '\'', false },
   { "Tab", '\t', false },   { "NewLine", '\n', false },
};

/* What HTML reads "&#128;" through "&#159;" as: Windows-1252, not the C1
 * controls those numbers name (0: undefined there). */
static const unsigned short k_cp1252[32] = {
   0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
   0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
   0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

/* A code point as UTF-8 into @p out (the shadow's room for it is the escape's
 * length, never less: see shadow_escape); its length. */
static size_t utf8_put(unsigned cp, char *out) {
   if (cp < 0x80) {
      out[0] = (char)cp;
      return 1;
   }
   if (cp < 0x800) {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      return 2;
   }
   if (cp < 0x10000) {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      return 3;
   }
   out[0] = (char)(0xF0 | (cp >> 18));
   out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
   out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
   out[3] = (char)(0x80 | (cp & 0x3F));
   return 4;
}

/* @p n hex digits at @p p: their value, or -1 when any isn't one. */
static long hex_n(const unsigned char *p, int n) {
   long v = 0;
   for (int i = 0; i < n; i++) {
      if (!isxdigit(p[i])) {
         return -1;
      }
      v = v * 16 + (isdigit(p[i]) ? p[i] - '0' : (tolower(p[i]) - 'a' + 10));
   }
   return v;
}

/* A numeric character reference at @p p ("&#N;", "&#xN;", the ';' optional
 * as HTML reads it; leading zeros any number): its length and code point, or
 * 0. */
static size_t numeric_ref(const unsigned char *p, unsigned *cp) {
   const bool hex = p[2] == 'x' || p[2] == 'X';
   size_t i = hex ? 3 : 2;
   while (p[i] == '0' && (hex ? isxdigit(p[i + 1]) : isdigit(p[i + 1]))) {
      i++;
   }
   unsigned long v = 0;
   size_t digits = 0;
   for (; hex ? isxdigit(p[i]) : isdigit(p[i]); i++, digits++) {
      if (digits == 7) {
         return 0; /* past any code point */
      }
      v = v * (hex ? 16 : 10) + (isdigit(p[i]) ? p[i] - '0' : (tolower(p[i]) - 'a' + 10));
   }
   if (digits == 0 || v > 0x10FFFF) {
      return 0;
   }
   if (p[i] == ';') {
      i++;
   }
   if (v >= 0x80 && v <= 0x9F) {
      v = k_cp1252[v - 0x80];
      if (v == 0) {
         return 0;
      }
   }
   *cp = (unsigned)v;
   return i;
}

/* A JSON, C or HTML character escape at @p p ("\\u005b", "\\U0000005b", a
 * surrogate pair, "\\n", "\\/", "&#91;", "&#x5b", "&lsqb;"): its length, with
 * what it reads as in the shadow written to @p out (@p out_len bytes: 0 for an
 * invisible character; the UTF-8 of a letter the shadow folds, so an escaped
 * lookalike reads as one); 0 when there is no such escape.  A reader, and a
 * JSON parser, decodes these, so an imitation spelled with them is one.  Every
 * escape is at least as long as what it writes, so the shadow never outgrows
 * the text.  "%XX" is not one: URL encoding is everywhere in text and nobody
 * reads it as the character. */
static size_t shadow_escape(const unsigned char *p, char out[4], size_t *out_len) {
   unsigned cp = 0;
   size_t n = 0;
   *out_len = 0;
   if (p[0] == '\\') {
      static const char k_short[] = "\\/\"nrtfb";
      static const char k_means[] = "\\/\"\n\r\t\f";
      const char *hit = p[1] ? strchr(k_short, p[1]) : NULL;
      if (hit) {
         const size_t k = (size_t)(hit - k_short);
         if (k < sizeof(k_means) - 1) {
            out[0] = k_means[k];
            *out_len = 1;
         }
         return 2; /* "\\b" (backspace) reads as nothing */
      }
      if (p[1] == 'U' && hex_n(p + 2, 8) >= 0) {
         cp = (unsigned)hex_n(p + 2, 8);
         n = 10;
      } else if ((p[1] == 'u' || p[1] == 'U') && hex_n(p + 2, 4) >= 0) {
         cp = (unsigned)hex_n(p + 2, 4);
         n = 6;
         if (cp >= 0xD800 && cp <= 0xDBFF && p[6] == '\\' && (p[7] == 'u' || p[7] == 'U')) {
            const long lo = hex_n(p + 8, 4);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
               cp = 0x10000 + ((cp - 0xD800) << 10) + (unsigned)(lo - 0xDC00);
               n = 12;
            }
         }
      } else {
         return 0;
      }
   } else if (p[0] == '&' && p[1] == '#') {
      n = numeric_ref(p, &cp);
      if (n == 0) {
         return 0;
      }
   } else if (p[0] == '&') {
      for (size_t k = 0; k < sizeof(k_named_refs) / sizeof(k_named_refs[0]); k++) {
         const char *name = k_named_refs[k].name;
         const bool legacy = k_named_refs[k].legacy;
         const size_t len = strlen(name);
         const bool same = legacy ? strncasecmp((const char *)p + 1, name, len) == 0
                                  : strncmp((const char *)p + 1, name, len) == 0;
         if (!same) {
            continue;
         }
         if (p[1 + len] == ';') {
            out[0] = k_named_refs[k].c;
            *out_len = 1;
            return len + 2;
         }
         if (legacy) {
            out[0] = k_named_refs[k].c;
            *out_len = 1;
            return len + 1;
         }
      }
      return 0;
   } else {
      return 0;
   }
   if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      return 0;
   }
   if (cp < 0x80) {
      if (cp < 0x20 && cp != '\n' && cp != '\r' && cp != '\t' && cp != '\v' && cp != '\f') {
         return 0;
      }
      out[0] = (char)cp;
      *out_len = 1;
      return n;
   }
   const int r = shadowed(cp);
   if (r > 0) {
      out[0] = (char)r;
      *out_len = 1;
   } else if (r < 0) {
      *out_len = utf8_put(cp, out);
   }
   return n;
}

/* @p text's shadow (@p line: line breaks shown as spaces).  False on
 * allocation failure (or a text past 4 GiB). */
static bool shadow_make(const char *text, bool line, shadow_t *sh) {
   memset(sh, 0, sizeof(*sh));
   const size_t len = strlen(text);
   if (len >= UINT32_MAX) {
      return false;
   }
   sh->s = malloc(len + 1);
   sh->src = malloc((len + 1) * sizeof(*sh->src));
   sh->sep_to = malloc((len + 1) * sizeof(*sh->sep_to));
   sh->rule_to = malloc((len + 1) * sizeof(*sh->rule_to));
   if (!sh->s || !sh->src || !sh->sep_to || !sh->rule_to) {
      shadow_free(sh);
      return false;
   }
   size_t off = 0;
   const unsigned char *p = (const unsigned char *)text;
   while (*p) {
      const uint32_t at = (uint32_t)(p - (const unsigned char *)text);
      char decoded[4];
      size_t decoded_len = 0;
      const size_t en = shadow_escape(p, decoded, &decoded_len);
      if (en > 0) {
         /* The whole escape is where what it stands for came from. */
         for (size_t k = 0; k < decoded_len; k++) {
            sh->s[off] = decoded[k];
            sh->src[off++] = at;
         }
         p += en;
         continue;
      }
      unsigned cp = *p;
      size_t n = 1;
      int r = -1;
      if (*p >= 0x80 || *p == '\t') {
         n = utf8_at(p, &cp);
         r = shadowed(cp);
      }
      if (r > 0) {
         sh->s[off] = (char)r;
         sh->src[off++] = at;
      } else if (r < 0) {
         for (size_t i = 0; i < n; i++) {
            sh->s[off] = (char)p[i];
            sh->src[off++] = at;
         }
      }
      p += n;
   }
   sh->s[off] = '\0';
   sh->src[off] = (uint32_t)len;
   sh->len = off;
   for (size_t i = 0; line && i < off; i++) {
      if (sh->s[i] == '\n' || sh->s[i] == '\r' || sh->s[i] == '\v' || sh->s[i] == '\f') {
         sh->s[i] = ' ';
      }
   }
   sh->sep_to[off] = (uint32_t)off;
   sh->rule_to[off] = (uint32_t)off;
   for (size_t i = off; i-- > 0;) {
      sh->sep_to[i] = is_sep(sh->s[i]) ? sh->sep_to[i + 1] : (uint32_t)i;
      sh->rule_to[i] = is_rule(sh->s[i]) ? sh->rule_to[i + 1] : (uint32_t)i;
   }
   return true;
}

/* Separators at shadow position @p i: where they end (@p need: at least one;
 * (size_t)-1 when there is none). */
static size_t seps_at(const shadow_t *sh, size_t i, bool need) {
   const size_t e = sh->sep_to[i];
   return (need && e == i) ? (size_t)-1 : e;
}

/* @p word (lowercase ASCII) at shadow position @p i, each letter in any
 * spelling that reads as it: where it ends, or 0. */
static size_t word_at(const shadow_t *sh, size_t i, const char *word) {
   for (const char *w = word; *w; w++) {
      unsigned cp;
      const size_t n = utf8_at((const unsigned char *)sh->s + i, &cp);
      if (cp == 0 || folded(cp) != *w) {
         return 0;
      }
      i += n;
   }
   return i;
}

/* The words @p words (NULL-terminated) at @p i, separators between each:
 * where they end, or 0. */
static size_t words_at(const shadow_t *sh, size_t i, const char *const *words) {
   for (size_t k = 0; words[k]; k++) {
      if (k > 0) {
         const size_t w = seps_at(sh, i, true);
         if (w == (size_t)-1) {
            return 0;
         }
         i = w;
      }
      i = word_at(sh, i, words[k]);
      if (i == 0) {
         return 0;
      }
   }
   return i;
}

/* A hyphen in any spelling at @p p ("-", "&#45;", "&#045;", "&#x2d;",
 * "&#x002d;", "&hyphen;", "&dash;", "&minus;", "%2d", "-"): its length,
 * or 0. */
static size_t hyphen_at(const char *p) {
   static const char *const k_hyphens[] = { "-",        "&#45;",  "&#045;",  "&#x2d;", "&#x002d;",
                                            "&hyphen;", "&dash;", "&minus;", "%2d",    "\\u002d" };
   for (size_t i = 0; i < sizeof(k_hyphens) / sizeof(k_hyphens[0]); i++) {
      const size_t n = strlen(k_hyphens[i]);
      if (strncasecmp(p, k_hyphens[i], n) == 0) {
         return n;
      }
   }
   return 0;
}

/* A tag-shaped string at shadow position @p i ("dawn-ctx-" and 8 hex digits,
 * its letters and hyphens in any spelling): where it ends, or 0. */
static size_t tag_at(const shadow_t *sh, size_t i) {
   size_t e = word_at(sh, i, "dawn");
   size_t h = e ? hyphen_at(sh->s + e) : 0;
   if (h == 0) {
      return 0;
   }
   e = word_at(sh, e + h, "ctx");
   h = e ? hyphen_at(sh->s + e) : 0;
   if (h == 0) {
      return 0;
   }
   e += h;
   for (int k = 0; k < 8; k++) {
      unsigned cp;
      const size_t n = utf8_at((const unsigned char *)sh->s + e, &cp);
      const char d = cp ? folded(cp) : 0;
      if (!((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f'))) {
         return 0;
      }
      e += n;
   }
   return e;
}

/* What a defused tag reads as: the shape kept, the digits withheld. */
#define TAG_DEFUSED "dawn_ctx_(withheld)"

/* An imitation of a DAWN marker at shadow position @p i: where it ends, with
 * what it becomes in @p out; 0 when there is none. */
static size_t imitation_at(const shadow_t *sh, size_t i, const char **out) {
   static const char *const k_end_turn[] = { "end", "turn", "context", NULL };
   static const char *const k_end_memory[] = { "end", "user", "memory", NULL };
   static const char *const k_turn[] = { "turn", "context", NULL };
   static const char *const k_memory[] = { "user", "memory", NULL };
   static const char *const k_end_summary[] = { "end", "conversation", "summary", NULL };
   static const char *const k_summary[] = { "conversation", "summary", NULL };
   static const char *const k_note[] = { "operator", "note", NULL };
   static const char *const k_updated[] = { "updated", "instructions", NULL };
   const char c = sh->s[i];
   if (is_rule(c)) {
      const size_t r = sh->rule_to[i];
      if (r - i < 2) {
         return 0;
      }
      const size_t w = seps_at(sh, r, false);
      static const struct {
         const char *const *words;
         const char *defused;
      } k_frames[] = {
         { k_end_turn, "- - END TURN CONTEXT (quoted)" },
         { k_end_memory, "- - END USER MEMORY (quoted)" },
         { k_turn, "- - TURN CONTEXT (quoted)" },
         { k_memory, "- - USER MEMORY (quoted)" },
         { k_end_summary, "- - END CONVERSATION SUMMARY (quoted)" },
         { k_summary, "- - CONVERSATION SUMMARY (quoted)" },
      };
      for (size_t k = 0; k < sizeof(k_frames) / sizeof(k_frames[0]); k++) {
         const size_t e = words_at(sh, w, k_frames[k].words);
         if (e > 0) {
            *out = k_frames[k].defused;
            return e;
         }
      }
      return 0;
   }
   if (c == '[' || c == '(' || c == '{' || c == '<' || c == '|') {
      const size_t e = words_at(sh, seps_at(sh, i + 1, false), k_note);
      if (e > 0) {
         *out = "(quoted Operator note";
      }
      return e;
   }
   if ((unsigned char)c < 0x80 && c != 'u' && c != 'U' && c != 'd' && c != 'D') {
      return 0;
   }
   size_t e = words_at(sh, i, k_updated);
   if (e > 0) {
      *out = "Updated (quoted) instructions"; /* split: it can't match again */
      return e;
   }
   e = tag_at(sh, i);
   if (e > 0) {
      *out = TAG_DEFUSED;
   }
   return e;
}

/* @p text rewritten where its shadow @p sh holds imitations into @p out
 * (NULL: only measure).  Returns the length written. */
static size_t defuse(const char *text, const shadow_t *sh, char *out) {
   size_t off = 0;
   size_t copied = 0; /* original bytes before this are written */
   for (size_t i = 0; i < sh->len;) {
      const char *defused = NULL;
      const size_t e = imitation_at(sh, i, &defused);
      if (e > 0) {
         const size_t from = sh->src[i];
         const size_t to = sh->src[e];
         const size_t d = strlen(defused);
         if (out) {
            memcpy(out + off, text + copied, from - copied);
            memcpy(out + off + (from - copied), defused, d);
         }
         off += (from - copied) + d;
         copied = to;
         i = e;
         continue;
      }
      /* A rule run that opens nothing is passed whole: every start inside it
       * sees the same words after it. */
      if (is_rule(sh->s[i]) && sh->rule_to[i] > i + 1) {
         i = sh->rule_to[i];
      } else {
         unsigned cp;
         i += utf8_at((const unsigned char *)sh->s + i, &cp);
      }
   }
   const size_t rest = strlen(text + copied);
   if (out) {
      memcpy(out + off, text + copied, rest);
   }
   return off + rest;
}

static char *neutralize(const char *text, bool line) {
   if (!text) {
      return NULL;
   }
   shadow_t sh;
   if (!shadow_make(text, line, &sh)) {
      return NULL;
   }
   char *out = malloc(defuse(text, &sh, NULL) + 1);
   if (out) {
      out[defuse(text, &sh, out)] = '\0';
   }
   shadow_free(&sh);
   if (out && line) {
      /* One line: the original's own breaks go too (U+2028, U+2029 and
       * U+0085 among them, each a space now). */
      size_t w = 0;
      for (size_t r = 0; out[r];) {
         const unsigned char *u = (const unsigned char *)out + r;
         if (*u == '\n' || *u == '\r' || *u == '\v' || *u == '\f') {
            out[w++] = ' ';
            r++;
         } else if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0xA8 || u[2] == 0xA9)) {
            out[w++] = ' ';
            r += 3;
         } else if (u[0] == 0xC2 && u[1] == 0x85) {
            out[w++] = ' ';
            r += 2;
         } else {
            out[w++] = out[r++];
         }
      }
      out[w] = '\0';
   }
   return out;
}

char *llm_context_neutralize(const char *text) {
   return neutralize(text, false);
}

char *llm_context_neutralize_owned(char *text) {
   char *safe = neutralize(text, false);
   free(text);
   return safe;
}

char *llm_context_neutralize_line(const char *text) {
   return neutralize(text, true);
}

bool llm_context_tag_secret(const char *tag, char hex[9]) {
   if (!tag) {
      return false;
   }
   shadow_t sh;
   if (!shadow_make(tag, false, &sh)) {
      return false;
   }
   const size_t e = tag_at(&sh, 0);
   bool whole = e > 0 && e == sh.len;
   /* DAWN's own tags are ASCII: anything else isn't one. */
   for (int k = 0; whole && k < 8; k++) {
      whole = isxdigit((unsigned char)sh.s[e - 8 + (size_t)k]) != 0;
   }
   if (whole) {
      for (int k = 0; k < 8; k++) {
         hex[k] = (char)tolower((unsigned char)sh.s[e - 8 + (size_t)k]);
      }
      hex[8] = '\0';
   }
   shadow_free(&sh);
   return whole;
}

/* An escape at @p p ("%XX", "&#N;", "&#xN;", "\uXXXX"): its length, with
 * the code point it stands for in @p cp; 0 when there is none. */
static size_t escape_at(const char *p, unsigned *cp) {
   char *end = NULL;
   if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      const char pair[3] = { p[1], p[2], '\0' };
      *cp = (unsigned)strtoul(pair, NULL, 16);
      return 3;
   }
   if (p[0] == '\\' && (p[1] == 'u' || p[1] == 'U')) {
      for (int i = 2; i < 6; i++) {
         if (!isxdigit((unsigned char)p[i])) {
            return 0;
         }
      }
      const char quad[5] = { p[2], p[3], p[4], p[5], '\0' };
      *cp = (unsigned)strtoul(quad, NULL, 16);
      return 6;
   }
   if (p[0] == '&' && p[1] == '#') {
      const bool hex = p[2] == 'x' || p[2] == 'X';
      const char *digits = p + (hex ? 3 : 2);
      if (!(hex ? isxdigit((unsigned char)*digits) : isdigit((unsigned char)*digits)) ||
          (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))) {
         return 0; /* strtoul would take a "0x" of its own */
      }
      const unsigned long v = strtoul(digits, &end, hex ? 16 : 10);
      if (end && *end == ';' && end - digits <= 7 && v <= 0x10FFFF) {
         *cp = (unsigned)v;
         return (size_t)(end - p) + 1;
      }
   }
   return 0;
}

/* The letters and digits of @p text (each as the ASCII it reads as, escapes
 * decoded) with the original span each came from.  Returns how many;
 * -1 on allocation failure. */
static long alnum_view(const char *text,
                       bool decode,
                       char **chars,
                       uint32_t **from,
                       uint32_t **to) {
   const size_t len = strlen(text);
   *chars = malloc(len + 1);
   *from = malloc((len + 1) * sizeof(**from));
   *to = malloc((len + 1) * sizeof(**to));
   if (!*chars || !*from || !*to || len >= UINT32_MAX) {
      free(*chars);
      free(*from);
      free(*to);
      return -1;
   }
   long n = 0;
   for (const char *p = text; *p;) {
      unsigned cp;
      size_t k = utf8_at((const unsigned char *)p, &cp);
      /* Decoding, an escape reads as what it stands for when that is a
       * letter or digit (else its own characters are read). */
      unsigned esc = 0;
      const size_t ek = decode ? escape_at(p, &esc) : 0;
      if (ek > 0 && folded(esc)) {
         cp = esc;
         k = ek;
      }
      const char c = folded(cp);
      if (c) {
         (*chars)[n] = c;
         (*from)[n] = (uint32_t)(p - text);
         (*to)[n] = (uint32_t)(p - text + k);
         n++;
      }
      p += k;
   }
   return n;
}

/* The copies of @p hex in @p text: each character of one marks its original
 * span in @p covered (bytes) and its start in @p starts.  Read twice, with
 * escapes decoded and as written, so neither reading hides one ("%4a" is a
 * "J" decoded and "4a" as written).  Returns how many copies; -1 on
 * allocation failure. */
static long find_secret(const char *text, const char *hex, char *covered, char *starts) {
   long found = 0;
   for (int view = 0; view < 2; view++) {
      char *chars;
      uint32_t *from;
      uint32_t *to;
      const long n = alnum_view(text, view == 0, &chars, &from, &to);
      if (n < 0) {
         return -1;
      }
      for (long i = 0; i + 8 <= n;) {
         if (memcmp(chars + i, hex, 8) != 0) {
            i++;
            continue;
         }
         found++;
         for (long k = i; covered && k < i + 8; k++) {
            memset(covered + from[k], 1, to[k] - from[k]);
            starts[from[k]] = 1;
         }
         i += 8;
      }
      free(chars);
      free(from);
      free(to);
   }
   return found;
}

char *llm_context_mask_tag(char *text, const char *tag) {
   char hex[9];
   if (!text || !llm_context_tag_secret(tag, hex) || !llm_context_carries_secret(text, hex)) {
      return text;
   }
   char *masked = llm_context_mask_secret(text, hex);
   free(text);
   return masked;
}

bool llm_context_carries_secret(const char *text, const char *hex) {
   if (!text || !hex || strlen(hex) != 8) {
      return false;
   }
   return find_secret(text, hex, NULL, NULL) != 0; /* -1: can't tell, fail closed */
}

char *llm_context_mask_secret(const char *text, const char *hex) {
   if (!text) {
      return NULL;
   }
   if (!hex || strlen(hex) != 8) {
      return strdup(text);
   }
   /* Each character of a copy of the secret becomes "x"; the rest is kept. */
   const size_t len = strlen(text);
   char *covered = calloc(len + 1, 1);
   char *starts = calloc(len + 1, 1);
   char *out = malloc(len + 1);
   if (!covered || !starts || !out || find_secret(text, hex, covered, starts) < 0) {
      free(covered);
      free(starts);
      free(out);
      return NULL;
   }
   size_t off = 0;
   for (size_t i = 0; i < len; i++) {
      if (!covered[i]) {
         out[off++] = text[i];
      } else if (starts[i]) {
         out[off++] = 'x';
      }
   }
   out[off] = '\0';
   free(covered);
   free(starts);
   return out;
}

/* What a withdrawn item or block says in place of its text. */
#define WITHDRAWN_ITEM "(withdrawn: the user forgot or deleted this)"
#define WITHDRAWN_BODY                                                              \
   "(withdrawn: this changed or was forgotten since; the current one comes with a " \
   "later turn)"

/* The handle a turn-context item line names ("[M7 source] ..."), or 0. */
static int line_handle(const char *line, size_t len) {
   if (len < 4 || line[0] != '[' || line[1] != 'M') {
      return 0;
   }
   int h = 0;
   size_t i = 2;
   while (i < len && line[i] >= '0' && line[i] <= '9' && h < 100000000) {
      h = h * 10 + (line[i] - '0');
      i++;
   }
   return (i > 2 && i < len && line[i] == ' ') ? h : 0;
}

int llm_context_withdraw_items(const char *text, const int *handles, int count, char **out) {
   if (!out) {
      return 1;
   }
   *out = NULL;
   if (!text || !handles || count <= 0 || !strstr(text, "[M")) {
      return 0;
   }
   size_t lines = 1;
   for (const char *p = text; *p; p++) {
      lines += *p == '\n';
   }
   /* Worst case each line becomes its prefix plus the note. */
   char *buf = malloc(strlen(text) + lines * (sizeof(WITHDRAWN_ITEM) + 16) + 1);
   if (!buf) {
      return 1;
   }
   bool changed = false;
   size_t off = 0;
   const char *line = text;
   while (*line) {
      const char *nl = strchr(line, '\n');
      const size_t len = nl ? (size_t)(nl - line) : strlen(line);
      const int h = line_handle(line, len);
      bool hit = false;
      for (int i = 0; h > 0 && !hit && i < count; i++) {
         hit = handles[i] == h;
      }
      size_t plen = 0;
      if (hit) {
         plen = (size_t)(strchr(line, ' ') - line) + 1; /* "[M7 " */
         hit = !(len == plen + sizeof(WITHDRAWN_ITEM) - 1 &&
                 strncmp(line + plen, WITHDRAWN_ITEM, len - plen) == 0);
      }
      if (hit) {
         memcpy(buf + off, line, plen);
         off += plen;
         memcpy(buf + off, WITHDRAWN_ITEM, sizeof(WITHDRAWN_ITEM) - 1);
         off += sizeof(WITHDRAWN_ITEM) - 1;
         changed = true;
      } else {
         memcpy(buf + off, line, len);
         off += len;
      }
      if (!nl) {
         break;
      }
      buf[off++] = '\n';
      line = nl + 1;
   }
   buf[off] = '\0';
   if (changed) {
      *out = buf;
   } else {
      free(buf);
   }
   return 0;
}

char *llm_context_withdraw_body(const char *text, bool *changed) {
   if (changed) {
      *changed = false;
   }
   if (!text) {
      return NULL;
   }
   /* The first line (the open) and the last non-empty line (the close). */
   const char *first_end = strchr(text, '\n');
   size_t end = strlen(text);
   while (end > 0 && text[end - 1] == '\n') {
      end--;
   }
   const char *last = text + end;
   while (last > text && last[-1] != '\n') {
      last--;
   }
   if (!first_end || last <= first_end) {
      return strdup(text); /* not a framed block */
   }
   const size_t open_len = (size_t)(first_end - text) + 1;
   const size_t close_len = strlen(last);
   const size_t len = open_len + sizeof(WITHDRAWN_BODY) + 1 + close_len + 1;
   char *out = malloc(len);
   if (!out) {
      return NULL;
   }
   snprintf(out, len, "%.*s%s\n%s", (int)open_len, text, WITHDRAWN_BODY, last);
   if (changed) {
      *changed = strcmp(out, text) != 0;
   }
   return out;
}
