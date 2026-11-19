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
 * Memory Similarity Implementation
 *
 * Text normalization, FNV-1a hashing, and Jaccard similarity for
 * fact deduplication.
 */

#include "memory/memory_similarity.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * English Stopwords
 *
 * Common words that don't contribute to semantic meaning.
 * Sorted alphabetically for binary search.
 * ============================================================================= */

static const char *STOPWORDS[] = {
   "a",    "about", "all",   "also",  "am",   "an",   "and",   "any",   "are",   "as",
   "at",   "be",    "been",  "but",   "by",   "can",  "could", "did",   "do",    "does",
   "for",  "from",  "had",   "has",   "have", "he",   "her",   "him",   "his",   "how",
   "i",    "if",    "in",    "into",  "is",   "it",   "its",   "just",  "like",  "me",
   "more", "my",    "no",    "not",   "now",  "of",   "on",    "or",    "our",   "out",
   "over", "said",  "she",   "so",    "some", "than", "that",  "the",   "their", "them",
   "then", "there", "these", "they",  "this", "to",   "up",    "us",    "was",   "we",
   "were", "what",  "when",  "which", "who",  "will", "with",  "would", "you",   "your"
};
static const int STOPWORD_COUNT = sizeof(STOPWORDS) / sizeof(STOPWORDS[0]);

/* =============================================================================
 * Helper: Check if word is a stopword (binary search)
 * ============================================================================= */

static bool is_stopword(const char *word) {
   int left = 0;
   int right = STOPWORD_COUNT - 1;

   while (left <= right) {
      int mid = (left + right) / 2;
      int cmp = strcmp(word, STOPWORDS[mid]);
      if (cmp == 0) {
         return true;
      } else if (cmp < 0) {
         right = mid - 1;
      } else {
         left = mid + 1;
      }
   }
   return false;
}

/* =============================================================================
 * Helper: qsort comparator for strings
 * ============================================================================= */

static int compare_strings(const void *a, const void *b) {
   return strcmp(*(const char **)a, *(const char **)b);
}

/* =============================================================================
 * Helper: Extract and sort words from text
 *
 * Returns array of word pointers (into working_buf) and count.
 * Caller provides working_buf to hold lowercase copy of text.
 * ============================================================================= */

static int extract_words(const char *text,
                         char *working_buf,
                         size_t buf_size,
                         char **words,
                         int max_words) {
   if (!text || !working_buf || !words || max_words <= 0) {
      return 0;
   }

   /* Copy text to working buffer for tokenization */
   size_t len = strlen(text);
   if (len >= buf_size) {
      len = buf_size - 1;
   }
   memcpy(working_buf, text, len);
   working_buf[len] = '\0';

   /* Convert to lowercase */
   for (size_t i = 0; i < len; i++) {
      working_buf[i] = (char)tolower((unsigned char)working_buf[i]);
   }

   /* Tokenize and filter stopwords */
   int word_count = 0;
   char *saveptr = NULL;
   char *token = strtok_r(working_buf, " \t\n\r.,;:!?\"'()-", &saveptr);

   while (token && word_count < max_words) {
      /* Skip short words and stopwords */
      if (strlen(token) > 1 && !is_stopword(token)) {
         words[word_count++] = token;
      }
      token = strtok_r(NULL, " \t\n\r.,;:!?\"'()-", &saveptr);
   }

   /* Sort words alphabetically */
   if (word_count > 1) {
      qsort(words, word_count, sizeof(char *), compare_strings);
   }

   return word_count;
}

/* =============================================================================
 * Text Normalization
 * ============================================================================= */

int memory_normalize_text(const char *text, char *out_normalized, size_t max_len, int *len_out) {
   if (len_out)
      *len_out = 0;

   if (!text || !out_normalized || max_len < 2) {
      return 1;
   }

   /* Allocate working buffer */
   size_t text_len = strlen(text);
   char *working_buf = malloc(text_len + 1);
   if (!working_buf) {
      return 1;
   }

   char *words[MEMORY_MAX_WORDS];
   int word_count = extract_words(text, working_buf, text_len + 1, words, MEMORY_MAX_WORDS);

   /* Build normalized output */
   size_t out_pos = 0;
   for (int i = 0; i < word_count && out_pos < max_len - 1; i++) {
      if (i > 0 && out_pos < max_len - 1) {
         out_normalized[out_pos++] = ' ';
      }

      size_t word_len = strlen(words[i]);
      if (out_pos + word_len >= max_len) {
         word_len = max_len - out_pos - 1;
      }
      memcpy(out_normalized + out_pos, words[i], word_len);
      out_pos += word_len;
   }
   out_normalized[out_pos] = '\0';

   free(working_buf);
   if (len_out)
      *len_out = (int)out_pos;
   return 0;
}

/* =============================================================================
 * FNV-1a Hashing
 * ============================================================================= */

uint32_t memory_hash_text(const char *normalized_text) {
   if (!normalized_text) {
      return 0;
   }

   /* FNV-1a 32-bit parameters */
   const uint32_t FNV_OFFSET_BASIS = 2166136261u;
   const uint32_t FNV_PRIME = 16777619u;

   uint32_t hash = FNV_OFFSET_BASIS;
   const unsigned char *ptr = (const unsigned char *)normalized_text;

   while (*ptr) {
      hash ^= *ptr++;
      hash *= FNV_PRIME;
   }

   return hash;
}

char *memory_text_fold(const char *text) {
   if (!text) {
      return NULL;
   }
   const size_t len = strlen(text);
   char *out = malloc(len + 1);
   if (!out) {
      return NULL;
   }
   size_t n = 0;
   bool space = false;
   for (const char *c = text; *c; c++) {
      if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
         space = n > 0;
         continue;
      }
      if (space) {
         out[n++] = ' ';
         space = false;
      }
      out[n++] = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
   }
   while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) {
      n--;
   }
   out[n] = '\0';
   return out;
}

uint32_t memory_normalize_and_hash(const char *text) {
   if (!text) {
      return 0;
   }

   char normalized[512];
   if (memory_normalize_text(text, normalized, sizeof(normalized), NULL) != 0) {
      return 0;
   }

   return memory_hash_text(normalized);
}

/* =============================================================================
 * Jaccard Similarity
 * ============================================================================= */

float memory_jaccard_similarity(const char *text_a, const char *text_b) {
   if (!text_a || !text_b) {
      return 0.0f;
   }

   /* Extract words from both texts */
   size_t len_a = strlen(text_a);
   size_t len_b = strlen(text_b);

   char *buf_a = malloc(len_a + 1);
   char *buf_b = malloc(len_b + 1);
   if (!buf_a || !buf_b) {
      free(buf_a);
      free(buf_b);
      return 0.0f;
   }

   char *words_a[MEMORY_MAX_WORDS];
   char *words_b[MEMORY_MAX_WORDS];

   int count_a = extract_words(text_a, buf_a, len_a + 1, words_a, MEMORY_MAX_WORDS);
   int count_b = extract_words(text_b, buf_b, len_b + 1, words_b, MEMORY_MAX_WORDS);

   if (count_a == 0 && count_b == 0) {
      free(buf_a);
      free(buf_b);
      return 1.0f; /* Both empty = identical */
   }
   if (count_a == 0 || count_b == 0) {
      free(buf_a);
      free(buf_b);
      return 0.0f; /* One empty, one not */
   }

   /* Count intersection using merge of sorted arrays */
   int intersection = 0;
   int i = 0, j = 0;
   while (i < count_a && j < count_b) {
      int cmp = strcmp(words_a[i], words_b[j]);
      if (cmp == 0) {
         intersection++;
         i++;
         j++;
      } else if (cmp < 0) {
         i++;
      } else {
         j++;
      }
   }

   /* Union = |A| + |B| - |intersection| */
   int union_size = count_a + count_b - intersection;

   free(buf_a);
   free(buf_b);

   return (union_size > 0) ? (float)intersection / (float)union_size : 0.0f;
}

/* =============================================================================
 * Duplicate Detection
 * ============================================================================= */

bool memory_is_duplicate(const char *text_a, const char *text_b, float threshold) {
   if (!text_a || !text_b) {
      return false;
   }

   /* Fast path: check hash equality */
   uint32_t hash_a = memory_normalize_and_hash(text_a);
   uint32_t hash_b = memory_normalize_and_hash(text_b);

   if (hash_a == hash_b && hash_a != 0) {
      return true;
   }

   /* Slow path: Jaccard similarity */
   float similarity = memory_jaccard_similarity(text_a, text_b);
   return similarity >= threshold;
}
