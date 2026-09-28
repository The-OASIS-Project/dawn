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
 * Hybrid ranking of a user's document chunks: one definition of "relevant
 * document" shared by the document_search tool, per-turn context injection
 * and recall.
 */

#ifndef DOCUMENT_RANK_H
#define DOCUMENT_RANK_H

#include <stdbool.h>
#include <stdint.h>

#include "tools/document_db.h"
#include "tools/document_embed_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Most keyword (BM25) hits one ranking considers. */
#define DOCUMENT_RANK_LEXICAL_MAX 64

/** Most distinct content words taken from a query. */
#define DOCUMENT_QUERY_TERMS_MAX 32
#define DOCUMENT_QUERY_TERM_LEN 48

/** How much of the corpus a ranking looks at. */
typedef struct {
   int semantic_top;  /**< chunks taken from the semantic channel (<= DOCUMENT_RANK_MAX) */
   int lexical_limit; /**< keyword hits considered (<= DOCUMENT_RANK_LEXICAL_MAX) */
   int phrase_top;    /**< phrase bonus over this many best by cosine, plus every keyword hit */
   bool body_phrase;  /**< phrase bonus also over chunk text (loads it for those chunks);
                           otherwise over document labels only */
   bool temporal;     /**< add proximity to a time named in the query (off where recency is
                           weighed elsewhere) */
} document_rank_opts_t;

/** One ranked chunk.  filename and text belong to the ranking. */
typedef struct {
   int64_t chunk_id;
   int64_t document_id;
   int chunk_index;
   int64_t created_at;
   const char *filename;
   const char *text; /**< NULL until loaded (document_ranking_load_text) */
   bool has_cosine;  /**< false without a query embedding, or when the chunk has none */
   float cosine;     /**< real cosine, keyword-only hits included; 0 when !has_cosine */
   float bm25;       /**< normalized keyword score [0, 1]; 0 when not a keyword hit */
   float hybrid;     /**< fused score, the ranking order */
   int label_terms;  /**< distinct query content words found in the document's label */
} document_ranked_t;

struct document_ranking_store;

/** A ranking, best first.  Free with document_ranking_free(). */
typedef struct {
   document_ranked_t *items;
   int count;
   document_rank_stats_t stats;          /**< semantic pool (pool 0 without a query embedding) */
   int query_terms;                      /**< distinct content words in the query */
   struct document_ranking_store *store; /**< private: what items point into */
} document_ranking_t;

/** A query's distinct content words, stemmed. */
typedef struct {
   char term[DOCUMENT_QUERY_TERMS_MAX][DOCUMENT_QUERY_TERM_LEN];
   int count;
} document_query_terms_t;

/**
 * @brief Rank the chunks @p user_id can access against a query
 *
 * Candidates are the top chunks by cosine plus every keyword (BM25) hit, so a
 * chunk either channel finds is considered.  Each is scored
 * `hybrid_vector_weight * cosine + hybrid_keyword_weight * bm25`, plus the
 * phrase bonus (query words in order in the label, or the text with
 * @p opts->body_phrase) and, with @p opts->temporal, proximity to a time the
 * query names.  Chunk text is not loaded unless the body phrase bonus needs
 * it; load it for the chunks you keep with document_ranking_load_text().
 *
 * @param query  Query text (keyword channel and label words)
 * @param qvec   Query embedding, or NULL for keywords only
 * @param dims   Its dimension (ignored when @p qvec is NULL)
 * @param out    [out] The ranking; empty on FAILURE
 * @return SUCCESS or FAILURE
 */
int document_rank_hybrid(int user_id,
                         const char *query,
                         const float *qvec,
                         int dims,
                         const document_rank_opts_t *opts,
                         document_ranking_t *out);

/**
 * @brief Load the text of chosen items (those without it)
 *
 * An item whose chunk was deleted meanwhile keeps text NULL.
 *
 * @param items  Pointers into @p ranking's items
 * @param n      How many
 * @return SUCCESS or FAILURE
 */
int document_ranking_load_text(int user_id,
                               document_ranking_t *ranking,
                               document_ranked_t *const *items,
                               int n);

/** Free a ranking's storage (safe on an empty or failed one). */
void document_ranking_free(document_ranking_t *ranking);

/**
 * @brief A query's distinct content words, stemmed
 *
 * Common function words ("the", "what", "about", …) are left out: they
 * carry nothing about which document is meant.  Words are runs of letters,
 * digits and non-ASCII characters, so "Tax_Return" is two words and
 * "résumé" one, on both sides of document_label_terms().
 */
void document_query_terms(const char *query, document_query_terms_t *out);

/** How many of @p terms appear, stemmed, among the words of @p label. */
int document_label_terms(const document_query_terms_t *terms, const char *label);

/**
 * @brief Whether a document's label names what the query asks for
 *
 * At least two of the query's content words, or its only one, appear in the
 * label.  This is a count of words, so it means the same thing in every
 * user's corpus (a keyword score's scale depends on the corpus).  Words found
 * only in a chunk's body don't count: an ordinary sentence shares some word
 * with most documents.
 */
bool document_label_matches(int label_terms, int query_terms);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_RANK_H */
