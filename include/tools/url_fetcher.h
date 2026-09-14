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
 *
 * Thread Safety: All functions in this module are thread-safe. Each call to
 * url_fetch_content() creates its own CURL handle and uses only stack-local
 * state. Multiple threads can safely fetch URLs concurrently.
 *
 * Security: This module includes SSRF (Server-Side Request Forgery) protection
 * by blocking requests to private IP ranges (localhost, 10.x.x.x, 172.16-31.x.x,
 * 192.168.x.x, 169.254.x.x, and IPv6 link-local addresses).
 */

#ifndef URL_FETCHER_H
#define URL_FETCHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define URL_FETCH_SUCCESS 0
#define URL_FETCH_ERROR_INVALID_URL 1
#define URL_FETCH_ERROR_NETWORK 2
#define URL_FETCH_ERROR_HTTP 3
#define URL_FETCH_ERROR_ALLOC 4
#define URL_FETCH_ERROR_EMPTY 5
#define URL_FETCH_ERROR_TOO_LARGE 6
#define URL_FETCH_ERROR_BLOCKED_URL 7
#define URL_FETCH_ERROR_INVALID_CONTENT_TYPE 8

/* Configuration */
#define URL_FETCH_MAX_SIZE (512 * 1024) /* 512KB max download */
#define URL_FETCH_TIMEOUT_SEC 15        /* Connection timeout */
#define URL_FETCH_USER_AGENT "DAWN/1.0 (Voice Assistant; +https://github.com/The-OASIS-Project)"
#define URL_FETCH_MAX_WHITELIST 32 /* Maximum whitelist entries */

/**
 * @brief Initialize the URL fetcher module
 *
 * Currently a no-op, provided for consistency with other tool modules
 * and future configuration needs.
 *
 * @return URL_FETCH_SUCCESS always
 */
int url_fetcher_init(void);

/**
 * @brief Clean up the URL fetcher module
 *
 * Clears the whitelist and frees any allocated memory.
 */
void url_fetcher_cleanup(void);

/**
 * @brief Check if the URL fetcher module is initialized
 *
 * @return 1 always (module is stateless and always ready)
 */
int url_fetcher_is_initialized(void);

/**
 * @brief Fetch URL and extract readable Markdown content
 *
 * Downloads the URL, validates Content-Type, converts HTML to Markdown,
 * and returns structured text suitable for LLM consumption.
 *
 * Security: Blocks requests to private/internal IP addresses to prevent SSRF.
 *
 * @param url URL to fetch (must be http:// or https://)
 * @param out_content Receives allocated string with extracted content (caller frees)
 * @param out_size Receives size of content in bytes (optional, can be NULL)
 * @return URL_FETCH_SUCCESS or error code
 */
int url_fetch_content(const char *url, char **out_content, size_t *out_size);

/**
 * @brief Fetch URL and extract readable Markdown content with base URL for link resolution
 *
 * Same as url_fetch_content but resolves relative URLs to absolute URLs in the output.
 *
 * @param url URL to fetch (must be http:// or https://)
 * @param out_content Receives allocated string with extracted content (caller frees)
 * @param out_size Receives size of content in bytes (optional, can be NULL)
 * @param base_url Base URL for resolving relative links (can be NULL to use fetch URL)
 * @return URL_FETCH_SUCCESS or error code
 */
int url_fetch_content_with_base(const char *url,
                                char **out_content,
                                size_t *out_size,
                                const char *base_url);

/**
 * @brief Validate URL format
 *
 * Checks that the URL uses http:// or https:// scheme.
 *
 * @param url URL to validate
 * @return 1 if valid http/https URL, 0 otherwise
 */
int url_is_valid(const char *url);

/**
 * @brief Check if URL points to a blocked (private/internal) address
 *
 * Blocks localhost, private IP ranges (RFC 1918), link-local addresses,
 * and cloud metadata endpoints to prevent SSRF attacks.
 * URLs matching whitelist entries are allowed even if they would otherwise be blocked.
 *
 * @param url URL to check
 * @return 1 if URL is blocked, 0 if allowed
 */
int url_is_blocked(const char *url);

/**
 * @brief Check if URL is blocked and resolve DNS, returning the resolved IP
 *
 * Same as url_is_blocked but also performs DNS resolution and returns the
 * resolved IP address for use with CURLOPT_RESOLVE (DNS pinning to prevent
 * TOCTOU/DNS rebinding attacks).
 *
 * @param url URL to check
 * @param resolved_ip Buffer for resolved IP (at least INET_ADDRSTRLEN bytes, can be NULL)
 * @param host_out Buffer for extracted hostname (can be NULL)
 * @param host_out_size Size of host_out buffer
 * @param port_out Receives extracted port number (can be NULL)
 * @return 1 if URL is blocked, 0 if allowed
 */
int url_is_blocked_with_resolve(const char *url,
                                char *resolved_ip,
                                char *host_out,
                                size_t host_out_size,
                                int *port_out);

/**
 * @brief Add a URL or CIDR network to the whitelist
 *
 * Whitelisted URLs/networks are allowed even if they would normally be blocked
 * by SSRF protection. Supports:
 * - Specific URLs: "http://192.168.1.100:8080/api"
 * - Hostnames: "wiki.local"
 * - IPv4 CIDR: "192.168.1.0/24"
 * - IPv4 address: "10.0.0.5"
 *
 * Thread Safety: This function modifies global state and should only be called
 * during initialization, not concurrently with url_fetch_content().
 *
 * @param entry URL, hostname, IP address, or CIDR network to whitelist
 * @return URL_FETCH_SUCCESS or URL_FETCH_ERROR_ALLOC if whitelist is full
 */
int url_whitelist_add(const char *entry);

/**
 * @brief Remove an entry from the whitelist
 *
 * @param entry Entry to remove (must match exactly what was added)
 * @return URL_FETCH_SUCCESS if removed, URL_FETCH_ERROR_INVALID_URL if not found
 */
int url_whitelist_remove(const char *entry);

/**
 * @brief Clear all whitelist entries
 */
void url_whitelist_clear(void);

/**
 * @brief Get the number of whitelist entries
 *
 * @return Number of entries currently in the whitelist
 */
int url_whitelist_count(void);

/**
 * @brief Get human-readable error message
 *
 * @param error_code Error code from url_fetch_content
 * @return Static string describing the error
 */
const char *url_fetch_error_string(int error_code);

/**
 * @brief Get LLM-actionable error message including retryability hints.
 *
 * Combines the error class with thread-local detail (HTTP status code,
 * curl error text) captured during the most recent url_fetch_content()
 * call.  Output messages tell the LLM what to do next: retry (transient
 * 5xx/network), stop (404 / blocked), or fall back to search snippets.
 *
 * Returns the number of bytes written (excluding NUL).  The thread-local
 * detail is reset at the start of every url_fetch_content() call.
 *
 * @param error_code Error code from url_fetch_content
 * @param out        Caller-supplied buffer
 * @param out_size   Buffer size (recommended >= 256)
 * @return Bytes written (excluding NUL)
 */
size_t url_fetch_error_describe(int error_code, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* URL_FETCHER_H */
