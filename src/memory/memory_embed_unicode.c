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
 * Unicode character classes and folding for the ONNX embedding tokenizer.
 */

#include "memory/memory_embed_unicode.h"

typedef struct {
   uint32_t lo;
   uint32_t hi;
} embed_cp_range_t;

typedef struct {
   uint32_t cp;
   uint8_t n;
   uint32_t out[EMBED_FOLD_MAX];
} embed_cp_map_t;

#include "memory_embed_unicode_tables.inc"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool in_ranges(const embed_cp_range_t *r, size_t n, uint32_t cp) {
   size_t lo = 0, hi = n;
   while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (cp < r[mid].lo) {
         hi = mid;
      } else if (cp > r[mid].hi) {
         lo = mid + 1;
      } else {
         return true;
      }
   }
   return false;
}

bool embed_cp_is_punct(uint32_t cp) {
   return in_ranges(k_punct, COUNT(k_punct), cp);
}

bool embed_cp_is_control(uint32_t cp) {
   return in_ranges(k_control, COUNT(k_control), cp);
}

bool embed_cp_is_space(uint32_t cp) {
   return in_ranges(k_space, COUNT(k_space), cp);
}

/* The reference tokenizer's CJK Unified Ideograph blocks. */
bool embed_cp_is_cjk(uint32_t cp) {
   return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
          (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) ||
          (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
          (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F);
}

/* Hangul syllable canonical decomposition (Unicode 3.12). */
#define HANGUL_S_BASE 0xAC00u
#define HANGUL_L_BASE 0x1100u
#define HANGUL_V_BASE 0x1161u
#define HANGUL_T_BASE 0x11A7u
#define HANGUL_V_COUNT 21u
#define HANGUL_T_COUNT 28u
#define HANGUL_S_COUNT 11172u

int embed_cp_fold(uint32_t cp, uint32_t out[EMBED_FOLD_MAX]) {
   if (cp >= HANGUL_S_BASE && cp < HANGUL_S_BASE + HANGUL_S_COUNT) {
      uint32_t s = cp - HANGUL_S_BASE;
      out[0] = HANGUL_L_BASE + s / (HANGUL_V_COUNT * HANGUL_T_COUNT);
      out[1] = HANGUL_V_BASE + (s % (HANGUL_V_COUNT * HANGUL_T_COUNT)) / HANGUL_T_COUNT;
      uint32_t t = s % HANGUL_T_COUNT;
      if (t == 0) {
         return 2;
      }
      out[2] = HANGUL_T_BASE + t;
      return 3;
   }
   if (in_ranges(k_mark, COUNT(k_mark), cp)) {
      return 0;
   }
   size_t lo = 0, hi = COUNT(k_fold);
   while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (cp < k_fold[mid].cp) {
         hi = mid;
      } else if (cp > k_fold[mid].cp) {
         lo = mid + 1;
      } else {
         for (int i = 0; i < k_fold[mid].n; i++) {
            out[i] = k_fold[mid].out[i];
         }
         return k_fold[mid].n;
      }
   }
   out[0] = cp;
   return 1;
}

size_t embed_utf8_decode(const char *s, size_t len, uint32_t *cp_out) {
   if (len == 0) {
      return 0;
   }
   const unsigned char *u = (const unsigned char *)s;
   uint32_t cp;
   size_t need;
   if (u[0] < 0x80) {
      *cp_out = u[0];
      return 1;
   } else if ((u[0] & 0xE0) == 0xC0) {
      cp = u[0] & 0x1F;
      need = 1;
   } else if ((u[0] & 0xF0) == 0xE0) {
      cp = u[0] & 0x0F;
      need = 2;
   } else if ((u[0] & 0xF8) == 0xF0) {
      cp = u[0] & 0x07;
      need = 3;
   } else {
      *cp_out = 0xFFFD;
      return 1;
   }
   if (need >= len) {
      *cp_out = 0xFFFD;
      return 1;
   }
   for (size_t i = 1; i <= need; i++) {
      if ((u[i] & 0xC0) != 0x80) {
         *cp_out = 0xFFFD;
         return 1;
      }
      cp = (cp << 6) | (u[i] & 0x3F);
   }
   /* Reject overlong forms, surrogates and out-of-range values. */
   if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
       (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      *cp_out = 0xFFFD;
      return 1;
   }
   *cp_out = cp;
   return need + 1;
}

int embed_utf8_encode(uint32_t cp, char out[4]) {
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
