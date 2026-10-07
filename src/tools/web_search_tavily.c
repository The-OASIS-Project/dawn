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
 * Web Search Module — Tavily /search adapter implementation.
 *
 * Posts JSON to https://api.tavily.com/search, normalizes results into the
 * shared search_result_t shape so the dispatcher in web_search.c can use
 * either provider transparently.
 */

#include "tools/web_search_tavily.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <sodium.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TAVILY_SEARCH_ENDPOINT "https://api.tavily.com/search"
#define TAVILY_SEARCH_TIMEOUT_SEC 15
#define TAVILY_CONNECT_TIMEOUT_SEC 5
#define TAVILY_MAX_REDIRS 3L

/* Inbound bounds caps per security review M2 — never trust upstream lengths. */
#define TAVILY_TITLE_MAX 512
#define TAVILY_URL_MAX 2048
#define TAVILY_PUBLISHED_DATE_MAX 64
#define TAVILY_INBOUND_RESULTS_MAX 25 /* Hard cap regardless of what server returns */

/* Tavily /search response can reasonably reach this size (~25 results × snippet
 * + framing). Capped to keep buffer growth bounded. */
#define TAVILY_RESPONSE_MAX_BYTES (512 * 1024)

/* =============================================================================
 * Helpers
 * ============================================================================= */

/**
 * @brief Map DAWN search_type_t to Tavily "topic" parameter.
 *        Tavily only has "general" or "news"; everything else collapses to
 *        general (DAWN's richer categories are SearXNG-only).
 */
static const char *map_topic(search_type_t type) {
   return (type == SEARCH_TYPE_NEWS) ? "news" : "general";
}

/**
 * @brief Map DAWN time_range string to Tavily "days" integer. Returns 0 to
 *        signal "omit field" (Tavily defaults to all-time for news topic).
 *        Only honored when topic=news.
 */
static int map_days(const char *time_range) {
   if (!time_range || time_range[0] == '\0') {
      return 0;
   }
   if (strcmp(time_range, "day") == 0) {
      return 1;
   }
   if (strcmp(time_range, "week") == 0) {
      return 7;
   }
   if (strcmp(time_range, "month") == 0) {
      return 30;
   }
   if (strcmp(time_range, "year") == 0) {
      return 365;
   }
   return 0;
}

/**
 * @brief strdup with hard length cap. Returns NULL if input is NULL.
 *        Trailing "..." marker added when truncation occurs (parity with
 *        SearXNG's truncate_string).
 */
static char *bounded_dup(const char *src, size_t max_len) {
   if (!src) {
      return NULL;
   }
   size_t len = strlen(src);
   if (len <= max_len) {
      char *out = malloc(len + 1);
      if (out) {
         memcpy(out, src, len + 1);
      }
      return out;
   }
   char *out = malloc(max_len + 4); /* room for "..." + NUL */
   if (!out) {
      return NULL;
   }
   memcpy(out, src, max_len);
   memcpy(out + max_len, "...", 4);
   return out;
}

/* Note on prompt-injection filtering: an earlier revision ran every
 * snippet/title through memory_filter_check + [DATA] wrap. The memory
 * filter's blocklist was authored for short factual text destined for
 * persistent storage and false-positives on the article-length natural
 * language Tavily returns ("](http" on every link, "you should" as
 * everyday English, etc.). Removed for the URL-fetch path; the filter
 * still gates memory ingestion via `memory_filter_check()` at storage
 * sites. The LLM treats tool-result content as data by virtue of being
 * a tool-call response, not as system instructions. */

/* Placeholder string inserted into the JSON tree in place of the real
 * api_key — keeps the secret out of json-c-managed memory (which is freed
 * by json_object_put without zeroing). The real key is substituted into
 * the final body buffer, which IS sodium_memzero'd before free. SEC-M2.
 *
 * The placeholder is plain ASCII so JSON escaping is a no-op; it's also
 * distinctive enough that strstr() will only match the slot we put it in. */
#define TAVILY_API_KEY_PLACEHOLDER "__DAWN_TAVILY_API_KEY_PLACEHOLDER__"

/**
 * @brief Build the Tavily /search JSON request body.
 *
 * Caller frees the returned heap buffer. Returns NULL on allocation failure
 * or invalid input.
 *
 * Body length is also written to *out_len so the caller can memset(0) the
 * buffer (which contains the API key) before free().
 */
static char *build_request_body(const char *api_key,
                                const char *query,
                                int max_results,
                                search_type_t type,
                                const char *time_range,
                                size_t *out_len) {
   if (!api_key || !query || !out_len) {
      return NULL;
   }
   /* Defense in depth: reject queries that contain the api_key placeholder.
    * The strstr() in the substitution path finds the FIRST occurrence in
    * the serialized JSON; today the placeholder lives in the api_key slot
    * which comes first, but if json-c ever changed field ordering OR if
    * the user-controlled query contained the literal placeholder, the real
    * key would be substituted into the wrong slot and sent to Tavily as
    * user input. The placeholder is non-natural so legitimate queries
    * cannot contain it. */
   if (strstr(query, TAVILY_API_KEY_PLACEHOLDER)) {
      return NULL;
   }
   struct json_object *root = json_object_new_object();
   if (!root) {
      return NULL;
   }

   /* SEC-M2: put a placeholder in the JSON tree, not the real key. */
   json_object_object_add(root, "api_key", json_object_new_string(TAVILY_API_KEY_PLACEHOLDER));
   json_object_object_add(root, "query", json_object_new_string(query));
   json_object_object_add(root, "topic", json_object_new_string(map_topic(type)));
   json_object_object_add(root, "search_depth", json_object_new_string("basic"));
   json_object_object_add(root, "max_results", json_object_new_int(max_results));
   json_object_object_add(root, "include_answer", json_object_new_boolean(0));

   if (type == SEARCH_TYPE_NEWS) {
      int days = map_days(time_range);
      if (days > 0) {
         json_object_object_add(root, "days", json_object_new_int(days));
      }
   }

   size_t serialized_len = 0;
   const char *serialized = json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN,
                                                              &serialized_len);
   if (!serialized) {
      json_object_put(root);
      return NULL;
   }

   /* Substitute placeholder with real api_key while copying into our
    * sodium_memzero-protected body buffer. */
   const char *placeholder_pos = strstr(serialized, TAVILY_API_KEY_PLACEHOLDER);
   if (!placeholder_pos) {
      json_object_put(root);
      return NULL;
   }
   const size_t placeholder_len = sizeof(TAVILY_API_KEY_PLACEHOLDER) - 1;
   const size_t key_len = strlen(api_key);
   const size_t prefix_len = (size_t)(placeholder_pos - serialized);
   const size_t suffix_len = serialized_len - prefix_len - placeholder_len;
   const size_t body_len = prefix_len + key_len + suffix_len;

   char *body = malloc(body_len + 1);
   if (!body) {
      json_object_put(root);
      return NULL;
   }
   memcpy(body, serialized, prefix_len);
   memcpy(body + prefix_len, api_key, key_len);
   memcpy(body + prefix_len + key_len, serialized + prefix_len + placeholder_len, suffix_len);
   body[body_len] = '\0';
   *out_len = body_len;

   json_object_put(root); /* safe — only placeholder bytes were in the tree */
   return body;
}

/**
 * @brief Parse Tavily response and populate response->results[]. Bounds-caps
 *        and skips entries that don't have both a title and URL.
 */
static void parse_response(const char *json_text, int max_results, search_response_t *response) {
   struct json_object *root = json_tokener_parse(json_text);
   if (!root) {
      OLOG_ERROR("tavily_search: failed to parse JSON response");
      response->error = strdup("Invalid JSON response from Tavily");
      return;
   }

   struct json_object *results_arr = NULL;
   if (!json_object_object_get_ex(root, "results", &results_arr) ||
       !json_object_is_type(results_arr, json_type_array)) {
      OLOG_WARNING("tavily_search: response missing 'results' array");
      json_object_put(root);
      response->error = strdup("No results in Tavily response");
      return;
   }

   int total = json_object_array_length(results_arr);
   if (total <= 0) {
      OLOG_INFO("tavily_search: no results for query");
      json_object_put(root);
      return; /* empty, not an error */
   }

   /* SEC-M2: hard inbound cap regardless of what max_results asked for. */
   if (total > TAVILY_INBOUND_RESULTS_MAX) {
      OLOG_WARNING("tavily_search: capping inbound results from %d to %d", total,
                   TAVILY_INBOUND_RESULTS_MAX);
      total = TAVILY_INBOUND_RESULTS_MAX;
   }
   int want = (total < max_results) ? total : max_results;
   if (want <= 0) {
      json_object_put(root);
      return;
   }

   response->results = calloc((size_t)want, sizeof(search_result_t));
   if (!response->results) {
      json_object_put(root);
      response->error = strdup("Memory allocation failed");
      return;
   }

   for (int i = 0; i < total && response->count < want; i++) {
      struct json_object *item = json_object_array_get_idx(results_arr, i);
      if (!item || !json_object_is_type(item, json_type_object)) {
         continue;
      }

      struct json_object *jtitle = NULL;
      struct json_object *jurl = NULL;
      struct json_object *jcontent = NULL;
      struct json_object *jscore = NULL;
      struct json_object *jdate = NULL;

      json_object_object_get_ex(item, "title", &jtitle);
      json_object_object_get_ex(item, "url", &jurl);
      json_object_object_get_ex(item, "content", &jcontent);
      json_object_object_get_ex(item, "score", &jscore);
      json_object_object_get_ex(item, "published_date", &jdate);

      const char *title_s = jtitle ? json_object_get_string(jtitle) : NULL;
      const char *url_s = jurl ? json_object_get_string(jurl) : NULL;
      if (!title_s || !url_s || !title_s[0] || !url_s[0]) {
         continue; /* skip results without title+url */
      }

      search_result_t *r = &response->results[response->count];
      r->title = bounded_dup(title_s, TAVILY_TITLE_MAX);
      r->url = bounded_dup(url_s, TAVILY_URL_MAX);
      r->engine = strdup("tavily");
      if (jcontent) {
         const char *content_s = json_object_get_string(jcontent);
         if (content_s) {
            r->snippet = bounded_dup(content_s, SEARXNG_SNIPPET_LEN);
         }
      }
      if (jdate) {
         const char *date_s = json_object_get_string(jdate);
         if (date_s) {
            r->published_date = bounded_dup(date_s, TAVILY_PUBLISHED_DATE_MAX);
         }
      }
      if (jscore) {
         r->searxng_score = (float)json_object_get_double(jscore);
      }

      response->count++;
   }

   json_object_put(root);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int web_search_tavily_is_configured(void) {
   if (strcmp(g_config.search.engine, "tavily") != 0) {
      return 0;
   }
   if (g_secrets.tavily_api_key[0] == '\0') {
      return 0;
   }
   return 1;
}

search_response_t *web_search_tavily_query_typed(const char *query,
                                                 int max_results,
                                                 search_type_t type,
                                                 const char *time_range) {
   if (!query || query[0] == '\0') {
      OLOG_ERROR("tavily_search: empty query");
      return NULL;
   }

   search_response_t *response = calloc(1, sizeof(search_response_t));
   if (!response) {
      OLOG_ERROR("tavily_search: failed to allocate response");
      return NULL;
   }

   /* Clamp max_results to [1, TAVILY_INBOUND_RESULTS_MAX]. */
   if (max_results <= 0) {
      max_results = SEARXNG_MAX_RESULTS;
   }
   if (max_results > TAVILY_INBOUND_RESULTS_MAX) {
      max_results = TAVILY_INBOUND_RESULTS_MAX;
   }

   /* Read API key once. We do not copy it onto the heap; build_request_body
    * embeds it directly into the JSON body. */
   const char *api_key = g_secrets.tavily_api_key;
   if (api_key[0] == '\0') {
      OLOG_WARNING("tavily_search: no API key set");
      response->error = strdup("Tavily API key not configured");
      return response;
   }

   size_t body_len = 0;
   char *body = build_request_body(api_key, query, max_results, type, time_range, &body_len);
   if (!body) {
      OLOG_ERROR("tavily_search: failed to build request body");
      response->error = strdup("Failed to build Tavily request");
      return response;
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_ERROR("tavily_search: failed to create CURL handle");
      /* SEC-L1: zero the secret-containing buffer before freeing.
       * sodium_memzero is the OASIS-wide convention and survives dead-store
       * elimination that would silently delete a plain memset. */
      sodium_memzero(body, body_len);
      free(body);
      response->error = strdup("Failed to initialize HTTP client");
      return response;
   }

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");
   headers = curl_slist_append(headers, "Accept: application/json");

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, TAVILY_RESPONSE_MAX_BYTES);

   curl_easy_setopt(curl, CURLOPT_URL, TAVILY_SEARCH_ENDPOINT);
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   /* SEC-M1: explicit TLS + protocol pinning + KEEPALIVE/HTTP2 via
    * curl_apply_dawn_defaults; protocol pin stays site-specific. */
   curl_apply_dawn_defaults(curl, "DAWN/1.0", (long)TAVILY_SEARCH_TIMEOUT_SEC, &buf);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)TAVILY_CONNECT_TIMEOUT_SEC);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");
#if LIBCURL_VERSION_NUM >= 0x075500
   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_MAXREDIRS, TAVILY_MAX_REDIRS);

   OLOG_INFO("tavily_search: querying topic=%s max_results=%d", map_topic(type), max_results);

   CURLcode res = curl_easy_perform(curl);

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);

   /* SEC-L1: zero the body buffer (contains API key) before free.
    * sodium_memzero survives dead-store elimination. */
   sodium_memzero(body, body_len);
   free(body);

   if (res != CURLE_OK) {
      OLOG_WARNING("tavily_search: request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      response->error = strdup(curl_easy_strerror(res));
      return response;
   }

   if (http_code == 401) {
      OLOG_WARNING("tavily_search: HTTP 401 — invalid API key");
      curl_buffer_free(&buf);
      response->error = strdup("Tavily authentication failed");
      return response;
   }
   if (http_code == 429) {
      OLOG_WARNING("tavily_search: HTTP 429 — rate limited by Tavily");
      curl_buffer_free(&buf);
      response->error = strdup("Tavily rate limit exceeded");
      return response;
   }
   if (http_code < 200 || http_code >= 300) {
      OLOG_WARNING("tavily_search: HTTP error %ld", http_code);
      curl_buffer_free(&buf);
      char err[64];
      snprintf(err, sizeof(err), "Tavily HTTP error %ld", http_code);
      response->error = strdup(err);
      return response;
   }

   if (buf.truncated) {
      OLOG_WARNING("tavily_search: response truncated at %zu bytes (max %d)", buf.size,
                   TAVILY_RESPONSE_MAX_BYTES);
   }

   if (!buf.data || buf.size == 0) {
      OLOG_WARNING("tavily_search: empty response");
      curl_buffer_free(&buf);
      response->error = strdup("Empty response from Tavily");
      return response;
   }

   parse_response(buf.data, max_results, response);
   curl_buffer_free(&buf);

   OLOG_INFO("tavily_search: returned %d results", response->count);
   return response;
}
