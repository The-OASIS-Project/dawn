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
 * Web Search Module - SearXNG Integration
 *
 * Provides web search capability via a local SearXNG instance.
 */

#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

#include <stddef.h>

/* Forward declaration — callers that use web_search_query_images_raw() include json-c themselves */
struct json_object;

#define SEARXNG_DEFAULT_URL "http://localhost:8384"
#define SEARXNG_MAX_RESULTS 8
#define SEARXNG_TIMEOUT_SEC 10
#define SEARXNG_SNIPPET_LEN 600
#define SEARCH_URL_MAX_LEN 2048

/**
 * @brief Search type enum for different search categories
 */
typedef enum {
   SEARCH_TYPE_WEB,         // General web search (default)
   SEARCH_TYPE_NEWS,        // News articles (categories=news)
   SEARCH_TYPE_FACTS,       // Wikipedia infoboxes (engines=wikipedia)
   SEARCH_TYPE_SCIENCE,     // Scientific content (categories=science)
   SEARCH_TYPE_IT,          // Tech/programming (categories=it)
   SEARCH_TYPE_SOCIAL,      // Social media (categories=social media)
   SEARCH_TYPE_QA,          // Q&A sites (categories=q&a)
   SEARCH_TYPE_DICTIONARY,  // Definitions (categories=dictionaries)
   SEARCH_TYPE_PAPERS,      // Academic papers (categories=scientific publications)
   SEARCH_TYPE_IMAGES       // Image search (categories=images, safesearch=1)
} search_type_t;

/**
 * @brief Search result structure
 *
 * Shared across all search providers (SearXNG, Tavily, future). Field name
 * `searxng_score` is retained for source compatibility but the value
 * semantics are provider-specific:
 *   - SearXNG: cross-engine confidence, typically 0.0–3.0+
 *   - Tavily:  per-result relevance, 0.0–1.0
 * The reranker in score_result() clamps this to [0, 100] so both ranges
 * feed the same scoring pipeline without overflow.
 */
typedef struct {
   char *title;
   char *url;
   char *snippet;
   char *engine;
   char *published_date; /* ISO 8601 from provider, or NULL */
   float searxng_score;  /* Provider-specific relevance — see struct docstring */
} search_result_t;

/**
 * @brief Search response structure
 */
typedef struct {
   search_result_t *results;
   int count;
   float query_time_sec;
   char *error;
} search_response_t;

/**
 * @brief Initialize the web search module
 * @param searxng_url Base URL for SearXNG (NULL for default localhost:8384)
 * @return 0 on success, 1 on failure
 */
int web_search_init(const char *searxng_url);

/**
 * @brief Perform a web search
 * @param query Search query string
 * @param max_results Maximum results to return (0 for default)
 * @return Search response (caller must free with web_search_free_response)
 */
search_response_t *web_search_query(const char *query, int max_results);

/**
 * @brief Perform a typed search with optional time filtering
 * @param query Search query string
 * @param max_results Maximum results to return (0 for default)
 * @param type Search type (see search_type_t enum)
 * @param time_range Time filter: "day", "week", "month", "year", or NULL
 * @return Search response (caller must free with web_search_free_response)
 */
search_response_t *web_search_query_typed(const char *query,
                                          int max_results,
                                          search_type_t type,
                                          const char *time_range);

/**
 * @brief Format search results for LLM consumption
 * @param response Search response
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written, or 0 on error (invalid params)
 */
int web_search_format_for_llm(const search_response_t *response, char *buffer, size_t buffer_size);

/**
 * @brief Free search response
 * @param response Response to free
 */
void web_search_free_response(search_response_t *response);

/**
 * @brief Perform an image search and return raw JSON response
 *
 * Returns the parsed json_object* root from SearXNG. Caller owns the reference
 * and must call json_object_put() when done. Image results have fields:
 * img_src, thumbnail_src, title, source, resolution.
 *
 * SearXNG returns one page of results; the caller takes as many as it needs.
 *
 * @param query Search query string
 * @return Parsed JSON root (caller must json_object_put), or NULL on error
 */
struct json_object *web_search_query_images_raw(const char *query);

/**
 * @brief Cleanup web search module
 */
void web_search_cleanup(void);

/**
 * @brief Check if web search module is initialized
 * @return 1 if initialized, 0 otherwise
 */
int web_search_is_initialized(void);

#endif /* WEB_SEARCH_H */
