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
 * Shared CURL buffer utilities for HTTP response handling
 */

#ifndef CURL_BUFFER_H
#define CURL_BUFFER_H

#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* CURLOPT_PROTOCOLS was deprecated in 7.85.0 in favor of CURLOPT_PROTOCOLS_STR.
 * Provide a compat macro so callers can use the string form everywhere. */
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0 */
#define DAWN_CURL_SET_PROTOCOLS(curl, str) curl_easy_setopt((curl), CURLOPT_PROTOCOLS_STR, (str))
#else
/* Fallback: parse the comma-separated string into the old bitmask flags. */
#define DAWN_CURL_SET_PROTOCOLS(curl, str) _dawn_curl_set_protocols_compat((curl), (str))

static inline CURLcode _dawn_curl_set_protocols_compat(CURL *curl, const char *protocols) {
   long mask = 0;
   /* Match exact comma-delimited tokens to avoid "http" matching inside "https" */
   const char *p = protocols;
   while (*p) {
      size_t len = 0;
      while (p[len] && p[len] != ',')
         len++;
      if (len == 5 && memcmp(p, "https", 5) == 0)
         mask |= CURLPROTO_HTTPS;
      else if (len == 4 && memcmp(p, "http", 4) == 0)
         mask |= CURLPROTO_HTTP;
      else if (len == 5 && memcmp(p, "imaps", 5) == 0)
         mask |= CURLPROTO_IMAPS;
      else if (len == 4 && memcmp(p, "imap", 4) == 0)
         mask |= CURLPROTO_IMAP;
      else if (len == 5 && memcmp(p, "smtps", 5) == 0)
         mask |= CURLPROTO_SMTPS;
      else if (len == 4 && memcmp(p, "smtp", 4) == 0)
         mask |= CURLPROTO_SMTP;
      p += len;
      if (*p == ',')
         p++;
   }
   return curl_easy_setopt(curl, CURLOPT_PROTOCOLS, mask);
}
#endif

// Buffer capacity constants
#define CURL_BUFFER_INITIAL_CAPACITY 4096

// Configurable max capacity - can be overridden before including this header
// Use smaller values for constrained use cases (e.g., web search)
#ifndef CURL_BUFFER_MAX_CAPACITY
#define CURL_BUFFER_MAX_CAPACITY (128 * 1024)  // 128KB default for LLM responses
#endif

// Predefined max capacities for different use cases
#define CURL_BUFFER_MAX_WEB_SEARCH \
   (1024 * 1024)                          // 1MB for web search (IT/science categories can be large)
#define CURL_BUFFER_MAX_LLM (128 * 1024)  // 128KB for LLM responses
#define CURL_BUFFER_MAX_STREAMING (256 * 1024)  // 256KB for streaming responses

/**
 * Buffer structure for accumulating CURL response data
 * Initialize with curl_buffer_init() or curl_buffer_init_with_max()
 */
typedef struct {
   char *data;           // Response data (null-terminated)
   size_t size;          // Current size of data (excluding null terminator)
   size_t capacity;      // Allocated capacity
   size_t max_capacity;  // Maximum allowed capacity (0 = use default)
   int truncated;        // Set to 1 if response exceeded max_capacity
} curl_buffer_t;

/**
 * CURL write callback with exponential buffer growth
 *
 * Usage:
 *   curl_buffer_t buffer = {NULL, 0, 0, 0};
 *   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
 *   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
 *   // ... perform request ...
 *   // buffer.data now contains null-terminated response
 *   curl_buffer_free(&buffer);
 *
 * @param contents Data received from CURL
 * @param size Size of each element
 * @param nmemb Number of elements
 * @param userp Pointer to curl_buffer_t
 * @return Number of bytes handled, or 0 on error
 */
static inline size_t curl_buffer_write_callback(void *contents,
                                                size_t size,
                                                size_t nmemb,
                                                void *userp) {
   curl_buffer_t *buf = (curl_buffer_t *)userp;

   // Overflow protection: check multiplication before performing it
   if (size > 0 && nmemb > SIZE_MAX / size) {
      return 0;  // Signal error to CURL - would overflow
   }
   size_t total_size = size * nmemb;

   // Use per-buffer max capacity if set, otherwise use compile-time default
   size_t max_cap = buf->max_capacity ? buf->max_capacity : CURL_BUFFER_MAX_CAPACITY;

   // Check if we need to grow the buffer
   // Overflow protection: check addition
   if (buf->size > SIZE_MAX - total_size - 1) {
      return 0;  // Signal error to CURL - would overflow
   }
   size_t required = buf->size + total_size + 1;

   if (required > buf->capacity) {
      // Exponential growth to reduce reallocations
      size_t new_capacity = buf->capacity ? buf->capacity : CURL_BUFFER_INITIAL_CAPACITY;
      // Prevent overflow by checking before doubling
      while (new_capacity < required && new_capacity <= max_cap / 2) {
         new_capacity *= 2;
      }
      // Cap at per-buffer maximum
      if (new_capacity < required && required <= max_cap) {
         new_capacity = required;  // Exact fit instead of max
      }
      // Handle exceeding max capacity gracefully - truncate instead of failing
      if (required > max_cap) {
         buf->truncated = 1;  // Mark as truncated for caller to check
         // Return total_size to let curl continue (we just won't store more data)
         // This allows partial results instead of complete failure
         return total_size;
      }

      char *new_data = (char *)realloc(buf->data, new_capacity);
      if (!new_data) {
         return 0;  // Signal error to CURL
      }
      buf->data = new_data;
      buf->capacity = new_capacity;
   }

   memcpy(&(buf->data[buf->size]), contents, total_size);
   buf->size += total_size;
   buf->data[buf->size] = '\0';

   return total_size;
}

/**
 * Initialize a curl buffer with default max capacity
 * @param buf Buffer to initialize
 */
static inline void curl_buffer_init(curl_buffer_t *buf) {
   buf->data = NULL;
   buf->size = 0;
   buf->capacity = 0;
   buf->max_capacity = 0;  // Use compile-time default
   buf->truncated = 0;
}

/**
 * Initialize a curl buffer with custom max capacity
 * @param buf Buffer to initialize
 * @param max_cap Maximum capacity in bytes (use CURL_BUFFER_MAX_* constants)
 */
static inline void curl_buffer_init_with_max(curl_buffer_t *buf, size_t max_cap) {
   buf->data = NULL;
   buf->size = 0;
   buf->capacity = 0;
   buf->max_capacity = max_cap;
   buf->truncated = 0;
}

/**
 * Reset a curl buffer for reuse (keeps allocated memory)
 * Use this when making multiple requests with the same buffer to avoid
 * repeated malloc/free cycles.
 * @param buf Buffer to reset
 */
static inline void curl_buffer_reset(curl_buffer_t *buf) {
   buf->size = 0;
   buf->truncated = 0;
   if (buf->data) {
      buf->data[0] = '\0';
   }
}

/**
 * Free a curl buffer's data
 * @param buf Buffer to free
 */
static inline void curl_buffer_free(curl_buffer_t *buf) {
   if (buf->data) {
      free(buf->data);
      buf->data = NULL;
   }
   buf->size = 0;
   buf->capacity = 0;
   // Preserve max_capacity setting for potential reinitialization
}

/**
 * Apply DAWN's standard defense-in-depth setopts to a CURL handle.
 *
 * Sets: TCP_KEEPALIVE, HTTP_VERSION_2, SSL_VERIFYPEER=1, SSL_VERIFYHOST=2,
 *       USERAGENT (caller-supplied, only if non-NULL/non-empty),
 *       TIMEOUT (caller-supplied, only if > 0),
 *       WRITEFUNCTION (curl_buffer_write_callback),
 *       WRITEDATA (caller-supplied buffer, only if non-NULL).
 *
 * Caller still owns: URL, POST/POSTFIELDS, HTTPHEADER, CUSTOMREQUEST,
 * any provider-specific options (FOLLOWLOCATION, REDIR_PROTOCOLS, etc.).
 *
 * Rationale: every cloud-facing libcurl handle in DAWN needs these
 * setopts.  Centralizing prevents drift if a future libcurl regresses
 * a default and prevents new code from accidentally omitting one of
 * the security setopts.  Same defense-in-depth pattern documented in
 * oauth_client.c — explicit setopts protect against environment-variable
 * overrides (CURL_CA_BUNDLE etc.) and version-specific default changes.
 *
 * HTTP/2 with TLS gracefully falls back to HTTP/1.1 if the server
 * doesn't support h2, so it's safe to set unconditionally on any
 * HTTPS endpoint.  Do NOT use this helper for IMAP/SMTP handles —
 * USERAGENT and HTTP_VERSION don't apply to those protocols.
 *
 * @param curl        CURL handle (must be non-NULL; no-op if NULL)
 * @param user_agent  User-Agent header value (skip if NULL or empty)
 * @param timeout_sec Overall transfer timeout in seconds (skip if <= 0)
 * @param resp        WRITEDATA pointer, typically `curl_buffer_t *`
 *                    (WRITEDATA skipped if NULL; WRITEFUNCTION still set)
 */
static inline void curl_apply_dawn_defaults(CURL *curl,
                                            const char *user_agent,
                                            long timeout_sec,
                                            void *resp) {
   if (!curl) {
      return;
   }
   /* CURLOPT_NOSIGNAL is mandatory for libcurl used from worker/listener threads:
    * without it libcurl may use signals (SIGALRM for timeouts) and a TLS write to
    * a peer-closed socket can raise SIGPIPE on the calling thread. The daemon also
    * ignores SIGPIPE process-wide (dawn.c), but NOSIGNAL is the documented
    * thread-safe setting. Requires a threadsafe resolver (libcurl's default). */
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
   curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   if (user_agent && user_agent[0]) {
      curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
   }
   if (timeout_sec > 0) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
   }
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   if (resp != NULL) {
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
   }
}

#endif  // CURL_BUFFER_H
