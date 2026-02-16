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
 * TF-IDF Extractive Summarization Implementation
 */

#include "tools/tfidf_summarizer.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Hash table size for word frequency counting (prime for better distribution) */
#define HASH_TABLE_SIZE 4099

/* Maximum word length to consider */
#define MAX_WORD_LEN 64

/* Position weight for first sentence (more important) */
#define FIRST_SENTENCE_WEIGHT 1.2f

/* Position weight for last sentence (more important) */
#define LAST_SENTENCE_WEIGHT 1.1f

/* Minimum words required for a valid sentence (filters fragments) */
#define MIN_WORDS_PER_SENTENCE 5

/* MMR lambda: balance between relevance (1.0) and diversity (0.0) */
#define MMR_LAMBDA 0.7f

/* =============================================================================
 * Noise Keywords - Sentences containing these are filtered (non-article content)
 * ============================================================================= */

static const char *NOISE_KEYWORDS[] = {
   /* Chart/visualization terms */
   "scatter plot", "bar chart", "pie chart", "line chart", "histogram", "x-axis", "y-axis",
   "axis label", "data point", "quadrant", "top-right", "top-left", "bottom-right", "bottom-left",
   "in red", "in blue", "in green", "in grey", "in gray", "shaded", "colored",
   /* Ad/UI/navigation elements */
   "ad break", "advertisement", "skip ad", "sponsored content", "click here", "read more",
   "see more", "watch live", "watch now", "sign up", "sign in", "log in", "subscribe now",
   "subscription", "newsletter", "follow us", "share on", "like us on", "download the app",
   /* Timestamps and live updates */
   "minute ago", "minutes ago", "hour ago", "hours ago", "second ago", "seconds ago", "day ago",
   "days ago", "just now", "live updates", "breaking news",
   /* Subscription/pricing patterns */
   "billed as", "free trial", "cancel anytime", "terms apply", "subscribed to", "subscribe to",
   "enter your details", "each week you'll receive", "stay up to date", "sign up for our",
   /* Error/system messages */
   "error with your request", "please try again",
   /* Author bios and footers */
   "works as a freelance", "works as a journalist", "help us improve", "improve our content",
   "about the author", "writer based in",
   /* Tag sections (typically footer metadata) */
   "tags:", NULL /* Sentinel */
};

/* =============================================================================
 * Stopwords - Common English words to ignore in TF-IDF calculation
 * ============================================================================= */

static const char *STOPWORDS[] = {
   "a",       "about",     "above",      "after",   "again",    "against",   "all",
   "am",      "an",        "and",        "any",     "are",      "aren't",    "as",
   "at",      "be",        "because",    "been",    "before",   "being",     "below",
   "between", "both",      "but",        "by",      "can",      "can't",     "cannot",
   "could",   "couldn't",  "did",        "didn't",  "do",       "does",      "doesn't",
   "doing",   "don't",     "down",       "during",  "each",     "few",       "for",
   "from",    "further",   "had",        "hadn't",  "has",      "hasn't",    "have",
   "haven't", "having",    "he",         "he'd",    "he'll",    "he's",      "her",
   "here",    "here's",    "hers",       "herself", "him",      "himself",   "his",
   "how",     "how's",     "i",          "i'd",     "i'll",     "i'm",       "i've",
   "if",      "in",        "into",       "is",      "isn't",    "it",        "it's",
   "its",     "itself",    "let's",      "me",      "more",     "most",      "mustn't",
   "my",      "myself",    "no",         "nor",     "not",      "of",        "off",
   "on",      "once",      "only",       "or",      "other",    "ought",     "our",
   "ours",    "ourselves", "out",        "over",    "own",      "same",      "shan't",
   "she",     "she'd",     "she'll",     "she's",   "should",   "shouldn't", "so",
   "some",    "such",      "than",       "that",    "that's",   "the",       "their",
   "theirs",  "them",      "themselves", "then",    "there",    "there's",   "these",
   "they",    "they'd",    "they'll",    "they're", "they've",  "this",      "those",
   "through", "to",        "too",        "under",   "until",    "up",        "very",
   "was",     "wasn't",    "we",         "we'd",    "we'll",    "we're",     "we've",
   "were",    "weren't",   "what",       "what's",  "when",     "when's",    "where",
   "where's", "which",     "while",      "who",     "who's",    "whom",      "why",
   "why's",   "with",      "won't",      "would",   "wouldn't", "you",       "you'd",
   "you'll",  "you're",    "you've",     "your",    "yours",    "yourself",  "yourselves",
   NULL /* Sentinel */
};

#define NUM_STOPWORDS (sizeof(STOPWORDS) / sizeof(STOPWORDS[0]) - 1)

/* =============================================================================
 * Data Structures
 * ============================================================================= */

/* Hash table entry for word -> document frequency mapping */
typedef struct hash_entry {
   char word[MAX_WORD_LEN];
   int doc_freq;            /* Number of sentences containing this word */
   struct hash_entry *next; /* Chain for collision handling */
} hash_entry_t;

/* Sentence with its score */
typedef struct {
   const char *start;          /* Pointer to start of sentence in original text */
   size_t length;              /* Length of sentence */
   float score;                /* TF-IDF score */
   int original_idx;           /* Original position for maintaining order */
   bool selected;              /* Selected for summary */
   hash_entry_t **word_vector; /* Cached word vector for MMR (NULL if not built) */
   float magnitude;            /* Cached magnitude for cosine similarity */
} sentence_t;

/* =============================================================================
 * Hash Table Functions (FNV-1a hash)
 * ============================================================================= */

/**
 * @brief FNV-1a hash function for strings
 */
static unsigned int hash_fnv1a(const char *str) {
   unsigned int hash = 2166136261u;
   while (*str) {
      hash ^= (unsigned char)*str++;
      hash *= 16777619u;
   }
   return hash % HASH_TABLE_SIZE;
}

/**
 * @brief Comparison function for stopword binary search
 */
static int stopword_cmp(const void *key, const void *element) {
   return strcmp((const char *)key, *(const char **)element);
}

/**
 * @brief Check if a word is a stopword using binary search
 * STOPWORDS array must be sorted alphabetically for this to work
 */
static bool is_stopword(const char *word) {
   return bsearch(word, STOPWORDS, NUM_STOPWORDS, sizeof(char *), stopword_cmp) != NULL;
}

/**
 * @brief Initialize hash table
 */
static hash_entry_t **hash_table_create(void) {
   hash_entry_t **table = calloc(HASH_TABLE_SIZE, sizeof(hash_entry_t *));
   return table;
}

/**
 * @brief Free hash table
 */
static void hash_table_free(hash_entry_t **table) {
   if (!table) {
      return;
   }

   for (int i = 0; i < HASH_TABLE_SIZE; i++) {
      hash_entry_t *entry = table[i];
      while (entry) {
         hash_entry_t *next = entry->next;
         free(entry);
         entry = next;
      }
   }
   free(table);
}

/**
 * @brief Clear hash table entries without freeing the table itself
 * This allows reusing the table to avoid repeated malloc/free overhead
 */
static void hash_table_clear(hash_entry_t **table) {
   if (!table) {
      return;
   }

   for (int i = 0; i < HASH_TABLE_SIZE; i++) {
      hash_entry_t *entry = table[i];
      while (entry) {
         hash_entry_t *next = entry->next;
         free(entry);
         entry = next;
      }
      table[i] = NULL;
   }
}

/**
 * @brief Increment document frequency for a word
 */
static void hash_table_increment(hash_entry_t **table, const char *word) {
   unsigned int idx = hash_fnv1a(word);
   hash_entry_t *entry = table[idx];

   /* Search for existing entry */
   while (entry) {
      if (strcmp(entry->word, word) == 0) {
         entry->doc_freq++;
         return;
      }
      entry = entry->next;
   }

   /* Create new entry */
   hash_entry_t *new_entry = calloc(1, sizeof(hash_entry_t));
   if (!new_entry) {
      return;
   }
   strncpy(new_entry->word, word, MAX_WORD_LEN - 1);
   new_entry->doc_freq = 1;
   new_entry->next = table[idx];
   table[idx] = new_entry;
}

/**
 * @brief Get document frequency for a word
 */
static int hash_table_get(hash_entry_t **table, const char *word) {
   unsigned int idx = hash_fnv1a(word);
   hash_entry_t *entry = table[idx];

   while (entry) {
      if (strcmp(entry->word, word) == 0) {
         return entry->doc_freq;
      }
      entry = entry->next;
   }
   return 0;
}

/* =============================================================================
 * Sentence Quality Filters
 * ============================================================================= */

/**
 * @brief Check if sentence starts with a lowercase letter (likely a fragment)
 */
static bool starts_with_lowercase(const char *start, size_t length) {
   /* Skip leading whitespace */
   while (length > 0 && isspace((unsigned char)*start)) {
      start++;
      length--;
   }

   if (length == 0) {
      return false;
   }

   /* Check first character */
   return islower((unsigned char)*start);
}

/**
 * @brief Count words in a sentence
 */
static int count_words(const char *start, size_t length) {
   int count = 0;
   bool in_word = false;
   const char *end = start + length;

   for (const char *p = start; p < end; p++) {
      bool is_word_char = isalnum((unsigned char)*p) || *p == '\'';
      if (is_word_char && !in_word) {
         count++;
         in_word = true;
      } else if (!is_word_char) {
         in_word = false;
      }
   }

   return count;
}

/**
 * @brief Check if sentence contains noise keywords (charts, ads, UI elements)
 */
static bool has_noise_keywords(const char *start, size_t length) {
   /* Use stack buffer for typical sentences, heap for very long ones */
   char stack_buf[512];
   char *sentence;
   bool heap_allocated = false;

   if (length < sizeof(stack_buf)) {
      sentence = stack_buf;
   } else {
      sentence = malloc(length + 1);
      if (!sentence) {
         return false;
      }
      heap_allocated = true;
   }

   memcpy(sentence, start, length);
   sentence[length] = '\0';

   /* Convert to lowercase for case-insensitive matching */
   for (char *p = sentence; *p; p++) {
      *p = tolower((unsigned char)*p);
   }

   bool found = false;
   for (int i = 0; NOISE_KEYWORDS[i] != NULL; i++) {
      if (strstr(sentence, NOISE_KEYWORDS[i]) != NULL) {
         found = true;
         break;
      }
   }

   if (heap_allocated) {
      free(sentence);
   }
   return found;
}

/**
 * @brief Orphan sentence prefixes - sentences starting with these likely lack context
 * These are conversational fillers, dangling conjunctions, or incomplete references
 */
static const char *ORPHAN_PREFIXES[] = {
   "i mean,",         /* Conversational filler */
   "but that's",      /* Dangling conjunction referencing missing antecedent */
   "and that's",      /* Dangling conjunction */
   "so that's",       /* Dangling conjunction */
   "now, that's",     /* Dangling reference to previous context */
   "now that's",      /* Dangling reference (no comma variant) */
   "that said,",      /* Reference to previous statement */
   "that being said", /* Reference to previous statement */
   "well,",           /* Conversational filler */
   "anyway,",         /* Topic shift without context */
   "you know,",       /* Conversational filler */
   "like i said,",    /* Reference to missing statement */
   "as i said,",      /* Reference to missing statement */
   "as mentioned,",   /* Reference to missing context */
   "see also",        /* Navigation/cross-reference */
   "related:",        /* Navigation element */
   "more:",           /* Navigation element */
   NULL               /* Sentinel */
};

/**
 * @brief Check if sentence appears to be an orphan (lacking necessary context)
 */
static bool is_orphan_sentence(const char *start, size_t length) {
   /* Use stack buffer for typical sentences */
   char stack_buf[256];
   char *prefix;
   bool heap_allocated = false;

   /* Only need first ~50 chars to check prefixes */
   size_t check_len = (length < 50) ? length : 50;

   if (check_len < sizeof(stack_buf)) {
      prefix = stack_buf;
   } else {
      prefix = malloc(check_len + 1);
      if (!prefix) {
         return false;
      }
      heap_allocated = true;
   }

   /* Skip leading whitespace and quotes */
   const char *p = start;
   const char *end = start + check_len;
   while (p < end && (isspace((unsigned char)*p) || *p == '"' || *p == '\'')) {
      p++;
   }

   /* Copy and lowercase */
   size_t j = 0;
   while (p < end && j < check_len) {
      prefix[j++] = tolower((unsigned char)*p++);
   }
   prefix[j] = '\0';

   /* Check against orphan prefixes */
   bool is_orphan = false;
   for (int i = 0; ORPHAN_PREFIXES[i] != NULL; i++) {
      if (strncmp(prefix, ORPHAN_PREFIXES[i], strlen(ORPHAN_PREFIXES[i])) == 0) {
         is_orphan = true;
         break;
      }
   }

   if (heap_allocated) {
      free(prefix);
   }
   return is_orphan;
}

/**
 * @brief Incomplete suffix patterns - sentences ending with these likely need continuation
 * Pattern: "the [ABBREVIATION]" at end suggests something should follow
 */
static const char *INCOMPLETE_SUFFIXES[] = {
   "the u.s.", /* "the U.S." without following noun/verb */
   "the u.k.", /* "the U.K." */
   "the e.u.", /* "the E.U." */
   "the u.n.", /* "the U.N." */
   "a u.s.",   /* "a U.S." */
   "a u.k.",   /* "a U.K." */
   " u.s.",    /* Catches "...that the U.S." pattern more broadly */
   NULL        /* Sentinel */
};

/**
 * @brief Check if sentence ends with an incomplete pattern (needs forward merge)
 */
static bool has_incomplete_ending(const char *start, size_t length) {
   if (length < 6) {
      return false; /* Too short to have these patterns */
   }

   /* Check the last ~15 characters of the sentence */
   size_t check_start = (length > 15) ? length - 15 : 0;
   size_t check_len = length - check_start;

   /* Use stack buffer */
   char suffix[32];
   if (check_len >= sizeof(suffix)) {
      check_len = sizeof(suffix) - 1;
   }

   /* Copy and lowercase the suffix */
   for (size_t i = 0; i < check_len; i++) {
      suffix[i] = tolower((unsigned char)start[check_start + i]);
   }
   suffix[check_len] = '\0';

   /* Strip trailing whitespace and punctuation for matching */
   size_t end = check_len;
   while (end > 0 && (isspace((unsigned char)suffix[end - 1]) || suffix[end - 1] == '"' ||
                      suffix[end - 1] == '\'' || suffix[end - 1] == ')')) {
      end--;
   }
   suffix[end] = '\0';

   /* Check if suffix ends with any incomplete pattern */
   for (int i = 0; INCOMPLETE_SUFFIXES[i] != NULL; i++) {
      size_t pat_len = strlen(INCOMPLETE_SUFFIXES[i]);
      if (end >= pat_len) {
         if (strcmp(suffix + end - pat_len, INCOMPLETE_SUFFIXES[i]) == 0) {
            return true;
         }
      }
   }

   return false;
}

/**
 * @brief Check if a sentence should be filtered out
 * @return true if sentence should be excluded from summary
 */
static bool should_filter_sentence(const char *start, size_t length, int sentence_idx) {
   /* Filter 1: Sentences starting with lowercase (likely fragments) */
   /* Exception: Don't filter first sentence even if lowercase */
   if (sentence_idx > 0 && starts_with_lowercase(start, length)) {
      return true;
   }

   /* Filter 2: Sentences with too few words */
   if (count_words(start, length) < MIN_WORDS_PER_SENTENCE) {
      return true;
   }

   /* Filter 3: Sentences containing noise keywords (charts, ads, UI elements) */
   if (has_noise_keywords(start, length)) {
      return true;
   }

   /* Note: Orphan sentences are now merged with predecessors in split_sentences() */
   /* rather than being filtered out, preserving their content with context */

   return false;
}

/* =============================================================================
 * Text Processing Functions
 * ============================================================================= */

/**
 * @brief Normalize a word (lowercase, alphanumeric only)
 * @return true if word is valid (not empty and not stopword)
 */
static bool normalize_word(const char *src, size_t len, char *dst) {
   if (len == 0 || len >= MAX_WORD_LEN) {
      return false;
   }

   size_t j = 0;
   for (size_t i = 0; i < len && j < MAX_WORD_LEN - 1; i++) {
      if (isalnum((unsigned char)src[i])) {
         dst[j++] = tolower((unsigned char)src[i]);
      }
   }
   dst[j] = '\0';

   /* Reject empty words, single chars, or stopwords */
   if (j < 2 || is_stopword(dst)) {
      return false;
   }

   return true;
}

/**
 * @brief Split text into sentences
 * @return Number of sentences found
 */
static int split_sentences(const char *text, sentence_t *sentences, int max_sentences) {
   int count = 0;
   const char *start = text;
   const char *p = text;

   while (*p && count < max_sentences) {
      /* Skip leading whitespace */
      while (*start && isspace((unsigned char)*start)) {
         start++;
      }
      if (!*start) {
         break;
      }

      p = start;

      /* Find sentence end (.!?) followed by space/newline/end or closing quotes
       * Skip over markdown constructs: [link text](url) */
      int in_bracket = 0; /* Track [link text] depth */
      int in_md_url = 0;  /* Track ](url) section */

      while (*p) {
         /* Track markdown link brackets */
         if (*p == '[' && !in_md_url) {
            in_bracket++;
            p++;
            continue;
         }
         if (*p == ']' && in_bracket > 0) {
            in_bracket--;
            /* Check for ](url) pattern */
            if (*(p + 1) == '(') {
               in_md_url = 1;
            }
            p++;
            continue;
         }
         if (in_md_url && *p == '(') {
            p++;
            continue;
         }
         if (in_md_url && *p == ')') {
            in_md_url = 0;
            p++;
            continue;
         }

         /* Skip sentence terminators inside markdown constructs */
         if (in_bracket > 0 || in_md_url) {
            p++;
            continue;
         }

         if (*p == '.' || *p == '!' || *p == '?') {
            /* Look ahead for sentence boundary */
            const char *next = p + 1;

            /* Skip closing quotes/parens (but not markdown parens) */
            while (*next && (*next == '"' || *next == '\'')) {
               next++;
            }

            /* Check if this is a real sentence end */
            if (!*next || isspace((unsigned char)*next)) {
               /* For periods, check if this is an abbreviation (not for ! or ?) */
               if (*p == '.' && str_is_abbreviation(text, p)) {
                  /* This is an abbreviation, keep looking */
                  p++;
                  continue;
               }
               /* Skip to next for length calculation */
               p = next;
               break;
            }
         }
         p++;
      }

      /* Calculate sentence length */
      size_t len = p - start;

      /* Skip very short "sentences" (less than 10 chars) */
      if (len >= 10) {
         /* Check if we should merge with the previous sentence */
         bool should_merge = false;

         if (count > 0) {
            /* Forward merge: previous sentence ends incomplete (e.g., "the U.S.") */
            if (has_incomplete_ending(sentences[count - 1].start, sentences[count - 1].length)) {
               should_merge = true;
            }
            /* Backward merge: current sentence starts with orphan prefix */
            else if (is_orphan_sentence(start, len)) {
               should_merge = true;
            }
         }

         if (should_merge) {
            /* Extend previous sentence to include this one */
            /* New length = end of current sentence - start of previous sentence */
            sentences[count - 1].length = (start + len) - sentences[count - 1].start;
            /* Don't increment count - we merged instead of adding */
         } else {
            sentences[count].start = start;
            sentences[count].length = len;
            sentences[count].score = 0.0f;
            sentences[count].original_idx = count;
            sentences[count].selected = false;
            count++;
         }
      }

      /* Move to next sentence */
      start = p;
      while (*start && isspace((unsigned char)*start)) {
         start++;
      }
      p = start;
   }

   return count;
}

/**
 * @brief Process words in a sentence, calling callback for each valid word
 */
typedef void (*word_callback_t)(const char *word, void *userdata);

static void process_sentence_words(const char *start,
                                   size_t length,
                                   word_callback_t callback,
                                   void *userdata) {
   char word[MAX_WORD_LEN];
   const char *end = start + length;
   const char *word_start = NULL;

   for (const char *p = start; p <= end; p++) {
      bool is_word_char = (p < end) && (isalnum((unsigned char)*p) || *p == '\'');

      if (is_word_char && !word_start) {
         word_start = p;
      } else if (!is_word_char && word_start) {
         size_t word_len = p - word_start;
         if (normalize_word(word_start, word_len, word)) {
            callback(word, userdata);
         }
         word_start = NULL;
      }
   }
}

/* =============================================================================
 * TF-IDF Calculation
 * ============================================================================= */

/* Context for word counting callback */
typedef struct {
   hash_entry_t **seen_in_sentence; /* Words seen in current sentence */
   hash_entry_t **doc_freq_table;   /* Global document frequency table */
} doc_freq_context_t;

static void count_doc_freq_callback(const char *word, void *userdata) {
   doc_freq_context_t *ctx = (doc_freq_context_t *)userdata;

   /* Only count each word once per sentence */
   if (hash_table_get(ctx->seen_in_sentence, word) == 0) {
      hash_table_increment(ctx->seen_in_sentence, word);
      hash_table_increment(ctx->doc_freq_table, word);
   }
}

/* Context for TF-IDF scoring callback */
typedef struct {
   hash_entry_t **doc_freq_table;
   int total_sentences;
   int word_count;
   float score_sum;
} tfidf_context_t;

static void calc_tfidf_callback(const char *word, void *userdata) {
   tfidf_context_t *ctx = (tfidf_context_t *)userdata;

   int df = hash_table_get(ctx->doc_freq_table, word);
   if (df > 0) {
      /* IDF = log(N / df) where N = total sentences, df = sentences containing word */
      float idf = logf((float)ctx->total_sentences / (float)df);
      /* TF = 1 for each occurrence (could be enhanced to count frequency) */
      ctx->score_sum += idf;
      ctx->word_count++;
   }
}

/**
 * @brief Calculate TF-IDF scores for all sentences
 * Skips sentences with score < 0 (already filtered)
 */
static void calculate_tfidf_scores(sentence_t *sentences,
                                   int num_sentences,
                                   hash_entry_t **doc_freq_table) {
   for (int i = 0; i < num_sentences; i++) {
      /* Skip already-filtered sentences (score < 0) */
      if (sentences[i].score < 0.0f) {
         continue;
      }

      tfidf_context_t ctx = { .doc_freq_table = doc_freq_table,
                              .total_sentences = num_sentences,
                              .word_count = 0,
                              .score_sum = 0.0f };

      process_sentence_words(sentences[i].start, sentences[i].length, calc_tfidf_callback, &ctx);

      /* Average TF-IDF score (normalize by word count) */
      if (ctx.word_count > 0) {
         sentences[i].score = ctx.score_sum / (float)ctx.word_count;
      } else {
         sentences[i].score = 0.0f;
      }

      /* Apply position weighting */
      if (i == 0) {
         sentences[i].score *= FIRST_SENTENCE_WEIGHT;
      } else if (i == num_sentences - 1) {
         sentences[i].score *= LAST_SENTENCE_WEIGHT;
      }
   }
}

/* =============================================================================
 * MMR (Maximal Marginal Relevance) Selection
 * ============================================================================= */

/* Context for building word vector */
typedef struct {
   hash_entry_t **word_counts;
   hash_entry_t **doc_freq_table;
   int total_sentences;
   float magnitude_sq;
} word_vector_context_t;

static void build_word_vector_callback(const char *word, void *userdata) {
   word_vector_context_t *ctx = (word_vector_context_t *)userdata;
   hash_table_increment(ctx->word_counts, word);
}

/**
 * @brief Build and cache word vectors for all non-filtered sentences
 * This is called once before MMR selection to avoid repeated allocations
 */
static void build_all_word_vectors(sentence_t *sentences,
                                   int num_sentences,
                                   hash_entry_t **doc_freq_table) {
   for (int i = 0; i < num_sentences; i++) {
      /* Skip filtered sentences */
      if (sentences[i].score < 0.0f) {
         sentences[i].word_vector = NULL;
         sentences[i].magnitude = 0.0f;
         continue;
      }

      /* Create word vector */
      hash_entry_t **vec = hash_table_create();
      if (!vec) {
         sentences[i].word_vector = NULL;
         sentences[i].magnitude = 0.0f;
         continue;
      }

      word_vector_context_t ctx = { .word_counts = vec,
                                    .doc_freq_table = doc_freq_table,
                                    .total_sentences = num_sentences,
                                    .magnitude_sq = 0.0f };

      process_sentence_words(sentences[i].start, sentences[i].length, build_word_vector_callback,
                             &ctx);

      /* Pre-compute magnitude for cosine similarity */
      float mag_sq = 0.0f;
      for (int j = 0; j < HASH_TABLE_SIZE; j++) {
         hash_entry_t *entry = vec[j];
         while (entry) {
            int df = hash_table_get(doc_freq_table, entry->word);
            float idf = (df > 0) ? logf((float)num_sentences / (float)df) : 0.0f;
            float tfidf = (float)entry->doc_freq * idf;
            mag_sq += tfidf * tfidf;
            entry = entry->next;
         }
      }

      sentences[i].word_vector = vec;
      sentences[i].magnitude = sqrtf(mag_sq);
   }
}

/**
 * @brief Free all cached word vectors
 */
static void free_all_word_vectors(sentence_t *sentences, int num_sentences) {
   for (int i = 0; i < num_sentences; i++) {
      if (sentences[i].word_vector) {
         hash_table_free(sentences[i].word_vector);
         sentences[i].word_vector = NULL;
      }
   }
}

/**
 * @brief Compute cosine similarity between two sentences using cached word vectors
 */
static float compute_sentence_similarity_cached(const sentence_t *s1,
                                                const sentence_t *s2,
                                                hash_entry_t **doc_freq_table,
                                                int total_sentences) {
   /* Use cached vectors */
   if (!s1->word_vector || !s2->word_vector) {
      return 0.0f;
   }

   if (s1->magnitude == 0.0f || s2->magnitude == 0.0f) {
      return 0.0f;
   }

   /* Compute dot product by iterating through s1's vector */
   float dot_product = 0.0f;
   for (int i = 0; i < HASH_TABLE_SIZE; i++) {
      hash_entry_t *entry = s1->word_vector[i];
      while (entry) {
         int count2 = hash_table_get(s2->word_vector, entry->word);
         if (count2 > 0) {
            int df = hash_table_get(doc_freq_table, entry->word);
            float idf = (df > 0) ? logf((float)total_sentences / (float)df) : 0.0f;
            float tfidf1 = (float)entry->doc_freq * idf;
            float tfidf2 = (float)count2 * idf;
            dot_product += tfidf1 * tfidf2;
         }
         entry = entry->next;
      }
   }

   return dot_product / (s1->magnitude * s2->magnitude);
}

/**
 * @brief Select sentences using MMR (Maximal Marginal Relevance)
 *
 * MMR balances relevance and diversity:
 * Score = λ * Relevance - (1-λ) * max_similarity_to_selected
 *
 * This avoids selecting redundant sentences that say the same thing.
 */
static void select_sentences_mmr(sentence_t *sentences,
                                 int num_sentences,
                                 int num_to_select,
                                 hash_entry_t **doc_freq_table) {
   if (num_to_select <= 0 || num_sentences <= 0) {
      return;
   }

   /* Find the highest-scoring non-filtered sentence as first selection */
   int best_idx = -1;
   float best_score = -1.0f;
   for (int i = 0; i < num_sentences; i++) {
      /* Skip filtered sentences (score < 0) and already selected */
      if (sentences[i].score < 0.0f || sentences[i].selected) {
         continue;
      }
      if (sentences[i].score > best_score) {
         best_score = sentences[i].score;
         best_idx = i;
      }
   }

   if (best_idx >= 0) {
      sentences[best_idx].selected = true;
      num_to_select--;
   }

   /* Iteratively select remaining sentences using MMR */
   while (num_to_select > 0) {
      best_idx = -1;
      best_score = -999999.0f;

      for (int i = 0; i < num_sentences; i++) {
         /* Skip filtered sentences (score < 0) and already selected */
         if (sentences[i].score < 0.0f || sentences[i].selected) {
            continue;
         }

         /* Find maximum similarity to any already-selected sentence */
         float max_sim = 0.0f;
         for (int j = 0; j < num_sentences; j++) {
            if (sentences[j].selected) {
               float sim = compute_sentence_similarity_cached(&sentences[i], &sentences[j],
                                                              doc_freq_table, num_sentences);
               if (sim > max_sim) {
                  max_sim = sim;
               }
            }
         }

         /* MMR score: balance relevance and novelty */
         float mmr_score = MMR_LAMBDA * sentences[i].score - (1.0f - MMR_LAMBDA) * max_sim;

         if (mmr_score > best_score) {
            best_score = mmr_score;
            best_idx = i;
         }
      }

      if (best_idx >= 0) {
         sentences[best_idx].selected = true;
         num_to_select--;
      } else {
         /* No more valid candidates */
         break;
      }
   }
}

/* =============================================================================
 * Sorting Functions
 * ============================================================================= */

/**
 * @brief Compare function for sorting sentences by original index (ascending)
 */
static int compare_by_index(const void *a, const void *b) {
   const sentence_t *sa = (const sentence_t *)a;
   const sentence_t *sb = (const sentence_t *)b;

   return sa->original_idx - sb->original_idx;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

const char *tfidf_error_string(int error_code) {
   switch (error_code) {
      case TFIDF_SUCCESS:
         return "Success";
      case TFIDF_ERROR_NULL_INPUT:
         return "Null input parameter";
      case TFIDF_ERROR_ALLOC:
         return "Memory allocation failed";
      case TFIDF_ERROR_NO_SENTENCES:
         return "No sentences found in input";
      case TFIDF_ERROR_TOO_MANY_SENTENCES:
         return "Too many sentences in input";
      default:
         return "Unknown error";
   }
}

int tfidf_summarize(const char *input_text,
                    char **output_summary,
                    float keep_ratio,
                    size_t min_sentences) {
   if (!input_text || !output_summary) {
      return TFIDF_ERROR_NULL_INPUT;
   }

   *output_summary = NULL;

   /* Allocate sentence array */
   sentence_t *sentences = calloc(TFIDF_MAX_SENTENCES, sizeof(sentence_t));
   if (!sentences) {
      return TFIDF_ERROR_ALLOC;
   }

   /* Split into sentences */
   int num_sentences = split_sentences(input_text, sentences, TFIDF_MAX_SENTENCES);

   if (num_sentences == 0) {
      free(sentences);
      return TFIDF_ERROR_NO_SENTENCES;
   }

   LOG_INFO("tfidf_summarize: Found %d sentences in input", num_sentences);

   /* Apply quality filters - mark low-quality sentences with score = -1 */
   int filtered_count = 0;
   for (int i = 0; i < num_sentences; i++) {
      if (should_filter_sentence(sentences[i].start, sentences[i].length, i)) {
         sentences[i].score = -1.0f; /* Mark as filtered */
         filtered_count++;
      }
   }

   int valid_sentences = num_sentences - filtered_count;
   if (filtered_count > 0) {
      LOG_INFO("tfidf_summarize: Filtered %d sentences (fragments/charts), %d remain",
               filtered_count, valid_sentences);
   }

   /* If very few valid sentences, return all valid ones */
   if (valid_sentences <= (int)min_sentences) {
      /* Build output from non-filtered sentences with overflow protection */
      size_t output_size = 0;
      for (int i = 0; i < num_sentences; i++) {
         if (sentences[i].score >= 0.0f) {
            if (output_size > SIZE_MAX - sentences[i].length - 2) {
               LOG_ERROR("tfidf_summarize: Output size overflow in early return");
               free(sentences);
               return TFIDF_ERROR_ALLOC;
            }
            output_size += sentences[i].length + 2;
         }
      }
      char *output = malloc(output_size + 1);
      if (!output) {
         free(sentences);
         return TFIDF_ERROR_ALLOC;
      }
      char *p = output;
      char *output_end = output + output_size;
      for (int i = 0; i < num_sentences; i++) {
         if (sentences[i].score >= 0.0f) {
            size_t space_needed = sentences[i].length + (p > output ? 1 : 0);
            if (p + space_needed > output_end) {
               LOG_ERROR("tfidf_summarize: Buffer overflow prevented in early return");
               break;
            }
            if (p > output) {
               *p++ = ' ';
            }
            memcpy(p, sentences[i].start, sentences[i].length);
            p += sentences[i].length;
         }
      }
      *p = '\0';
      free(sentences);
      *output_summary = output;
      return TFIDF_SUCCESS;
   }

   /* Build document frequency table (only from non-filtered sentences) */
   hash_entry_t **doc_freq_table = hash_table_create();
   if (!doc_freq_table) {
      free(sentences);
      return TFIDF_ERROR_ALLOC;
   }

   /* Create a reusable "seen" hash table to track words within each sentence.
    * This avoids malloc/free churn from creating a new table for each sentence. */
   hash_entry_t **seen = hash_table_create();
   if (!seen) {
      hash_table_free(doc_freq_table);
      free(sentences);
      return TFIDF_ERROR_ALLOC;
   }

   for (int i = 0; i < num_sentences; i++) {
      if (sentences[i].score < 0.0f) {
         continue; /* Skip filtered sentences */
      }

      doc_freq_context_t ctx = { .seen_in_sentence = seen, .doc_freq_table = doc_freq_table };
      process_sentence_words(sentences[i].start, sentences[i].length, count_doc_freq_callback,
                             &ctx);

      hash_table_clear(seen); /* Clear for next sentence, reuse table */
   }
   hash_table_free(seen);

   /* Calculate TF-IDF scores (only for non-filtered sentences) */
   calculate_tfidf_scores(sentences, num_sentences, doc_freq_table);

   /* Determine how many sentences to keep (based on valid sentences) */
   int num_to_keep = (int)(valid_sentences * keep_ratio);
   if (num_to_keep < (int)min_sentences) {
      num_to_keep = (int)min_sentences;
   }
   if (num_to_keep > valid_sentences) {
      num_to_keep = valid_sentences;
   }

   LOG_INFO("tfidf_summarize: Selecting %d of %d valid sentences using MMR (%.0f%%)", num_to_keep,
            valid_sentences, (float)num_to_keep / valid_sentences * 100);

   /* Build word vectors once for all sentences (optimization: avoids O(n^2) allocations) */
   build_all_word_vectors(sentences, num_sentences, doc_freq_table);

   /* Use MMR to select sentences (balances relevance and diversity) */
   select_sentences_mmr(sentences, num_sentences, num_to_keep, doc_freq_table);

   /* Clean up word vectors and doc_freq_table after MMR selection */
   free_all_word_vectors(sentences, num_sentences);
   hash_table_free(doc_freq_table);

   /* Sort by original index to maintain document order */
   qsort(sentences, num_sentences, sizeof(sentence_t), compare_by_index);

   /* Calculate output size with overflow protection */
   size_t output_size = 0;
   for (int i = 0; i < num_sentences; i++) {
      if (sentences[i].selected) {
         if (output_size > SIZE_MAX - sentences[i].length - 2) {
            LOG_ERROR("tfidf_summarize: Output size overflow");
            free(sentences);
            return TFIDF_ERROR_ALLOC;
         }
         output_size += sentences[i].length + 2; /* +2 for space between sentences */
      }
   }

   /* Allocate and build output */
   char *output = malloc(output_size + 1);
   if (!output) {
      free(sentences);
      return TFIDF_ERROR_ALLOC;
   }

   char *p = output;
   char *output_end = output + output_size;
   for (int i = 0; i < num_sentences; i++) {
      if (sentences[i].selected) {
         size_t space_needed = sentences[i].length + (p > output ? 1 : 0);
         if (p + space_needed > output_end) {
            LOG_ERROR("tfidf_summarize: Buffer overflow prevented");
            break;
         }
         if (p > output) {
            *p++ = ' ';
         }
         memcpy(p, sentences[i].start, sentences[i].length);
         p += sentences[i].length;
      }
   }
   *p = '\0';

   free(sentences);
   *output_summary = output;

   LOG_INFO("tfidf_summarize: Produced %zu byte summary from %zu byte input", strlen(output),
            strlen(input_text));

   return TFIDF_SUCCESS;
}
