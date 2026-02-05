/* Enable GNU extensions for strcasestr */
#define _GNU_SOURCE

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
 * Implements web search capability via a local SearXNG instance using
 * libcurl for HTTP requests and json-c for response parsing.
 */

#include "tools/web_search.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* For strcasestr */

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "tools/string_utils.h"

// =============================================================================
// Module State (thread-safe with mutex protection)
// =============================================================================

static char *searxng_base_url = NULL;
static int module_initialized = 0;
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;

// =============================================================================
// Lifecycle
// =============================================================================

int web_search_init(const char *searxng_url) {
   pthread_mutex_lock(&module_mutex);

   if (module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      LOG_WARNING("web_search: Already initialized");
      return 0;
   }

   // Store base URL (or use default)
   if (searxng_url) {
      searxng_base_url = strdup(searxng_url);
   } else {
      searxng_base_url = strdup(SEARXNG_DEFAULT_URL);
   }

   if (!searxng_base_url) {
      pthread_mutex_unlock(&module_mutex);
      LOG_ERROR("web_search: Failed to allocate URL string");
      return 1;
   }

   // Note: curl_global_init() is called in main() - not here
   // This avoids conflicts with other modules using libcurl

   module_initialized = 1;
   pthread_mutex_unlock(&module_mutex);
   LOG_INFO("web_search: Initialized with URL %s", searxng_base_url);
   return 0;
}

void web_search_cleanup(void) {
   pthread_mutex_lock(&module_mutex);

   if (!module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      return;
   }

   if (searxng_base_url) {
      free(searxng_base_url);
      searxng_base_url = NULL;
   }

   // Note: curl_global_cleanup() is called in main() - not here
   module_initialized = 0;
   pthread_mutex_unlock(&module_mutex);
   LOG_INFO("web_search: Cleanup complete");
}

int web_search_is_initialized(void) {
   // Use atomic load for thread-safe access without full mutex lock
   return __atomic_load_n(&module_initialized, __ATOMIC_ACQUIRE);
}

// =============================================================================
// Helper Functions
// =============================================================================

static char *truncate_string(const char *str, size_t max_len) {
   if (!str) {
      return NULL;
   }

   size_t len = strlen(str);
   if (len <= max_len) {
      // Avoid double strlen by using memcpy with known length
      char *copy = malloc(len + 1);
      if (copy) {
         memcpy(copy, str, len + 1);
      }
      return copy;
   }

   // Allocate for truncated string + "..." + null terminator
   char *truncated = malloc(max_len + 4);
   if (!truncated) {
      return NULL;
   }

   // Use memcpy instead of strncpy (we know the exact length)
   memcpy(truncated, str, max_len);
   // Append "..." directly (4 bytes including null terminator)
   memcpy(truncated + max_len, "...", 4);
   return truncated;
}

static char *get_json_string(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (json_object_object_get_ex(obj, key, &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         return strdup(str);
      }
   }
   return NULL;
}

/* Note: Host extraction moved to string_utils.h as extract_url_host() */

/**
 * @brief Score a search result for reranking
 *
 * Scoring factors:
 * - Has title: +2
 * - Has snippet: +2
 * - Known quality engine (wikipedia, github): +2
 * - Query keyword match in title: +5
 * - Query keyword match in snippet: +3
 *
 * @param r Search result to score
 * @param query Original search query (for keyword matching)
 * @return Score (higher = better)
 */
static int score_result(const search_result_t *r, const char *query) {
   int score = 0;

   if (r->title && r->title[0]) {
      score += 2;
   }
   if (r->snippet && r->snippet[0]) {
      score += 2;
   }

   // Boost known quality engines
   if (r->engine) {
      if (strcmp(r->engine, "wikipedia") == 0) {
         score += 3;
      } else if (strcmp(r->engine, "github") == 0) {
         score += 2;
      } else if (strcmp(r->engine, "stackoverflow") == 0) {
         score += 2;
      }
   }

   // Check for query keyword matches (5+ char tokens only)
   // Note: Buffer sizes reduced for embedded stack efficiency (512KB threads).
   // Matching first 128 chars is sufficient for relevance scoring.
   if (query && (r->title || r->snippet)) {
      // Simple case-insensitive substring check for the query
      char query_lower[128];
      size_t qlen = strlen(query);
      if (qlen >= sizeof(query_lower)) {
         qlen = sizeof(query_lower) - 1;
      }
      for (size_t i = 0; i < qlen; i++) {
         query_lower[i] = (char)tolower((unsigned char)query[i]);
      }
      query_lower[qlen] = '\0';

      // Only match if query is 5+ characters
      if (qlen >= 5) {
         if (r->title) {
            char title_lower[128];
            size_t tlen = strlen(r->title);
            if (tlen >= sizeof(title_lower)) {
               tlen = sizeof(title_lower) - 1;
            }
            for (size_t i = 0; i < tlen; i++) {
               title_lower[i] = (char)tolower((unsigned char)r->title[i]);
            }
            title_lower[tlen] = '\0';
            if (strstr(title_lower, query_lower)) {
               score += 5;
            }
         }
         if (r->snippet) {
            char snippet_lower[192];
            size_t slen = strlen(r->snippet);
            if (slen >= sizeof(snippet_lower)) {
               slen = sizeof(snippet_lower) - 1;
            }
            for (size_t i = 0; i < slen; i++) {
               snippet_lower[i] = (char)tolower((unsigned char)r->snippet[i]);
            }
            snippet_lower[slen] = '\0';
            if (strstr(snippet_lower, query_lower)) {
               score += 3;
            }
         }
      }
   }

   return score;
}

/**
 * @brief Comparison function for qsort - sort results by score descending
 */
typedef struct {
   search_result_t result;
   int score;
} scored_result_t;

static int compare_scored_results(const void *a, const void *b) {
   const scored_result_t *ra = (const scored_result_t *)a;
   const scored_result_t *rb = (const scored_result_t *)b;
   return rb->score - ra->score;  // Descending order
}

/**
 * @brief Check if a title should be filtered based on configured title_filters
 *
 * Uses case-insensitive substring matching against all configured filters.
 *
 * @param title Result title to check
 * @return true if title should be filtered (excluded), false to keep
 */
static bool should_filter_title(const char *title) {
   if (!title || title[0] == '\0') {
      return false;
   }

   const dawn_config_t *config = config_get();
   if (!config || config->search.title_filters_count == 0) {
      return false;
   }

   for (int i = 0; i < config->search.title_filters_count; i++) {
      if (config->search.title_filters[i][0] != '\0') {
         if (strcasestr(title, config->search.title_filters[i]) != NULL) {
            LOG_INFO("web_search: Filtering result with title containing '%s': %s",
                     config->search.title_filters[i], title);
            return true;
         }
      }
   }

   return false;
}

// =============================================================================
// Search Query (internal implementation)
// =============================================================================

/**
 * @brief Max results per host for deduplication
 */
#define MAX_RESULTS_PER_HOST 2

/**
 * @brief Max results to process from SearXNG response
 *
 * Safety limit to prevent memory exhaustion if server returns huge arrays.
 * We only need to process enough to find max_results good candidates after
 * deduplication and reranking.
 */
#define MAX_RESULTS_TO_PROCESS 100

/**
 * @brief Parse results from the standard "results" array with deduplication and reranking
 *
 * Applies host-based deduplication (max 2 results per domain) and reranks
 * results by quality score before returning.
 *
 * @param results_array JSON array of search results
 * @param response Response structure to populate
 * @param max_results Maximum results to return
 * @param query Original search query (for relevance scoring)
 */
static void parse_results_array(struct json_object *results_array,
                                search_response_t *response,
                                int max_results,
                                const char *query) {
   int total_count = json_object_array_length(results_array);
   if (total_count == 0) {
      return;
   }

   // Cap total_count to prevent memory exhaustion from malicious responses
   if (total_count > MAX_RESULTS_TO_PROCESS) {
      LOG_WARNING("web_search: Capping results from %d to %d", total_count, MAX_RESULTS_TO_PROCESS);
      total_count = MAX_RESULTS_TO_PROCESS;
   }

   // Allocate temporary storage for all results (before dedup/rerank)
   scored_result_t *temp_results = calloc(total_count, sizeof(scored_result_t));
   if (!temp_results) {
      LOG_ERROR("web_search: Failed to allocate temp results array");
      response->error = strdup("Memory allocation failed");
      return;
   }

   // Track host counts for deduplication (128 bytes per host sufficient)
   typedef struct {
      char host[128];
      int count;
   } host_count_t;
   host_count_t *host_counts = calloc(total_count, sizeof(host_count_t));
   int unique_hosts = 0;

   if (!host_counts) {
      LOG_ERROR("web_search: Failed to allocate host counts");
      free(temp_results);
      response->error = strdup("Memory allocation failed");
      return;
   }

   int accepted_count = 0;

   // First pass: parse all results with host deduplication
   for (int i = 0; i < total_count && accepted_count < max_results * 2; i++) {
      struct json_object *item = json_object_array_get_idx(results_array, i);
      if (!item) {
         continue;
      }

      // Get URL for host deduplication
      char *url = get_json_string(item, "url");
      if (!url) {
         continue;
      }

      // Extract host for dedup check (128 bytes sufficient for hostnames)
      char host[128];
      extract_url_host(url, host, sizeof(host));

      // Check/update host count
      int host_idx = -1;
      for (int h = 0; h < unique_hosts; h++) {
         if (strcmp(host_counts[h].host, host) == 0) {
            host_idx = h;
            break;
         }
      }

      if (host_idx == -1) {
         // New host
         if (unique_hosts < total_count) {
            strncpy(host_counts[unique_hosts].host, host, sizeof(host_counts[0].host) - 1);
            host_counts[unique_hosts].host[sizeof(host_counts[0].host) - 1] = '\0';
            host_counts[unique_hosts].count = 1;
            unique_hosts++;
         }
      } else {
         // Existing host - check limit
         if (host_counts[host_idx].count >= MAX_RESULTS_PER_HOST) {
            free(url);
            continue;  // Skip this result (host limit reached)
         }
         host_counts[host_idx].count++;
      }

      // Get title and check title filters before accepting
      char *title = get_json_string(item, "title");
      if (should_filter_title(title)) {
         free(url);
         free(title);
         continue;  // Skip this result (filtered title)
      }

      // Accept this result
      temp_results[accepted_count].result.url = url;
      temp_results[accepted_count].result.title = title;
      temp_results[accepted_count].result.engine = get_json_string(item, "engine");

      // Get snippet (called "content" in SearXNG)
      char *full_snippet = get_json_string(item, "content");
      if (full_snippet) {
         temp_results[accepted_count].result.snippet = truncate_string(full_snippet,
                                                                       SEARXNG_SNIPPET_LEN);
         free(full_snippet);
      }

      // Score the result for reranking
      temp_results[accepted_count].score = score_result(&temp_results[accepted_count].result,
                                                        query);
      accepted_count++;
   }

   free(host_counts);

   if (accepted_count == 0) {
      free(temp_results);
      return;
   }

   // Sort by score (descending)
   qsort(temp_results, accepted_count, sizeof(scored_result_t), compare_scored_results);

   // Copy top results to response
   int final_count = (accepted_count > max_results) ? max_results : accepted_count;
   response->results = calloc(final_count, sizeof(search_result_t));
   if (!response->results) {
      // Free all allocated strings
      for (int i = 0; i < accepted_count; i++) {
         free(temp_results[i].result.title);
         free(temp_results[i].result.url);
         free(temp_results[i].result.snippet);
         free(temp_results[i].result.engine);
      }
      free(temp_results);
      LOG_ERROR("web_search: Failed to allocate final results array");
      response->error = strdup("Memory allocation failed");
      return;
   }

   for (int i = 0; i < final_count; i++) {
      response->results[i] = temp_results[i].result;
      response->count++;
   }

   // Free remaining results that weren't used
   for (int i = final_count; i < accepted_count; i++) {
      free(temp_results[i].result.title);
      free(temp_results[i].result.url);
      free(temp_results[i].result.snippet);
      free(temp_results[i].result.engine);
   }

   free(temp_results);
}

/**
 * @brief Parse results from the "infoboxes" array (Wikipedia facts)
 */
static void parse_infoboxes_array(struct json_object *infoboxes_array,
                                  search_response_t *response,
                                  int max_results) {
   int infobox_count = json_object_array_length(infoboxes_array);
   if (infobox_count == 0) {
      return;
   }

   if (infobox_count > max_results) {
      infobox_count = max_results;
   }

   response->results = calloc(infobox_count, sizeof(search_result_t));
   if (!response->results) {
      LOG_ERROR("web_search: Failed to allocate infobox results array");
      response->error = strdup("Memory allocation failed");
      return;
   }

   for (int i = 0; i < infobox_count; i++) {
      struct json_object *item = json_object_array_get_idx(infoboxes_array, i);
      if (!item) {
         continue;
      }

      // Infobox structure: {infobox: title, content: description, id: url, engine: "wikipedia"}
      response->results[response->count].title = get_json_string(item, "infobox");
      response->results[response->count].engine = get_json_string(item, "engine");

      // Get URL from id field or from urls array
      char *url = get_json_string(item, "id");
      if (!url) {
         struct json_object *urls_array = NULL;
         if (json_object_object_get_ex(item, "urls", &urls_array) &&
             json_object_array_length(urls_array) > 0) {
            struct json_object *first_url = json_object_array_get_idx(urls_array, 0);
            if (first_url) {
               url = get_json_string(first_url, "url");
            }
         }
      }
      response->results[response->count].url = url;

      // Get content as snippet (may be longer than regular snippets)
      char *full_content = get_json_string(item, "content");
      if (full_content) {
         // Use larger snippet length for facts (more useful context)
         response->results[response->count].snippet = truncate_string(full_content, 500);
         free(full_content);
      }

      response->count++;
   }
}

// =============================================================================
// Search Query (public API)
// =============================================================================

search_response_t *web_search_query_typed(const char *query, int max_results, search_type_t type) {
   if (!module_initialized) {
      LOG_ERROR("web_search: Module not initialized");
      return NULL;
   }

   if (!query || query[0] == '\0') {
      LOG_ERROR("web_search: Empty query");
      return NULL;
   }

   if (max_results <= 0) {
      max_results = SEARXNG_MAX_RESULTS;
   }

   // Allocate response structure
   search_response_t *response = calloc(1, sizeof(search_response_t));
   if (!response) {
      LOG_ERROR("web_search: Failed to allocate response");
      return NULL;
   }

   // Create CURL handle
   CURL *curl = curl_easy_init();
   if (!curl) {
      LOG_ERROR("web_search: Failed to create CURL handle");
      response->error = strdup("Failed to initialize HTTP client");
      return response;
   }

   // URL-encode the query
   char *encoded_query = curl_easy_escape(curl, query, 0);
   if (!encoded_query) {
      LOG_ERROR("web_search: Failed to encode query");
      curl_easy_cleanup(curl);
      response->error = strdup("Failed to encode search query");
      return response;
   }

   // Build URL based on search type
   char url[SEARCH_URL_MAX_LEN];
   const char *type_str = "web";

   // Map search type to SearXNG category or engines parameter
   // Note: Some search types use engines= instead of categories= because
   // SearXNG category names are lowercase single words (e.g., "news", "science")
   // while multi-word categories like "social media" are not valid.
   const char *category = NULL;
   switch (type) {
      case SEARCH_TYPE_NEWS:
         category = "news";
         type_str = "news";
         break;
      case SEARCH_TYPE_FACTS:
         // Wikipedia uses engines parameter, not categories
         snprintf(url, sizeof(url), "%s/search?q=%s&format=json&engines=wikipedia",
                  searxng_base_url, encoded_query);
         type_str = "facts";
         curl_free(encoded_query);
         goto do_request;
      case SEARCH_TYPE_SCIENCE:
         category = "science";
         type_str = "science";
         break;
      case SEARCH_TYPE_IT:
         category = "it";
         type_str = "it";
         break;
      case SEARCH_TYPE_SOCIAL:
         // "social media" is not a valid SearXNG category - use engines
         snprintf(url, sizeof(url), "%s/search?q=%s&format=json&engines=reddit,twitter",
                  searxng_base_url, encoded_query);
         type_str = "social";
         curl_free(encoded_query);
         goto do_request;
      case SEARCH_TYPE_QA:
         // "q&a" is not a valid SearXNG category - use specific engines
         snprintf(url, sizeof(url), "%s/search?q=%s&format=json&engines=stackoverflow,superuser",
                  searxng_base_url, encoded_query);
         type_str = "q&a";
         curl_free(encoded_query);
         goto do_request;
      case SEARCH_TYPE_DICTIONARY:
         // Note: "dictionaries" may not be a valid category on all instances
         category = "general";
         type_str = "dictionary";
         break;
      case SEARCH_TYPE_PAPERS:
         // "scientific publications" is not valid - use academic engines
         snprintf(url, sizeof(url),
                  "%s/search?q=%s&format=json&engines=arxiv,google_scholar,semantic_scholar",
                  searxng_base_url, encoded_query);
         type_str = "papers";
         curl_free(encoded_query);
         goto do_request;
      case SEARCH_TYPE_WEB:
      default:
         type_str = "web";
         break;
   }

   // Build URL with optional category
   if (category) {
      char *encoded_category = curl_easy_escape(curl, category, 0);
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json&categories=%s", searxng_base_url,
               encoded_query, encoded_category);
      curl_free(encoded_category);
   } else {
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json", searxng_base_url, encoded_query);
   }
   curl_free(encoded_query);

do_request:
   LOG_INFO("web_search: Querying [%s] %s", type_str, url);

   // Set up response buffer with web search max capacity (512KB)
   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, CURL_BUFFER_MAX_WEB_SEARCH);

   // Configure CURL
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, SEARXNG_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "DAWN/1.0");

   // Perform request
   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      LOG_ERROR("web_search: Request failed: %s", curl_easy_strerror(res));
      curl_easy_cleanup(curl);
      curl_buffer_free(&buffer);
      response->error = strdup(curl_easy_strerror(res));
      return response;
   }

   // Check HTTP response code
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (http_code != 200) {
      LOG_ERROR("web_search: HTTP error %ld", http_code);
      curl_buffer_free(&buffer);
      char err_msg[64];
      snprintf(err_msg, sizeof(err_msg), "HTTP error %ld", http_code);
      response->error = strdup(err_msg);
      return response;
   }

   // Warn if response was truncated (but continue with partial data)
   if (buffer.truncated) {
      LOG_WARNING("web_search: Response truncated at %zu bytes (exceeded max capacity)",
                  buffer.size);
   }

   // Parse JSON response
   struct json_object *root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!root) {
      LOG_ERROR("web_search: Failed to parse JSON response");
      response->error = strdup("Invalid JSON response from search service");
      return response;
   }

   // For FACTS type, check infoboxes first (Wikipedia returns data there)
   if (type == SEARCH_TYPE_FACTS) {
      struct json_object *infoboxes_array = NULL;
      if (json_object_object_get_ex(root, "infoboxes", &infoboxes_array) &&
          json_object_array_length(infoboxes_array) > 0) {
         parse_infoboxes_array(infoboxes_array, response, max_results);
         if (response->count > 0) {
            json_object_put(root);
            LOG_INFO("web_search: Found %d infobox results", response->count);
            return response;
         }
      }
      // Fall through to check results array if no infoboxes
   }

   // Extract results array (standard path for WEB and NEWS, fallback for FACTS)
   struct json_object *results_array = NULL;
   if (!json_object_object_get_ex(root, "results", &results_array)) {
      LOG_WARNING("web_search: No 'results' field in response");
      json_object_put(root);
      if (type == SEARCH_TYPE_FACTS) {
         response->error = strdup("No Wikipedia data found for this query");
      } else {
         response->error = strdup("No results in response");
      }
      return response;
   }

   int result_count = json_object_array_length(results_array);
   if (result_count == 0) {
      LOG_INFO("web_search: No results found for query");
      json_object_put(root);
      return response;  // No error, just empty results
   }

   parse_results_array(results_array, response, max_results, query);

   json_object_put(root);
   LOG_INFO("web_search: Found %d results (deduplicated, reranked)", response->count);
   return response;
}

search_response_t *web_search_query(const char *query, int max_results) {
   return web_search_query_typed(query, max_results, SEARCH_TYPE_WEB);
}

// =============================================================================
// Format for LLM
// =============================================================================

int web_search_format_for_llm(const search_response_t *response, char *buffer, size_t buffer_size) {
   if (!response || !buffer || buffer_size == 0) {
      return 0;
   }

   if (response->error) {
      int n = snprintf(buffer, buffer_size, "Search failed: %s", response->error);
      return (n < 0) ? 0 : ((size_t)n >= buffer_size ? (int)buffer_size - 1 : n);
   }

   if (response->count == 0) {
      int n = snprintf(buffer, buffer_size, "No search results found.");
      return (n < 0) ? 0 : ((size_t)n >= buffer_size ? (int)buffer_size - 1 : n);
   }

   size_t written = 0;
   size_t remaining = buffer_size;
   int n;

/* Helper macro: write to buffer, update written/remaining, break on truncation */
#define SAFE_SNPRINTF(...)                                    \
   do {                                                       \
      n = snprintf(buffer + written, remaining, __VA_ARGS__); \
      if (n < 0)                                              \
         goto done;                                           \
      if ((size_t)n >= remaining) {                           \
         written = buffer_size - 1;                           \
         goto done;                                           \
      }                                                       \
      written += (size_t)n;                                   \
      remaining -= (size_t)n;                                 \
   } while (0)

   SAFE_SNPRINTF("Web search results:\n\n");

   for (int i = 0; i < response->count && remaining > 1; i++) {
      search_result_t *r = &response->results[i];

      SAFE_SNPRINTF("%d. %s", i + 1, r->title ? r->title : "(no title)");

      if (r->engine) {
         SAFE_SNPRINTF(" [%s]", r->engine);
      }

      SAFE_SNPRINTF("\n");

      if (r->snippet) {
         SAFE_SNPRINTF("   %s\n", r->snippet);
      }

      if (r->url) {
         SAFE_SNPRINTF("   URL: %s\n", r->url);
      }

      SAFE_SNPRINTF("\n");
   }

#undef SAFE_SNPRINTF

done:
   return (int)written;
}

// =============================================================================
// Free Response
// =============================================================================

void web_search_free_response(search_response_t *response) {
   if (!response) {
      return;
   }

   if (response->results) {
      for (int i = 0; i < response->count; i++) {
         if (response->results[i].title) {
            free(response->results[i].title);
         }
         if (response->results[i].url) {
            free(response->results[i].url);
         }
         if (response->results[i].snippet) {
            free(response->results[i].snippet);
         }
         if (response->results[i].engine) {
            free(response->results[i].engine);
         }
      }
      free(response->results);
   }

   if (response->error) {
      free(response->error);
   }

   free(response);
}
