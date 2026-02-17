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
 * URL Fetcher - Fetch and extract readable content from URLs as Markdown
 */

#include "tools/url_fetcher.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "tools/html_parser.h"
#include "utils/string_utils.h"

// =============================================================================
// Constants
// =============================================================================

// Retry configuration for transient failures
#define URL_FETCH_MAX_RETRIES 3
#define URL_FETCH_RETRY_DELAY_MS 500

// FlareSolverr configuration for bypassing Cloudflare/bot protection
// FlareSolverr is a Docker service that uses headless Chromium to solve JS challenges
// Run: docker run -d --name flaresolverr -p 8191:8191 flaresolverr/flaresolverr:latest
// These can be overridden via config file (see CONFIG_FILE_DESIGN.md [url_fetcher.flaresolverr])
#ifndef FLARESOLVERR_ENDPOINT
#define FLARESOLVERR_ENDPOINT "http://127.0.0.1:8191/v1"
#endif
#ifndef FLARESOLVERR_TIMEOUT_SEC
#define FLARESOLVERR_TIMEOUT_SEC 30
#endif
#ifndef FLARESOLVERR_MAX_RESPONSE
// 4MB max response - intentionally large to support fetching academic papers,
// documentation, and other large text-heavy pages. The HTML parser will
// reduce this to a much smaller markdown output (capped at 2MB).
// Do not reduce without considering the use case of downloading large documents.
#define FLARESOLVERR_MAX_RESPONSE (4 * 1024 * 1024)
#endif
#define FLARESOLVERR_AVAILABILITY_CACHE_TTL_SEC 60  // Cache availability check for 60 seconds

// Allowed Content-Types for text processing (with lengths for prefix matching)
typedef struct {
   const char *type;
   size_t len;
} content_type_entry_t;

static const content_type_entry_t ALLOWED_CONTENT_TYPES[] = {
   { "text/html", 9 },        { "text/plain", 10 }, { "application/xhtml+xml", 21 },
   { "application/xml", 15 }, { "text/xml", 8 },    { NULL, 0 }
};

// =============================================================================
// Config Accessor Functions for FlareSolverr
// =============================================================================

/**
 * @brief Check if FlareSolverr is enabled in config
 */
static inline int flaresolverr_is_enabled(void) {
   return g_config.url_fetcher.flaresolverr.enabled;
}

/**
 * @brief Get FlareSolverr endpoint from config, with fallback to default
 */
static inline const char *flaresolverr_get_endpoint(void) {
   if (g_config.url_fetcher.flaresolverr.endpoint[0] != '\0') {
      return g_config.url_fetcher.flaresolverr.endpoint;
   }
   return FLARESOLVERR_ENDPOINT;
}

/**
 * @brief Get FlareSolverr timeout from config, with fallback to default
 */
static inline int flaresolverr_get_timeout(void) {
   if (g_config.url_fetcher.flaresolverr.timeout_sec > 0) {
      return g_config.url_fetcher.flaresolverr.timeout_sec;
   }
   return FLARESOLVERR_TIMEOUT_SEC;
}

/**
 * @brief Get FlareSolverr max response size from config, with fallback to default
 */
static inline size_t flaresolverr_get_max_response(void) {
   if (g_config.url_fetcher.flaresolverr.max_response_bytes > 0) {
      return g_config.url_fetcher.flaresolverr.max_response_bytes;
   }
   return FLARESOLVERR_MAX_RESPONSE;
}

/**
 * @brief Check if a CURL error is retryable (transient failure)
 */
static int is_retryable_curl_error(CURLcode code) {
   switch (code) {
      case CURLE_COULDNT_CONNECT:
      case CURLE_OPERATION_TIMEDOUT:
      case CURLE_GOT_NOTHING:
      case CURLE_SEND_ERROR:
      case CURLE_RECV_ERROR:
      case CURLE_PARTIAL_FILE:
         return 1;
      default:
         return 0;
   }
}

/**
 * @brief Check if an HTTP status code is retryable
 */
static int is_retryable_http_code(long http_code) {
   // Retry on server errors (5xx) and rate limiting (429)
   return (http_code >= 500 && http_code < 600) || http_code == 429;
}

// =============================================================================
// Whitelist Storage
// =============================================================================

typedef enum {
   WHITELIST_TYPE_URL,      // Full URL prefix match
   WHITELIST_TYPE_HOST,     // Hostname match
   WHITELIST_TYPE_IP,       // Single IP address
   WHITELIST_TYPE_CIDR_V4,  // IPv4 CIDR network
} whitelist_type_t;

typedef struct {
   whitelist_type_t type;
   char *entry;           // Original entry string
   size_t entry_len;      // Cached strlen for efficiency
   unsigned int network;  // For CIDR: network address
   unsigned int netmask;  // For CIDR: netmask
} whitelist_entry_t;

// Module state (consistent with web_search.c pattern)
static int module_initialized = 0;
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;

static whitelist_entry_t whitelist[URL_FETCH_MAX_WHITELIST];
static int whitelist_count = 0;

// =============================================================================
// Lifecycle Functions (for consistency with other tools)
// =============================================================================

int url_fetcher_init(void) {
   pthread_mutex_lock(&module_mutex);

   if (module_initialized) {
      LOG_WARNING("url_fetcher: Module already initialized");
      pthread_mutex_unlock(&module_mutex);
      return URL_FETCH_SUCCESS;
   }

   // Clear whitelist on init
   for (int i = 0; i < whitelist_count; i++) {
      if (whitelist[i].entry) {
         free(whitelist[i].entry);
         whitelist[i].entry = NULL;
      }
   }
   whitelist_count = 0;

   module_initialized = 1;
   pthread_mutex_unlock(&module_mutex);

   // Load whitelist entries from config
   for (int i = 0; i < g_config.url_fetcher.whitelist_count; i++) {
      if (g_config.url_fetcher.whitelist[i] && g_config.url_fetcher.whitelist[i][0] != '\0') {
         if (url_whitelist_add(g_config.url_fetcher.whitelist[i]) == 0) {
            LOG_INFO("url_fetcher: Loaded whitelist entry from config: %s",
                     g_config.url_fetcher.whitelist[i]);
         }
      }
   }

   return URL_FETCH_SUCCESS;
}

void url_fetcher_cleanup(void) {
   pthread_mutex_lock(&module_mutex);

   if (!module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      return;
   }

   for (int i = 0; i < whitelist_count; i++) {
      if (whitelist[i].entry) {
         free(whitelist[i].entry);
         whitelist[i].entry = NULL;
      }
   }
   whitelist_count = 0;

   module_initialized = 0;
   pthread_mutex_unlock(&module_mutex);
}

int url_fetcher_is_initialized(void) {
   return __atomic_load_n(&module_initialized, __ATOMIC_ACQUIRE);
}

// =============================================================================
// Whitelist Functions
// =============================================================================

/**
 * @brief Parse CIDR notation into network and mask
 */
static int parse_cidr(const char *cidr, unsigned int *network, unsigned int *netmask) {
   char ip_part[64];
   int prefix_len;

   // Find the /
   const char *slash = strchr(cidr, '/');
   if (!slash)
      return 0;

   // Copy IP part
   size_t ip_len = slash - cidr;
   if (ip_len >= sizeof(ip_part))
      return 0;
   strncpy(ip_part, cidr, ip_len);
   ip_part[ip_len] = '\0';

   // Parse prefix length
   prefix_len = atoi(slash + 1);
   if (prefix_len < 0 || prefix_len > 32)
      return 0;

   // Parse IP
   struct in_addr addr;
   if (inet_pton(AF_INET, ip_part, &addr) != 1)
      return 0;

   *network = ntohl(addr.s_addr);

   // Calculate netmask
   if (prefix_len == 0) {
      *netmask = 0;
   } else {
      *netmask = 0xFFFFFFFF << (32 - prefix_len);
   }

   // Apply mask to network
   *network &= *netmask;

   return 1;
}

/**
 * @brief Check if an IP matches a CIDR entry
 */
static int ip_matches_cidr(const char *ip, unsigned int network, unsigned int netmask) {
   struct in_addr addr;
   if (inet_pton(AF_INET, ip, &addr) != 1)
      return 0;

   unsigned int ip_num = ntohl(addr.s_addr);
   return (ip_num & netmask) == network;
}

void url_whitelist_clear(void) {
   pthread_mutex_lock(&module_mutex);
   for (int i = 0; i < whitelist_count; i++) {
      if (whitelist[i].entry) {
         free(whitelist[i].entry);
         whitelist[i].entry = NULL;
      }
   }
   whitelist_count = 0;
   pthread_mutex_unlock(&module_mutex);
}

int url_whitelist_count(void) {
   return __atomic_load_n(&whitelist_count, __ATOMIC_ACQUIRE);
}

int url_whitelist_add(const char *entry) {
   if (!entry || strlen(entry) == 0)
      return URL_FETCH_ERROR_INVALID_URL;

   pthread_mutex_lock(&module_mutex);

   if (whitelist_count >= URL_FETCH_MAX_WHITELIST) {
      LOG_WARNING("url_fetcher: Whitelist full (max %d entries)", URL_FETCH_MAX_WHITELIST);
      pthread_mutex_unlock(&module_mutex);
      return URL_FETCH_ERROR_ALLOC;
   }

   whitelist_entry_t *wl = &whitelist[whitelist_count];
   wl->entry = strdup(entry);
   if (!wl->entry) {
      pthread_mutex_unlock(&module_mutex);
      return URL_FETCH_ERROR_ALLOC;
   }
   wl->entry_len = strlen(entry);  // Cache strlen

   // Determine entry type
   if (strncmp(entry, "http://", 7) == 0 || strncmp(entry, "https://", 8) == 0) {
      // Full URL
      wl->type = WHITELIST_TYPE_URL;
      LOG_INFO("url_fetcher: Added URL to whitelist: %s", entry);
   } else if (strchr(entry, '/') != NULL) {
      // CIDR notation
      if (parse_cidr(entry, &wl->network, &wl->netmask)) {
         wl->type = WHITELIST_TYPE_CIDR_V4;
         LOG_INFO("url_fetcher: Added CIDR to whitelist: %s", entry);
      } else {
         // Might be a path-like hostname, treat as host
         wl->type = WHITELIST_TYPE_HOST;
         LOG_INFO("url_fetcher: Added hostname to whitelist: %s", entry);
      }
   } else {
      // Check if it's an IP address or hostname
      struct in_addr addr;
      if (inet_pton(AF_INET, entry, &addr) == 1) {
         wl->type = WHITELIST_TYPE_IP;
         wl->network = ntohl(addr.s_addr);
         wl->netmask = 0xFFFFFFFF;
         LOG_INFO("url_fetcher: Added IP to whitelist: %s", entry);
      } else {
         wl->type = WHITELIST_TYPE_HOST;
         LOG_INFO("url_fetcher: Added hostname to whitelist: %s", entry);
      }
   }

   whitelist_count++;
   pthread_mutex_unlock(&module_mutex);
   return URL_FETCH_SUCCESS;
}

int url_whitelist_remove(const char *entry) {
   if (!entry)
      return URL_FETCH_ERROR_INVALID_URL;

   pthread_mutex_lock(&module_mutex);
   for (int i = 0; i < whitelist_count; i++) {
      if (whitelist[i].entry && strcmp(whitelist[i].entry, entry) == 0) {
         free(whitelist[i].entry);
         // Shift remaining entries
         for (int j = i; j < whitelist_count - 1; j++) {
            whitelist[j] = whitelist[j + 1];
         }
         whitelist_count--;
         whitelist[whitelist_count].entry = NULL;
         LOG_INFO("url_fetcher: Removed from whitelist: %s", entry);
         pthread_mutex_unlock(&module_mutex);
         return URL_FETCH_SUCCESS;
      }
   }
   pthread_mutex_unlock(&module_mutex);

   return URL_FETCH_ERROR_INVALID_URL;
}

/**
 * @brief Check if a URL/host/IP matches any whitelist entry
 * Note: Called with module_mutex NOT held (read-only access to whitelist)
 * Whitelist modifications should only happen during initialization
 */
static int is_whitelisted(const char *url, const char *host, const char *ip) {
   int count = __atomic_load_n(&whitelist_count, __ATOMIC_ACQUIRE);
   for (int i = 0; i < count; i++) {
      whitelist_entry_t *wl = &whitelist[i];
      if (!wl->entry)
         continue;

      switch (wl->type) {
         case WHITELIST_TYPE_URL:
            // URL prefix match (use cached entry_len)
            if (url && strncmp(url, wl->entry, wl->entry_len) == 0) {
               LOG_INFO("url_fetcher: URL matches whitelist entry: %s", wl->entry);
               return 1;
            }
            break;

         case WHITELIST_TYPE_HOST:
            // Hostname match (case-insensitive)
            if (host && strcasecmp(host, wl->entry) == 0) {
               LOG_INFO("url_fetcher: Host matches whitelist entry: %s", wl->entry);
               return 1;
            }
            break;

         case WHITELIST_TYPE_IP:
            // Exact IP match
            if (ip && ip_matches_cidr(ip, wl->network, wl->netmask)) {
               LOG_INFO("url_fetcher: IP matches whitelist entry: %s", wl->entry);
               return 1;
            }
            break;

         case WHITELIST_TYPE_CIDR_V4:
            // CIDR match
            if (ip && ip_matches_cidr(ip, wl->network, wl->netmask)) {
               LOG_INFO("url_fetcher: IP matches whitelist CIDR: %s", wl->entry);
               return 1;
            }
            break;
      }
   }

   return 0;
}

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Check if Content-Type is allowed for text processing
 *
 * Uses prefix matching instead of substring search for efficiency.
 * Content-Type headers typically start with the MIME type: "text/html; charset=utf-8"
 */
static int is_allowed_content_type(const char *content_type) {
   if (!content_type)
      return 0;

   // Use prefix match with pre-computed lengths (more efficient than substring search)
   for (int i = 0; ALLOWED_CONTENT_TYPES[i].type != NULL; i++) {
      if (strncasecmp(content_type, ALLOWED_CONTENT_TYPES[i].type, ALLOWED_CONTENT_TYPES[i].len) ==
          0) {
         // Verify it's a complete type (followed by ; or end of string or space)
         char next = content_type[ALLOWED_CONTENT_TYPES[i].len];
         if (next == '\0' || next == ';' || next == ' ') {
            return 1;
         }
      }
   }
   return 0;
}

// =============================================================================
// FlareSolverr Integration (Cloudflare bypass)
// =============================================================================

// Cached FlareSolverr availability status
static int flaresolverr_available_cached = -1;  // -1 = unknown, 0 = unavailable, 1 = available
static time_t flaresolverr_check_time = 0;

/**
 * @brief Check if FlareSolverr service is available (with TTL caching)
 *
 * Caches the result for FLARESOLVERR_AVAILABILITY_CACHE_TTL_SEC seconds to avoid
 * repeated HEAD requests on every 403 error.
 *
 * @return 1 if available, 0 if not
 */
static int flaresolverr_is_available(void) {
   time_t now = time(NULL);

   // Check cache validity
   if (flaresolverr_available_cached >= 0 &&
       (now - flaresolverr_check_time) < FLARESOLVERR_AVAILABILITY_CACHE_TTL_SEC) {
      return flaresolverr_available_cached;
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      flaresolverr_available_cached = 0;
      flaresolverr_check_time = now;
      return 0;
   }

   curl_easy_setopt(curl, CURLOPT_URL, flaresolverr_get_endpoint());
   curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2);

   CURLcode res = curl_easy_perform(curl);
   curl_easy_cleanup(curl);

   flaresolverr_available_cached = (res == CURLE_OK) ? 1 : 0;
   flaresolverr_check_time = now;

   if (!flaresolverr_available_cached) {
      LOG_INFO("url_fetcher: FlareSolverr not available at %s (cached for %ds)",
               flaresolverr_get_endpoint(), FLARESOLVERR_AVAILABILITY_CACHE_TTL_SEC);
   }

   return flaresolverr_available_cached;
}

/**
 * @brief Convert 4 hex digits to unsigned int (faster than sscanf)
 */
static inline unsigned int hex4_to_uint(const char *s) {
   unsigned int v = 0;
   for (int i = 0; i < 4; i++) {
      v <<= 4;
      char c = s[i];
      if (c >= '0' && c <= '9')
         v |= (unsigned int)(c - '0');
      else if (c >= 'a' && c <= 'f')
         v |= (unsigned int)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
         v |= (unsigned int)(c - 'A' + 10);
   }
   return v;
}

/**
 * @brief Decode a JSON-escaped string in place, with proper UTF-8 encoding
 *
 * Handles standard JSON escape sequences: \n, \t, \r, \", \\, \/, \uXXXX
 * Unicode escapes are properly encoded as UTF-8.
 *
 * @param src Source string (starting after opening quote)
 * @param dest Destination buffer
 * @param dest_size Size of destination buffer
 * @return Number of bytes written (excluding null terminator), or 0 on error
 */
static size_t json_unescape_string(const char *src, char *dest, size_t dest_size) {
   if (!src || !dest || dest_size == 0)
      return 0;

   size_t out_len = 0;
   while (*src && out_len < dest_size - 4) {  // Reserve space for potential UTF-8 (up to 4 bytes)
      if (*src == '\\' && *(src + 1)) {
         src++;
         switch (*src) {
            case 'n':
               dest[out_len++] = '\n';
               break;
            case 't':
               dest[out_len++] = '\t';
               break;
            case 'r':
               dest[out_len++] = '\r';
               break;
            case '"':
               dest[out_len++] = '"';
               break;
            case '\\':
               dest[out_len++] = '\\';
               break;
            case '/':
               dest[out_len++] = '/';
               break;
            case 'b':
               dest[out_len++] = '\b';
               break;
            case 'f':
               dest[out_len++] = '\f';
               break;
            case 'u':
               // Unicode escape \uXXXX
               if (isxdigit(src[1]) && isxdigit(src[2]) && isxdigit(src[3]) && isxdigit(src[4])) {
                  unsigned int code = hex4_to_uint(src + 1);

                  // Encode as UTF-8
                  if (code < 0x80) {
                     dest[out_len++] = (char)code;
                  } else if (code < 0x800) {
                     dest[out_len++] = (char)(0xC0 | (code >> 6));
                     dest[out_len++] = (char)(0x80 | (code & 0x3F));
                  } else {
                     dest[out_len++] = (char)(0xE0 | (code >> 12));
                     dest[out_len++] = (char)(0x80 | ((code >> 6) & 0x3F));
                     dest[out_len++] = (char)(0x80 | (code & 0x3F));
                  }
                  src += 4;  // Skip the 4 hex digits
               }
               break;
            default:
               dest[out_len++] = *src;
               break;
         }
         src++;
      } else if (*src == '"') {
         break;  // End of JSON string
      } else {
         dest[out_len++] = *src++;
      }
   }
   dest[out_len] = '\0';
   return out_len;
}

/**
 * @brief Simple JSON string extractor (finds value for a key in JSON)
 *
 * Extracts string value for a given key from JSON response.
 * This is a lightweight alternative to a full JSON parser.
 *
 * @param json JSON string to search
 * @param key Key to find (without quotes)
 * @param out_value Buffer for extracted value
 * @param out_size Size of output buffer
 * @return 1 if found, 0 if not found
 */
static int json_extract_string(const char *json,
                               const char *key,
                               char *out_value,
                               size_t out_size) {
   if (!json || !key || !out_value || out_size == 0)
      return 0;

   // Build search pattern: "key":
   char pattern[128];
   snprintf(pattern, sizeof(pattern), "\"%s\":", key);

   const char *key_pos = strstr(json, pattern);
   if (!key_pos)
      return 0;

   // Move past the key and colon
   const char *value_start = key_pos + strlen(pattern);

   // Skip whitespace
   while (*value_start && (*value_start == ' ' || *value_start == '\t' || *value_start == '\n'))
      value_start++;

   if (*value_start != '"')
      return 0;  // Not a string value

   value_start++;  // Skip opening quote

   // Use common unescape function
   json_unescape_string(value_start, out_value, out_size);
   return 1;
}

/**
 * @brief Extract integer value from JSON
 */
static int json_extract_int(const char *json, const char *key, int *out_value) {
   if (!json || !key || !out_value)
      return 0;

   char pattern[128];
   snprintf(pattern, sizeof(pattern), "\"%s\":", key);

   const char *key_pos = strstr(json, pattern);
   if (!key_pos)
      return 0;

   const char *value_start = key_pos + strlen(pattern);
   while (*value_start && (*value_start == ' ' || *value_start == '\t'))
      value_start++;

   if (!isdigit(*value_start) && *value_start != '-')
      return 0;

   *out_value = atoi(value_start);
   return 1;
}

/**
 * @brief Escape a URL for safe embedding in JSON string
 *
 * Escapes characters that could break JSON parsing: " \ and control chars
 *
 * @param url URL to escape
 * @param dest Destination buffer
 * @param dest_size Size of destination buffer
 * @return Number of bytes written (excluding null terminator)
 */
static size_t json_escape_url(const char *url, char *dest, size_t dest_size) {
   if (!url || !dest || dest_size == 0)
      return 0;

   size_t out_len = 0;
   while (*url && out_len < dest_size - 2) {  // Reserve space for escape + char + null
      switch (*url) {
         case '"':
            dest[out_len++] = '\\';
            dest[out_len++] = '"';
            break;
         case '\\':
            dest[out_len++] = '\\';
            dest[out_len++] = '\\';
            break;
         case '\n':
            dest[out_len++] = '\\';
            dest[out_len++] = 'n';
            break;
         case '\r':
            dest[out_len++] = '\\';
            dest[out_len++] = 'r';
            break;
         case '\t':
            dest[out_len++] = '\\';
            dest[out_len++] = 't';
            break;
         default:
            if ((unsigned char)*url < 32) {
               // Skip other control characters
            } else {
               dest[out_len++] = *url;
            }
            break;
      }
      url++;
   }
   dest[out_len] = '\0';
   return out_len;
}

/**
 * @brief Fetch URL using FlareSolverr to bypass Cloudflare protection
 *
 * FlareSolverr uses a headless browser to solve JavaScript challenges.
 * This is slower than direct curl but can bypass bot protection.
 *
 * @param url URL to fetch
 * @param out_html Receives allocated HTML content (caller frees)
 * @param out_size Receives size of content (optional, can be NULL)
 * @return URL_FETCH_SUCCESS or error code
 */
static int flaresolverr_fetch(const char *url, char **out_html, size_t *out_size) {
   if (!url || !out_html)
      return URL_FETCH_ERROR_INVALID_URL;

   *out_html = NULL;
   if (out_size)
      *out_size = 0;

   // Escape URL for JSON embedding (prevent JSON injection)
   size_t url_len = strlen(url);
   size_t escaped_url_size = url_len * 2 + 1;  // Worst case: every char escaped
   char *escaped_url = malloc(escaped_url_size);
   if (!escaped_url)
      return URL_FETCH_ERROR_ALLOC;

   json_escape_url(url, escaped_url, escaped_url_size);

   // Build JSON request body
   // Format: {"cmd":"request.get","url":"...","maxTimeout":60000}
   size_t body_size = strlen(escaped_url) + 128;
   char *request_body = malloc(body_size);
   if (!request_body) {
      free(escaped_url);
      return URL_FETCH_ERROR_ALLOC;
   }

   snprintf(request_body, body_size, "{\"cmd\":\"request.get\",\"url\":\"%s\",\"maxTimeout\":%d}",
            escaped_url, flaresolverr_get_timeout() * 1000);
   free(escaped_url);

   LOG_INFO("url_fetcher: Trying FlareSolverr fallback for %s", url);

   // Initialize CURL
   CURL *curl = curl_easy_init();
   if (!curl) {
      free(request_body);
      return URL_FETCH_ERROR_ALLOC;
   }

   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, flaresolverr_get_max_response());

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");

   curl_easy_setopt(curl, CURLOPT_URL, flaresolverr_get_endpoint());
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                    flaresolverr_get_timeout() + 10);  // Extra buffer for processing
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);

   CURLcode res = curl_easy_perform(curl);

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);
   free(request_body);

   if (res != CURLE_OK) {
      LOG_WARNING("url_fetcher: FlareSolverr request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_NETWORK;
   }

   if (!buffer.data || buffer.size == 0) {
      LOG_WARNING("url_fetcher: FlareSolverr returned empty response");
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_EMPTY;
   }

   // Parse FlareSolverr JSON response
   // Expected format:
   // {"status":"ok","message":"...","solution":{"status":200,"response":"<html>..."}}

   // Check status
   char status[32];
   if (!json_extract_string(buffer.data, "status", status, sizeof(status)) ||
       strcmp(status, "ok") != 0) {
      char message[256];
      json_extract_string(buffer.data, "message", message, sizeof(message));
      LOG_WARNING("url_fetcher: FlareSolverr failed: %s", message);
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_HTTP;
   }

   // Check HTTP status from solution - search within solution object only
   int http_status = 0;
   const char *solution_start = strstr(buffer.data, "\"solution\":");
   if (solution_start) {
      json_extract_int(solution_start, "status", &http_status);
   }

   if (http_status != 200) {
      LOG_WARNING("url_fetcher: FlareSolverr got HTTP %d for %s", http_status, url);
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_HTTP;
   }

   // Extract the HTML response - search within solution object for efficiency
   const char *response_key = solution_start ? strstr(solution_start, "\"response\":") : NULL;
   if (!response_key) {
      LOG_WARNING("url_fetcher: FlareSolverr response missing 'response' field");
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_EMPTY;
   }

   // The response field contains the HTML - we need to extract it
   // It's a JSON string, so we need to handle escapes
   const char *html_start = response_key + strlen("\"response\":");
   while (*html_start && (*html_start == ' ' || *html_start == '\t'))
      html_start++;

   if (*html_start != '"') {
      LOG_WARNING("url_fetcher: FlareSolverr response format error");
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_EMPTY;
   }
   html_start++;

   // Allocate buffer for HTML (worst case: same size as remaining JSON)
   size_t remaining = buffer.size - (size_t)(html_start - buffer.data);
   char *html = malloc(remaining + 1);
   if (!html) {
      curl_buffer_free(&buffer);
      return URL_FETCH_ERROR_ALLOC;
   }

   // Decode JSON string escapes using common helper
   size_t html_len = json_unescape_string(html_start, html, remaining + 1);

   curl_buffer_free(&buffer);

   if (html_len < 10) {
      LOG_WARNING("url_fetcher: FlareSolverr returned too little content");
      free(html);
      return URL_FETCH_ERROR_EMPTY;
   }

   LOG_INFO("url_fetcher: FlareSolverr success - got %zu bytes of HTML", html_len);

   *out_html = html;
   if (out_size)
      *out_size = html_len;

   return URL_FETCH_SUCCESS;
}

/**
 * @brief Try FlareSolverr fallback and extract content
 *
 * Common helper for all FlareSolverr fallback paths. Fetches via FlareSolverr,
 * extracts markdown, and returns the result.
 *
 * @param url URL to fetch
 * @param base_url Base URL for relative link resolution (can be NULL, defaults to url)
 * @param out_content Receives extracted markdown (caller frees on success)
 * @param out_size Receives content size (optional, can be NULL)
 * @return URL_FETCH_SUCCESS on success, error code otherwise
 */
static int try_flaresolverr_fallback(const char *url,
                                     const char *base_url,
                                     char **out_content,
                                     size_t *out_size) {
   if (!flaresolverr_is_enabled() || !flaresolverr_is_available()) {
      return URL_FETCH_ERROR_NETWORK;
   }

   char *flare_html = NULL;
   size_t flare_size = 0;
   int flare_result = flaresolverr_fetch(url, &flare_html, &flare_size);

   if (flare_result != URL_FETCH_SUCCESS || !flare_html) {
      free(flare_html);  // safe if NULL
      return URL_FETCH_ERROR_NETWORK;
   }

   char *extracted = NULL;
   const char *effective_base = base_url ? base_url : url;

   int html_result = html_extract_text_with_base(flare_html, flare_size, &extracted,
                                                 effective_base);
   free(flare_html);

   if (html_result != HTML_PARSE_SUCCESS || !extracted) {
      free(extracted);
      return URL_FETCH_ERROR_EMPTY;
   }

   size_t extracted_len = strlen(extracted);
   if (extracted_len == 0) {
      free(extracted);
      return URL_FETCH_ERROR_EMPTY;
   }

   LOG_INFO("url_fetcher: FlareSolverr fallback extracted %zu bytes of markdown", extracted_len);
   LOG_INFO("url_fetcher: Content preview:\n%.2000s%s", extracted,
            extracted_len > 2000 ? "\n... (truncated)" : "");

   *out_content = extracted;
   if (out_size) {
      *out_size = extracted_len;
   }
   return URL_FETCH_SUCCESS;
}

// =============================================================================
// SSRF Protection
// =============================================================================

/**
 * @brief Check if an IP address is in a private/blocked range
 */
static int is_private_ipv4(const char *ip) {
   struct in_addr addr;
   if (inet_pton(AF_INET, ip, &addr) != 1) {
      return 0;  // Not a valid IPv4
   }

   unsigned int ip_num = ntohl(addr.s_addr);

   // 127.0.0.0/8 (localhost)
   if ((ip_num & 0xFF000000) == 0x7F000000)
      return 1;

   // 10.0.0.0/8
   if ((ip_num & 0xFF000000) == 0x0A000000)
      return 1;

   // 172.16.0.0/12
   if ((ip_num & 0xFFF00000) == 0xAC100000)
      return 1;

   // 192.168.0.0/16
   if ((ip_num & 0xFFFF0000) == 0xC0A80000)
      return 1;

   // 169.254.0.0/16 (link-local)
   if ((ip_num & 0xFFFF0000) == 0xA9FE0000)
      return 1;

   // 0.0.0.0/8 (this network)
   if ((ip_num & 0xFF000000) == 0x00000000)
      return 1;

   return 0;
}

/**
 * @brief Check if an IPv6 address is in a blocked range
 */
static int is_private_ipv6(const char *ip) {
   struct in6_addr addr;
   if (inet_pton(AF_INET6, ip, &addr) != 1) {
      return 0;  // Not a valid IPv6
   }

   // ::1 (localhost)
   static const unsigned char localhost[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
   if (memcmp(&addr, localhost, 16) == 0)
      return 1;

   // fe80::/10 (link-local)
   if (addr.s6_addr[0] == 0xFE && (addr.s6_addr[1] & 0xC0) == 0x80)
      return 1;

   // fc00::/7 (unique local)
   if ((addr.s6_addr[0] & 0xFE) == 0xFC)
      return 1;

   // :: (unspecified)
   static const unsigned char unspec[16] = { 0 };
   if (memcmp(&addr, unspec, 16) == 0)
      return 1;

   return 0;
}

/**
 * @brief Extract host and port from URL
 * @param url URL to parse
 * @param host Output buffer for host (caller provides)
 * @param host_size Size of host buffer
 * @param port Output pointer for port number (optional, can be NULL)
 * @return 1 if successful, 0 on error
 */
static int extract_host_port(const char *url, char *host, size_t host_size, int *port) {
   if (!url || !host || host_size == 0)
      return 0;

   const char *host_start = NULL;
   int default_port = 80;

   if (strncmp(url, "http://", 7) == 0) {
      host_start = url + 7;
      default_port = 80;
   } else if (strncmp(url, "https://", 8) == 0) {
      host_start = url + 8;
      default_port = 443;
   } else {
      return 0;
   }

   // Extract host (stop at /, :, or ?)
   size_t i = 0;
   while (*host_start && *host_start != '/' && *host_start != ':' && *host_start != '?' &&
          i < host_size - 1) {
      host[i++] = *host_start++;
   }
   host[i] = '\0';

   if (i == 0)
      return 0;

   // Extract port if present
   if (port) {
      if (*host_start == ':') {
         *port = atoi(host_start + 1);
         if (*port <= 0 || *port > 65535)
            *port = default_port;
      } else {
         *port = default_port;
      }
   }

   return 1;
}

/**
 * @brief Check if URL is blocked and resolve IP for CURLOPT_RESOLVE
 *
 * This function performs DNS resolution once and returns the resolved IP
 * to prevent TOCTOU attacks where DNS could return different IPs between
 * the block check and the actual fetch.
 *
 * @param url URL to check
 * @param resolved_ip Output buffer for resolved IPv4 address (INET_ADDRSTRLEN size)
 * @param host_out Output buffer for extracted host (optional, can be NULL)
 * @param host_out_size Size of host_out buffer
 * @param port_out Output for port number (optional, can be NULL)
 * @return 1 if blocked, 0 if allowed
 */
static int url_is_blocked_with_resolve(const char *url,
                                       char *resolved_ip,
                                       char *host_out,
                                       size_t host_out_size,
                                       int *port_out) {
   if (!url)
      return 1;

   if (resolved_ip)
      resolved_ip[0] = '\0';

   // Extract host and port from URL
   char host[512];
   int port = 80;
   if (!extract_host_port(url, host, sizeof(host), &port)) {
      return 1;  // Invalid URL
   }

   // Copy host out if requested
   if (host_out && host_out_size > 0) {
      strncpy(host_out, host, host_out_size - 1);
      host_out[host_out_size - 1] = '\0';
   }
   if (port_out)
      *port_out = port;

   // Check for localhost variants first (before DNS)
   if (strcasecmp(host, "localhost") == 0)
      return 1;
   if (strcasecmp(host, "localhost.localdomain") == 0)
      return 1;

   // Check for cloud metadata endpoints
   if (strcmp(host, "169.254.169.254") == 0)
      return 1;
   if (strcasecmp(host, "metadata.google.internal") == 0)
      return 1;

   // Check IPv6 in brackets
   if (host[0] == '[') {
      char ipv6[128];
      size_t j = 0;
      for (size_t k = 1; k < strlen(host) && host[k] != ']' && j < sizeof(ipv6) - 1; k++) {
         ipv6[j++] = host[k];
      }
      ipv6[j] = '\0';
      if (is_private_ipv6(ipv6))
         return 1;
   }

   // Check if it's a direct IP address
   if (is_private_ipv4(host))
      return 1;
   if (is_private_ipv6(host))
      return 1;

   // Single DNS resolution for both whitelist check and SSRF protection
   char first_ipv4[INET_ADDRSTRLEN] = "";
   int is_blocked = 0;

   struct addrinfo hints = { 0 };
   struct addrinfo *result = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   if (getaddrinfo(host, NULL, &hints, &result) == 0) {
      for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
         char ip_str[INET6_ADDRSTRLEN];

         if (rp->ai_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));

            // Save first IPv4 for CURLOPT_RESOLVE
            if (first_ipv4[0] == '\0') {
               strncpy(first_ipv4, ip_str, sizeof(first_ipv4) - 1);
               first_ipv4[sizeof(first_ipv4) - 1] = '\0';
            }

            // Check if this IP is private/blocked
            if (is_private_ipv4(ip_str)) {
               is_blocked = 1;
               break;
            }
         } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)rp->ai_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));

            // Check if this IPv6 is private/blocked
            if (is_private_ipv6(ip_str)) {
               is_blocked = 1;
               break;
            }
         }
      }
      freeaddrinfo(result);
   }

   // Check whitelist with the resolved IP (before returning blocked status)
   if (is_whitelisted(url, host, first_ipv4[0] ? first_ipv4 : NULL)) {
      // Whitelisted - return the resolved IP for curl to use
      if (resolved_ip && first_ipv4[0]) {
         strncpy(resolved_ip, first_ipv4, INET_ADDRSTRLEN - 1);
         resolved_ip[INET_ADDRSTRLEN - 1] = '\0';
      }
      return 0;  // Not blocked
   }

   if (is_blocked) {
      return 1;  // Blocked
   }

   // Not blocked - return resolved IP for CURLOPT_RESOLVE
   if (resolved_ip && first_ipv4[0]) {
      strncpy(resolved_ip, first_ipv4, INET_ADDRSTRLEN - 1);
      resolved_ip[INET_ADDRSTRLEN - 1] = '\0';
   }

   return 0;  // Not blocked
}

int url_is_blocked(const char *url) {
   // Public wrapper uses internal function without IP capture
   return url_is_blocked_with_resolve(url, NULL, NULL, 0, NULL);
}

// =============================================================================
// Public API
// =============================================================================

int url_is_valid(const char *url) {
   if (!url)
      return 0;

   if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
      return 0;
   }

   const char *host_start = url + (url[4] == 's' ? 8 : 7);
   if (strlen(host_start) < 1)
      return 0;
   if (!isalnum(*host_start) && *host_start != '[')
      return 0;

   return 1;
}

int url_fetch_content_with_base(const char *url,
                                char **out_content,
                                size_t *out_size,
                                const char *base_url) {
   if (!url || !out_content) {
      return URL_FETCH_ERROR_INVALID_URL;
   }
   *out_content = NULL;
   if (out_size)
      *out_size = 0;

   if (!url_is_valid(url)) {
      LOG_WARNING("url_fetcher: Invalid URL: %s", url);
      return URL_FETCH_ERROR_INVALID_URL;
   }

   // SSRF protection with DNS pinning to prevent TOCTOU attacks
   // We resolve DNS once here and pass the IP to curl via CURLOPT_RESOLVE
   char resolved_ip[INET_ADDRSTRLEN] = "";
   char host[512] = "";
   int port = 80;

   if (url_is_blocked_with_resolve(url, resolved_ip, host, sizeof(host), &port)) {
      LOG_WARNING("url_fetcher: Blocked URL (private/internal address): %s", url);
      return URL_FETCH_ERROR_BLOCKED_URL;
   }

   LOG_INFO("url_fetcher: Fetching %s (resolved to %s:%d)", url,
            resolved_ip[0] ? resolved_ip : "unresolved", port);

   // Create CURL handle once and reuse across retries (saves ~100-200us per retry)
   CURL *curl = curl_easy_init();
   if (!curl) {
      LOG_ERROR("url_fetcher: Failed to create CURL handle");
      return URL_FETCH_ERROR_NETWORK;
   }

   // Build DNS pinning resolve list once (reused across retries)
   struct curl_slist *resolve_list = NULL;
   if (resolved_ip[0] && host[0]) {
      char resolve_entry[600];
      snprintf(resolve_entry, sizeof(resolve_entry), "%s:%d:%s", host, port, resolved_ip);
      resolve_list = curl_slist_append(resolve_list, resolve_entry);
      LOG_INFO("url_fetcher: DNS pinned: %s", resolve_entry);
   }

   // Build headers list once (reused across retries)
   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,text/plain");
   headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

   // Retry loop for transient failures
   curl_buffer_t buffer;
   CURLcode res = CURLE_OK;
   long http_code = 0;
   char *content_type = NULL;
   int retry_count = 0;

   for (retry_count = 0; retry_count <= URL_FETCH_MAX_RETRIES; retry_count++) {
      if (retry_count > 0) {
         // Exponential backoff: 500ms, 1000ms, 2000ms
         int delay_ms = URL_FETCH_RETRY_DELAY_MS * (1 << (retry_count - 1));
         LOG_WARNING("url_fetcher: Retry %d/%d for %s (delay %dms)", retry_count,
                     URL_FETCH_MAX_RETRIES, url, delay_ms);
         usleep(delay_ms * 1000);

         // Reset CURL handle for retry (preserves connection pool)
         curl_easy_reset(curl);
      }

      curl_buffer_init_with_max(&buffer, URL_FETCH_MAX_SIZE);

      // Set options (need to set each retry since curl_easy_reset clears them)
      if (resolve_list) {
         curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
      }
      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, URL_FETCH_USER_AGENT);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, URL_FETCH_TIMEOUT_SEC);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

      res = curl_easy_perform(curl);

      // Check for retryable curl errors
      if (res != CURLE_OK) {
         // Check if this was a buffer truncation (response too large)
         // We can still use the partial content we received
         if (res == CURLE_WRITE_ERROR && buffer.truncated && buffer.data && buffer.size > 0) {
            LOG_WARNING("url_fetcher: Response truncated at %zuKB (max %zuKB) on attempt %d for %s",
                        buffer.size / 1024, URL_FETCH_MAX_SIZE / 1024, retry_count + 1, url);
            // Get HTTP code and content type before breaking (needed for post-loop checks)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
            // Continue processing with truncated content - don't treat as error
            // The truncated flag will be checked later to add a notice
            // Clear the error so post-loop check doesn't fail
            res = CURLE_OK;
            break;
         }
         LOG_WARNING("url_fetcher: Network error on attempt %d: %s", retry_count + 1,
                     curl_easy_strerror(res));
         curl_buffer_free(&buffer);
         if (is_retryable_curl_error(res) && retry_count < URL_FETCH_MAX_RETRIES) {
            continue;  // Retry with same handle
         }
         // Non-retryable error - cleanup and return
         curl_slist_free_all(headers);
         if (resolve_list)
            curl_slist_free_all(resolve_list);
         curl_easy_cleanup(curl);
         return URL_FETCH_ERROR_NETWORK;
      }

      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

      // Check for retryable HTTP errors
      if (is_retryable_http_code(http_code) && retry_count < URL_FETCH_MAX_RETRIES) {
         LOG_WARNING("url_fetcher: HTTP %ld on attempt %d, will retry", http_code, retry_count + 1);
         curl_buffer_free(&buffer);
         continue;  // Retry with same handle
      }

      // Success or non-retryable error - exit loop
      break;
   }

   // Final error checking after all retries exhausted
   // NOTE: Must be done BEFORE curl_easy_cleanup() since content_type points to CURL's internal memory
   if (res != CURLE_OK) {
      LOG_ERROR("url_fetcher: Network error after %d retries: %s", retry_count,
                curl_easy_strerror(res));
      curl_slist_free_all(headers);
      if (resolve_list)
         curl_slist_free_all(resolve_list);
      curl_easy_cleanup(curl);

      // For redirect loops (often caused by JS-based paywalls), try FlareSolverr
      if (res == CURLE_TOO_MANY_REDIRECTS) {
         LOG_INFO("url_fetcher: Redirect loop detected, trying FlareSolverr fallback");
         if (try_flaresolverr_fallback(url, base_url, out_content, out_size) == URL_FETCH_SUCCESS) {
            return URL_FETCH_SUCCESS;
         }
         LOG_WARNING("url_fetcher: FlareSolverr fallback failed for redirect loop");
      }

      return URL_FETCH_ERROR_NETWORK;
   }

   if (http_code < 200 || http_code >= 300) {
      LOG_WARNING("url_fetcher: HTTP error %ld for %s after %d attempts", http_code, url,
                  retry_count + 1);
      curl_buffer_free(&buffer);
      curl_slist_free_all(headers);
      if (resolve_list)
         curl_slist_free_all(resolve_list);
      curl_easy_cleanup(curl);

      // If we got 401/403, try FlareSolverr as fallback (bot protection)
      // Note: Some sites like Reuters use 401 Unauthorized instead of 403 for bot blocking
      if (http_code == 401 || http_code == 403) {
         LOG_INFO("url_fetcher: HTTP %ld, trying FlareSolverr fallback", http_code);
         if (try_flaresolverr_fallback(url, base_url, out_content, out_size) == URL_FETCH_SUCCESS) {
            return URL_FETCH_SUCCESS;
         }
         LOG_WARNING("url_fetcher: FlareSolverr fallback failed for %s", url);
      }

      return URL_FETCH_ERROR_HTTP;
   }

   // Validate Content-Type (must be done before curl_easy_cleanup since content_type
   // points to CURL's internal memory which is freed by cleanup)
   if (content_type && !is_allowed_content_type(content_type)) {
      LOG_WARNING("url_fetcher: Invalid Content-Type '%s' for %s", content_type, url);
      curl_buffer_free(&buffer);
      curl_slist_free_all(headers);
      if (resolve_list)
         curl_slist_free_all(resolve_list);
      curl_easy_cleanup(curl);
      return URL_FETCH_ERROR_INVALID_CONTENT_TYPE;
   }

   if (!buffer.data || buffer.size == 0) {
      LOG_WARNING("url_fetcher: Empty response from %s", url);
      curl_buffer_free(&buffer);
      curl_slist_free_all(headers);
      if (resolve_list)
         curl_slist_free_all(resolve_list);
      curl_easy_cleanup(curl);
      return URL_FETCH_ERROR_EMPTY;
   }

   LOG_INFO("url_fetcher: Downloaded %zu bytes from %s (Content-Type: %s)", buffer.size, url,
            content_type ? content_type : "unknown");

   // Cleanup CURL resources now (after we're done with content_type pointer)
   curl_slist_free_all(headers);
   if (resolve_list)
      curl_slist_free_all(resolve_list);
   curl_easy_cleanup(curl);

   // Check if HTML
   const char *html_start = strcasestr_portable(buffer.data, "<html");
   if (!html_start)
      html_start = strcasestr_portable(buffer.data, "<!doctype");

   char *extracted = NULL;
   int result;
   int was_truncated = buffer.truncated;

   // Use provided base_url or fall back to the fetched URL
   const char *effective_base = base_url ? base_url : url;

   if (html_start || strcasestr_portable(buffer.data, "<body")) {
      int html_result = html_extract_text_with_base(buffer.data, buffer.size, &extracted,
                                                    effective_base);

      // Translate HTML parser error codes to URL fetcher error codes
      switch (html_result) {
         case HTML_PARSE_SUCCESS:
            result = URL_FETCH_SUCCESS;
            break;
         case HTML_PARSE_ERROR_INVALID_INPUT:
            result = URL_FETCH_ERROR_INVALID_URL;
            break;
         case HTML_PARSE_ERROR_ALLOC:
            result = URL_FETCH_ERROR_ALLOC;
            break;
         case HTML_PARSE_ERROR_EMPTY:
            result = URL_FETCH_ERROR_EMPTY;
            break;
         default:
            result = URL_FETCH_ERROR_EMPTY;
            break;
      }
   } else {
      extracted = strdup(buffer.data);
      result = extracted ? URL_FETCH_SUCCESS : URL_FETCH_ERROR_ALLOC;
   }

   curl_buffer_free(&buffer);

   if (result != URL_FETCH_SUCCESS) {
      // If content extraction failed (likely JS-rendered page), try FlareSolverr
      LOG_INFO("url_fetcher: Direct fetch failed with %d, trying FlareSolverr fallback", result);
      if (try_flaresolverr_fallback(url, base_url, out_content, out_size) == URL_FETCH_SUCCESS) {
         return URL_FETCH_SUCCESS;
      }
      LOG_WARNING("url_fetcher: FlareSolverr fallback also failed");
      return result;
   }

   size_t extracted_len = strlen(extracted);

   // If extraction succeeded but returned empty/minimal content, try FlareSolverr
   // (Many JS-rendered pages return valid HTML with no text content)
   if (extracted_len < 100) {
      LOG_INFO("url_fetcher: Extracted only %zu bytes, trying FlareSolverr fallback",
               extracted_len);
      char *flare_content = NULL;
      size_t flare_len = 0;
      if (try_flaresolverr_fallback(url, base_url, &flare_content, &flare_len) ==
          URL_FETCH_SUCCESS) {
         free(extracted);
         extracted = flare_content;
         extracted_len = flare_len;
      } else {
         LOG_WARNING("url_fetcher: FlareSolverr fallback failed, using minimal content");
         // Keep original extracted content even if minimal
      }
   }

   LOG_INFO("url_fetcher: Extracted %zu bytes of markdown content%s", extracted_len,
            was_truncated ? " (truncated)" : "");

   // Log a preview of the content for debugging (truncate at 2000 chars)
   if (extracted_len > 0) {
      LOG_INFO("url_fetcher: Content preview:\n%.2000s%s", extracted,
               extracted_len > 2000 ? "\n... (truncated)" : "");
   }

   // Add truncation notice if content was cut off
   if (was_truncated) {
      const char *notice = "\n\n---\n**Note: This page was truncated due to size limits. "
                           "The content above may be incomplete.**\n";
      size_t notice_len = strlen(notice);
      char *with_notice = realloc(extracted, extracted_len + notice_len + 1);
      if (with_notice) {
         memcpy(with_notice + extracted_len, notice, notice_len + 1);
         extracted = with_notice;
         extracted_len += notice_len;
      }
   }

   *out_content = extracted;
   if (out_size)
      *out_size = extracted_len;

   return URL_FETCH_SUCCESS;
}

int url_fetch_content(const char *url, char **out_content, size_t *out_size) {
   return url_fetch_content_with_base(url, out_content, out_size, NULL);
}

const char *url_fetch_error_string(int error_code) {
   switch (error_code) {
      case URL_FETCH_SUCCESS:
         return "Success";
      case URL_FETCH_ERROR_INVALID_URL:
         return "Invalid URL";
      case URL_FETCH_ERROR_NETWORK:
         return "Network error";
      case URL_FETCH_ERROR_HTTP:
         return "HTTP error";
      case URL_FETCH_ERROR_ALLOC:
         return "Memory allocation error";
      case URL_FETCH_ERROR_EMPTY:
         return "Empty or invalid content";
      case URL_FETCH_ERROR_TOO_LARGE:
         return "Content too large";
      case URL_FETCH_ERROR_BLOCKED_URL:
         return "URL blocked (private/internal address)";
      case URL_FETCH_ERROR_INVALID_CONTENT_TYPE:
         return "Invalid content type (not text/html)";
      default:
         return "Unknown error";
   }
}
