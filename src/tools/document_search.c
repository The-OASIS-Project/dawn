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
 * Embeds the query, ranks the accessible chunks (document_rank: semantic +
 * keyword + phrase), and returns the top results with citations.
 */

#include "tools/document_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_rank.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_SEARCH_MAX_RESULTS 5
#define DOC_SEARCH_MAX_CONTEXT_TOKENS 2000
/* Semantic candidates: the top chunks by cosine over the whole accessible
 * corpus.  A chunk below this with no lexical hit can't reach the results. */
#define DOC_SEARCH_SEMANTIC_TOPN 200
/* Keyword (BM25) hits considered, and the top-by-cosine count the phrase bonus
 * is computed over (every keyword hit gets it too; see document_rank.h). */
#define DOC_SEARCH_BM25_CAND_LIMIT 50
#define DOC_SEARCH_PHRASE_TOPN 50

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
 * Search Callback
 * ============================================================================= */

/* The first DOC_SEARCH_MAX_RESULTS matches at or above @p min_score whose text
 * still loads, best first.  Text is loaded a batch at a time; each chunk
 * deleted since it was ranked is taken off @p total_matches. */
static int collect_results(int user_id,
                           document_ranking_t *ranking,
                           float min_score,
                           document_ranked_t **out,
                           int *n_out,
                           int *total_matches) {
   *n_out = 0;
   int next = 0;
   while (*n_out < DOC_SEARCH_MAX_RESULTS) {
      document_ranked_t *batch[DOC_SEARCH_MAX_RESULTS];
      int n_batch = 0;
      while (next < ranking->count && n_batch < DOC_SEARCH_MAX_RESULTS - *n_out &&
             ranking->items[next].hybrid >= min_score) {
         batch[n_batch++] = &ranking->items[next++];
      }
      if (n_batch == 0) {
         break;
      }
      if (document_ranking_load_text(user_id, ranking, batch, n_batch) != SUCCESS) {
         return FAILURE;
      }
      for (int i = 0; i < n_batch; i++) {
         if (batch[i]->text) {
            out[(*n_out)++] = batch[i];
         } else {
            (*total_matches)--;
         }
      }
   }
   return SUCCESS;
}

static char *doc_search_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: no search query provided.");

   int user_id = tool_get_current_user_id();
   if (user_id <= 0)
      return strdup(TOOL_GUEST_REFUSAL);
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

   const document_rank_opts_t opts = {
      .semantic_top = DOC_SEARCH_SEMANTIC_TOPN,
      .lexical_limit = DOC_SEARCH_BM25_CAND_LIMIT,
      .phrase_top = DOC_SEARCH_PHRASE_TOPN,
      .body_phrase = true,
      .temporal = true,
   };
   document_ranking_t ranking;
   const int rank_rc = document_rank_hybrid(user_id, value, query_vec, dims, &opts, &ranking);
   free(query_vec);
   query_vec = NULL;
   if (rank_rc != SUCCESS) {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: could not rank documents.");
   }
   if (ranking.stats.pool == 0 && ranking.count == 0) {
      document_ranking_free(&ranking);
      return strdup("No documents indexed. Upload documents via the WebUI first.");
   }

   const float min_score = g_config.documents.search_min_score;
   int total_matches = 0;
   for (int i = 0; i < ranking.count; i++) {
      if (ranking.items[i].hybrid >= min_score)
         total_matches++;
   }
   if (total_matches == 0) {
      document_ranking_free(&ranking);
      return strdup("No relevant documents found for this query.");
   }

   /* The results that can be shown, with their text.  A chunk deleted since
    * it was ranked is no longer a match. */
   document_ranked_t *results[DOC_SEARCH_MAX_RESULTS];
   int n_results = 0;
   if (collect_results(user_id, &ranking, min_score, results, &n_results, &total_matches) !=
       SUCCESS) {
      document_ranking_free(&ranking);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: could not load document text.");
   }
   if (n_results == 0) {
      document_ranking_free(&ranking);
      return strdup("No relevant documents found for this query.");
   }

   /* Format top results with token budget. */
   int token_budget = DOC_SEARCH_MAX_CONTEXT_TOKENS;
   int result_buf_size = token_budget * 5; /* ~5 chars per token, generous */
   char *result = malloc((size_t)result_buf_size);
   if (!result) {
      document_ranking_free(&ranking);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");
   }

   int pos = 0;
   int shown = 0;
   pos += snprintf(result + pos, (size_t)(result_buf_size - pos),
                   "DOCUMENT SEARCH RESULTS (showing up to %d of %d matches):\n",
                   DOC_SEARCH_MAX_RESULTS, total_matches);

   for (int i = 0; i < n_results; i++) {
      const document_ranked_t *it = results[i];
      int chunk_tokens = ((int)strlen(it->text) + 3) / 4;
      if (shown > 0 && chunk_tokens > token_budget)
         break; /* Would exceed budget */

      shown++;
      token_budget -= chunk_tokens;

      pos += snprintf(result + pos, (size_t)(result_buf_size - pos),
                      "\n[%d] (score: %.2f) %s, chunk %d:\n%s\n", shown, it->hybrid, it->filename,
                      it->chunk_index + 1, it->text);

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

   document_ranking_free(&ranking);
   return result;
}
