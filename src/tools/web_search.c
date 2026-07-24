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
#include <time.h>

#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/curl_buffer.h"
#include "logging.h"
#include "tools/tavily_rate_limit.h"
#include "tools/url_snippet_cache.h"
#include "tools/web_search_tavily.h"
#include "utils/string_utils.h"

/* Publish every (url, snippet) pair from a successful search response to
 * the URL → snippet cache. Used by url_tool.c to substitute the
 * pre-extracted snippet when a chrome-dominated fetch is detected. */
static void publish_snippets_to_cache(const search_response_t *response) {
   if (!response) {
      return;
   }
   for (int i = 0; i < response->count; i++) {
      const search_result_t *r = &response->results[i];
      url_snippet_cache_put(r->url, r->snippet);
   }
}

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
      OLOG_WARNING("web_search: Already initialized");
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
      OLOG_ERROR("web_search: Failed to allocate URL string");
      return 1;
   }

   // Note: curl_global_init() is called in main() - not here
   // This avoids conflicts with other modules using libcurl

   /* Release-store to pair with the acquire-load in web_search_is_initialized()
    * (which reads without the mutex) — a plain store here races that load. */
   __atomic_store_n(&module_initialized, 1, __ATOMIC_RELEASE);
   pthread_mutex_unlock(&module_mutex);
   OLOG_INFO("web_search: Initialized with URL %s", searxng_base_url);
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
   OLOG_INFO("web_search: Cleanup complete");
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
 * @brief Free all dynamically allocated fields in a search result
 */
static void free_result_fields(search_result_t *r) {
   free(r->title);
   free(r->url);
   free(r->snippet);
   free(r->engine);
   free(r->published_date);
}

/**
 * @brief Parse ISO 8601 date string and return epoch time
 * @return epoch time, or 0 on parse failure
 */
static time_t parse_published_date(const char *date_str) {
   if (!date_str || date_str[0] == '\0') {
      return 0;
   }

   struct tm tm_val;
   memset(&tm_val, 0, sizeof(tm_val));

   /* Try ISO 8601 with time first, then date-only */
   char *end = strptime(date_str, "%Y-%m-%dT%H:%M:%S", &tm_val);
   if (!end) {
      end = strptime(date_str, "%Y-%m-%d", &tm_val);
   }
   if (!end) {
      return 0;
   }

   /* Use timegm() — SearXNG dates are UTC, mktime() would apply local timezone */
   return timegm(&tm_val);
}

/**
 * @brief Score a search result for reranking
 *
 * Scoring factors:
 * - SearXNG score × 4 (cross-engine confidence): 0-12+
 * - Has title: +2
 * - Has snippet: +2
 * - Wikipedia engine: +3
 * - GitHub/SO engine: +2
 * - Per-keyword title match (4+ char words): +3 each, cap +9
 * - Per-keyword snippet match (4+ char words): +1 each, cap +5
 * - Published < 24h ago: +4
 * - Published < 7 days: +2
 * - Published < 30 days: +1
 *
 * @param r Search result to score
 * @param query Original search query (for keyword matching)
 * @param now Current time (call time(NULL) once before scoring loop)
 * @return Score (higher = better)
 */
static int score_result(const search_result_t *r, const char *query, time_t now) {
   int score = 0;

   /* SearXNG cross-engine confidence score (clamp to prevent UB on float→int) */
   float clamped_score = r->searxng_score;
   if (clamped_score < 0.0f)
      clamped_score = 0.0f;
   if (clamped_score > 100.0f)
      clamped_score = 100.0f;
   score += (int)(clamped_score * 4.0f);

   if (r->title && r->title[0]) {
      score += 2;
   }
   if (r->snippet && r->snippet[0]) {
      score += 2;
   }

   /* Boost known quality engines */
   if (r->engine) {
      if (strcmp(r->engine, "wikipedia") == 0) {
         score += 3;
      } else if (strcmp(r->engine, "github") == 0) {
         score += 2;
      } else if (strcmp(r->engine, "stackoverflow") == 0) {
         score += 2;
      }
   }

   /* Tokenized keyword matching — split query on spaces, skip words < 4 chars */
   if (query && (r->title || r->snippet)) {
      int title_bonus = 0;
      int snippet_bonus = 0;
      const char *p = query;

      while (*p) {
         /* Skip leading spaces */
         while (*p == ' ') {
            p++;
         }
         if (*p == '\0') {
            break;
         }

         /* Find end of word */
         const char *word_start = p;
         while (*p && *p != ' ') {
            p++;
         }
         size_t word_len = (size_t)(p - word_start);

         /* Skip short words (stopwords) */
         if (word_len < 4) {
            continue;
         }

         /* Null-terminate the token for strcasestr (no lowercasing needed —
          * strcasestr handles case-insensitivity on both operands) */
         char token[64];
         size_t copy_len = (word_len < sizeof(token) - 1) ? word_len : sizeof(token) - 1;
         memcpy(token, word_start, copy_len);
         token[copy_len] = '\0';

         if (r->title && title_bonus < 9 && strcasestr(r->title, token)) {
            title_bonus += 3;
         }
         if (r->snippet && snippet_bonus < 5 && strcasestr(r->snippet, token)) {
            snippet_bonus += 1;
         }
      }

      score += title_bonus;
      score += snippet_bonus;
   }

   /* Recency boost based on published date */
   if (now > 0 && r->published_date) {
      time_t pub_time = parse_published_date(r->published_date);
      if (pub_time > 0) {
         double age_sec = difftime(now, pub_time);
         if (age_sec >= 0) {
            if (age_sec < 86400) { /* < 24 hours */
               score += 4;
            } else if (age_sec < 604800) { /* < 7 days */
               score += 2;
            } else if (age_sec < 2592000) { /* < 30 days */
               score += 1;
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
            OLOG_INFO("web_search: Filtering result with title containing '%s': %s",
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
#define MAX_RESULTS_PER_HOST 3

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
      OLOG_WARNING("web_search: Capping results from %d to %d", total_count,
                   MAX_RESULTS_TO_PROCESS);
      total_count = MAX_RESULTS_TO_PROCESS;
   }

   // Allocate temporary storage for all results (before dedup/rerank)
   scored_result_t *temp_results = calloc(total_count, sizeof(scored_result_t));
   if (!temp_results) {
      OLOG_ERROR("web_search: Failed to allocate temp results array");
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
      OLOG_ERROR("web_search: Failed to allocate host counts");
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

      // Parse SearXNG score (cross-engine confidence)
      struct json_object *score_obj = NULL;
      if (json_object_object_get_ex(item, "score", &score_obj)) {
         temp_results[accepted_count].result.searxng_score = (float)json_object_get_double(
             score_obj);
      }

      // Parse published date (ISO 8601)
      temp_results[accepted_count].result.published_date = get_json_string(item, "publishedDate");

      accepted_count++;
   }

   free(host_counts);

   if (accepted_count == 0) {
      free(temp_results);
      return;
   }

   // Score all results (call time() once for recency calculations)
   time_t now = time(NULL);
   for (int i = 0; i < accepted_count; i++) {
      temp_results[i].score = score_result(&temp_results[i].result, query, now);
   }

   // Sort by score (descending)
   qsort(temp_results, accepted_count, sizeof(scored_result_t), compare_scored_results);

   // Copy top results to response
   int final_count = (accepted_count > max_results) ? max_results : accepted_count;
   response->results = calloc(final_count, sizeof(search_result_t));
   if (!response->results) {
      for (int i = 0; i < accepted_count; i++) {
         free_result_fields(&temp_results[i].result);
      }
      free(temp_results);
      OLOG_ERROR("web_search: Failed to allocate final results array");
      response->error = strdup("Memory allocation failed");
      return;
   }

   for (int i = 0; i < final_count; i++) {
      response->results[i] = temp_results[i].result;
      response->count++;
   }

   // Free remaining results that weren't used
   for (int i = final_count; i < accepted_count; i++) {
      free_result_fields(&temp_results[i].result);
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
      OLOG_ERROR("web_search: Failed to allocate infobox results array");
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

search_response_t *web_search_query_typed(const char *query,
                                          int max_results,
                                          search_type_t type,
                                          const char *time_range) {
   if (!module_initialized) {
      OLOG_ERROR("web_search: Module not initialized");
      return NULL;
   }

   if (!query || query[0] == '\0') {
      OLOG_ERROR("web_search: Empty query");
      return NULL;
   }

   if (max_results <= 0) {
      max_results = SEARXNG_MAX_RESULTS;
   }

   /* TOCTOU-safe read of the engine string into a local; WebUI settings POST
    * can mutate g_config mid-call. */
   char engine[32];
   snprintf(engine, sizeof(engine), "%s", g_config.search.engine);

   /* Honor [search] engine = "disabled" — short-circuit with an empty
    * response (no error string; tool dispatcher treats empty results as
    * "no results found"). Image search remains on the SearXNG path
    * regardless because Tavily has no image search and "disabled" image
    * search is meaningless. */
   if (type != SEARCH_TYPE_IMAGES && strcmp(engine, "disabled") == 0) {
      OLOG_INFO("web_search: search.engine = 'disabled', returning empty response");
      search_response_t *empty = calloc(1, sizeof(search_response_t));
      return empty; /* may be NULL on OOM; caller handles */
   }

   /* Tavily dispatch — opt-in alternative to SearXNG. Image search is
    * Tavily-incompatible and stays on the SearXNG path. The per-user rate
    * limit isolates abuse to one user's bucket. */
   if (type != SEARCH_TYPE_IMAGES && strcmp(engine, "tavily") == 0 &&
       web_search_tavily_is_configured()) {
      if (tavily_rate_limit_check(tavily_rate_limit_resolve_user_id())) {
         search_response_t *tav = web_search_tavily_query_typed(query, max_results, type,
                                                                time_range);
         if (tav && !tav->error) {
            publish_snippets_to_cache(tav);
            return tav;
         }
         OLOG_INFO("tavily_search: %s, falling back to SearXNG",
                   tav && tav->error ? tav->error : "null response");
         if (tav) {
            web_search_free_response(tav);
         }
      } else {
         OLOG_WARNING("tavily_search: rate-limited, falling back to SearXNG");
      }
   }

   // Allocate response structure
   search_response_t *response = calloc(1, sizeof(search_response_t));
   if (!response) {
      OLOG_ERROR("web_search: Failed to allocate response");
      return NULL;
   }

   // Create CURL handle
   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_ERROR("web_search: Failed to create CURL handle");
      response->error = strdup("Failed to initialize HTTP client");
      return response;
   }

   // URL-encode the query
   char *encoded_query = curl_easy_escape(curl, query, 0);
   if (!encoded_query) {
      OLOG_ERROR("web_search: Failed to encode query");
      curl_easy_cleanup(curl);
      response->error = strdup("Failed to encode search query");
      return response;
   }

   // Validate time_range up front
   bool has_time_range = time_range && time_range[0] &&
                         (strcmp(time_range, "day") == 0 || strcmp(time_range, "week") == 0 ||
                          strcmp(time_range, "month") == 0 || strcmp(time_range, "year") == 0);

   // Build URL based on search type — single path, no gotos
   char url[SEARCH_URL_MAX_LEN];
   const char *type_str = "web";
   const char *category = NULL;
   const char *engines = NULL;
   bool supports_time_range = false;

   switch (type) {
      case SEARCH_TYPE_NEWS:
         category = "news";
         type_str = "news";
         supports_time_range = true;
         break;
      case SEARCH_TYPE_FACTS:
         engines = "wikipedia";
         type_str = "facts";
         break;
      case SEARCH_TYPE_SCIENCE:
         category = "science";
         type_str = "science";
         supports_time_range = true;
         break;
      case SEARCH_TYPE_IT:
         category = "it";
         type_str = "it";
         break;
      case SEARCH_TYPE_SOCIAL:
         engines = "reddit,twitter";
         type_str = "social";
         break;
      case SEARCH_TYPE_QA:
         engines = "stackoverflow,superuser";
         type_str = "q&a";
         break;
      case SEARCH_TYPE_DICTIONARY:
         category = "general";
         type_str = "dictionary";
         supports_time_range = true;
         break;
      case SEARCH_TYPE_PAPERS:
         engines = "arxiv,google_scholar,semantic_scholar";
         type_str = "papers";
         break;
      case SEARCH_TYPE_WEB:
      default:
         type_str = "web";
         supports_time_range = true;
         break;
   }

   // Track whether we need a supplemental news query
   bool needs_news_supplement = has_time_range && !supports_time_range;

   // Build URL — single code path
   if (engines) {
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json&engines=%s", searxng_base_url,
               encoded_query, engines);
   } else if (category) {
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json&categories=%s", searxng_base_url,
               encoded_query, category);
   } else {
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json", searxng_base_url, encoded_query);
   }

   // Only append time_range for types that support it
   if (has_time_range && supports_time_range) {
      size_t len = strlen(url);
      snprintf(url + len, sizeof(url) - len, "&time_range=%s", time_range);
   }

   curl_free(encoded_query);
   OLOG_INFO("web_search: Querying [%s] %s", type_str, url);

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
      OLOG_ERROR("web_search: Request failed: %s", curl_easy_strerror(res));
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
      OLOG_ERROR("web_search: HTTP error %ld", http_code);
      curl_buffer_free(&buffer);
      char err_msg[64];
      snprintf(err_msg, sizeof(err_msg), "HTTP error %ld", http_code);
      response->error = strdup(err_msg);
      return response;
   }

   // Warn if response was truncated (but continue with partial data)
   if (buffer.truncated) {
      OLOG_WARNING("web_search: Response truncated at %zu bytes (exceeded max capacity)",
                   buffer.size);
   }

   // Parse JSON response
   struct json_object *root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!root) {
      OLOG_ERROR("web_search: Failed to parse JSON response");
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
            OLOG_INFO("web_search: Found %d infobox results", response->count);
            return response;
         }
      }
      // Fall through to check results array if no infoboxes
   }

   // Extract results array (standard path for WEB and NEWS, fallback for FACTS)
   struct json_object *results_array = NULL;
   if (!json_object_object_get_ex(root, "results", &results_array)) {
      OLOG_WARNING("web_search: No 'results' field in response");
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
      OLOG_INFO("web_search: No results found for query");
      json_object_put(root);
      return response;  // No error, just empty results
   }

   parse_results_array(results_array, response, max_results, query);

   json_object_put(root);

   // Supplemental news query: when time_range was requested but the primary type
   // doesn't support it, merge in time-filtered news results to add recency context.
   // Guard: only supplement from top-level calls (not from recursive supplement calls)
   static _Thread_local int supplement_depth = 0;
   if (needs_news_supplement && response->count < max_results && supplement_depth == 0) {
      supplement_depth++;
      int news_slots = max_results - response->count;
      OLOG_INFO("web_search: Supplementing %s results with %d news slots (time_range=%s)", type_str,
                news_slots, time_range);

      search_response_t *news = web_search_query_typed(query, news_slots, SEARCH_TYPE_NEWS,
                                                       time_range);
      if (news && !news->error && news->count > 0) {
         // Grow the results array to hold both sets
         int merged_count = response->count + news->count;
         if (merged_count > max_results) {
            merged_count = max_results;
         }
         int news_to_add = merged_count - response->count;

         /* realloc(NULL, size) is valid C and acts like malloc */
         search_result_t *merged = realloc(response->results,
                                           merged_count * sizeof(search_result_t));
         if (merged) {
            response->results = merged;
            // Move news results into the merged array (transfer ownership)
            for (int i = 0; i < news_to_add; i++) {
               response->results[response->count + i] = news->results[i];
               // Clear source so free_response doesn't double-free
               memset(&news->results[i], 0, sizeof(search_result_t));
            }
            response->count = merged_count;
            OLOG_INFO("web_search: Merged %d news results (total: %d)", news_to_add,
                      response->count);
         }
      }
      if (news) {
         web_search_free_response(news);
      }
      supplement_depth--;
   }

   OLOG_INFO("web_search: Found %d results (deduplicated, reranked)", response->count);
   publish_snippets_to_cache(response);
   return response;
}

search_response_t *web_search_query(const char *query, int max_results) {
   return web_search_query_typed(query, max_results, SEARCH_TYPE_WEB, NULL);
}

struct json_object *web_search_query_images_raw(const char *query, int max_results) {
   if (!module_initialized) {
      OLOG_ERROR("web_search: Module not initialized");
      return NULL;
   }

   if (!query || query[0] == '\0') {
      OLOG_ERROR("web_search: Empty query");
      return NULL;
   }

   if (max_results <= 0) {
      max_results = SEARXNG_MAX_RESULTS;
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_ERROR("web_search: Failed to create CURL handle");
      return NULL;
   }

   char *encoded_query = curl_easy_escape(curl, query, 0);
   if (!encoded_query) {
      OLOG_ERROR("web_search: Failed to encode query");
      curl_easy_cleanup(curl);
      return NULL;
   }

   char url[SEARCH_URL_MAX_LEN];
   snprintf(url, sizeof(url), "%s/search?q=%s&format=json&categories=images&safesearch=1",
            searxng_base_url, encoded_query);
   curl_free(encoded_query);
   OLOG_INFO("web_search: Image query: %s", url);

   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, CURL_BUFFER_MAX_WEB_SEARCH);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, SEARXNG_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "DAWN/1.0");

   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      OLOG_ERROR("web_search: Image search request failed: %s", curl_easy_strerror(res));
      curl_easy_cleanup(curl);
      curl_buffer_free(&buffer);
      return NULL;
   }

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (http_code != 200) {
      OLOG_ERROR("web_search: Image search HTTP error %ld", http_code);
      curl_buffer_free(&buffer);
      return NULL;
   }

   struct json_object *root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!root) {
      OLOG_ERROR("web_search: Failed to parse image search JSON response");
      return NULL;
   }

   return root; /* Caller owns the reference — must json_object_put() */
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

   BUF_PRINTF(buffer, written, remaining, "Web search results:\n\n");

   for (int i = 0; i < response->count && remaining > 1; i++) {
      search_result_t *r = &response->results[i];

      BUF_PRINTF(buffer, written, remaining, "%d. %s", i + 1, r->title ? r->title : "(no title)");

      if (r->engine) {
         BUF_PRINTF(buffer, written, remaining, " [%s]", r->engine);
      }

      if (r->published_date && r->published_date[0]) {
         /* Show date only (first 10 chars: YYYY-MM-DD) */
         BUF_PRINTF(buffer, written, remaining, " (%.10s)", r->published_date);
      }

      BUF_PRINTF(buffer, written, remaining, "\n");

      if (r->snippet) {
         BUF_PRINTF(buffer, written, remaining, "   %s\n", r->snippet);
      }

      if (r->url) {
         BUF_PRINTF(buffer, written, remaining, "   URL: %s\n", r->url);
      }

      BUF_PRINTF(buffer, written, remaining, "\n");
   }

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
         free_result_fields(&response->results[i]);
      }
      free(response->results);
   }

   if (response->error) {
      free(response->error);
   }

   free(response);
}
