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
 * - OAuth callbacks (SmartThings)
 * - Health check endpoint
 */

#define _GNU_SOURCE /* For strcasestr */

#include <pthread.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
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
#endif /* ENABLE_AUTH */

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
 * @brief Send JSON response with optional Set-Cookie header
 *
 * @param wsi HTTP connection
 * @param status HTTP status code
 * @param json_body JSON string to send
 * @param cookie Cookie value to set (NULL for no cookie, empty string to clear)
 * @param cookie_max_age Max-Age for cookie (0 = session cookie, >0 = persistent)
 * @return 0 on success, -1 on failure
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
      return -1;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return -1;
   if (lws_add_http_header_content_length(wsi, body_len, &p, end))
      return -1;

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
         return -1;
   }

   if (lws_finalize_http_header(wsi, &p, end))
      return -1;

   /* Write headers first */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return -1;

   /* Write body - use LWS_WRITE_HTTP_FINAL to indicate completion */
   n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP_FINAL);
   if (n < 0)
      return -1;

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
 * @return 0 on success, -1 on failure
 */
static int send_nocache_json_response(struct lws *wsi, int status, const char *json_body) {
   size_t body_len = strlen(json_body);
   unsigned char buffer[LWS_PRE + 4096];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return -1;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return -1;
   if (lws_add_http_header_content_length(wsi, body_len, &p, end))
      return -1;

   /* Add no-cache headers to prevent token caching */
   if (lws_add_http_header_by_name(wsi, (unsigned char *)"Cache-Control:",
                                   (unsigned char *)"no-store, no-cache, must-revalidate, private",
                                   44, &p, end))
      return -1;
   if (lws_add_http_header_by_name(wsi, (unsigned char *)"Pragma:", (unsigned char *)"no-cache", 8,
                                   &p, end))
      return -1;

   if (lws_finalize_http_header(wsi, &p, end))
      return -1;

   /* Write headers first */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return -1;

   /* Write body - use LWS_WRITE_HTTP_FINAL to indicate completion */
   n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP_FINAL);
   if (n < 0)
      return -1;

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
 * @return -1 to close connection after response
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
      LOG_WARNING("WebUI: Rate limited IP (in-memory): %s (normalized: %s)", client_ip,
                  normalized_ip);
      auth_db_log_event("RATE_LIMITED", NULL, client_ip, "Too many failed attempts");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Too many attempts. Try again later.\"}");
      send_auth_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, response, NULL, 0);
      return -1;
   }

   /* Also check database for persistence across restarts */
   time_t window_start = time(NULL) - RATE_LIMIT_WINDOW_SEC;
   int recent_failures = auth_db_count_recent_failures(normalized_ip, window_start);
   if (recent_failures >= RATE_LIMIT_MAX_ATTEMPTS) {
      LOG_WARNING("WebUI: Rate limited IP (database): %s (normalized: %s)", client_ip,
                  normalized_ip);
      auth_db_log_event("RATE_LIMITED", NULL, client_ip, "Too many failed attempts");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Too many attempts. Try again later.\"}");
      send_auth_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, response, NULL, 0);
      return -1;
   }

   /* Parse JSON body */
   struct json_object *req = json_tokener_parse(pss->post_body);
   if (!req) {
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid JSON\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL, 0);
      return -1;
   }

   /* Extract and validate CSRF token */
   struct json_object *csrf_obj;
   if (!json_object_object_get_ex(req, "csrf_token", &csrf_obj)) {
      json_object_put(req);
      LOG_WARNING("WebUI: Login attempt without CSRF token from %s", client_ip);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Missing CSRF token\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL, 0);
      return -1;
   }

   const char *csrf_token = json_object_get_string(csrf_obj);
   unsigned char csrf_nonce[AUTH_CSRF_NONCE_SIZE];
   if (!auth_verify_csrf_token_extract_nonce(csrf_token, csrf_nonce)) {
      json_object_put(req);
      LOG_WARNING("WebUI: Invalid CSRF token from %s", client_ip);
      auth_db_log_event("CSRF_FAILED", NULL, client_ip, "Invalid or expired CSRF token");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Invalid or expired token. Please refresh.\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL, 0);
      return -1;
   }

   /* Check for CSRF token replay (single-use enforcement) */
   if (csrf_is_nonce_used(csrf_nonce)) {
      json_object_put(req);
      LOG_WARNING("WebUI: CSRF token replay attempt from %s", client_ip);
      auth_db_log_event("CSRF_REPLAY", NULL, client_ip, "CSRF token reuse detected");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Token already used. Please refresh.\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL, 0);
      return -1;
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
      return -1;
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
      LOG_WARNING("WebUI: Login failed - user not found: %s from %s", username, client_ip);
      auth_db_log_attempt(normalized_ip, username, false);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid credentials\"}");
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED, response, NULL, 0);
      return -1;
   }

   /* Check if account is locked */
   time_t now = time(NULL);
   if (user.lockout_until > now) {
      json_object_put(req);
      LOG_WARNING("WebUI: Login failed - account locked: %s from %s", username, client_ip);
      auth_db_log_attempt(normalized_ip, username, false);
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Account temporarily locked\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL, 0);
      return -1;
   } else if (user.lockout_until > 0 && user.lockout_until <= now) {
      /* Lockout expired - reset failed attempts counter */
      auth_db_reset_failed_attempts(username);
      auth_db_set_lockout(username, 0);
      LOG_INFO("WebUI: Lockout expired, reset failed attempts: %s", username);
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
            LOG_WARNING("WebUI: Account locked due to %d failed attempts: %s",
                        updated_user.failed_attempts, username);
         }
      }

      LOG_WARNING("WebUI: Login failed - wrong password: %s from %s", username, client_ip);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid credentials\"}");
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED, response, NULL, 0);
      return -1;
   }

   json_object_put(req);

   /* Generate session token */
   char session_token[AUTH_TOKEN_LEN];
   if (auth_generate_token(session_token) != AUTH_CRYPTO_SUCCESS) {
      LOG_ERROR("WebUI: Failed to generate session token");
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Server error\"}");
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, response, NULL, 0);
      return -1;
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
      LOG_ERROR("WebUI: Failed to create session for user: %s", username);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Server error\"}");
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, response, NULL, 0);
      auth_secure_zero(session_token, sizeof(session_token));
      return -1;
   }

   /* Reset failed attempts and update last login */
   auth_db_reset_failed_attempts(username);
   rate_limiter_reset(&s_login_rate, normalized_ip); /* Clear in-memory rate limit on success */
   auth_db_update_last_login(username);
   auth_db_log_attempt(normalized_ip, username, true);
   auth_db_log_event("LOGIN_SUCCESS", username, client_ip, "WebUI login successful");

   LOG_INFO("WebUI: User logged in: %s from %s%s", username, client_ip,
            remember_me ? " (remember me)" : "");

   /* Send success response with session cookie
    * remember_me: persistent cookie for 30 days; otherwise session cookie */
   int cookie_max_age = remember_me ? AUTH_REMEMBER_ME_TIMEOUT_SEC : 0;
   snprintf(response, sizeof(response), "{\"success\":true,\"username\":\"%s\",\"is_admin\":%s}",
            username, user.is_admin ? "true" : "false");
   send_auth_response(wsi, HTTP_STATUS_OK, response, session_token, cookie_max_age);

   /* Clear session token from stack after use */
   auth_secure_zero(session_token, sizeof(session_token));
   return -1;
}

/**
 * @brief Check if request Origin/Referer matches our host (CSRF protection)
 * @param wsi HTTP connection
 * @return true if same-origin, false if cross-origin
 */
static bool is_same_origin_request(struct lws *wsi) {
   char host[256] = { 0 };
   char origin[256] = { 0 };
   char referer[512] = { 0 };

   /* Get our Host header (what the browser thinks we are) */
   int host_len = lws_hdr_copy(wsi, host, sizeof(host), WSI_TOKEN_HOST);
   if (host_len <= 0) {
      return true; /* No host header - allow (unlikely) */
   }

   /* Check Origin header (preferred for CORS/CSRF) */
   int origin_len = lws_hdr_copy(wsi, origin, sizeof(origin), WSI_TOKEN_ORIGIN);
   if (origin_len > 0) {
      /* Origin should be https://host or http://host */
      char expected_https[280], expected_http[280];
      snprintf(expected_https, sizeof(expected_https), "https://%s", host);
      snprintf(expected_http, sizeof(expected_http), "http://%s", host);
      if (strcmp(origin, expected_https) == 0 || strcmp(origin, expected_http) == 0) {
         return true;
      }
      LOG_WARNING("CSRF: Origin mismatch - expected %s, got %s", host, origin);
      return false;
   }

   /* No Origin - check Referer as fallback */
   int referer_len = lws_hdr_copy(wsi, referer, sizeof(referer), WSI_TOKEN_HTTP_REFERER);
   if (referer_len > 0) {
      /* Referer should start with https://host/ or http://host/ */
      char expected_https[280], expected_http[280];
      snprintf(expected_https, sizeof(expected_https), "https://%s/", host);
      snprintf(expected_http, sizeof(expected_http), "http://%s/", host);
      if (strncmp(referer, expected_https, strlen(expected_https)) == 0 ||
          strncmp(referer, expected_http, strlen(expected_http)) == 0) {
         return true;
      }
      LOG_WARNING("CSRF: Referer mismatch - expected %s, got %s", host, referer);
      return false;
   }

   /* No Origin or Referer - allow for API/curl usage */
   return true;
}

/**
 * @brief Handle POST /api/auth/logout
 * @param wsi HTTP connection
 * @return -1 to close connection after response
 */
static int handle_auth_logout(struct lws *wsi) {
   /* CSRF protection: verify request is same-origin */
   if (!is_same_origin_request(wsi)) {
      LOG_WARNING("WebUI: Blocked cross-origin logout attempt");
      lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
      return -1;
   }

   char token[AUTH_TOKEN_LEN];
   if (extract_session_cookie(wsi, token)) {
      auth_session_t session;
      if (auth_db_get_session(token, &session) == AUTH_DB_SUCCESS) {
         char client_ip[64] = "unknown";
         lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
         auth_db_log_event("logout", session.username, client_ip, "WebUI logout");
         auth_db_delete_session(token);
         LOG_INFO("WebUI: User logged out: %s", session.username);
      }
   }

   /* Use simple HTTP status - no body needed, avoids lws_write issues.
    * JavaScript redirects regardless of response content. */
   lws_return_http_status(wsi, HTTP_STATUS_OK, NULL);
   return -1;
}

/**
 * @brief Handle GET /api/auth/status
 * @param wsi HTTP connection
 * @return -1 to close connection after response
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
   return -1;
}

/**
 * @brief Handle GET /api/auth/csrf
 * @param wsi HTTP connection
 * @return -1 to close connection after response
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
      LOG_WARNING("WebUI: CSRF rate limited: %s", normalized_ip);
      send_nocache_json_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS,
                                 "{\"error\":\"Too many requests\"}");
      return -1;
   }

   char csrf_token[AUTH_CSRF_TOKEN_LEN];

   if (auth_generate_csrf_token(csrf_token) != AUTH_CRYPTO_SUCCESS) {
      LOG_ERROR("WebUI: Failed to generate CSRF token");
      send_nocache_json_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "{\"error\":\"Failed to generate token\"}");
      return -1;
   }

   char response[256];
   snprintf(response, sizeof(response), "{\"csrf_token\":\"%s\"}", csrf_token);

   /* Clear token from stack */
   auth_secure_zero(csrf_token, sizeof(csrf_token));

   /* Use no-cache response to prevent browser/proxy caching */
   send_nocache_json_response(wsi, HTTP_STATUS_OK, response);
   return -1;
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
            return -1;
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

         /* Image API endpoints (require auth) */
         /* GET /api/images/:id - download image */
         if (strncmp(path, "/api/images/", 12) == 0 && pss && !pss->is_post) {
            auth_session_t session;
            if (is_request_authenticated(wsi, &session)) {
               const char *image_id = path + 12;
               return webui_images_handle_download(wsi, image_id, session.user_id);
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

         /* Public paths that don't require auth */
         bool is_public_path =
             (strcmp(path, "/login.html") == 0 || strcmp(path, "/health") == 0 ||
              strncmp(path, "/css/", 5) == 0 || strncmp(path, "/fonts/", 7) == 0 ||
              strcmp(path, "/favicon.svg") == 0 ||
              /* AudioWorklet.addModule() doesn't send cookies, so worklet must be public */
              strcmp(path, "/js/audio/capture-worklet.js") == 0);

         /* Check authentication for protected paths */
         if (!is_public_path && !is_request_authenticated(wsi, NULL)) {
            /* Redirect to login page */
            unsigned char buffer[LWS_PRE + 256];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_FOUND, &p, end))
               return -1;
            if (lws_add_http_header_by_name(wsi, (unsigned char *)"Location:",
                                            (unsigned char *)"/login.html", 11, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, 0, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            return -1; /* Close connection */
         }
#endif /* ENABLE_AUTH */

         /* SmartThings OAuth callback - extract code and state from URL */
         if (strncmp(path, "/smartthings/callback", 21) == 0) {
            /* Generate inline HTML that extracts params and posts to opener */
            static const char oauth_callback_html[] =
                "<!DOCTYPE html><html><head><title>SmartThings Auth</title></head>"
                "<body><script>"
                "const params = new URLSearchParams(window.location.search);"
                "const code = params.get('code');"
                "const state = params.get('state');"
                "const error = params.get('error');"
                "if (window.opener) {"
                "  window.opener.postMessage({"
                "    type: 'smartthings_oauth_callback',"
                "    code: code,"
                "    state: state,"
                "    error: error"
                "  }, window.location.origin);"
                "  setTimeout(function() { window.close(); }, 500);"
                "} else {"
                "  document.body.innerHTML = '<p>Authorization ' + "
                "    (code ? 'successful' : 'failed') + '. You can close this window.</p>';"
                "}"
                "</script><p>Processing authorization...</p></body></html>";

            unsigned char buffer[LWS_PRE + sizeof(oauth_callback_html)];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return -1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"text/html", 9, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, sizeof(oauth_callback_html) - 1, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            n = lws_write(wsi, (unsigned char *)oauth_callback_html,
                          sizeof(oauth_callback_html) - 1, LWS_WRITE_HTTP);
            if (n < 0)
               return -1;

            return -1; /* Close connection after response */
         }

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
            unsigned char buffer[LWS_PRE + 512];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return -1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"application/json", 16, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, body_len, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP);
            if (n < 0)
               return -1;

            return -1; /* Close connection after response */
         }

         /* Default to index.html for root */
         if (strcmp(path, "/") == 0) {
            strncpy(path, "/index.html", sizeof(path) - 1);
         }

         /* Prevent directory traversal - check for patterns including URL-encoded */
         if (contains_path_traversal(path)) {
            LOG_WARNING("WebUI: Directory traversal attempt blocked: %s", path);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

         /* Build full filesystem path */
         snprintf(filepath, sizeof(filepath), "%s%s", s_www_path, path);

         /* Second layer: verify resolved path is within www directory */
         if (!is_path_within_www(filepath, s_www_path)) {
            LOG_WARNING("WebUI: Path escape attempt blocked: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

         /* Get MIME type */
         mime_type = get_mime_type(filepath);

         /* Serve the file (no extra headers - CSP set via meta tag) */
         n = lws_serve_http_file(wsi, filepath, mime_type, NULL, 0);
         if (n < 0) {
            /* File not found or error */
            LOG_WARNING("WebUI: File not found: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            return -1;
         }
         if (n > 0) {
            /* File is being sent, connection will close after */
            return 0;
         }
         break;
      }

      case LWS_CALLBACK_HTTP_FILE_COMPLETION:
         /* File transfer complete */
         return -1; /* Close connection */

#ifdef ENABLE_AUTH
      case LWS_CALLBACK_HTTP_BODY: {
         /* Accumulate POST body */
         if (!pss)
            return -1;

         /* Image upload - route to image handler */
         if (pss->image_session) {
            return webui_images_handle_upload_body(wsi, pss->image_session, in, len);
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
            return -1;

         /* Image upload complete */
         if (pss->image_session) {
            int result = webui_images_handle_upload_complete(wsi, pss->image_session);
            pss->image_session = NULL; /* Session freed by handler */
            return result;
         }

         /* Handle login endpoint */
         if (strcmp(pss->path, "/api/auth/login") == 0) {
            return handle_auth_login(wsi, pss);
         }

         /* Unknown POST endpoint */
         lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
         return -1;
      }

      case LWS_CALLBACK_CLOSED_HTTP:
         /* Connection closed - clean up image session if any */
         if (pss && pss->image_session) {
            webui_images_session_free(pss->image_session);
            pss->image_session = NULL;
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
      LOG_INFO("WebUI: Cleared in-memory rate limit for IP: %s (normalized: %s)", ip_address,
               normalized_ip);
   } else {
      rate_limiter_clear_all(&s_login_rate);
      LOG_INFO("WebUI: Cleared all in-memory rate limits");
   }
}
#endif /* ENABLE_AUTH */
