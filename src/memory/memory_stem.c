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
 * Porter2 stemmer wrapper around libstemmer (Snowball).
 *
 * libstemmer is BSD-3-Clause; linked dynamically as a shared library.
 * See DEPENDENCIES.md.
 */

#include "memory/memory_stem.h"

#include <ctype.h>
#include <libstemmer.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "memory/memory_types.h"
#include "utils/string_utils.h"

/* Compile-time tripwire: the stems output buffer must comfortably exceed
 * the maximum fact_text length, since Porter2 only shortens (never grows)
 * tokens and the framing replaces delimiters 1-to-1 with spaces.  If
 * MEMORY_FACT_TEXT_MAX is ever bumped above this constant, the stems
 * buffer used by callers needs to grow in lock-step. */
_Static_assert(MEMORY_FACT_STEMS_MAX >= MEMORY_FACT_TEXT_MAX,
               "MEMORY_FACT_STEMS_MAX must be >= MEMORY_FACT_TEXT_MAX so the BM25 "
               "index never silently truncates fact_text mid-token");

static struct sb_stemmer *s_stemmer = NULL;
static pthread_mutex_t s_stemmer_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Atomic so the fast-path read in memory_stem_string can race against a
 * concurrent first-caller init without UB.  memory_stem_init re-checks
 * under the mutex so the flag's role is purely "have we tried init at
 * least once?" — the mutex still serializes the actual sb_stemmer_new
 * call. */
static _Atomic bool s_init_attempted = false;

/* Canonical memory-subsystem tokenizer delimiter set.  Declared `extern` in
 * memory_stem.h so memory_fact_search.c::tokenize_query and
 * memory_callback.c::tokenize_query share the exact same boundary class
 * — keeps extraction-time stems and query-time tokens aligned on token shape.
 * NOT consumed by memory_db.c::build_fts5_match_expr (already-stemmed input,
 * space-only split). */
const char *const MEMORY_TOKEN_DELIMS = " \t\n\r,.;:!?\"'()[]{}/-";

int memory_stem_tokenize_in_place(char *input,
                                  const char **tokens_out,
                                  int max_tokens,
                                  int min_len) {
   if (input == NULL || tokens_out == NULL || max_tokens <= 0)
      return 0;

   /* Lowercase the whole buffer in one pass.  strtok_r will rewrite the
    * delimiters as NULs, but lowercasing first means every emitted token
    * is already lowercase regardless of which side of a delimiter it sits.
    */
   for (size_t i = 0; input[i] != '\0'; i++) {
      input[i] = (char)tolower((unsigned char)input[i]);
   }

   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(input, MEMORY_TOKEN_DELIMS, &saveptr);
   while (tok != NULL && count < max_tokens) {
      if ((int)strlen(tok) >= min_len) {
         tokens_out[count++] = tok;
      }
      tok = strtok_r(NULL, MEMORY_TOKEN_DELIMS, &saveptr);
   }
   return count;
}

int memory_stem_tokenize_padded(const char *keywords,
                                char tokens[][64],
                                int max_tokens,
                                int min_len) {
   if (keywords == NULL || tokens == NULL || max_tokens <= 0)
      return 0;

   /* Working copy sized to MEMORY_FACT_STEMS_MAX so the tokenizer never
    * silently loses tail tokens on long input.  Longer input truncates
    * with no log (rare for real queries — UI input boxes cap well below
    * this).  Caller's `tokens[][64]` storage shape sets the row cap. */
   char buf[MEMORY_FACT_STEMS_MAX];
   safe_strscpy(buf, keywords);

   /* Use a stack pointer-array sized to max_tokens (caller passes a small
    * cap, usually 8).  Hard cap at 64 to bound the VLA. */
   if (max_tokens > 64)
      max_tokens = 64;
   const char *raw_tokens[64];
   int n = memory_stem_tokenize_in_place(buf, raw_tokens, max_tokens, min_len);
   for (int i = 0; i < n; i++) {
      snprintf(tokens[i], 64, "%s", raw_tokens[i]);
   }
   return n;
}

int memory_stem_init(void) {
   pthread_mutex_lock(&s_stemmer_mutex);
   if (s_stemmer != NULL) {
      pthread_mutex_unlock(&s_stemmer_mutex);
      return 0;
   }
   s_init_attempted = true;
   /* "english" with UTF_8 charset selects Porter2 (the "english" algorithm
    * in libstemmer 2.x is Porter2; "porter" would be Porter1). */
   s_stemmer = sb_stemmer_new("english", "UTF_8");
   if (s_stemmer == NULL) {
      OLOG_WARNING("memory_stem: sb_stemmer_new('english','UTF_8') failed — "
                   "stemming disabled, BM25 will index raw lowercased tokens");
      pthread_mutex_unlock(&s_stemmer_mutex);
      return 1;
   }
   pthread_mutex_unlock(&s_stemmer_mutex);
   return 0;
}

void memory_stem_shutdown(void) {
   pthread_mutex_lock(&s_stemmer_mutex);
   if (s_stemmer != NULL) {
      sb_stemmer_delete(s_stemmer);
      s_stemmer = NULL;
   }
   s_init_attempted = false;
   pthread_mutex_unlock(&s_stemmer_mutex);
}

/* Lowercase + stem a single token in-place into `dst` (size `dst_sz`).
 * Returns length written (excluding NUL), or 0 on failure / empty input.
 * Caller holds s_stemmer_mutex when s_stemmer != NULL. */
static size_t stem_one(const char *tok, size_t tok_len, char *dst, size_t dst_sz) {
   if (tok == NULL || tok_len == 0 || dst == NULL || dst_sz < 2)
      return 0;

   /* Lowercase first into a working buffer (libstemmer is case-sensitive
    * — Porter2 only handles lowercased input).  Sized to MEMORY_FACT_TEXT_MAX
    * so the longest legitimate fact_text fragment lands without truncation;
    * the previous 128-byte limit could cut multi-byte UTF-8 codepoints
    * (e.g., Cyrillic 2-byte sequences at byte 127) and feed malformed
    * input to libstemmer.  Excess past MEMORY_FACT_TEXT_MAX is still
    * truncated as a final safety net for any caller that feeds longer
    * input. */
   char lower[MEMORY_FACT_TEXT_MAX];
   if (tok_len >= sizeof(lower))
      tok_len = sizeof(lower) - 1;
   for (size_t i = 0; i < tok_len; i++) {
      lower[i] = (char)tolower((unsigned char)tok[i]);
   }
   lower[tok_len] = '\0';

   const char *stemmed = lower;
   int stemmed_len = (int)tok_len;

   if (s_stemmer != NULL) {
      const sb_symbol *out = sb_stemmer_stem(s_stemmer, (const sb_symbol *)lower, (int)tok_len);
      if (out != NULL) {
         stemmed = (const char *)out;
         stemmed_len = sb_stemmer_length(s_stemmer);
      }
   }

   if (stemmed_len <= 0)
      return 0;
   size_t copy = (size_t)stemmed_len;
   if (copy >= dst_sz)
      copy = dst_sz - 1;
   memcpy(dst, stemmed, copy);
   dst[copy] = '\0';
   return copy;
}

/* Maximum tokens a single fact_text or query can produce after tokenization.
 * Bounded so the in-place pointer array stays stack-allocated.  At an
 * average english word length of ~5 chars + 1 delimiter, MEMORY_FACT_STEMS_MAX
 * (768) caps at ~128 tokens; 256 is comfortable headroom for very-short-word
 * pathological input. */
#define MEMORY_STEM_TOKENS_MAX 256

int memory_stem_string(const char *input, char *out, size_t out_sz) {
   if (out == NULL || out_sz == 0)
      return 0;
   out[0] = '\0';
   if (input == NULL || input[0] == '\0')
      return 0;

   /* Auto-init on first use so callers don't have to thread init through
    * their startup order.  Threadsafe: memory_stem_init guards itself. */
   if (!s_init_attempted) {
      (void)memory_stem_init();
   }

   /* Working copy so we can NUL-write delimiters in place via the shared
    * tokenizer helper.  Sized to MEMORY_FACT_STEMS_MAX (the subsystem-wide
    * post-stem buffer constant) so a maximal fact_text always lands without
    * silent truncation; trim is still a final safety net for any caller
    * feeding longer input. */
   char buf[MEMORY_FACT_STEMS_MAX];
   size_t in_len = strlen(input);
   if (in_len >= sizeof(buf))
      in_len = sizeof(buf) - 1;
   memcpy(buf, input, in_len);
   buf[in_len] = '\0';

   const char *tokens[MEMORY_STEM_TOKENS_MAX];
   int token_count = memory_stem_tokenize_in_place(buf, tokens, MEMORY_STEM_TOKENS_MAX, 2);

   pthread_mutex_lock(&s_stemmer_mutex);

   size_t out_off = 0;
   int count = 0;
   for (int i = 0; i < token_count; i++) {
      size_t tlen = strlen(tokens[i]);
      char stem[64];
      size_t slen = stem_one(tokens[i], tlen, stem, sizeof(stem));
      if (slen == 0)
         continue;
      size_t need = slen + (out_off > 0 ? 1 : 0);
      if (out_off + need >= out_sz)
         break;
      if (out_off > 0)
         out[out_off++] = ' ';
      memcpy(out + out_off, stem, slen);
      out_off += slen;
      count++;
   }
   out[out_off] = '\0';

   pthread_mutex_unlock(&s_stemmer_mutex);
   return count;
}

int memory_stem_count(const char *input) {
   if (input == NULL || input[0] == '\0')
      return 0;
   /* Identical tokenization to memory_stem_string but without writing
    * output.  Stays cheap so callers can size their FTS5 expressions. */
   char buf[MEMORY_FACT_STEMS_MAX];
   size_t in_len = strlen(input);
   if (in_len >= sizeof(buf))
      in_len = sizeof(buf) - 1;
   memcpy(buf, input, in_len);
   buf[in_len] = '\0';

   const char *tokens[MEMORY_STEM_TOKENS_MAX];
   return memory_stem_tokenize_in_place(buf, tokens, MEMORY_STEM_TOKENS_MAX, 2);
}
