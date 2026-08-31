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
 * Document Search Tool - RAG semantic search over uploaded documents
 *
 * Embeds the query, loads all accessible chunks, computes cosine similarity
 * with optional keyword boosting, and returns top results with citations.
 */

#include "tools/document_search.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "core/time_query_parser.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_SEARCH_MAX_RESULTS 5
#define DOC_SEARCH_MAX_CONTEXT_TOKENS 2000
#define DOC_SEARCH_MAX_CHUNKS 10000
/* v61 hybrid search: lexical BM25 candidate cap, and the count of top-cosine
 * candidates the (more expensive) phrase bonus is computed over.  Phrase bonus
 * runs on union(semantic top-N, ALL lexical hits) so an exact-label note with
 * weak cosine still gets scored even if it falls below the semantic top-N. */
#define DOC_SEARCH_BM25_CAND_LIMIT 50
#define DOC_SEARCH_PHRASE_TOPN 50
#define DOC_SEARCH_PHRASE_MAX_TOKENS 256

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *doc_search_callback(const char *action, char *value, int *should_respond);
static bool doc_search_is_available(void);

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const treg_param_t doc_search_params[] = {
   {
       .name = "query",
       .description = "The search query — what you want to find in the user's documents",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t doc_search_metadata = {
   .name = "document_search",
   .device_string = "document search",
   .description = "Search ONLY the user's saved documents and notes (hybrid keyword + semantic). "
                  "This is a TARGETED follow-up: use it when you already know the answer lives in "
                  "an uploaded file or filed note. For a BROAD 'what do we know about X' question "
                  "that could also touch memory or the calendar, call 'recall' FIRST (it spans all "
                  "sources) and use this only to drill into documents specifically. "
                  "Results rank EXACT label matches first, so to pull back a specific saved item "
                  "ask for its label (e.g. 'public bio'). For the verbatim full text of a known "
                  "note, prefer document_read with its exact label. Returns excerpts with source "
                  "citations. Do NOT use this for general web searches.",
   .params = doc_search_params,
   .param_count = 1,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = 0,
   .is_available = doc_search_is_available,
   .callback = doc_search_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int document_search_tool_register(void) {
   return tool_registry_register(&doc_search_metadata);
}

/* =============================================================================
 * Availability Check
 * ============================================================================= */

static bool doc_search_is_available(void) {
   return embedding_engine_available();
}

/* =============================================================================
 * Internal: Query tokenization (shared by the phrase bonus)
 * ============================================================================= */

typedef struct {
   const char *words[32];
   int lengths[32];
   int count;
} query_words_t;

static void tokenize_query(const char *query, query_words_t *qw) {
   qw->count = 0;
   const char *p = query;

   while (*p && qw->count < 32) {
      while (*p && !isalnum((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && isalnum((unsigned char)*p))
         p++;
      int len = (int)(p - start);
      if (len >= 3) { /* Skip very short words */
         qw->words[qw->count] = start;
         qw->lengths[qw->count] = len;
         qw->count++;
      }
   }
}

/* =============================================================================
 * Internal: Ordered / contiguous phrase bonus (v61, Req 3)
 *
 * BM25 is bag-of-words; this adds the "how many words match AND how many match
 * IN ORDER" signal the user asked for.  Returns max(ordered_ratio,
 * contiguous_ratio) in [0, 1]: ordered_ratio is the fraction of query words that
 * appear in the text in their query order; contiguous_ratio is the longest run
 * of query words appearing back-to-back in the text, over the query length.  An
 * exact label phrase ("Public Bio") yields 1.0.
 * ============================================================================= */

/* Case-insensitive token equality. */
static bool tok_eq_ci(const char *a, int al, const char *b, int bl) {
   if (al != bl)
      return false;
   for (int i = 0; i < al; i++) {
      if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
         return false;
   }
   return true;
}

/* Tokenize `text` into up to `max` (start,len) spans over alnum runs. */
static int tokenize_text_spans(const char *text, const char **starts, int *lens, int max) {
   int n = 0;
   const char *p = text;
   while (*p && n < max) {
      while (*p && !isalnum((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && isalnum((unsigned char)*p))
         p++;
      starts[n] = start;
      lens[n] = (int)(p - start);
      n++;
   }
   return n;
}

static float phrase_bonus(const char *text, const query_words_t *qw) {
   if (!text || qw->count == 0)
      return 0.0f;

   const char *tstart[DOC_SEARCH_PHRASE_MAX_TOKENS];
   int tlen[DOC_SEARCH_PHRASE_MAX_TOKENS];
   int nt = tokenize_text_spans(text, tstart, tlen, DOC_SEARCH_PHRASE_MAX_TOKENS);
   if (nt == 0)
      return 0.0f;
   const int nq = qw->count;

   /* Ordered: greedy in-order match of query words against the text stream. */
   int ordered = 0;
   int ti = 0;
   for (int qi = 0; qi < nq; qi++) {
      for (int p = ti; p < nt; p++) {
         if (tok_eq_ci(tstart[p], tlen[p], qw->words[qi], qw->lengths[qi])) {
            ordered++;
            ti = p + 1;
            break;
         }
      }
   }

   /* Contiguous: longest run of consecutive query words appearing back-to-back
    * in the text (any starting offset in the query). */
   int best_run = 0;
   for (int ts = 0; ts < nt; ts++) {
      for (int qs = 0; qs < nq; qs++) {
         int run = 0;
         while (ts + run < nt && qs + run < nq &&
                tok_eq_ci(tstart[ts + run], tlen[ts + run], qw->words[qs + run],
                          qw->lengths[qs + run]))
            run++;
         if (run > best_run)
            best_run = run;
      }
   }

   float ordered_ratio = (float)ordered / (float)nq;
   float contiguous_ratio = (float)best_run / (float)nq;
   return ordered_ratio > contiguous_ratio ? ordered_ratio : contiguous_ratio;
}

/* =============================================================================
 * Search Callback
 * ============================================================================= */

/* One fused candidate: a chunk scored by the semantic channel, the lexical (BM25)
 * channel, or both, plus the phrase bonus.  text/filename point into the backing
 * chunks[] or lexical-hit arrays (valid for the function's lifetime). */
typedef struct {
   int64_t chunk_id;
   const char *text;
   const char *filename;
   int chunk_index;
   int64_t created_at;
   float cosine; /* pure cosine, 0 if lexical-only */
   float bm25;   /* normalized [0,1], 0 if semantic-only */
   float hybrid; /* fused score (sorted on) */
} cand_t;

static int cand_compare_cosine(const void *a, const void *b) {
   float ca = ((const cand_t *)a)->cosine, cb = ((const cand_t *)b)->cosine;
   return (cb > ca) - (cb < ca);
}

static int cand_compare_hybrid(const void *a, const void *b) {
   float ha = ((const cand_t *)a)->hybrid, hb = ((const cand_t *)b)->hybrid;
   return (hb > ha) - (hb < ha);
}

static char *doc_search_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: no search query provided.");

   int user_id = tool_get_current_user_id();
   int dims = embedding_engine_dims();

   if (dims <= 0)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: embedding engine not initialized.");

   /* Embed the query */
   float *query_vec = malloc((size_t)dims * sizeof(float));
   if (!query_vec)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");

   int out_dims = 0;
   if (embedding_engine_embed(value, query_vec, dims, &out_dims) != 0 || out_dims != dims) {
      free(query_vec);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: failed to generate query embedding.");
   }
   float query_norm = embedding_engine_l2_norm(query_vec, dims);

   /* Load all accessible chunks */
   int max_chunks = DOC_SEARCH_MAX_CHUNKS;
   document_chunk_t *chunks = calloc((size_t)max_chunks, sizeof(document_chunk_t));
   float *emb_buf = malloc((size_t)max_chunks * (size_t)dims * sizeof(float));
   if (!chunks || !emb_buf) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");
   }

   int chunk_count = 0;
   if (document_db_chunk_search_load(user_id, chunks, emb_buf, dims, max_chunks, &chunk_count) !=
           SUCCESS ||
       chunk_count <= 0) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      return strdup("No documents indexed. Upload documents via the WebUI first.");
   }

   /* Parse the query once for temporal expressions.  Skip the work when the
    * feature is disabled (temporal_weight == 0), matching the facts path. */
   time_query_t tq = { 0 };
   float temporal_weight = g_config.memory.temporal_weight;
   if (temporal_weight > 0.0f)
      time_query_parse(value, (int64_t)time(NULL), &tq);

   /* ---- Channel 2: lexical (BM25), its OWN candidate set (NOT a re-rank of the
    * semantic top-K).  A chunk the semantic channel buries still surfaces here. */
   doc_bm25_hit_t *lex = calloc(DOC_SEARCH_BM25_CAND_LIMIT, sizeof(doc_bm25_hit_t));
   float *lex_scores = calloc(DOC_SEARCH_BM25_CAND_LIMIT, sizeof(float));
   int lex_count = 0;
   if (lex && lex_scores) {
      document_db_chunk_search_bm25(user_id, value, g_config.documents.fts_label_weight,
                                    g_config.documents.fts_body_weight, lex, lex_scores,
                                    DOC_SEARCH_BM25_CAND_LIMIT, &lex_count);
   }

   /* ---- Fuse.  Candidate pool = every loaded semantic chunk (+ any lexical-only
    * hit not present in the loaded set, possible only when the semantic load was
    * capped on a very large corpus). */
   cand_t *cand = calloc((size_t)chunk_count + (size_t)lex_count, sizeof(cand_t));
   if (!cand) {
      free(query_vec);
      free(chunks);
      free(emb_buf);
      free(lex);
      free(lex_scores);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");
   }

   const float vec_w = g_config.documents.hybrid_vector_weight;
   const float kw_w = g_config.documents.hybrid_keyword_weight;
   const float phrase_w = g_config.documents.phrase_bonus_weight;

   int ncand = 0;
   for (int i = 0; i < chunk_count; i++) {
      float cosine = embedding_engine_cosine_with_norms(query_vec, chunks[i].embedding, dims,
                                                        query_norm, chunks[i].embedding_norm);
      cand[ncand].chunk_id = chunks[i].id;
      cand[ncand].text = chunks[i].text;
      cand[ncand].filename = chunks[i].doc_filename;
      cand[ncand].chunk_index = chunks[i].chunk_index;
      cand[ncand].created_at = chunks[i].created_at;
      cand[ncand].cosine = cosine;
      cand[ncand].bm25 = 0.0f;
      ncand++;
   }
   free(query_vec);

   /* Merge lexical hits by chunk id: set bm25 on the existing cand, or append a
    * lexical-only cand (cosine 0). */
   for (int j = 0; j < lex_count; j++) {
      int found = -1;
      for (int k = 0; k < ncand; k++) {
         if (cand[k].chunk_id == lex[j].id) {
            found = k;
            break;
         }
      }
      if (found >= 0) {
         cand[found].bm25 = lex_scores[j];
      } else {
         cand[ncand].chunk_id = lex[j].id;
         cand[ncand].text = lex[j].text;
         cand[ncand].filename = lex[j].filename;
         cand[ncand].chunk_index = lex[j].chunk_index;
         cand[ncand].created_at = lex[j].created_at;
         cand[ncand].cosine = 0.0f;
         cand[ncand].bm25 = lex_scores[j];
         ncand++;
      }
   }

   /* Base hybrid (pre-phrase): vec*cosine + kw*bm25 (+ temporal proximity). */
   for (int i = 0; i < ncand; i++) {
      float h = vec_w * cand[i].cosine + kw_w * cand[i].bm25;
      if (tq.found && cand[i].created_at > 0)
         h += temporal_weight * time_query_proximity(&tq, cand[i].created_at);
      cand[i].hybrid = h;
   }

   /* Phrase bonus over union(semantic top-N by cosine, ALL lexical hits) (M-4):
    * sort by cosine to find the top-N; bm25>0 cands are always eligible so an
    * exact-label note that ranks below the semantic top-N still gets its bonus. */
   query_words_t qw;
   tokenize_query(value, &qw);
   if (phrase_w > 0.0f && qw.count > 0) {
      qsort(cand, (size_t)ncand, sizeof(cand_t), cand_compare_cosine);
      for (int i = 0; i < ncand; i++) {
         if (i >= DOC_SEARCH_PHRASE_TOPN && cand[i].bm25 <= 0.0f)
            continue;
         /* Label (filename) AND body — the label phrase is the strongest
          * exact-retrieval signal; take the max. */
         float pb_label = phrase_bonus(cand[i].filename, &qw);
         float pb_body = phrase_bonus(cand[i].text, &qw);
         cand[i].hybrid += phrase_w * (pb_label > pb_body ? pb_label : pb_body);
      }
   }

   /* Final ranking by fused score. */
   qsort(cand, (size_t)ncand, sizeof(cand_t), cand_compare_hybrid);

   const float min_score = g_config.documents.search_min_score;
   int total_matches = 0;
   for (int i = 0; i < ncand; i++) {
      if (cand[i].hybrid >= min_score)
         total_matches++;
   }

   /* Format top results with token budget. */
   int max_results = DOC_SEARCH_MAX_RESULTS;
   int token_budget = DOC_SEARCH_MAX_CONTEXT_TOKENS;
   int result_buf_size = token_budget * 5; /* ~5 chars per token, generous */
   char *result = malloc((size_t)result_buf_size);
   if (!result) {
      free(chunks);
      free(emb_buf);
      free(lex);
      free(lex_scores);
      free(cand);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");
   }

   if (total_matches == 0) {
      free(chunks);
      free(emb_buf);
      free(lex);
      free(lex_scores);
      free(cand);
      free(result);
      return strdup("No relevant documents found for this query.");
   }

   int pos = 0;
   int shown = 0;
   pos += snprintf(result + pos, (size_t)(result_buf_size - pos),
                   "DOCUMENT SEARCH RESULTS (showing up to %d of %d matches):\n", max_results,
                   total_matches);

   for (int i = 0; i < ncand && shown < max_results; i++) {
      if (cand[i].hybrid < min_score)
         break;

      int chunk_tokens = ((int)strlen(cand[i].text) + 3) / 4;
      if (shown > 0 && chunk_tokens > token_budget)
         break; /* Would exceed budget */

      shown++;
      token_budget -= chunk_tokens;

      pos += snprintf(result + pos, (size_t)(result_buf_size - pos),
                      "\n[%d] (score: %.2f) %s, chunk %d:\n%s\n", shown, cand[i].hybrid,
                      cand[i].filename, cand[i].chunk_index + 1, cand[i].text);

      if (pos >= result_buf_size - 256)
         break;
   }

   int omitted = total_matches - shown;
   if (omitted > 0) {
      snprintf(result + pos, (size_t)(result_buf_size - pos),
               "\n[%d additional match%s omitted — token budget reached. "
               "User can ask for more detail.]",
               omitted, omitted == 1 ? "" : "es");
   }

   free(chunks);
   free(emb_buf);
   free(lex);
   free(lex_scores);
   free(cand);
   return result;
}
