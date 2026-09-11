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
 * WebUI HTTP Handlers - Static file serving and authentication endpoints
 *
 * This module handles:
 * - Static file serving from www/ directory
 * - Authentication API endpoints (/api/auth/*)
 * - Health check endpoint
 */

#define _GNU_SOURCE /* For strcasestr */

#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/ota.h"
#include "core/rate_limiter.h"
#include "logging.h"
#include "ui/metrics.h"
#include "version.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

#ifdef ENABLE_AUTH
#include <sodium.h>

#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "image_store.h"
#include "memory/memory_db_aliases.h"
#include "webui/webui_documents.h"
#include "webui/webui_images.h"
#endif

/* HTTP 429 Too Many Requests - not defined in older libwebsockets */
#ifndef HTTP_STATUS_TOO_MANY_REQUESTS
#define HTTP_STATUS_TOO_MANY_REQUESTS 429
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Rate limiting for login attempts */
#define RATE_LIMIT_WINDOW_SEC (15 * 60) /* 15 minutes */
#define RATE_LIMIT_MAX_ATTEMPTS 20      /* Max attempts per IP in window */

/* CSRF endpoint rate limiting */
#define CSRF_RATE_LIMIT_WINDOW_SEC 60 /* 1 minute */
#define CSRF_RATE_LIMIT_MAX 30        /* Max 30 tokens per minute per IP */

/* Service token rate limiting (machine-to-machine image API) */
#define SERVICE_RATE_LIMIT_WINDOW_SEC 60 /* 1 minute */
#define SERVICE_RATE_LIMIT_MAX 120       /* Max 120 image fetches per minute */

#ifdef ENABLE_AUTH
/* CSRF nonce tracking for single-use tokens */
#define CSRF_USED_NONCE_SIZE 16    /* Nonce size in bytes */
#define CSRF_USED_NONCE_COUNT 1024 /* Track last 1024 used nonces */
_Static_assert((CSRF_USED_NONCE_COUNT & (CSRF_USED_NONCE_COUNT - 1)) == 0,
               "CSRF_USED_NONCE_COUNT must be power of 2");

#define CSRF_RATE_LIMIT_SLOTS 32 /* Track up to 32 concurrent IPs */
#endif

/* Login rate limiting */
#define LOGIN_RATE_LIMIT_SLOTS 32 /* Track up to 32 concurrent IPs */

#ifdef ENABLE_AUTH
/* Dummy password hash for timing equalization on non-existent users.
 * Uses Argon2id with same parameters as real hashes to ensure constant timing. */
static const char DUMMY_PASSWORD_HASH[] = "$argon2id$v=19$m=16384,t=3,p=1$"
                                          "aaaaaaaaaaaaaaaaaaaaaa$"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#endif

/* =============================================================================
 * Module State
 * ============================================================================= */

#ifdef ENABLE_AUTH
/* CSRF single-use nonce tracking (circular buffer)
 * Tracks recently used nonces to prevent token replay within validity window */
static struct {
   unsigned char nonces[CSRF_USED_NONCE_COUNT][CSRF_USED_NONCE_SIZE];
   int head;
   pthread_mutex_t mutex;
} s_csrf_used = { .head = 0, .mutex = PTHREAD_MUTEX_INITIALIZER };

/* CSRF endpoint rate limiting (uses generic rate limiter) */
static rate_limit_entry_t s_csrf_rate_entries[CSRF_RATE_LIMIT_SLOTS];
static rate_limiter_t s_csrf_rate = RATE_LIMITER_STATIC_INIT(s_csrf_rate_entries,
                                                             CSRF_RATE_LIMIT_SLOTS,
                                                             CSRF_RATE_LIMIT_MAX,
                                                             CSRF_RATE_LIMIT_WINDOW_SEC);

/* Login rate limiting (uses generic rate limiter) */
static rate_limit_entry_t s_login_rate_entries[LOGIN_RATE_LIMIT_SLOTS];
static rate_limiter_t s_login_rate = RATE_LIMITER_STATIC_INIT(s_login_rate_entries,
                                                              LOGIN_RATE_LIMIT_SLOTS,
                                                              RATE_LIMIT_MAX_ATTEMPTS,
                                                              RATE_LIMIT_WINDOW_SEC);

/* Service token rate limiting (machine-to-machine image API) */
#define SERVICE_RATE_LIMIT_SLOTS 32
static rate_limit_entry_t s_service_rate_entries[SERVICE_RATE_LIMIT_SLOTS];
static rate_limiter_t s_service_rate = RATE_LIMITER_STATIC_INIT(s_service_rate_entries,
                                                                SERVICE_RATE_LIMIT_SLOTS,
                                                                SERVICE_RATE_LIMIT_MAX,
                                                                SERVICE_RATE_LIMIT_WINDOW_SEC);
#endif /* ENABLE_AUTH */

/* =============================================================================
 * HTTP Security Headers
 * ============================================================================= */

/* Pre-formatted security headers for lws_serve_http_file() */
static char s_static_security_headers[1024];
static int s_static_security_headers_len = 0;

/* CSP policy — shared between add_security_headers() and the static
 * string assembled in webui_security_headers_init().  Per-directive
 * audit (kept here so when a consumer goes away, the corresponding
 * directive permission can be tightened without spelunking):
 *
 *   default-src 'self'           — fallback for any directive not below
 *   script-src 'wasm-unsafe-eval'  — Opus decoder WebAssembly
 *   script-src 'unsafe-inline'   — small inline bootstrap blocks in index.html
 *   style-src 'unsafe-inline'    — inline style attributes set by JS modules
 *   connect-src wss: ws:         — WebSocket to webui_server.c
 *   img-src data:                — inline thumbnails, DawnFormat fallbacks
 *   img-src blob:                — image-store generated blob previews
 *   media-src blob:              — silent-audio bridge in www/js/audio/media-session.js
 *   manifest-src 'self'          — manifest.json
 *   worker-src blob:             — AudioWorklet + Opus decoder workers
 *
 * If a directive's listed consumer is removed, drop the permission. */
static const char s_csp_policy[] = "default-src 'self'; "
                                   "script-src 'self' 'wasm-unsafe-eval' 'unsafe-inline'; "
                                   "style-src 'self' 'unsafe-inline'; connect-src 'self' wss: ws:; "
                                   "img-src 'self' data: blob:; "
                                   "media-src 'self' blob:; "
                                   "manifest-src 'self'; worker-src 'self' blob:";

int webui_add_security_headers(struct lws *wsi, unsigned char **p, unsigned char *end) {
   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"Content-Security-Policy:",
                                   (const unsigned char *)s_csp_policy, (int)strlen(s_csp_policy),
                                   p, end))
      return FAILURE;

   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"X-Frame-Options:",
                                   (const unsigned char *)"DENY", 4, p, end))
      return FAILURE;

   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"X-Content-Type-Options:",
                                   (const unsigned char *)"nosniff", 7, p, end))
      return FAILURE;

   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"Referrer-Policy:",
                                   (const unsigned char *)"strict-origin-when-cross-origin", 31, p,
                                   end))
      return FAILURE;

   static const char permissions[] = "camera=(self), geolocation=(), payment=()";
   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"Permissions-Policy:",
                                   (const unsigned char *)permissions, (int)strlen(permissions), p,
                                   end))
      return FAILURE;

   /* HSTS only when HTTPS is enabled (RFC 6797: MUST ignore on non-secure) */
   if (g_config.webui.https) {
      static const char hsts[] = "max-age=31536000; includeSubDomains";
      if (lws_add_http_header_by_name(wsi, (const unsigned char *)"Strict-Transport-Security:",
                                      (const unsigned char *)hsts, (int)strlen(hsts), p, end))
         return FAILURE;
   }

   return SUCCESS;
}

const char *webui_get_static_security_headers(int *out_len) {
   if (out_len)
      *out_len = s_static_security_headers_len;
   return s_static_security_headers;
}

void webui_security_headers_init(void) {
   int n = snprintf(s_static_security_headers, sizeof(s_static_security_headers),
                    "Content-Security-Policy: %s\x0d\x0a"
                    "X-Frame-Options: DENY\x0d\x0a"
                    "X-Content-Type-Options: nosniff\x0d\x0a"
                    "Referrer-Policy: strict-origin-when-cross-origin\x0d\x0a"
                    "Permissions-Policy: camera=(self), geolocation=(), payment=()\x0d\x0a"
                    "%s",
                    s_csp_policy,
                    g_config.webui.https
                        ? "Strict-Transport-Security: max-age=31536000; includeSubDomains\x0d\x0a"
                        : "");

   if (n >= (int)sizeof(s_static_security_headers)) {
      OLOG_ERROR("WebUI: Security headers string truncated (%d >= %zu)", n,
                 sizeof(s_static_security_headers));
      n = (int)sizeof(s_static_security_headers) - 1;
   }
   s_static_security_headers_len = n;

   OLOG_INFO("WebUI: Security headers initialized (%d bytes, HSTS=%s)",
             s_static_security_headers_len, g_config.webui.https ? "on" : "off");
}

/* =============================================================================
 * Static Asset Caching — ETag + Cache-Control
 *
 * HTML responses use Cache-Control: no-store so a fresh deploy is visible on
 * the user's next load without a hard refresh. Every other static asset gets
 * a strong ETag derived from mtime+size plus Cache-Control: no-cache — the
 * browser revalidates on every request and we return 304 Not Modified when
 * the ETag still matches. First-visit bandwidth is unchanged; every
 * subsequent page load collapses the ~1.4 MB asset bundle into a handful of
 * ~200-byte 304s.
 * ============================================================================= */

/* Strong ETag fingerprint of a static-asset file. mtime+size+inode so two
 * different files can never collide even if mtime and size happen to match. */
static void format_etag(const struct stat *st, char *out, size_t out_size) {
   snprintf(out, out_size, "\"%lx-%lx-%lx\"", (unsigned long)st->st_mtime,
            (unsigned long)st->st_size, (unsigned long)st->st_ino);
}

/* HTML is always served fresh — skip the ETag path so a new deploy takes
 * effect without a user-side refresh. Covers index.html, login.html, etc. */
static bool mime_is_html(const char *mime_type) {
   return mime_type && strncmp(mime_type, "text/html", 9) == 0;
}

/* True for assets whose identity is encoded in the filename and that change
 * rarely: fonts (woff2/woff/ttf — IBM Plex Mono, Source Sans 3) and minified
 * vendor libraries (marked.min.js, purify.min.js — updated via curl -o which
 * bumps mtime → ETag still protects a revalidating client). These get
 * public + max-age + immutable to kill the revalidation RTT on slow links
 * (LTE satellites), where it causes perceptible FOUT on fonts. */
static bool path_is_immutable_asset(const char *path) {
   if (!path)
      return false;
   size_t len = strlen(path);
   static const char *const suffixes[] = { ".woff2", ".woff", ".ttf", ".min.js" };
   for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
      size_t slen = strlen(suffixes[i]);
      if (len > slen && strcmp(path + len - slen, suffixes[i]) == 0)
         return true;
   }
   return false;
}

/* Build the per-request additional-headers string passed to
 * lws_serve_http_file(): the shared security-headers blob + the cache policy
 * for this file. Three tiers:
 *   - HTML                   → Cache-Control: no-store (no ETag).
 *   - Fonts + vendor min.js  → public, max-age=30d, immutable + ETag.
 *   - Everything else        → no-cache + ETag (revalidate every request).
 * Returns bytes written, or 0 on overflow (caller falls back to
 * security-headers-only so CSP/X-Frame-Options are never dropped). */
/* How a request path maps onto the optional /aurora subpath mount. */
typedef enum {
   AURORA_NOT_AURORA, /* not under /aurora — caller serves from www as usual */
   AURORA_REDIRECT,   /* bare /aurora — caller 302s to /aurora/ */
   AURORA_SERVE,      /* serve out_sub from the Aurora root */
   AURORA_FORBIDDEN   /* dotfile / hidden component / overflow — refuse */
} aurora_route_t;

/* Map a request path onto the Aurora mount. Only call when the feature is
 * enabled (s_aurora_path validated + set at init). The exact "/aurora" boundary
 * keeps /auroraXXX out of the subtree, and the leading-slash remainder means a
 * later realpath containment on the Aurora root is the authoritative escape
 * guard (this only pre-rejects the obvious dotfile cases). On AURORA_SERVE,
 * out_sub receives the file subpath under the Aurora root (always begins '/'). */
static aurora_route_t aurora_route(const char *path, char *out_sub, size_t out_sub_len) {
   if (strcmp(path, "/aurora") == 0)
      return AURORA_REDIRECT;
   if (strncmp(path, "/aurora/", 8) != 0)
      return AURORA_NOT_AURORA;
   /* "/aurora" is 7 chars, so path+7 keeps the leading '/'; a bare "/aurora/"
    * maps to the SPA shell. */
   const char *sub = (strcmp(path, "/aurora/") == 0) ? "/index.html" : path + 7;
   /* Reject dotfiles / hidden components (contains_path_traversal only catches
    * ".."); blocks /aurora/.git/config, /aurora/.env, and "/.." underflow. */
   if (strstr(sub, "/."))
      return AURORA_FORBIDDEN;
   if ((size_t)snprintf(out_sub, out_sub_len, "%s", sub) >= out_sub_len)
      return AURORA_FORBIDDEN;
   return AURORA_SERVE;
}

static int build_static_cache_headers(char *buf,
                                      size_t buf_size,
                                      const char *path,
                                      const char *mime_type,
                                      const char *etag) {
   int sec_len;
   const char *sec_hdrs = webui_get_static_security_headers(&sec_len);
   if (sec_len <= 0 || (size_t)sec_len >= buf_size)
      return 0;
   memcpy(buf, sec_hdrs, sec_len);

   int remain = (int)buf_size - sec_len;
   int n;
   if (mime_is_html(mime_type)) {
      n = snprintf(buf + sec_len, remain, "Cache-Control: no-store\x0d\x0a");
   } else if (path_is_immutable_asset(path)) {
      n = snprintf(buf + sec_len, remain,
                   "ETag: %s\x0d\x0a"
                   "Cache-Control: public, max-age=2592000, immutable\x0d\x0a",
                   etag);
   } else {
      n = snprintf(buf + sec_len, remain,
                   "ETag: %s\x0d\x0a"
                   "Cache-Control: no-cache\x0d\x0a",
                   etag);
   }
   if (n < 0 || n >= remain)
      return 0;
   return sec_len + n;
}

/* Write a 304 Not Modified response (headers only). The header set here is
 * intentionally the same shape as the 200 path's per-request block — ETag,
 * Cache-Control, and the full security-header set — so the browser's cached
 * representation gets the same CSP/X-Frame-Options/etc. it saw originally.
 * Keeping these in sync is load-bearing: diverging between 200 and 304 would
 * make intermediate caches treat the representation as stale. */
static int send_304_not_modified(struct lws *wsi, const char *etag) {
   unsigned char buffer[LWS_PRE + 1024];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, HTTP_STATUS_NOT_MODIFIED, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"ETag:", (const unsigned char *)etag,
                                   (int)strlen(etag), &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_name(wsi, (const unsigned char *)"Cache-Control:",
                                   (const unsigned char *)"no-cache", 8, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (webui_add_security_headers(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_finalize_http_header(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;

   return lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
}

/* =============================================================================
 * Auth Helper Functions
 * ============================================================================= */

#ifdef ENABLE_AUTH

/**
 * @brief Extract session token from Cookie header
 * @param wsi WebSocket/HTTP connection
 * @param token_out Buffer to store token (must be AUTH_TOKEN_LEN bytes)
 * @return true if token found, false otherwise
 */
static bool extract_session_cookie(struct lws *wsi, char *token_out) {
   char cookie_buf[512];
   int len = lws_hdr_copy(wsi, cookie_buf, sizeof(cookie_buf), WSI_TOKEN_HTTP_COOKIE);
   if (len <= 0) {
      return false;
   }

   /* Parse cookie header for dawn_session=<token> */
   const char *prefix = AUTH_COOKIE_NAME "=";
   char *start = strstr(cookie_buf, prefix);
   if (!start) {
      return false;
   }

   start += strlen(prefix);
   char *end = strchr(start, ';');
   size_t token_len = end ? (size_t)(end - start) : strlen(start);

   if (token_len >= AUTH_TOKEN_LEN || token_len == 0) {
      return false;
   }

   memcpy(token_out, start, token_len);
   token_out[token_len] = '\0';
   return true;
}

/**
 * @brief Check if request is authenticated via session cookie
 * @param wsi WebSocket/HTTP connection
 * @param session_out If not NULL, filled with session info on success
 * @return true if authenticated, false otherwise
 */
bool is_request_authenticated(struct lws *wsi, auth_session_t *session_out) {
   char token[AUTH_TOKEN_LEN];
   if (!extract_session_cookie(wsi, token)) {
      return false;
   }

   auth_session_t session;
   if (auth_db_get_session(token, &session) != AUTH_DB_SUCCESS) {
      return false;
   }

   /* Update session activity */
   auth_db_update_session_activity(token);

   if (session_out) {
      *session_out = session;
   }
   return true;
}

/**
 * @brief Check if request has a valid service Bearer token.
 *
 * Used for machine-to-machine access (MIRAGE fetching images).
 * Validates against g_secrets.service_token with constant-time comparison.
 *
 * @param wsi HTTP connection
 * @return true if valid Bearer token present, false otherwise
 */
static size_t s_service_token_len = 0; /* cached at first use */

static bool is_service_token_authenticated(struct lws *wsi) {
   if (g_secrets.service_token[0] == '\0') {
      return false; /* no service token configured */
   }

   /* Cache token length on first call (token is immutable after config load) */
   if (s_service_token_len == 0) {
      s_service_token_len = strlen(g_secrets.service_token);
   }

   char auth_buf[512];
   int len = lws_hdr_copy(wsi, auth_buf, sizeof(auth_buf), WSI_TOKEN_HTTP_AUTHORIZATION);
   if (len <= 7) {
      return false;
   }

   /* Check "Bearer " prefix (case-sensitive per RFC 6750) */
   if (strncmp(auth_buf, "Bearer ", 7) != 0) {
      return false;
   }

   /* Constant-time comparison over fixed size to prevent length leaking.
    * Pad both values into CONFIG_API_KEY_MAX buffers so timing is independent
    * of actual token length. */
   const char *token = auth_buf + 7;
   size_t token_len = strlen(token);

   char padded_token[CONFIG_API_KEY_MAX] = { 0 };
   char padded_expected[CONFIG_API_KEY_MAX] = { 0 };
   size_t copy_len = token_len < sizeof(padded_token) ? token_len : sizeof(padded_token) - 1;
   memcpy(padded_token, token, copy_len);
   memcpy(padded_expected, g_secrets.service_token, s_service_token_len);

   return sodium_memcmp(padded_token, padded_expected, sizeof(padded_token)) == 0;
}

/**
 * @brief Send JSON response with optional Set-Cookie header
 *
 * @param wsi HTTP connection
 * @param status HTTP status code
 * @param json_body JSON string to send
 * @param cookie Cookie value to set (NULL for no cookie, empty string to clear)
 * @param cookie_max_age Max-Age for cookie (0 = session cookie, >0 = persistent)
 * @return 0 on success, LWS_CLOSE_CONNECTION on failure
 */
static int send_auth_response(struct lws *wsi,
                              int status,
                              const char *json_body,
                              const char *cookie,
                              int cookie_max_age) {
   size_t body_len = strlen(json_body);
   unsigned char buffer[LWS_PRE + 4096];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_content_length(wsi, body_len, &p, end))
      return LWS_CLOSE_CONNECTION;

   /* Add Set-Cookie header if provided */
   if (cookie) {
      char cookie_header[256];
      if (cookie[0] == '\0') {
         /* Clear cookie */
         snprintf(cookie_header, sizeof(cookie_header),
                  "%s=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0", AUTH_COOKIE_NAME);
      } else if (cookie_max_age > 0) {
         /* Set persistent cookie with explicit expiry ("Remember me" enabled) */
         snprintf(cookie_header, sizeof(cookie_header),
                  "%s=%s; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=%d", AUTH_COOKIE_NAME,
                  cookie, cookie_max_age);
      } else {
         /* Set session cookie (expires when browser closes) */
         snprintf(cookie_header, sizeof(cookie_header),
                  "%s=%s; Path=/; HttpOnly; Secure; SameSite=Strict", AUTH_COOKIE_NAME, cookie);
      }
      if (lws_add_http_header_by_name(wsi, (unsigned char *)"Set-Cookie:",
                                      (unsigned char *)cookie_header, (int)strlen(cookie_header),
                                      &p, end))
         return LWS_CLOSE_CONNECTION;
   }

   if (webui_add_security_headers(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_finalize_http_header(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;

   /* Write headers first */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   /* Write body - use LWS_WRITE_HTTP_FINAL to indicate completion */
   n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP_FINAL);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   return 0;
}

/**
 * @brief Send JSON response with no-cache headers
 *
 * Used for sensitive endpoints like CSRF token generation where caching
 * would be a security risk.
 *
 * @param wsi HTTP connection
 * @param status HTTP status code
 * @param json_body JSON string to send
 * @return 0 on success, LWS_CLOSE_CONNECTION on failure
 */
static int send_nocache_json_response(struct lws *wsi, int status, const char *json_body) {
   size_t body_len = strlen(json_body);
   unsigned char buffer[LWS_PRE + 4096];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_content_length(wsi, body_len, &p, end))
      return LWS_CLOSE_CONNECTION;

   /* Add no-cache headers to prevent token caching */
   if (lws_add_http_header_by_name(wsi, (unsigned char *)"Cache-Control:",
                                   (unsigned char *)"no-store, no-cache, must-revalidate, private",
                                   44, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_add_http_header_by_name(wsi, (unsigned char *)"Pragma:", (unsigned char *)"no-cache", 8,
                                   &p, end))
      return LWS_CLOSE_CONNECTION;

   if (webui_add_security_headers(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;
   if (lws_finalize_http_header(wsi, &p, end))
      return LWS_CLOSE_CONNECTION;

   /* Write headers first */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   /* Write body - use LWS_WRITE_HTTP_FINAL to indicate completion */
   n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP_FINAL);
   if (n < 0)
      return LWS_CLOSE_CONNECTION;

   return 0;
}

/**
 * @brief Record a CSRF nonce as used (single-use enforcement)
 *
 * @param nonce 16-byte nonce from CSRF token
 */
static void csrf_record_used_nonce(const unsigned char *nonce) {
   pthread_mutex_lock(&s_csrf_used.mutex);
   memcpy(s_csrf_used.nonces[s_csrf_used.head], nonce, CSRF_USED_NONCE_SIZE);
   s_csrf_used.head = (s_csrf_used.head + 1) &
                      (CSRF_USED_NONCE_COUNT - 1); /* Bitwise AND for power-of-2 */
   pthread_mutex_unlock(&s_csrf_used.mutex);
}

/**
 * @brief Check if a CSRF nonce has already been used
 *
 * @param nonce 16-byte nonce from CSRF token
 * @return true if already used (replay attack), false if fresh
 */
static bool csrf_is_nonce_used(const unsigned char *nonce) {
   pthread_mutex_lock(&s_csrf_used.mutex);
   for (int i = 0; i < CSRF_USED_NONCE_COUNT; i++) {
      if (sodium_memcmp(s_csrf_used.nonces[i], nonce, CSRF_USED_NONCE_SIZE) == 0) {
         pthread_mutex_unlock(&s_csrf_used.mutex);
         return true;
      }
   }
   pthread_mutex_unlock(&s_csrf_used.mutex);
   return false;
}

/* =============================================================================
 * Auth Endpoint Handlers
 * ============================================================================= */

/**
 * @brief Handle POST /api/auth/login
 * @param wsi HTTP connection
 * @param pss Session data containing POST body
 * @return LWS_CLOSE_CONNECTION to close connection after response
 */
static int handle_auth_login(struct lws *wsi, struct http_session_data *pss) {
   char response[256];

   /* Get client IP early for rate limiting and logging */
   char client_ip[64] = "unknown";
   lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));

   /* Normalize IP for rate limiting (IPv6 /64 prefix) */
   char normalized_ip[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip(client_ip, normalized_ip, sizeof(normalized_ip));

   /* Check IP-based rate limiting - in-memory fast-path first, then database */
   if (rate_limiter_check(&s_login_rate, normalized_ip)) {
      OLOG_WARNING("WebUI: Rate limited IP (in-memory): %s (normalized: %s)", client_ip,
                   normalized_ip);
      auth_db_log_event("RATE_LIMITED", NULL, client_ip, "Too many failed attempts");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Too many attempts. Try again later.\"}");
      send_auth_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Also check database for persistence across restarts */
   time_t window_start = time(NULL) - RATE_LIMIT_WINDOW_SEC;
   int recent_failures = 0;
   auth_db_count_recent_failures(normalized_ip, window_start, &recent_failures);
   if (recent_failures >= RATE_LIMIT_MAX_ATTEMPTS) {
      OLOG_WARNING("WebUI: Rate limited IP (database): %s (normalized: %s)", client_ip,
                   normalized_ip);
      auth_db_log_event("RATE_LIMITED", NULL, client_ip, "Too many failed attempts");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Too many attempts. Try again later.\"}");
      send_auth_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Parse JSON body */
   struct json_object *req = json_tokener_parse(pss->post_body);
   if (!req) {
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid JSON\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Extract and validate CSRF token */
   struct json_object *csrf_obj;
   if (!json_object_object_get_ex(req, "csrf_token", &csrf_obj)) {
      json_object_put(req);
      OLOG_WARNING("WebUI: Login attempt without CSRF token from %s", client_ip);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Missing CSRF token\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   const char *csrf_token = json_object_get_string(csrf_obj);
   unsigned char csrf_nonce[AUTH_CSRF_NONCE_SIZE];
   if (!auth_verify_csrf_token_extract_nonce(csrf_token, csrf_nonce)) {
      json_object_put(req);
      OLOG_WARNING("WebUI: Invalid CSRF token from %s", client_ip);
      auth_db_log_event("CSRF_FAILED", NULL, client_ip, "Invalid or expired CSRF token");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Invalid or expired token. Please refresh.\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Check for CSRF token replay (single-use enforcement) */
   if (csrf_is_nonce_used(csrf_nonce)) {
      json_object_put(req);
      OLOG_WARNING("WebUI: CSRF token replay attempt from %s", client_ip);
      auth_db_log_event("CSRF_REPLAY", NULL, client_ip, "CSRF token reuse detected");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Token already used. Please refresh.\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Mark CSRF token as used (do this early, even before checking credentials) */
   csrf_record_used_nonce(csrf_nonce);

   struct json_object *username_obj, *password_obj;
   if (!json_object_object_get_ex(req, "username", &username_obj) ||
       !json_object_object_get_ex(req, "password", &password_obj)) {
      json_object_put(req);
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Missing username or password\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   const char *username = json_object_get_string(username_obj);
   const char *password = json_object_get_string(password_obj);

   /* Check for "Remember me" option */
   struct json_object *remember_obj;
   bool remember_me = false;
   if (json_object_object_get_ex(req, "remember_me", &remember_obj)) {
      remember_me = json_object_get_boolean(remember_obj);
   }

   /* Get user from database */
   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      /* Timing equalization: perform dummy password hash verification
       * to prevent timing attacks that could enumerate valid usernames */
      (void)auth_verify_password(DUMMY_PASSWORD_HASH, password);
      json_object_put(req);
      OLOG_WARNING("WebUI: Login failed - user not found: %s from %s", username, client_ip);
      auth_db_log_attempt(normalized_ip, username, false);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid credentials\"}");
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Check if account is locked */
   time_t now = time(NULL);
   if (user.lockout_until > now) {
      json_object_put(req);
      OLOG_WARNING("WebUI: Login failed - account locked: %s from %s", username, client_ip);
      auth_db_log_attempt(normalized_ip, username, false);
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Account temporarily locked\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   } else if (user.lockout_until > 0 && user.lockout_until <= now) {
      /* Lockout expired - reset failed attempts counter */
      auth_db_reset_failed_attempts(username);
      auth_db_set_lockout(username, 0);
      OLOG_INFO("WebUI: Lockout expired, reset failed attempts: %s", username);
   }

   /* Verify password - auth_verify_password returns bool (true=success) */
   if (!auth_verify_password(user.password_hash, password)) {
      json_object_put(req);
      auth_db_increment_failed_attempts(username);
      auth_db_log_attempt(normalized_ip, username, false);

      /* Check if account should be locked after this failed attempt */
      auth_user_t updated_user;
      if (auth_db_get_user(username, &updated_user) == AUTH_DB_SUCCESS) {
         if (updated_user.failed_attempts >= AUTH_MAX_LOGIN_ATTEMPTS) {
            time_t lockout_until = time(NULL) + AUTH_LOCKOUT_DURATION_SEC;
            auth_db_set_lockout(username, lockout_until);
            auth_db_log_event("ACCOUNT_LOCKED", username, client_ip,
                              "Too many failed login attempts");
            OLOG_WARNING("WebUI: Account locked due to %d failed attempts: %s",
                         updated_user.failed_attempts, username);
         }
      }

      OLOG_WARNING("WebUI: Login failed - wrong password: %s from %s", username, client_ip);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid credentials\"}");
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   json_object_put(req);

   /* Generate session token */
   char session_token[AUTH_TOKEN_LEN];
   if (auth_generate_token(session_token) != AUTH_CRYPTO_SUCCESS) {
      OLOG_ERROR("WebUI: Failed to generate session token");
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Server error\"}");
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, response, NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Get User-Agent header for session tracking */
   char user_agent[AUTH_USER_AGENT_MAX] = "Unknown";
   int ua_len = lws_hdr_copy(wsi, user_agent, sizeof(user_agent), WSI_TOKEN_HTTP_USER_AGENT);
   if (ua_len <= 0) {
      strncpy(user_agent, "Unknown", sizeof(user_agent));
   }

   /* Create session in database */
   if (auth_db_create_session(user.id, session_token, client_ip, user_agent, remember_me) !=
       AUTH_DB_SUCCESS) {
      OLOG_ERROR("WebUI: Failed to create session for user: %s", username);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Server error\"}");
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, response, NULL, 0);
      auth_secure_zero(session_token, sizeof(session_token));
      return LWS_CLOSE_CONNECTION;
   }

   /* Reset failed attempts and update last login */
   auth_db_reset_failed_attempts(username);
   rate_limiter_reset(&s_login_rate, normalized_ip); /* Clear in-memory rate limit on success */
   auth_db_update_last_login(username);
   auth_db_log_attempt(normalized_ip, username, true);
   auth_db_log_event("LOGIN_SUCCESS", username, client_ip, "WebUI login successful");

   OLOG_INFO("WebUI: User logged in: %s from %s%s", username, client_ip,
             remember_me ? " (remember me)" : "");

   /* Send success response with session cookie
    * remember_me: persistent cookie for 30 days; otherwise session cookie */
   int cookie_max_age = remember_me ? AUTH_REMEMBER_ME_TIMEOUT_SEC : 0;
   snprintf(response, sizeof(response), "{\"success\":true,\"username\":\"%s\",\"is_admin\":%s}",
            username, user.is_admin ? "true" : "false");
   send_auth_response(wsi, HTTP_STATUS_OK, response, session_token, cookie_max_age);

   /* Clear session token from stack after use */
   auth_secure_zero(session_token, sizeof(session_token));
   return LWS_CLOSE_CONNECTION;
}

/**
 * @brief Check if request Origin/Referer matches our host (CSRF protection).
 *
 * Same-origin if the Origin (or, as fallback, the Referer) host matches our own
 * Host header. A request with NEITHER header is allowed (non-browser clients —
 * curl, satellites — don't send them, and CSRF is a browser-only attack). Used
 * for state-changing REST endpoints AND the WebSocket upgrade
 * (webui_server.c, LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION).
 *
 * @param wsi connection (must be called while request headers are still present,
 *            i.e. during the HTTP request / WS handshake, not post-establish).
 * @return true if same-origin (or no Origin/Referer), false if cross-origin.
 */
/* True if `origin` exactly matches an entry in the [webui] allowed_origins
 * comma-separated allowlist (extra trusted front-ends beyond same-origin). */
static bool origin_in_allowlist(const char *origin) {
   const char *list = g_config.webui.allowed_origins;
   if (!list[0] || !origin || !origin[0]) {
      return false;
   }
   char buf[sizeof(g_config.webui.allowed_origins)];
   snprintf(buf, sizeof(buf), "%s", list);
   char *save = NULL;
   for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      while (*tok == ' ' || *tok == '\t') {
         tok++;
      }
      size_t len = strlen(tok);
      while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) {
         tok[--len] = '\0';
      }
      if (tok[0] && strcmp(origin, tok) == 0) {
         return true;
      }
   }
   return false;
}

bool webui_is_same_origin_request(struct lws *wsi) {
   char host[256] = { 0 };
   char origin[256] = { 0 };
   char referer[512] = { 0 };

   /* Detect header presence with total_length (independent of our buffer): a
    * present-but-too-long header makes lws_hdr_copy return -1, which must be a
    * REJECT (can't validate), never a silent fall-through/allow. */
   int host_total = lws_hdr_total_length(wsi, WSI_TOKEN_HOST);
   int origin_total = lws_hdr_total_length(wsi, WSI_TOKEN_ORIGIN);
   int referer_total = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_REFERER);

   /* Check Origin (preferred). */
   if (origin_total > 0) {
      if (lws_hdr_copy(wsi, origin, sizeof(origin), WSI_TOKEN_ORIGIN) <= 0) {
         OLOG_WARNING("CSRF: Origin present but unreadable (too long); rejecting");
         return false;
      }
      /* Enforce ONLY browser-shaped origins (scheme://host). Browsers send
       * either scheme://host (RFC 6455) or the literal "null" (opaque origin:
       * sandboxed iframe / cross-origin redirect) — the latter is attacker-
       * inducible, so REJECT "null"/empty. A bare host with no scheme is a
       * native (non-browser) client — e.g. a DAP satellite sets a bare host as
       * its origin — and is allowed, since a browser can never emit that. */
      if (strncmp(origin, "http://", 7) != 0 && strncmp(origin, "https://", 8) != 0) {
         if (origin[0] == '\0' || strcmp(origin, "null") == 0) {
            OLOG_WARNING("CSRF: opaque/null Origin; rejecting");
            return false;
         }
         return true; /* bare-host non-browser client (satellite/CLI) */
      }
      /* Browser origin: must match our Host. No readable Host ⇒ can't validate
       * ⇒ reject. */
      if (host_total <= 0 || lws_hdr_copy(wsi, host, sizeof(host), WSI_TOKEN_HOST) <= 0) {
         OLOG_WARNING("CSRF: browser Origin '%s' but no readable Host; rejecting", origin);
         return false;
      }
      char expected_https[280], expected_http[280];
      snprintf(expected_https, sizeof(expected_https), "https://%s", host);
      snprintf(expected_http, sizeof(expected_http), "http://%s", host);
      if (strcmp(origin, expected_https) == 0 || strcmp(origin, expected_http) == 0) {
         return true;
      }
      /* Not our own origin — allow only if explicitly whitelisted (a
       * separately-hosted front-end / HUD / dev server). */
      if (origin_in_allowlist(origin)) {
         return true;
      }
      OLOG_WARNING("CSRF: Origin mismatch - expected host %s, got %s", host, origin);
      return false;
   }

   /* No Origin - check Referer as fallback (same browser-shaped treatment). */
   if (referer_total > 0) {
      if (lws_hdr_copy(wsi, referer, sizeof(referer), WSI_TOKEN_HTTP_REFERER) <= 0) {
         OLOG_WARNING("CSRF: Referer present but unreadable (too long); rejecting");
         return false;
      }
      if (strncmp(referer, "http://", 7) != 0 && strncmp(referer, "https://", 8) != 0) {
         return true; /* non-browser */
      }
      if (host_total <= 0 || lws_hdr_copy(wsi, host, sizeof(host), WSI_TOKEN_HOST) <= 0) {
         OLOG_WARNING("CSRF: browser Referer but no readable Host; rejecting");
         return false;
      }
      char expected_https[280], expected_http[280];
      snprintf(expected_https, sizeof(expected_https), "https://%s/", host);
      snprintf(expected_http, sizeof(expected_http), "http://%s/", host);
      if (strncmp(referer, expected_https, strlen(expected_https)) == 0 ||
          strncmp(referer, expected_http, strlen(expected_http)) == 0) {
         return true;
      }
      OLOG_WARNING("CSRF: Referer mismatch - expected host %s, got %s", host, referer);
      return false;
   }

   /* No Origin or Referer - non-browser client (curl, satellite). Allow. */
   return true;
}

/**
 * @brief Handle POST /api/auth/logout
 * @param wsi HTTP connection
 * @return LWS_CLOSE_CONNECTION to close connection after response
 */
static int handle_auth_logout(struct lws *wsi) {
   /* CSRF protection: verify request is same-origin */
   if (!webui_is_same_origin_request(wsi)) {
      OLOG_WARNING("WebUI: Blocked cross-origin logout attempt");
      lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
      return LWS_CLOSE_CONNECTION;
   }

   char token[AUTH_TOKEN_LEN];
   if (extract_session_cookie(wsi, token)) {
      auth_session_t session;
      if (auth_db_get_session(token, &session) == AUTH_DB_SUCCESS) {
         char client_ip[64] = "unknown";
         lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
         auth_db_log_event("logout", session.username, client_ip, "WebUI logout");
         auth_db_delete_session(token);
         /* Release session_manager slots immediately instead of waiting for the
          * 30-minute idle timeout — logout is an explicit signal the user is done. */
         webui_destroy_sessions_by_auth_token(token);
         OLOG_INFO("WebUI: User logged out: %s", session.username);
      }
   }

   /* Use simple HTTP status - no body needed, avoids lws_write issues.
    * JavaScript redirects regardless of response content. */
   lws_return_http_status(wsi, HTTP_STATUS_OK, NULL);
   return LWS_CLOSE_CONNECTION;
}

/* =============================================================================
 * Memory entity-merge REST endpoints (v43, design §14)
 *
 * Three thin authenticated endpoints that delegate to the alias surface in
 * memory_db_alias.c.  All three use send_auth_response which has a 4096-byte
 * stack body cap — Phase 1 alias counts comfortably fit; bulk callers should
 * use the WebSocket message variants which stream JSON.
 * ============================================================================= */

/* GET /api/memory/entities/:id/aliases */
static int handle_memory_entity_aliases_get(struct lws *wsi, int64_t entity_id) {
   auth_session_t session;
   if (!is_request_authenticated(wsi, &session)) {
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED,
                         "{\"success\":false,\"error\":\"unauthenticated\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }
   if (entity_id <= 0) {
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST,
                         "{\"success\":false,\"error\":\"invalid entity_id\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   memory_alias_listing_row_t rows[32];
   int count = 0;
   if (memory_db_entity_alias_list(session.user_id, entity_id, rows, 32, &count) !=
       MEMORY_DB_SUCCESS) {
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                         "{\"success\":false,\"error\":\"query failed\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Build via json-c so canonical_name fields containing " or \ are
    * escaped correctly (JSON-injection guard).  Source-of-truth shape
    * matches the WebSocket variant in webui_memory.c. */
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "success", json_object_new_boolean(1));
   json_object_object_add(resp, "entity_id", json_object_new_int64(entity_id));
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < count; i++) {
      struct json_object *row = json_object_new_object();
      json_object_object_add(row, "link_id", json_object_new_int64(rows[i].link_id));
      json_object_object_add(row, "source_entity_id",
                             json_object_new_int64(rows[i].source_entity_id));
      json_object_object_add(row, "source_canonical_name",
                             json_object_new_string(rows[i].source_canonical_name));
      json_object_object_add(row, "link_kind", json_object_new_string(rows[i].link_kind));
      json_object_object_add(row, "composite_score",
                             json_object_new_double((double)rows[i].composite_score));
      json_object_object_add(row, "linked_at", json_object_new_int64(rows[i].linked_at));
      json_object_array_add(arr, row);
   }
   json_object_object_add(resp, "aliases", arr);
   const char *body = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
   send_auth_response(wsi, HTTP_STATUS_OK, body, NULL, 0);
   json_object_put(resp);
   return LWS_CLOSE_CONNECTION;
}

/* GET /api/memory/entity-merge-proposals — pending merge proposals for the
 * authenticated user, sorted by proposed_at DESC.  link-user-self queues
 * 0.70-0.90 candidates as proposals; the WebUI renders them for review. */
static int handle_memory_merge_proposals_get(struct lws *wsi) {
   auth_session_t session;
   if (!is_request_authenticated(wsi, &session)) {
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED,
                         "{\"success\":false,\"error\":\"unauthenticated\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Cap the REST surface at 16 entries to fit comfortably in the 4096-byte
    * send_auth_response stack body; the WebSocket variant lifts to 64 for
    * bulk callers. */
   memory_alias_proposal_row_t rows[16];
   int count = 0;
   if (memory_db_proposal_list_pending(session.user_id, rows, 16, &count) != MEMORY_DB_SUCCESS) {
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                         "{\"success\":false,\"error\":\"query failed\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }

   /* Build via json-c — see handle_memory_entity_aliases_get for rationale. */
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "success", json_object_new_boolean(1));
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < count; i++) {
      struct json_object *row = json_object_new_object();
      json_object_object_add(row, "proposal_id", json_object_new_int64(rows[i].proposal_id));
      json_object_object_add(row, "source_entity_id",
                             json_object_new_int64(rows[i].source_entity_id));
      json_object_object_add(row, "target_entity_id",
                             json_object_new_int64(rows[i].target_entity_id));
      json_object_object_add(row, "source_canonical_name",
                             json_object_new_string(rows[i].source_canonical_name));
      json_object_object_add(row, "target_canonical_name",
                             json_object_new_string(rows[i].target_canonical_name));
      json_object_object_add(row, "composite_score",
                             json_object_new_double((double)rows[i].composite_score));
      json_object_object_add(row, "proposed_at", json_object_new_int64(rows[i].proposed_at));
      json_object_array_add(arr, row);
   }
   json_object_object_add(resp, "proposals", arr);
   const char *body = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
   send_auth_response(wsi, HTTP_STATUS_OK, body, NULL, 0);
   json_object_put(resp);
   return LWS_CLOSE_CONNECTION;
}

/* POST /api/memory/entities/:source/link-to/:target — body is currently
 * ignored; reason defaults to "operator".  WebSocket variant supports a
 * full reason string. */
static int handle_memory_entity_link_post(struct lws *wsi, int64_t source_id, int64_t target_id) {
   /* CSRF protection: verify request is same-origin (matches the
    * /api/auth/logout pattern at handle_auth_logout above).  Without this
    * a cross-site POST to /api/memory/entities/X/link-to/Y could ride a
    * session cookie and silently soft-link entities for the victim. */
   if (!webui_is_same_origin_request(wsi)) {
      OLOG_WARNING("WebUI: Blocked cross-origin entity link-to attempt");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, "{\"success\":false,\"error\":\"forbidden\"}",
                         NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }
   auth_session_t session;
   if (!is_request_authenticated(wsi, &session)) {
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED,
                         "{\"success\":false,\"error\":\"unauthenticated\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }
   if (source_id <= 0 || target_id <= 0 || source_id == target_id) {
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST,
                         "{\"success\":false,\"error\":\"invalid source/target ids\"}", NULL, 0);
      return LWS_CLOSE_CONNECTION;
   }
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(session.user_id, source_id, target_id, "soft", "operator",
                                        -1.0f, NULL, &link_id);
   char body[256];
   if (rc == MEMORY_DB_SUCCESS) {
      snprintf(body, sizeof(body),
               "{\"success\":true,\"link_id\":%lld,\"source_entity_id\":%lld,"
               "\"target_entity_id\":%lld}",
               (long long)link_id, (long long)source_id, (long long)target_id);
      send_auth_response(wsi, HTTP_STATUS_OK, body, NULL, 0);
   } else {
      snprintf(body, sizeof(body),
               "{\"success\":false,\"error\":\"link failed (entity not found, self-link, "
               "or source has dependents)\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, body, NULL, 0);
   }
   return LWS_CLOSE_CONNECTION;
}

/* Path parser: tries `/api/memory/entities/<num>/aliases` and returns the
 * entity_id on hit, or -1 on miss.  Pure-string parsing; no allocation. */
static int64_t parse_alias_path(const char *path) {
   const char *prefix = "/api/memory/entities/";
   size_t plen = strlen(prefix);
   if (strncmp(path, prefix, plen) != 0)
      return -1;
   const char *id_start = path + plen;
   char *endp = NULL;
   long long id = strtoll(id_start, &endp, 10);
   if (!endp || id <= 0)
      return -1;
   if (strcmp(endp, "/aliases") != 0)
      return -1;
   return (int64_t)id;
}

/* Parser for `/api/memory/entities/<src>/link-to/<tgt>`. */
static bool parse_link_path(const char *path, int64_t *out_src, int64_t *out_tgt) {
   const char *prefix = "/api/memory/entities/";
   size_t plen = strlen(prefix);
   if (strncmp(path, prefix, plen) != 0)
      return false;
   const char *src_start = path + plen;
   char *endp = NULL;
   long long src = strtoll(src_start, &endp, 10);
   if (!endp || src <= 0 || strncmp(endp, "/link-to/", 9) != 0)
      return false;
   const char *tgt_start = endp + 9;
   char *endp2 = NULL;
   long long tgt = strtoll(tgt_start, &endp2, 10);
   if (!endp2 || tgt <= 0 || *endp2 != '\0')
      return false;
   *out_src = (int64_t)src;
   *out_tgt = (int64_t)tgt;
   return true;
}

/**
 * @brief Handle GET /api/auth/status
 * @param wsi HTTP connection
 * @return LWS_CLOSE_CONNECTION to close connection after response
 */
static int handle_auth_status(struct lws *wsi) {
   auth_session_t session;
   char response[256];

   if (is_request_authenticated(wsi, &session)) {
      snprintf(response, sizeof(response),
               "{\"authenticated\":true,\"username\":\"%s\",\"is_admin\":%s}", session.username,
               session.is_admin ? "true" : "false");
   } else {
      snprintf(response, sizeof(response), "{\"authenticated\":false}");
   }

   send_auth_response(wsi, HTTP_STATUS_OK, response, NULL, 0);
   return LWS_CLOSE_CONNECTION;
}

/**
 * @brief Handle GET /api/auth/csrf
 * @param wsi HTTP connection
 * @return LWS_CLOSE_CONNECTION to close connection after response
 *
 * Returns a CSRF token for use in login and other state-changing requests.
 * Token is HMAC-signed and valid for AUTH_CSRF_TIMEOUT_SEC seconds.
 */
static int handle_auth_csrf(struct lws *wsi) {
   /* Get client IP and normalize for rate limiting (IPv6 /64 prefix) */
   char client_ip[64] = "unknown";
   char normalized_ip[RATE_LIMIT_IP_SIZE];
   lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
   rate_limiter_normalize_ip(client_ip, normalized_ip, sizeof(normalized_ip));

   /* Check CSRF endpoint rate limiting (prevent DoS via token generation) */
   if (rate_limiter_check(&s_csrf_rate, normalized_ip)) {
      OLOG_WARNING("WebUI: CSRF rate limited: %s", normalized_ip);
      send_nocache_json_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS,
                                 "{\"error\":\"Too many requests\"}");
      return LWS_CLOSE_CONNECTION;
   }

   char csrf_token[AUTH_CSRF_TOKEN_LEN];

   if (auth_generate_csrf_token(csrf_token) != AUTH_CRYPTO_SUCCESS) {
      OLOG_ERROR("WebUI: Failed to generate CSRF token");
      send_nocache_json_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "{\"error\":\"Failed to generate token\"}");
      return LWS_CLOSE_CONNECTION;
   }

   char response[256];
   snprintf(response, sizeof(response), "{\"csrf_token\":\"%s\"}", csrf_token);

   /* Clear token from stack */
   auth_secure_zero(csrf_token, sizeof(csrf_token));

   /* Use no-cache response to prevent browser/proxy caching */
   send_nocache_json_response(wsi, HTTP_STATUS_OK, response);
   return LWS_CLOSE_CONNECTION;
}

#endif /* ENABLE_AUTH */

/* =============================================================================
 * HTTP Protocol Callback
 * ============================================================================= */

int callback_http(struct lws *wsi,
                  enum lws_callback_reasons reason,
                  void *user,
                  void *in,
                  size_t len) {
   struct http_session_data *pss = (struct http_session_data *)user;

   switch (reason) {
      case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
      case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
         /* Allow all connections */
         return 0;

      case LWS_CALLBACK_HTTP: {
         /* Serve static files - allocate buffers only when needed */
         char path[512];
         char filepath[768];
         const char *mime_type;
         int n;

         if (len < 1) {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
            return LWS_CLOSE_CONNECTION;
         }

         /* Get requested path */
         strncpy(path, (const char *)in, sizeof(path) - 1);
         path[sizeof(path) - 1] = '\0';

         /* Initialize session data */
         if (pss) {
            strncpy(pss->path, path, sizeof(pss->path) - 1);
            pss->path[sizeof(pss->path) - 1] = '\0';
            pss->post_body_len = 0;
            pss->post_body[0] = '\0';
            pss->is_post = (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) > 0);
         }

         /* OAuth callback — serve a minimal HTML page that posts code back to opener */
         if (strcmp(path, "/oauth/callback") == 0) {
            static const char OAUTH_CALLBACK_HTML[] =
                "<!DOCTYPE html>"
                "<html><head><meta charset=\"utf-8\"><title>Authorization</title>"
                "<style>body{background:#121417;color:#e6e6e6;font-family:sans-serif;"
                "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
                "</style></head><body>"
                "<p id=\"msg\">Processing authorization...</p>"
                "<script src=\"/js/oauth-callback.js\"></script>"
                "</body></html>";
            unsigned char buffer[LWS_PRE + 4096];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];
            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return 1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"text/html", 9, &p, end))
               return 1;
            if (lws_add_http_header_content_length(wsi, strlen(OAUTH_CALLBACK_HTML), &p, end))
               return 1;
            if (webui_add_security_headers(wsi, &p, end))
               return 1;
            if (lws_finalize_http_header(wsi, &p, end))
               return 1;
            int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return 1;
            lws_write(wsi, (unsigned char *)OAUTH_CALLBACK_HTML, strlen(OAUTH_CALLBACK_HTML),
                      LWS_WRITE_HTTP_FINAL);
            if (lws_http_transaction_completed(wsi))
               return LWS_CLOSE_CONNECTION;
            return 0;
         }

#ifdef ENABLE_AUTH
         /* Auth API endpoints - no auth required for these */
         if (strcmp(path, "/api/auth/status") == 0) {
            return handle_auth_status(wsi);
         }

         if (strcmp(path, "/api/auth/csrf") == 0) {
            return handle_auth_csrf(wsi);
         }

         if (strcmp(path, "/api/auth/logout") == 0) {
            return handle_auth_logout(wsi);
         }

         /* POST /api/auth/login - defer to body completion */
         if (strcmp(path, "/api/auth/login") == 0 && pss && pss->is_post) {
            /* Return 0 to allow body callbacks */
            return 0;
         }

         /* Memory entity-merge REST (v43) */
         if (pss && !pss->is_post) {
            int64_t alias_entity_id = parse_alias_path(path);
            if (alias_entity_id > 0) {
               return handle_memory_entity_aliases_get(wsi, alias_entity_id);
            }
            if (strcmp(path, "/api/memory/entity-merge-proposals") == 0) {
               return handle_memory_merge_proposals_get(wsi);
            }
         }
         if (pss && pss->is_post) {
            int64_t src = 0, tgt = 0;
            if (parse_link_path(path, &src, &tgt)) {
               return handle_memory_entity_link_post(wsi, src, tgt);
            }
         }

         /* Image API endpoints (require auth) */
         /* GET /api/ota/<platform>/<version>/image?uuid=..&token=.. — OTA image
          * download.  The one-time, uuid+version-bound token (consumed inside
          * ota_authorize_image_download) is the auth gate; TLS is required and
          * the per-IP limiter is only a coarse DoS guard. */
         if (strncmp(path, "/api/ota/", 9) == 0 && pss && !pss->is_post) {
            if (g_config.ota.require_tls && !lws_is_ssl(wsi)) {
               lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
               return LWS_CLOSE_CONNECTION;
            }
            char peer_ip[64] = "unknown";
            lws_get_peer_simple(wsi, peer_ip, sizeof(peer_ip));
            char norm_ip[RATE_LIMIT_IP_SIZE];
            rate_limiter_normalize_ip(peer_ip, norm_ip, sizeof(norm_ip));
            if (rate_limiter_check(&s_service_rate, norm_ip)) {
               lws_return_http_status(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, NULL);
               return LWS_CLOSE_CONNECTION;
            }

            char platform[16] = "";
            char version[OTA_VERSION_MAX] = "";
            /* widths: platform 15, version OTA_VERSION_MAX-1 (=31) */
            if (sscanf(path + 9, "%15[^/]/%31[^/]/image", platform, version) == 2) {
               char ubuf[80] = "";
               char tbuf[OTA_TOKEN_HEX + 8] = "";
               const char *uuid = lws_get_urlarg_by_name(wsi, "uuid=", ubuf, sizeof(ubuf));
               const char *token = lws_get_urlarg_by_name(wsi, "token=", tbuf, sizeof(tbuf));
               char img[1024];
               if (uuid && token &&
                   ota_authorize_image_download(uuid, platform, version, token, img, sizeof(img)) ==
                       SUCCESS) {
                  char hdrs[64];
                  int hl = snprintf(hdrs, sizeof(hdrs), "Cache-Control: no-store\r\n");
                  int n = lws_serve_http_file(wsi, img, "application/octet-stream", hdrs, hl);
                  if (n < 0) {
                     lws_return_http_status(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL);
                     return LWS_CLOSE_CONNECTION;
                  }
                  return 0; /* served (sync or async via file-completion callback) */
               }
               lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
               return LWS_CLOSE_CONNECTION;
            }
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            return LWS_CLOSE_CONNECTION;
         }

         /* GET /api/images/:id - download image */
         if (strncmp(path, "/api/images/", 12) == 0 && pss && !pss->is_post) {
            const char *image_id = path + 12;

            /* Try session auth first (normal browser path) */
            auth_session_t session;
            if (is_request_authenticated(wsi, &session)) {
               return webui_images_handle_download(wsi, image_id, session.user_id);
            }

            /* Try Bearer token (machine-to-machine: MIRAGE, etc.)
             * Rate limit ALL Bearer attempts (valid or not) to throttle brute-force. */
            {
               char auth_probe[512];
               int hlen = lws_hdr_copy(wsi, auth_probe, sizeof(auth_probe),
                                       WSI_TOKEN_HTTP_AUTHORIZATION);
               if (hlen > 7 && strncmp(auth_probe, "Bearer ", 7) == 0) {
                  char peer_ip[64] = "unknown";
                  lws_get_peer_simple(wsi, peer_ip, sizeof(peer_ip));
                  char norm_ip[RATE_LIMIT_IP_SIZE];
                  rate_limiter_normalize_ip(peer_ip, norm_ip, sizeof(norm_ip));
                  if (rate_limiter_check(&s_service_rate, norm_ip)) {
                     OLOG_WARNING("webui_http: Bearer rate limit exceeded from %s", peer_ip);
                     lws_return_http_status(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, NULL);
                     return LWS_CLOSE_CONNECTION;
                  }
                  if (is_service_token_authenticated(wsi)) {
                     if (!lws_is_ssl(wsi)) {
                        OLOG_WARNING("webui_http: Bearer token used without TLS from %s", peer_ip);
                     }
                     /* user_id=0: service token can access non-private images (generated,
                      * search, document) but not private uploads or MMS */
                     return webui_images_handle_download(wsi, image_id, 0);
                  }
               }
            }

            /* Fall through to auth redirect */
         }

         /* GET /api/documents/original/:id - download a stored original file.
          * Session-cookie only (no Bearer/service-token branch): originals are
          * private, and the store's owner-only access also denies user_id 0. */
         if (strncmp(path, "/api/documents/original/", 24) == 0 && pss && !pss->is_post) {
            const char *blob_id = path + 24;
            auth_session_t session;
            if (is_request_authenticated(wsi, &session)) {
               return webui_documents_handle_original_download(wsi, blob_id, session.user_id);
            }
            /* Fall through to auth redirect */
         }

         /* POST /api/images - upload image (defer to body completion) */
         if (strcmp(path, "/api/images") == 0 && pss && pss->is_post) {
            auth_session_t session;
            if (is_request_authenticated(wsi, &session)) {
               pss->image_session = NULL;
               int result = webui_images_handle_upload_start(wsi, &pss->image_session,
                                                             session.user_id);
               if (result == 0) {
                  /* Return 0 to allow body callbacks */
                  return 0;
               }
               return result; /* Error response already sent */
            }
            /* Fall through to auth redirect */
         }

         /* POST /api/documents/summarize - TF-IDF summarize document text */
         if (strcmp(path, "/api/documents/summarize") == 0 && pss && pss->is_post) {
            if (is_request_authenticated(wsi, NULL)) {
               /* Allocate dynamic body buffer for large document content */
               const size_t max_body = (size_t)g_config.documents.max_extracted_size_kb * 1024 +
                                       1024;
               pss->large_body = malloc(32 * 1024); /* Start with 32KB */
               if (!pss->large_body) {
                  lws_return_http_status(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL);
                  return LWS_CLOSE_CONNECTION;
               }
               pss->large_body_len = 0;
               pss->large_body_cap = 32 * 1024;
               (void)max_body; /* Used during body accumulation */
               return 0;       /* Defer to body completion */
            }
         }

         /* POST /api/documents - upload document (defer to body completion) */
         if (strcmp(path, "/api/documents") == 0 && pss && pss->is_post) {
            auth_session_t session;
            if (is_request_authenticated(wsi, &session)) {
               pss->document_session = NULL;
               int result = webui_documents_handle_upload_start(wsi, &pss->document_session,
                                                                session.user_id);
               if (result == 0) {
                  return 0;
               }
               return result;
            }
         }

         /* Public paths that don't require auth */
         bool is_public_path =
             (strcmp(path, "/login.html") == 0 || strcmp(path, "/health") == 0 ||
              strncmp(path, "/css/", 5) == 0 || strncmp(path, "/fonts/", 7) == 0 ||
              strcmp(path, "/favicon.svg") == 0 || strcmp(path, "/manifest.json") == 0 ||
              /* Login page JS/CSS extracted from inline by CSP hardening */
              strcmp(path, "/js/login.js") == 0 ||
              /* AudioWorklet.addModule() doesn't send cookies, so worklet must be public */
              strcmp(path, "/js/audio/capture-worklet.js") == 0 ||
              /* OAuth callback — popup may not have session cookie */
              strcmp(path, "/oauth/callback") == 0 ||
              /* OAuth callback JS (loaded by callback page) */
              strcmp(path, "/js/oauth-callback.js") == 0);

         /* Check authentication for protected paths */
         if (!is_public_path && !is_request_authenticated(wsi, NULL)) {
            /* Redirect to login page */
            unsigned char buffer[LWS_PRE + 768];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_FOUND, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (lws_add_http_header_by_name(wsi, (unsigned char *)"Location:",
                                            (unsigned char *)"/login.html", 11, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (lws_add_http_header_content_length(wsi, 0, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (webui_add_security_headers(wsi, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (lws_finalize_http_header(wsi, &p, end))
               return LWS_CLOSE_CONNECTION;

            n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return LWS_CLOSE_CONNECTION;

            return LWS_CLOSE_CONNECTION; /* Close connection */
         }
#endif /* ENABLE_AUTH */

         /* Health check endpoint - returns JSON status */
         if (strcmp(path, "/health") == 0) {
            dawn_metrics_t snapshot;
            metrics_get_snapshot(&snapshot);

            /* Build JSON response */
            char json_body[512];
            snprintf(json_body, sizeof(json_body),
                     "{\"status\":\"ok\",\"version\":\"%s\",\"git_sha\":\"%s\","
                     "\"uptime_seconds\":%ld,\"state\":\"%s\",\"queries\":%u,"
                     "\"active_sessions\":%d}",
                     VERSION_NUMBER, GIT_SHA, (long)metrics_get_uptime(),
                     dawn_state_name(snapshot.current_state), snapshot.queries_total,
                     s_client_count);

            size_t body_len = strlen(json_body);
            unsigned char buffer[LWS_PRE + 1024];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"application/json", 16, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (lws_add_http_header_content_length(wsi, body_len, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (webui_add_security_headers(wsi, &p, end))
               return LWS_CLOSE_CONNECTION;
            if (lws_finalize_http_header(wsi, &p, end))
               return LWS_CLOSE_CONNECTION;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return LWS_CLOSE_CONNECTION;

            n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP);
            if (n < 0)
               return LWS_CLOSE_CONNECTION;

            return LWS_CLOSE_CONNECTION; /* Close connection after response */
         }

         /* Aurora SPA subpath (optional, config-gated): serve its own dist root
          * at /aurora/ with the prefix stripped, reusing the traversal +
          * realpath-containment guards anchored on the Aurora root (never
          * s_www_path). Inactive unless [webui] aurora_path validated at init. */
         const char *serve_root = s_www_path;
         const char *serve_path = path;
         char aurora_sub[512];
         if (s_aurora_path[0] != '\0') {
            aurora_route_t ar = aurora_route(path, aurora_sub, sizeof(aurora_sub));
            if (ar == AURORA_FORBIDDEN) {
               OLOG_WARNING("WebUI: Aurora path blocked: %s", path);
               lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
               return LWS_CLOSE_CONNECTION;
            }
            if (ar == AURORA_REDIRECT) {
               /* Normalize bare /aurora → /aurora/ so relative assets resolve.
                * 768 matches the login redirect below: the security-header set
                * (~429B) + status + Location overruns a 256B buffer, which would
                * silently abort the write. */
               unsigned char rbuf[LWS_PRE + 768];
               unsigned char *rstart = &rbuf[LWS_PRE];
               unsigned char *rp = rstart;
               unsigned char *rend = &rbuf[sizeof(rbuf) - 1];
               if (lws_add_http_header_status(wsi, HTTP_STATUS_FOUND, &rp, rend) ||
                   lws_add_http_header_by_name(wsi, (unsigned char *)"Location:",
                                               (unsigned char *)"/aurora/", 8, &rp, rend) ||
                   lws_add_http_header_content_length(wsi, 0, &rp, rend) ||
                   webui_add_security_headers(wsi, &rp, rend) ||
                   lws_finalize_http_header(wsi, &rp, rend))
                  return LWS_CLOSE_CONNECTION;
               lws_write(wsi, rstart, (size_t)(rp - rstart), LWS_WRITE_HTTP_HEADERS);
               return LWS_CLOSE_CONNECTION;
            }
            if (ar == AURORA_SERVE) {
               serve_root = s_aurora_path;
               serve_path = aurora_sub;
            }
         }

         /* Default to index.html for root */
         if (strcmp(path, "/") == 0) {
            strncpy(path, "/index.html", sizeof(path) - 1);
         }

         /* Prevent directory traversal - check for patterns including URL-encoded */
         if (contains_path_traversal(path)) {
            OLOG_WARNING("WebUI: Directory traversal attempt blocked: %s", path);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return LWS_CLOSE_CONNECTION;
         }

         /* Build full filesystem path (Aurora requests use their own root). */
         int fp_len = snprintf(filepath, sizeof(filepath), "%s%s", serve_root, serve_path);
         if (fp_len < 0 || (size_t)fp_len >= sizeof(filepath)) {
            OLOG_WARNING("WebUI: served path too long, refused: %s%s", serve_root, serve_path);
            lws_return_http_status(wsi, HTTP_STATUS_REQ_URI_TOO_LONG, NULL);
            return LWS_CLOSE_CONNECTION;
         }

         /* Second layer: verify the resolved path is within the served root
          * (realpath containment anchored on serve_root — s_www_path or the
          * validated Aurora dist root, never a mix). */
         if (!is_path_within_www(filepath, serve_root)) {
            OLOG_WARNING("WebUI: Path escape attempt blocked: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return LWS_CLOSE_CONNECTION;
         }

         /* Get MIME type */
         mime_type = get_mime_type(filepath);

         /* Stat the file for ETag-based revalidation. A failure here (missing
          * file, symlink, etc.) falls through to lws_serve_http_file() which
          * handles the 404 path below. */
         struct stat st;
         /* 3 x up to 16 hex digits + 2 dashes + 2 quotes + NUL = ~54; 64 is
          * headroom. */
         char etag[64] = "";
         bool have_etag = false;
         if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode) && !mime_is_html(mime_type)) {
            format_etag(&st, etag, sizeof(etag));
            have_etag = true;

            /* If-None-Match may be a comma-separated list per RFC 7232 §3.2;
             * tokenize and match each entry. "*" matches any current
             * representation (client says "304 if resource exists"). 256 bytes
             * holds ~10 tokens' worth so this stays robust against browsers
             * that accumulate cached variants. */
            char inm[256];
            int inm_len = lws_hdr_copy(wsi, inm, sizeof(inm), WSI_TOKEN_HTTP_IF_NONE_MATCH);
            if (inm_len > 0) {
               char *saveptr = NULL;
               char *tok = strtok_r(inm, ",", &saveptr);
               while (tok) {
                  while (*tok == ' ' || *tok == '\t')
                     tok++;
                  char *tok_end = tok + strlen(tok);
                  while (tok_end > tok && (tok_end[-1] == ' ' || tok_end[-1] == '\t'))
                     tok_end--;
                  *tok_end = '\0';
                  if (strcmp(tok, "*") == 0 || strcmp(tok, etag) == 0) {
                     send_304_not_modified(wsi, etag);
                     return LWS_CLOSE_CONNECTION;
                  }
                  tok = strtok_r(NULL, ",", &saveptr);
               }
            }
         }

         /* Build per-request headers = security headers + cache policy [+ ETag].
          * Falls back to security-headers-only on formatting overflow so we
          * never drop CSP/X-Frame-Options. */
         char per_req_headers[1536];
         int per_req_len = build_static_cache_headers(per_req_headers, sizeof(per_req_headers),
                                                      filepath, mime_type, have_etag ? etag : "");
         const char *sec_hdrs;
         int sec_hdr_len;
         if (per_req_len > 0) {
            sec_hdrs = per_req_headers;
            sec_hdr_len = per_req_len;
         } else {
            sec_hdrs = webui_get_static_security_headers(&sec_hdr_len);
         }

         n = lws_serve_http_file(wsi, filepath, mime_type, sec_hdrs, sec_hdr_len);
         if (n < 0) {
            /* File not found or error */
            OLOG_WARNING("WebUI: File not found: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            return LWS_CLOSE_CONNECTION;
         }
         if (n > 0) {
            /* File is being sent, connection will close after */
            return 0;
         }
         break;
      }

      case LWS_CALLBACK_HTTP_FILE_COMPLETION:
         /* File transfer complete */
         return LWS_CLOSE_CONNECTION;

#ifdef ENABLE_AUTH
      case LWS_CALLBACK_HTTP_BODY: {
         /* Accumulate POST body */
         if (!pss)
            return LWS_CLOSE_CONNECTION;

         /* Image upload - route to image handler */
         if (pss->image_session) {
            return webui_images_handle_upload_body(wsi, pss->image_session, in, len);
         }

         /* Document upload - route to document handler */
         if (pss->document_session) {
            return webui_documents_handle_upload_body(wsi, pss->document_session, in, len);
         }

         /* Large POST body (dynamically allocated for summarize etc.) */
         if (pss->large_body) {
            const size_t max_body = (size_t)g_config.documents.max_extracted_size_kb * 1024 + 1024;
            if (pss->large_body_len + len > max_body) {
               OLOG_WARNING("webui_http: large POST body exceeds limit (%zu + %zu > %zu)",
                            pss->large_body_len, len, max_body);
               return LWS_CLOSE_CONNECTION;
            }
            /* Grow buffer if needed */
            if (pss->large_body_len + len >= pss->large_body_cap) {
               size_t new_cap = pss->large_body_cap * 2;
               if (new_cap < pss->large_body_len + len + 1)
                  new_cap = pss->large_body_len + len + 1;
               if (new_cap > max_body + 1)
                  new_cap = max_body + 1;
               char *new_buf = realloc(pss->large_body, new_cap);
               if (!new_buf) {
                  OLOG_ERROR("webui_http: failed to grow large POST body buffer");
                  return LWS_CLOSE_CONNECTION;
               }
               pss->large_body = new_buf;
               pss->large_body_cap = new_cap;
            }
            memcpy(pss->large_body + pss->large_body_len, in, len);
            pss->large_body_len += len;
            pss->large_body[pss->large_body_len] = '\0';
            return 0;
         }

         /* Regular POST body */
         size_t remaining = HTTP_MAX_POST_BODY - pss->post_body_len - 1;
         size_t to_copy = (len < remaining) ? len : remaining;

         if (to_copy > 0) {
            memcpy(pss->post_body + pss->post_body_len, in, to_copy);
            pss->post_body_len += to_copy;
            pss->post_body[pss->post_body_len] = '\0';
         }
         return 0;
      }

      case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
         /* POST body complete - process it */
         if (!pss)
            return LWS_CLOSE_CONNECTION;

         /* Image upload complete */
         if (pss->image_session) {
            int result = webui_images_handle_upload_complete(wsi, pss->image_session);
            pss->image_session = NULL; /* Session freed by handler */
            return result;
         }

         /* Document upload complete */
         if (pss->document_session) {
            int result = webui_documents_handle_upload_complete(wsi, pss->document_session);
            pss->document_session = NULL; /* Session freed by handler */
            return result;
         }

         /* Document summarize endpoint (uses large_body for big documents) */
         if (strcmp(pss->path, "/api/documents/summarize") == 0) {
            const char *body = pss->large_body ? pss->large_body : pss->post_body;
            size_t body_len = pss->large_body ? pss->large_body_len : pss->post_body_len;
            int result = webui_documents_handle_summarize(wsi, body, body_len);
            free(pss->large_body);
            pss->large_body = NULL;
            return result;
         }

         /* Handle login endpoint */
         if (strcmp(pss->path, "/api/auth/login") == 0) {
            return handle_auth_login(wsi, pss);
         }

         /* Unknown POST endpoint */
         lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
         return LWS_CLOSE_CONNECTION;
      }

      case LWS_CALLBACK_CLOSED_HTTP:
         /* Connection closed - clean up upload sessions if any */
         if (pss && pss->image_session) {
            webui_images_session_free(pss->image_session);
            pss->image_session = NULL;
         }
         if (pss && pss->document_session) {
            webui_documents_session_free(pss->document_session);
            pss->document_session = NULL;
         }
         if (pss && pss->large_body) {
            free(pss->large_body);
            pss->large_body = NULL;
         }
         break;
#endif /* ENABLE_AUTH */

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Public API Functions
 * ============================================================================= */

#ifdef ENABLE_AUTH
/**
 * @brief Clear login rate limit for an IP address or all IPs
 *
 * Used by admin operations to unlock IPs that have been rate-limited.
 * The in-memory rate limiter provides fast-path rejection for brute force
 * attempts; this function clears that state.
 *
 * @param ip_address IP to clear, or NULL to clear all rate limits
 */
void webui_clear_login_rate_limit(const char *ip_address) {
   if (ip_address) {
      /* Normalize IP before resetting (same normalization used during check) */
      char normalized_ip[RATE_LIMIT_IP_SIZE];
      rate_limiter_normalize_ip(ip_address, normalized_ip, sizeof(normalized_ip));
      rate_limiter_reset(&s_login_rate, normalized_ip);
      OLOG_INFO("WebUI: Cleared in-memory rate limit for IP: %s (normalized: %s)", ip_address,
                normalized_ip);
   } else {
      rate_limiter_clear_all(&s_login_rate);
      OLOG_INFO("WebUI: Cleared all in-memory rate limits");
   }
}
#endif /* ENABLE_AUTH */
