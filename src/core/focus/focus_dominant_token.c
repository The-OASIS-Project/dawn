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
 * Dominant-token over-inclusion heuristic — implementation.  See
 * include/core/focus/focus_dominant_token.h for the contract and algorithm
 * details.
 */

#include "core/focus/focus_dominant_token.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Tokenization
 *
 * Lowercase, alphanumeric runs ≥ MIN_TOKEN_LEN, dropping a curated
 * stopword list.  Token list pulled from the Phase B-i Python prototype
 * (benchmarks/bench_focus_heuristic_probe.py) — keep the two lists in
 * sync if either is extended.
 *
 * Caps:
 *   MAX_POOL_CANDIDATES — production worst case is MAX_FOCUS_SOURCES (16) *
 *     per_source_max_candidates (build_focus_block.c computes this as
 *     MIN(top_k + window/2, top_k*2); at top_k=12 + window=8 → 16).  16 * 16 = 256.
 *     Bump above 256 if MAX_FOCUS_SOURCES grows OR per_source_max_candidates
 *     bumps.  See architecture review (2026-05-13) — earlier cap of 64 could
 *     be hit at production defaults (top_k=12 + 6 adapters), silently
 *     no-op'ing the heuristic when it was most needed.
 *   MAX_TOKENS_PER_TEXT — per-candidate text token cap; typical pool entries
 *     have far fewer.
 *   MAX_QUERY_TOKENS — query token count; natural-language queries ≤ ~10.
 * Heap-allocate the per-candidate token buffer to keep worst-case stack
 * usage bounded; per-turn heap churn is one calloc/free pair.
 * ============================================================================= */

#define MIN_TOKEN_LEN 4
#define MAX_TOKENS_PER_TEXT 64
#define MAX_QUERY_TOKENS 32
#define MAX_POOL_CANDIDATES 256
#define MAX_TOKEN_BYTES 32 /* per-token byte cap; longer truncates */

typedef struct {
   char text[MAX_TOKEN_BYTES];
} token_t;

/* Stopwords kept in sync with bench_focus_heuristic_probe.py.  Curated
 * list, maintenance-order (NOT strictly alphabetical — adding bsearch
 * would require sorting + a _Static_assert on order).  ~105 entries.
 *
 * Lookup uses a first-character short-circuit (see is_stopword below):
 * we group entries roughly by first letter, so most query tokens reject
 * after ~1-3 char comparisons rather than walking the whole table.
 * This beats bsearch's branch overhead at this scale. */
static const char *const STOPWORDS[] = {
   "about",   "after",     "again",     "against",    "also",     "always", "ancestor", "any",
   "anyway",  "anywhere",  "around",    "because",    "been",     "before", "being",    "between",
   "both",    "could",     "does",      "doesn",      "doing",    "done",   "down",     "each",
   "either",  "enough",    "ever",      "every",      "everyone", "from",   "have",     "having",
   "here",    "hers",      "herself",   "himself",    "into",     "itself", "just",     "last",
   "less",    "like",      "lots",      "many",       "might",    "more",   "most",     "much",
   "must",    "myself",    "never",     "next",       "none",     "noone",  "nothing",  "onto",
   "only",    "other",     "ours",      "ourselves",  "over",     "right",  "same",     "some",
   "someone", "something", "sometimes", "still",      "such",     "than",   "that",     "thats",
   "their",   "theirs",    "them",      "themselves", "there",    "these",  "they",     "thing",
   "things",  "this",      "those",     "through",    "until",    "very",   "want",     "wanted",
   "wanting", "wants",     "were",      "what",       "whats",    "when",   "where",    "which",
   "while",   "with",      "without",   "would",      "your",     "yours",  "yourself", "user",
   "users",
};

static const size_t STOPWORD_COUNT = sizeof(STOPWORDS) / sizeof(STOPWORDS[0]);

/**
 * @brief Stopword check with first-character short-circuit.
 *
 * At ~105 entries, branch-predictable first-char comparison rejects
 * most non-matches in 1 cycle (vs strcmp's full byte-walk).  Tokens
 * are already lowercased by tokenize_text, so the leading-byte
 * comparison is sound — no case-folding concern.
 */
static bool is_stopword(const char *token) {
   const char first = token[0];
   for (size_t i = 0; i < STOPWORD_COUNT; i++) {
      if (STOPWORDS[i][0] != first) {
         continue;
      }
      if (strcmp(STOPWORDS[i], token) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Tokenize text into lowercase alphanumeric runs ≥ MIN_TOKEN_LEN.
 *
 * Skips stopwords and over-MAX_TOKEN_BYTES runs (truncates rather than
 * splits — long URLs / IDs collapse to a single bounded token).
 *
 * Returns count of tokens emitted into @p out (capped at @p max_out).
 * Subsequent occurrences of the same token are kept — the caller
 * dedupes via set semantics (membership test, not count).
 */
static int tokenize_text(const char *text, token_t *out, int max_out) {
   if (text == NULL || out == NULL || max_out <= 0) {
      return 0;
   }

   int count = 0;
   const char *p = text;

   while (*p && count < max_out) {
      /* Skip non-alphanumeric. */
      while (*p && !isalnum((unsigned char)*p)) {
         p++;
      }
      if (!*p) {
         break;
      }

      /* Read alphanumeric run, lowercase into buffer. */
      char buf[MAX_TOKEN_BYTES];
      size_t blen = 0;
      while (*p && isalnum((unsigned char)*p) && blen < sizeof(buf) - 1) {
         buf[blen++] = (char)tolower((unsigned char)*p);
         p++;
      }
      buf[blen] = '\0';

      /* Skip the rest of an over-cap run so we don't emit a fragment +
       * start a new token from the middle. */
      while (*p && isalnum((unsigned char)*p)) {
         p++;
      }

      if (blen >= MIN_TOKEN_LEN && !is_stopword(buf)) {
         memcpy(out[count].text, buf, blen + 1);
         count++;
      }
   }

   return count;
}

/* =============================================================================
 * Set membership over the token_t array.
 *
 * Linear scan — tokens-per-text and query tokens both bounded at small
 * counts (≤ 64 and ≤ 32 respectively).  A hash set would add cache miss
 * overhead at these scales.
 * ============================================================================= */

static bool token_set_contains(const token_t *set, int count, const char *needle) {
   for (int i = 0; i < count; i++) {
      if (strcmp(set[i].text, needle) == 0) {
         return true;
      }
   }
   return false;
}

/* =============================================================================
 * Public entry point.
 * ============================================================================= */

/* TODO (deferred from 2026-05-13 review, coding-standards H-1): function
 * length 158 lines vs 50-line soft target.  Four sequential phases
 * (guards / tokenize query / tokenize-pool + detect dominant / apply
 * penalty) share the two heap arrays.  Natural extraction point is a
 * `detect_dominant_tokens()` helper returning a boolean mask + count;
 * the remaining penalty pass then fits the soft target.  Defer until
 * next touch of the file (e.g., the bitmask popcount optimization
 * from embedded review M3) so both refactors land together and only
 * one paid bench re-validation is needed. */
void focus_apply_dominant_token_penalty(bool enabled,
                                        float threshold,
                                        float base_penalty,
                                        const char *query,
                                        focus_candidate_t *pool,
                                        int pool_count) {
   /* No-op gates — match the documented contract in the header. */
   if (!enabled) {
      return;
   }
   if (pool == NULL || pool_count < 2) {
      return;
   }
   if (query == NULL || query[0] == '\0') {
      return;
   }
   if (threshold <= 0.0f || threshold > 1.0f) {
      return;
   }
   if (base_penalty <= 0.0f) {
      return;
   }

   /* Pool size cap is a safety guard, not a contract — `focus_compose`'s
    * worst-case pool sizing is well below MAX_POOL_CANDIDATES at current
    * MAX_FOCUS_SOURCES * per_source_max_candidates, but a future cap bump
    * could push past it.  Cap silently rather than abort so the rest of
    * the pipeline continues; the diagnostic log surfaces the truncation. */
   if (pool_count > MAX_POOL_CANDIDATES) {
      OLOG_WARNING("focus_dominant_token: pool_count=%d > MAX_POOL_CANDIDATES=%d — "
                   "heuristic skipped (raise the cap or split the call)",
                   pool_count, MAX_POOL_CANDIDATES);
      return;
   }

   /* Tokenize query. */
   token_t query_toks[MAX_QUERY_TOKENS];
   const int n_query = tokenize_text(query, query_toks, MAX_QUERY_TOKENS);
   if (n_query == 0) {
      return; /* Pure stopword query or empty after filtering. */
   }

   /* Tokenize each candidate.  Per-candidate token arrays are heap-
    * allocated as one flat 2D buffer (single calloc, not N small ones)
    * to keep stack usage bounded and avoid per-candidate heap churn.
    * Worst case at MAX_POOL_CANDIDATES=256: 256 * 64 * 32 = 512 KB heap
    * for the duration of the call, freed at function exit.  Negligible
    * on Jetson glibc tcmalloc fast path. */
   token_t(*cand_toks)[MAX_TOKENS_PER_TEXT] = calloc((size_t)pool_count, sizeof(*cand_toks));
   if (cand_toks == NULL) {
      OLOG_WARNING(
          "focus_dominant_token: OOM allocating candidate token buffer — heuristic skipped");
      return;
   }
   int *cand_tok_counts = calloc((size_t)pool_count, sizeof(*cand_tok_counts));
   if (cand_tok_counts == NULL) {
      free(cand_toks);
      cand_toks = NULL;
      OLOG_WARNING("focus_dominant_token: OOM allocating tok count buffer — heuristic skipped");
      return;
   }

   for (int i = 0; i < pool_count; i++) {
      cand_tok_counts[i] = tokenize_text(pool[i].text, cand_toks[i], MAX_TOKENS_PER_TEXT);
   }

   /* For each query token, count pool members that contain it. */
   int query_tok_freq[MAX_QUERY_TOKENS] = { 0 };
   for (int q = 0; q < n_query; q++) {
      for (int i = 0; i < pool_count; i++) {
         if (token_set_contains(cand_toks[i], cand_tok_counts[i], query_toks[q].text)) {
            query_tok_freq[q]++;
         }
      }
   }

   /* Mark dominant tokens — frequency strictly greater than threshold * pool_count.
    * Strict-greater rather than ≥ matches the Python prototype semantics. */
   const float threshold_count = threshold * (float)pool_count;
   bool is_dominant[MAX_QUERY_TOKENS] = { false };
   int n_dominant = 0;
   for (int q = 0; q < n_query; q++) {
      if ((float)query_tok_freq[q] > threshold_count) {
         is_dominant[q] = true;
         n_dominant++;
      }
   }

   /* If no tokens are dominant, the over-inclusion pathology isn't
    * present — nothing to penalize, fast-out. */
   if (n_dominant == 0) {
      free(cand_toks);
      cand_toks = NULL;
      free(cand_tok_counts);
      cand_tok_counts = NULL;
      return;
   }

   /* Apply penalty per candidate.
    *
    * shared      = query tokens this candidate contains
    * shared_dom  = shared tokens that are dominant
    * shared_nd   = shared tokens that are NOT dominant
    *
    * Penalty:
    *   0                                       if shared_dom is empty
    *   base_penalty                            if shared_nd is empty
    *   base_penalty * |shared_dom| / |shared|  otherwise (diluted) */
   int n_penalized = 0;
   for (int i = 0; i < pool_count; i++) {
      int n_shared = 0;
      int n_shared_dom = 0;
      for (int q = 0; q < n_query; q++) {
         if (token_set_contains(cand_toks[i], cand_tok_counts[i], query_toks[q].text)) {
            n_shared++;
            if (is_dominant[q]) {
               n_shared_dom++;
            }
         }
      }

      if (n_shared_dom == 0) {
         continue;
      }

      float penalty;
      if (n_shared == n_shared_dom) {
         penalty = base_penalty;
      } else {
         penalty = base_penalty * (float)n_shared_dom / (float)n_shared;
      }

      /* Clamp to ≥ 0 — semantic_score is bounded 0..1 by adapter contract.
       * FOCUS_SCORE_NA candidates (negative sentinel) are not penalized;
       * subtracting from a negative would push them further negative and
       * the score-aggregator treats negative semantic_score as "not
       * applicable" elsewhere. */
      if (pool[i].semantic_score < 0.0f) {
         continue;
      }

      const float before = pool[i].semantic_score;
      pool[i].semantic_score -= penalty;
      if (pool[i].semantic_score < 0.0f) {
         pool[i].semantic_score = 0.0f;
      }
      n_penalized++;

      (void)before; /* read only by the OLOG_DEBUG below (a no-op unless built -DDEBUG) */
      OLOG_DEBUG("focus_dominant_token: penalty %.3f applied to '%s' (semantic %.3f → %.3f, "
                 "shared %d, dominant %d)",
                 penalty, pool[i].item_id ? pool[i].item_id : "(no id)", before,
                 pool[i].semantic_score, n_shared, n_shared_dom);
   }

   if (n_penalized > 0) {
      OLOG_INFO("focus_dominant_token: penalized %d/%d candidates (%d dominant tokens detected)",
                n_penalized, pool_count, n_dominant);
   }

   free(cand_toks);
   cand_toks = NULL;
   free(cand_tok_counts);
   cand_tok_counts = NULL;
}
