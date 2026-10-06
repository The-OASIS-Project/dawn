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
 * Content words of a text, stemmed.  See memory_terms.h.
 */

#include "memory/memory_terms.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "memory/memory_stem.h"

/* Words joined per stemming pass: memory_stem_string() cuts its input at
 * MEMORY_FACT_STEMS_MAX (768) bytes, so a long text is stemmed in batches. */
#define MEMORY_TERMS_BATCH 640

/* Byte length of the separator at @p p: ASCII other than a letter or digit,
 * or a UTF-8 punctuation/space character; 0 when @p p starts a word character. */
static size_t separator_len(const unsigned char *p) {
   if (p[0] < 0x80) {
      return isalnum(p[0]) ? 0 : 1;
   }
   if (p[0] >= 0xC2 && p[0] <= 0xDF && p[1] >= 0x80 && p[1] <= 0xBF) {
      const unsigned cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
      /* Latin-1 punctuation and symbols (no-break space, «», ¿, ...), × and ÷,
       * but not the letters and digits in that block (ª µ º ¹ ² ³ ¼ ½ ¾). */
      const bool letter = cp == 0xAA || cp == 0xB2 || cp == 0xB3 || cp == 0xB5 || cp == 0xB9 ||
                          cp == 0xBA || (cp >= 0xBC && cp <= 0xBE);
      return ((cp <= 0xBF && !letter) || cp == 0xD7 || cp == 0xF7) ? 2 : 0;
   }
   if (p[0] >= 0xE0 && p[0] <= 0xEF && p[1] >= 0x80 && p[1] <= 0xBF && p[2] >= 0x80 &&
       p[2] <= 0xBF) {
      const unsigned cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
      /* General punctuation (spaces, dashes, quotes, ellipsis), supplemental
       * punctuation, CJK punctuation (not its letters and numerals, U+3005-3007
       * and U+3021-303C), and the byte-order mark. */
      const bool cjk_punct = (cp >= 0x3000 && cp <= 0x3004) || (cp >= 0x3008 && cp <= 0x3020) ||
                             (cp >= 0x303D && cp <= 0x303F);
      const bool punct = (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x2E00 && cp <= 0x2E7F) ||
                         cjk_punct || cp == 0xFEFF;
      return punct ? 3 : 0;
   }
   return 0; /* other letters, and malformed bytes, stay in the word */
}

const char *memory_terms_next_word(const char *p, const char **start, size_t *len) {
   size_t sep = 0;
   while (*p && (sep = separator_len((const unsigned char *)p)) > 0) {
      p += sep;
   }
   if (!*p) {
      return NULL;
   }
   *start = p;
   while (*p && separator_len((const unsigned char *)p) == 0) {
      p++;
   }
   *len = (size_t)(p - *start);
   return p;
}

/* The Latin-1 capitals À–Þ are UTF-8 C3 80–9E (× at C3 97 is not a letter). */
unsigned char memory_terms_fold_at(const char *w, size_t i) {
   const unsigned char c = (unsigned char)w[i];
   if (c < 0x80) {
      return (unsigned char)tolower(c);
   }
   if (i > 0 && (unsigned char)w[i - 1] == 0xC3 && c >= 0x80 && c <= 0x9E && c != 0x97) {
      return (unsigned char)(c + 0x20);
   }
   return c;
}

/* Function words that say nothing about what a text is about.  Kept short on
 * purpose: a word missing here costs little (a name still needs two words to
 * match), a content word added here would be ignored.  The codebase's other
 * lists serve other purposes (the dominant-token list drops words under four
 * letters, which would lose words like "tax", "vet" and "ai"). */
static const char *const STOPWORDS[] = {
   "a",    "about", "all",   "am",    "an",    "and",    "any",   "are",    "as",    "at",   "be",
   "been", "but",   "by",    "can",   "could", "did",    "do",    "does",   "for",   "from", "get",
   "had",  "has",   "have",  "he",    "her",   "him",    "his",   "how",    "i",     "if",   "in",
   "into", "is",    "it",    "its",   "just",  "know",   "let",   "me",     "my",    "no",   "not",
   "of",   "on",    "or",    "our",   "out",   "please", "she",   "should", "so",    "some", "tell",
   "than", "that",  "the",   "their", "them",  "then",   "there", "these",  "they",  "this", "to",
   "up",   "us",    "was",   "we",    "were",  "what",   "when",  "where",  "which", "who",  "why",
   "will", "with",  "would", "you",   "your",
};

static bool is_stopword(const char *word) {
   for (size_t i = 0; i < sizeof(STOPWORDS) / sizeof(STOPWORDS[0]); i++) {
      if (strcmp(STOPWORDS[i], word) == 0) {
         return true;
      }
   }
   return false;
}

/* Append @p text's words (case-folded; content words only when @p content) to
 * @p buf, space-separated, until @p max_words or @p buf is full.  *@p next is
 * where the text continues.  Returns words added. */
static int join_words(const char *text,
                      bool content,
                      int max_words,
                      char *buf,
                      size_t size,
                      const char **next) {
   size_t off = 0;
   int n = 0;
   const char *start = NULL;
   size_t len = 0;
   buf[0] = '\0';
   const char *p = text;
   *next = text;
   while (n < max_words) {
      const char *after = memory_terms_next_word(p, &start, &len);
      if (!after) {
         *next = p + strlen(p);
         break;
      }
      if (len >= 2 && len < MEMORY_TERM_LEN && off + len + 2 > size) {
         break; /* full: this word starts the next batch */
      }
      p = after;
      *next = p;
      if (len < 2 || len >= MEMORY_TERM_LEN) {
         continue; /* single letters, and runs too long to be a word */
      }
      char word[MEMORY_TERM_LEN];
      for (size_t i = 0; i < len; i++) {
         word[i] = (char)memory_terms_fold_at(start, i);
      }
      word[len] = '\0';
      if (content && is_stopword(word)) {
         continue;
      }
      if (off > 0) {
         buf[off++] = ' ';
      }
      memcpy(buf + off, word, len + 1);
      off += len;
      n++;
   }
   return n;
}


static int cmp_word(const void *a, const void *b) {
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Append the stems in @p stems (space-separated) that @p out doesn't hold
 * yet: a repeated word never takes room a later one needs.  word[0..sorted)
 * is sorted (earlier batches); this batch's words follow it unsorted. */
static void add_stems(memory_terms_t *out, char *stems, size_t *off, int sorted) {
   char *save = NULL;
   for (char *tok = strtok_r(stems, " ", &save); tok && out->count < MEMORY_TERMS_WORDS_MAX;
        tok = strtok_r(NULL, " ", &save)) {
      const size_t len = strlen(tok);
      if (len >= MEMORY_TERM_LEN || *off + len + 1 > sizeof(out->line)) {
         continue;
      }
      const char *key = tok;
      bool seen = sorted > 0 &&
                  bsearch(&key, out->word, (size_t)sorted, sizeof(out->word[0]), cmp_word) != NULL;
      for (int i = sorted; i < out->count && !seen; i++) {
         seen = strcmp(out->word[i], tok) == 0;
      }
      if (seen) {
         continue;
      }
      memcpy(out->line + *off, tok, len + 1);
      out->word[out->count++] = out->line + *off;
      *off += len + 1;
   }
}

void memory_terms_from_text(const char *text, bool content_only, memory_terms_t *out) {
   out->count = 0;
   if (!text) {
      return;
   }
   /* One hold of the stemmer's lock per batch of words, not per word. */
   size_t off = 0;
   const char *p = text;
   while (*p && out->count < MEMORY_TERMS_WORDS_MAX && off < sizeof(out->line)) {
      char words[MEMORY_TERMS_BATCH];
      char stems[MEMORY_TERMS_BATCH];
      const char *next = p;
      if (join_words(p, content_only, MEMORY_TERMS_WORDS_MAX, words, sizeof(words), &next) > 0 &&
          memory_stem_string(words, stems, sizeof(stems)) > 0) {
         add_stems(out, stems, &off, out->count);
         /* Sorted (and distinct: add_stems keeps it so), so a lookup, and
          * the next batch's duplicate check, is a binary search. */
         qsort(out->word, (size_t)out->count, sizeof(out->word[0]), cmp_word);
      }
      if (next == p) {
         break;
      }
      p = next;
   }
}

int memory_terms_stem_line(const char *text, bool content_only, char *out, size_t size) {
   if (!out || size == 0) {
      return 0;
   }
   out[0] = '\0';
   memory_terms_t *terms = malloc(sizeof(*terms));
   if (!terms) {
      return 0;
   }
   memory_terms_from_text(text, content_only, terms);
   size_t off = 0;
   int n = 0;
   for (int i = 0; i < terms->count; i++) {
      const size_t len = strlen(terms->word[i]);
      if (off + len + (off > 0 ? 1 : 0) + 1 > size) {
         break;
      }
      if (off > 0) {
         out[off++] = ' ';
      }
      memcpy(out + off, terms->word[i], len + 1);
      off += len;
      n++;
   }
   free(terms);
   return n;
}

bool memory_terms_has(const memory_terms_t *terms, const char *word) {
   return terms && word && terms->count > 0 &&
          bsearch(&word, terms->word, (size_t)terms->count, sizeof(terms->word[0]), cmp_word);
}

int memory_terms_count_in(const memory_terms_t *terms, const char *stem_line) {
   if (!terms || terms->count == 0 || !stem_line) {
      return 0;
   }
   int count = 0;
   char word[MEMORY_TERM_LEN];
   for (const char *p = stem_line; *p;) {
      const char *end = strchr(p, ' ');
      const size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len > 0 && len < sizeof(word)) {
         memcpy(word, p, len);
         word[len] = '\0';
         count += memory_terms_has(terms, word) ? 1 : 0;
      }
      if (!end) {
         break;
      }
      p = end + 1;
   }
   return count;
}

int memory_terms_word_count(const char *text) {
   int n = 0;
   const char *start = NULL;
   size_t len = 0;
   for (const char *p = text; p && (p = memory_terms_next_word(p, &start, &len)) != NULL;) {
      n += len < MEMORY_TERM_LEN ? 1 : 0; /* "X Corp" is two words */
   }
   return n;
}

/* Whether @p name appears in @p query as a phrase: its words, case-folded,
 * back to back and in order. */
static bool phrase_in(const char *query, const char *name) {
   const char *qs = NULL;
   size_t ql = 0;
   for (const char *q = query; q && (q = memory_terms_next_word(q, &qs, &ql)) != NULL;) {
      /* Try the name starting at this query word. */
      const char *qw = qs;
      size_t qwl = ql;
      const char *qnext = q;
      const char *ns = NULL;
      size_t nl = 0;
      bool all = true;
      for (const char *n = name; (n = memory_terms_next_word(n, &ns, &nl)) != NULL;) {
         if (!qw || nl != qwl) {
            all = false;
            break;
         }
         for (size_t k = 0; k < nl && all; k++) {
            all = memory_terms_fold_at(ns, k) == memory_terms_fold_at(qw, k);
         }
         if (!all) {
            break;
         }
         qnext = qnext ? memory_terms_next_word(qnext, &qw, &qwl) : NULL;
         if (!qnext) {
            qw = NULL;
         }
      }
      if (all) {
         return true;
      }
   }
   return false;
}

int memory_terms_names(const memory_terms_t *terms,
                       const char *query,
                       const char *name,
                       const char *name_stems,
                       int content_words,
                       int words) {
   if (!terms || !query || !name || !name_stems || content_words <= 0) {
      return 0;
   }
   const int matched = memory_terms_count_in(terms, name_stems);
   if (content_words >= 2) {
      return matched >= 2 ? matched : 0;
   }
   if (matched == 0) {
      return 0;
   }
   return (words <= 1 || phrase_in(query, name)) ? 1 : 0;
}

bool memory_terms_enough(int matched, int of) {
   if (of <= 0) {
      return false;
   }
   return matched >= (of < 2 ? of : 2);
}
