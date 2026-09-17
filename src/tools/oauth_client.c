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
 * OAuth 2.0 client — PKCE S256, token exchange, encrypted storage, auto-refresh.
 * Shared module for Google Calendar, Gmail, and future OAuth providers.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/oauth_client.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/crypto_store.h"
#include "core/curl_buffer.h"
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define OAUTH_STATE_LEN 32           /* Random state token bytes */
#define OAUTH_VERIFIER_LEN 32        /* PKCE code_verifier random bytes */
#define OAUTH_MAX_PENDING 8          /* Max pending auth flows globally */
#define OAUTH_MAX_PER_USER 2         /* Max pending flows per user */
#define OAUTH_PENDING_EXPIRY_SEC 300 /* 5 minute expiry for pending states */
#define OAUTH_REFRESH_MARGIN_SEC 300 /* Refresh tokens 5 min before expiry */
#define OAUTH_CURL_TIMEOUT 30        /* HTTP timeout for token requests */
#define OAUTH_ACCOUNT_MUTEX_SLOTS 16 /* Per-account mutex hash table size */

/* Base64url character set (no padding) */
static const char BASE64URL_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* =============================================================================
 * Pending State Tracking
 * ============================================================================= */

typedef struct {
   bool active;
   int user_id;
   char state[64];         /* Base64url-encoded state token */
   char code_verifier[64]; /* Base64url-encoded PKCE verifier */
   char redirect_uri[256];
   time_t created_at;
} pending_oauth_state_t;

static pending_oauth_state_t s_pending[OAUTH_MAX_PENDING];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Per-Account Refresh Mutex
 * ============================================================================= */

static pthread_mutex_t s_acct_mutexes[OAUTH_ACCOUNT_MUTEX_SLOTS] = {
   PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
   PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
   PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
   PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
   PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
   PTHREAD_MUTEX_INITIALIZER,
};

/** FNV-1a hash to pick a per-account mutex */
static unsigned int acct_mutex_index(const char *account_key) {
   unsigned int hash = 2166136261u;
   for (const char *p = account_key; *p; p++) {
      hash ^= (unsigned char)*p;
      hash *= 16777619u;
   }
   return hash % OAUTH_ACCOUNT_MUTEX_SLOTS;
}

/* =============================================================================
 * Base64url Encoding (no padding)
 * ============================================================================= */

static size_t base64url_encode(const unsigned char *data, size_t len, char *out, size_t out_len) {
   size_t needed = ((len * 4) / 3) + 4; /* Conservative estimate */
   if (out_len < needed)
      return 0;

   size_t pos = 0;
   size_t i = 0;
   while (i < len) {
      size_t start = i;
      uint32_t octet_a = data[i++];
      uint32_t octet_b = i < len ? data[i++] : 0;
      uint32_t octet_c = i < len ? data[i++] : 0;
      uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

      size_t remaining = len - start;
      out[pos++] = BASE64URL_CHARS[(triple >> 18) & 0x3F];
      out[pos++] = BASE64URL_CHARS[(triple >> 12) & 0x3F];
      if (remaining > 1)
         out[pos++] = BASE64URL_CHARS[(triple >> 6) & 0x3F];
      if (remaining > 2)
         out[pos++] = BASE64URL_CHARS[triple & 0x3F];
   }
   out[pos] = '\0';
   return pos;
}

/* =============================================================================
 * URL Encoding
 * ============================================================================= */

static size_t url_encode(const char *str, char *out, size_t out_len) {
   static const char hex[] = "0123456789ABCDEF";
   size_t pos = 0;
   for (const char *p = str; *p && pos + 3 < out_len; p++) {
      unsigned char c = (unsigned char)*p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_' || c == '.' || c == '~') {
         out[pos++] = c;
      } else {
         out[pos++] = '%';
         out[pos++] = hex[c >> 4];
         out[pos++] = hex[c & 0x0F];
      }
   }
   out[pos] = '\0';
   return pos;
}

/* =============================================================================
 * curl Response Buffer
 *
 * Uses the shared curl_buffer_t (include/core/curl_buffer.h) with a 1 MB
 * cap on token endpoint responses.  The shared helper truncates with a
 * flag instead of aborting mid-transfer — callers MUST check
 * `resp.truncated` after curl_easy_perform and reject the response.
 * ============================================================================= */

#define OAUTH_MAX_RESPONSE_SIZE (1024 * 1024) /* 1 MB cap on token endpoint responses */

/* Thread-local "last refresh saw invalid_grant" flag.  Set inside
 * oauth_refresh() when the upstream returns 400 with
 * `{"error":"invalid_grant"}` (token revoked at the provider).  Read by
 * downstream tool wrappers via oauth_was_last_refresh_revoked() so they
 * can substitute a "re-link this account" message instead of a generic
 * "network error".  Reset to zero on every non-revoked refresh outcome
 * so a stale value can't bleed into the next call on the same thread. */
static __thread int s_last_refresh_revoked = 0;
static __thread char s_last_revoked_account[128] = { 0 };

int oauth_was_last_refresh_revoked(char *account_out, size_t out_size) {
   if (account_out && out_size > 0) {
      snprintf(account_out, out_size, "%s", s_last_revoked_account);
   }
   return s_last_refresh_revoked ? 1 : 0;
}

static void oauth_set_last_revoked(int revoked, const char *account_key) {
   s_last_refresh_revoked = revoked ? 1 : 0;
   if (revoked && account_key) {
      snprintf(s_last_revoked_account, sizeof(s_last_revoked_account), "%s", account_key);
   } else if (!revoked) {
      s_last_revoked_account[0] = '\0';
   }
}

/* =============================================================================
 * Provider Setup
 * ============================================================================= */

int oauth_google_provider(const char *client_id,
                          const char *client_secret,
                          const char *redirect_uri,
                          const char *scopes,
                          oauth_provider_config_t *out) {
   if (!client_id || !client_id[0] || !client_secret || !client_secret[0] || !out)
      return 1;

   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "google");
   snprintf(out->client_id, sizeof(out->client_id), "%s", client_id);
   snprintf(out->client_secret, sizeof(out->client_secret), "%s", client_secret);
   snprintf(out->auth_endpoint, sizeof(out->auth_endpoint),
            "https://accounts.google.com/o/oauth2/v2/auth");
   snprintf(out->token_endpoint, sizeof(out->token_endpoint),
            "https://oauth2.googleapis.com/token");
   snprintf(out->revoke_endpoint, sizeof(out->revoke_endpoint),
            "https://oauth2.googleapis.com/revoke");
   snprintf(out->redirect_uri, sizeof(out->redirect_uri), "%s", redirect_uri ? redirect_uri : "");
   snprintf(out->scopes, sizeof(out->scopes), "%s",
            scopes ? scopes : "https://www.googleapis.com/auth/calendar");
   /* Google behavior: creds in the POST body, PKCE S256, full auth-URL params. */
   out->token_auth_basic = false;
   out->use_pkce = true;
   out->minimal_auth_params = false;
   return 0;
}

int oauth_build_google_provider(const char *scopes, oauth_provider_config_t *out) {
   extern secrets_config_t g_secrets;
   extern dawn_config_t g_config;

   if (g_secrets.google_client_id[0] == '\0' || g_secrets.google_client_secret[0] == '\0') {
      OLOG_ERROR("Google OAuth not configured — set google_client_id/secret in secrets.toml");
      return 1;
   }

   char redirect_uri[256];
   if (g_secrets.google_redirect_url[0] != '\0') {
      snprintf(redirect_uri, sizeof(redirect_uri), "%s", g_secrets.google_redirect_url);
   } else {
      int port = g_config.webui.port > 0 ? g_config.webui.port : 3000;
      snprintf(redirect_uri, sizeof(redirect_uri), "https://localhost:%d/oauth/callback", port);
   }

   return oauth_google_provider(g_secrets.google_client_id, g_secrets.google_client_secret,
                                redirect_uri, scopes, out);
}

int oauth_schwab_provider(const char *client_id,
                          const char *client_secret,
                          const char *redirect_uri,
                          oauth_provider_config_t *out) {
   if (!client_id || !client_id[0] || !client_secret || !client_secret[0] || !out)
      return 1;

   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "schwab");
   snprintf(out->client_id, sizeof(out->client_id), "%s", client_id);
   snprintf(out->client_secret, sizeof(out->client_secret), "%s", client_secret);
   snprintf(out->auth_endpoint, sizeof(out->auth_endpoint),
            "https://api.schwabapi.com/v1/oauth/authorize");
   snprintf(out->token_endpoint, sizeof(out->token_endpoint),
            "https://api.schwabapi.com/v1/oauth/token");
   out->revoke_endpoint[0] = '\0'; /* Schwab has no revocation endpoint */
   snprintf(out->redirect_uri, sizeof(out->redirect_uri), "%s", redirect_uri ? redirect_uri : "");
   out->scopes[0] = '\0'; /* Schwab grants its default scope; no scope param sent */
   /* Schwab behavior: client creds as HTTP Basic on the token endpoint, no PKCE,
    * minimal auth-URL params (no access_type/prompt/include_granted_scopes). */
   out->token_auth_basic = true;
   out->use_pkce = false;
   out->minimal_auth_params = true;
   return 0;
}

int oauth_build_schwab_provider(oauth_provider_config_t *out) {
   extern secrets_config_t g_secrets;

   if (g_secrets.schwab_client_id[0] == '\0' || g_secrets.schwab_client_secret[0] == '\0') {
      OLOG_ERROR("Schwab OAuth not configured — set schwab client_id/secret in "
                 "secrets.toml [secrets.schwab]");
      return 1;
   }

   const char *redirect_uri = g_secrets.schwab_redirect_url[0] != '\0'
                                  ? g_secrets.schwab_redirect_url
                                  : "https://127.0.0.1:8000/callback";

   return oauth_schwab_provider(g_secrets.schwab_client_id, g_secrets.schwab_client_secret,
                                redirect_uri, out);
}

/* =============================================================================
 * Pending State Management
 * ============================================================================= */

static void cleanup_expired_states(void) {
   time_t now = time(NULL);
   for (int i = 0; i < OAUTH_MAX_PENDING; i++) {
      if (s_pending[i].active && (now - s_pending[i].created_at) > OAUTH_PENDING_EXPIRY_SEC) {
         sodium_memzero(&s_pending[i], sizeof(s_pending[i]));
      }
   }
}

static int count_user_pending(int user_id) {
   int count = 0;
   for (int i = 0; i < OAUTH_MAX_PENDING; i++) {
      if (s_pending[i].active && s_pending[i].user_id == user_id)
         count++;
   }
   return count;
}

/* =============================================================================
 * OAuth Flow: Auth URL Generation
 * ============================================================================= */

int oauth_get_auth_url(const oauth_provider_config_t *provider,
                       int user_id,
                       char *url_buf,
                       size_t url_len,
                       char *state_buf,
                       size_t state_len) {
   if (!provider || !url_buf || !state_buf)
      return 1;

   if (crypto_store_init() != 0)
      return 1;

   pthread_mutex_lock(&s_pending_mutex);
   cleanup_expired_states();

   /* Check per-user limit */
   if (count_user_pending(user_id) >= OAUTH_MAX_PER_USER) {
      pthread_mutex_unlock(&s_pending_mutex);
      OLOG_WARNING("oauth: user %d already has %d pending flows", user_id, OAUTH_MAX_PER_USER);
      return 1;
   }

   /* Find free slot */
   int slot = -1;
   for (int i = 0; i < OAUTH_MAX_PENDING; i++) {
      if (!s_pending[i].active) {
         slot = i;
         break;
      }
   }
   if (slot < 0) {
      pthread_mutex_unlock(&s_pending_mutex);
      OLOG_WARNING("oauth: all %d pending slots full", OAUTH_MAX_PENDING);
      return 1;
   }

   /* Generate PKCE code_verifier + challenge (only when the provider uses PKCE;
    * Schwab does not, and rejecting the extra params keeps its auth URL clean). */
   unsigned char verifier_raw[OAUTH_VERIFIER_LEN];
   char code_verifier[64] = { 0 };
   char code_challenge[64] = { 0 };
   if (provider->use_pkce) {
      randombytes_buf(verifier_raw, sizeof(verifier_raw));
      base64url_encode(verifier_raw, sizeof(verifier_raw), code_verifier, sizeof(code_verifier));

      unsigned char hash[crypto_hash_sha256_BYTES];
      crypto_hash_sha256(hash, (const unsigned char *)code_verifier, strlen(code_verifier));
      base64url_encode(hash, sizeof(hash), code_challenge, sizeof(code_challenge));
      sodium_memzero(verifier_raw, sizeof(verifier_raw));
   }

   /* Generate state token (32 random bytes → base64url) */
   unsigned char state_raw[OAUTH_STATE_LEN];
   randombytes_buf(state_raw, sizeof(state_raw));
   char state[64];
   base64url_encode(state_raw, sizeof(state_raw), state, sizeof(state));

   /* Store pending state (code_verifier is empty for non-PKCE providers) */
   s_pending[slot].active = true;
   s_pending[slot].user_id = user_id;
   snprintf(s_pending[slot].state, sizeof(s_pending[slot].state), "%s", state);
   snprintf(s_pending[slot].code_verifier, sizeof(s_pending[slot].code_verifier), "%s",
            code_verifier);
   snprintf(s_pending[slot].redirect_uri, sizeof(s_pending[slot].redirect_uri), "%s",
            provider->redirect_uri);
   s_pending[slot].created_at = time(NULL);

   pthread_mutex_unlock(&s_pending_mutex);

   /* Build authorization URL incrementally: required base, then optional
    * scope / Google-only / PKCE segments. */
   char enc_client_id[256], enc_redirect[512];
   url_encode(provider->client_id, enc_client_id, sizeof(enc_client_id));
   url_encode(provider->redirect_uri, enc_redirect, sizeof(enc_redirect));

   int off = snprintf(url_buf, url_len,
                      "%s?client_id=%s&redirect_uri=%s&response_type=code&state=%s",
                      provider->auth_endpoint, enc_client_id, enc_redirect, state);
   if (off < 0 || (size_t)off >= url_len) {
      OLOG_ERROR("oauth: auth URL truncated");
      return 1;
   }

   /* scope — omitted entirely when the provider defines no scopes (Schwab) */
   if (provider->scopes[0]) {
      char enc_scopes[1024];
      url_encode(provider->scopes, enc_scopes, sizeof(enc_scopes));
      int n = snprintf(url_buf + off, url_len - (size_t)off, "&scope=%s", enc_scopes);
      if (n < 0 || (size_t)n >= url_len - (size_t)off) {
         OLOG_ERROR("oauth: auth URL truncated");
         return 1;
      }
      off += n;
   }

   /* Google-only incremental-auth params */
   if (!provider->minimal_auth_params) {
      int n = snprintf(url_buf + off, url_len - (size_t)off,
                       "&access_type=offline&prompt=consent&include_granted_scopes=true");
      if (n < 0 || (size_t)n >= url_len - (size_t)off) {
         OLOG_ERROR("oauth: auth URL truncated");
         return 1;
      }
      off += n;
   }

   /* PKCE challenge */
   if (provider->use_pkce) {
      char enc_challenge[128];
      url_encode(code_challenge, enc_challenge, sizeof(enc_challenge));
      int n = snprintf(url_buf + off, url_len - (size_t)off,
                       "&code_challenge=%s&code_challenge_method=S256", enc_challenge);
      if (n < 0 || (size_t)n >= url_len - (size_t)off) {
         OLOG_ERROR("oauth: auth URL truncated");
         return 1;
      }
      off += n;
   }

   snprintf(state_buf, state_len, "%s", state);

   sodium_memzero(code_verifier, sizeof(code_verifier)); /* Only copy is in s_pending */

   return 0;
}

/* =============================================================================
 * Google Userinfo: Fetch email after token exchange
 * ============================================================================= */

static void oauth_fetch_google_email(const char *access_token, char *email_out, size_t email_len) {
   email_out[0] = '\0';

   CURL *curl = curl_easy_init();
   if (!curl)
      return;

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, OAUTH_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_URL, "https://www.googleapis.com/oauth2/v2/userinfo");
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)OAUTH_CURL_TIMEOUT);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");

   /* Set Authorization: Bearer header */
   char auth_header[2200];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
   struct curl_slist *headers = curl_slist_append(NULL, auth_header);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);
   sodium_memzero(auth_header, sizeof(auth_header));

   if (cres != CURLE_OK || http_code != 200 || resp.truncated) {
      if (resp.truncated) {
         OLOG_WARNING("oauth: userinfo response exceeded %d byte cap; rejecting",
                      OAUTH_MAX_RESPONSE_SIZE);
      } else {
         OLOG_WARNING("oauth: failed to fetch Google userinfo (HTTP %ld)", http_code);
      }
      if (resp.data) {
         sodium_memzero(resp.data, resp.size);
      }
      curl_buffer_free(&resp);
      return;
   }

   json_object *root = json_tokener_parse(resp.data);
   sodium_memzero(resp.data, resp.size);
   curl_buffer_free(&resp);
   if (!root)
      return;

   json_object *j_email;
   if (json_object_object_get_ex(root, "email", &j_email)) {
      snprintf(email_out, email_len, "%s", json_object_get_string(j_email));
   }
   json_object_put(root);
}

/* =============================================================================
 * OAuth Flow: Code Exchange
 * ============================================================================= */

int oauth_exchange_code(const oauth_provider_config_t *provider,
                        const char *code,
                        const char *state,
                        int user_id,
                        oauth_token_set_t *out_tokens) {
   if (!provider || !code || !state || !out_tokens)
      return 1;

   memset(out_tokens, 0, sizeof(*out_tokens));

   /* Look up and validate pending state */
   char code_verifier[64] = { 0 };

   pthread_mutex_lock(&s_pending_mutex);
   cleanup_expired_states();

   int found = -1;
   for (int i = 0; i < OAUTH_MAX_PENDING; i++) {
      if (s_pending[i].active && s_pending[i].user_id == user_id &&
          strlen(s_pending[i].state) == strlen(state) &&
          sodium_memcmp(s_pending[i].state, state, strlen(state)) == 0) {
         found = i;
         break;
      }
   }

   if (found < 0) {
      pthread_mutex_unlock(&s_pending_mutex);
      OLOG_ERROR("oauth: invalid or expired state token");
      return 1;
   }

   /* Copy verifier and clear the pending slot */
   snprintf(code_verifier, sizeof(code_verifier), "%s", s_pending[found].code_verifier);
   sodium_memzero(&s_pending[found], sizeof(s_pending[found]));
   pthread_mutex_unlock(&s_pending_mutex);

   /* Build POST body incrementally.  Credentials go in the body only when the
    * provider does NOT use HTTP Basic on the token endpoint (Basic providers set
    * them via CURLOPT_USERNAME/PASSWORD below); the PKCE verifier only when the
    * provider uses PKCE. */
   char post_body[2048];
   /* enc_code sized to match oauth_complete_from_redirect's raw_code[1024] so a
    * long (but in-range) code can't silently truncate here. */
   char enc_code[1024], enc_redirect[512];
   url_encode(code, enc_code, sizeof(enc_code));
   url_encode(provider->redirect_uri, enc_redirect, sizeof(enc_redirect));

   int off = snprintf(post_body, sizeof(post_body),
                      "grant_type=authorization_code&code=%s&redirect_uri=%s", enc_code,
                      enc_redirect);
   if (off < 0 || (size_t)off >= sizeof(post_body)) {
      OLOG_ERROR("oauth: POST body truncated");
      sodium_memzero(code_verifier, sizeof(code_verifier));
      sodium_memzero(post_body, sizeof(post_body));
      sodium_memzero(enc_code, sizeof(enc_code));
      return 1;
   }

   if (!provider->token_auth_basic) {
      char enc_client_id[256], enc_client_secret[512];
      url_encode(provider->client_id, enc_client_id, sizeof(enc_client_id));
      url_encode(provider->client_secret, enc_client_secret, sizeof(enc_client_secret));
      int n = snprintf(post_body + off, sizeof(post_body) - (size_t)off,
                       "&client_id=%s&client_secret=%s", enc_client_id, enc_client_secret);
      sodium_memzero(enc_client_secret, sizeof(enc_client_secret));
      if (n < 0 || (size_t)n >= sizeof(post_body) - (size_t)off) {
         OLOG_ERROR("oauth: POST body truncated");
         sodium_memzero(code_verifier, sizeof(code_verifier));
         sodium_memzero(post_body, sizeof(post_body));
         sodium_memzero(enc_code, sizeof(enc_code));
         return 1;
      }
      off += n;
   }

   if (provider->use_pkce) {
      char enc_verifier[128];
      url_encode(code_verifier, enc_verifier, sizeof(enc_verifier));
      int n = snprintf(post_body + off, sizeof(post_body) - (size_t)off, "&code_verifier=%s",
                       enc_verifier);
      sodium_memzero(enc_verifier, sizeof(enc_verifier));
      if (n < 0 || (size_t)n >= sizeof(post_body) - (size_t)off) {
         OLOG_ERROR("oauth: POST body truncated");
         sodium_memzero(code_verifier, sizeof(code_verifier));
         sodium_memzero(post_body, sizeof(post_body));
         sodium_memzero(enc_code, sizeof(enc_code));
         return 1;
      }
      off += n;
   }

   sodium_memzero(code_verifier, sizeof(code_verifier));

   /* HTTP POST to token endpoint */
   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_ERROR("oauth: curl_easy_init failed");
      return 1;
   }

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, OAUTH_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_URL, provider->token_endpoint);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
   /* Basic providers (Schwab) authenticate the client with HTTP Basic; libcurl
    * emits a correctly padded Authorization header. */
   if (provider->token_auth_basic) {
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
      curl_easy_setopt(curl, CURLOPT_USERNAME, provider->client_id);
      curl_easy_setopt(curl, CURLOPT_PASSWORD, provider->client_secret);
   }
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)OAUTH_CURL_TIMEOUT);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");

   CURLcode cres = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   sodium_memzero(post_body, sizeof(post_body));
   sodium_memzero(enc_code, sizeof(enc_code));

   if (cres != CURLE_OK || http_code != 200 || resp.truncated) {
      if (resp.truncated) {
         OLOG_ERROR("oauth: token exchange response exceeded %d byte cap; rejecting",
                    OAUTH_MAX_RESPONSE_SIZE);
      } else {
         OLOG_ERROR("oauth: token exchange failed (HTTP %ld, curl %d)", http_code, cres);
      }
      if (resp.data) {
         sodium_memzero(resp.data, resp.size);
      }
      curl_buffer_free(&resp);
      return 1;
   }

   /* Parse JSON response */
   json_object *root = json_tokener_parse(resp.data);
   sodium_memzero(resp.data, resp.size);
   curl_buffer_free(&resp);
   if (!root) {
      OLOG_ERROR("oauth: failed to parse token response JSON");
      return 1;
   }

   json_object *j_access, *j_refresh, *j_expires, *j_scope;
   if (json_object_object_get_ex(root, "access_token", &j_access))
      snprintf(out_tokens->access_token, sizeof(out_tokens->access_token), "%s",
               json_object_get_string(j_access));
   if (json_object_object_get_ex(root, "refresh_token", &j_refresh))
      snprintf(out_tokens->refresh_token, sizeof(out_tokens->refresh_token), "%s",
               json_object_get_string(j_refresh));
   if (json_object_object_get_ex(root, "expires_in", &j_expires))
      out_tokens->expires_at = time(NULL) + json_object_get_int64(j_expires);
   if (json_object_object_get_ex(root, "scope", &j_scope))
      snprintf(out_tokens->scopes, sizeof(out_tokens->scopes), "%s",
               json_object_get_string(j_scope));

   json_object_put(root);

   /* The code exchange IS the original authorization — stamp the link time so
    * downstream can warn before a fixed-lifetime refresh token (e.g. Schwab's
    * 7-day) lapses. */
   out_tokens->linked_at = time(NULL);

   if (!out_tokens->access_token[0]) {
      OLOG_ERROR("oauth: no access_token in response");
      return 1;
   }

   /* Fetch user email from provider userinfo endpoint (Google-specific) */
   if (strcmp(provider->provider, "google") == 0) {
      oauth_fetch_google_email(out_tokens->access_token, out_tokens->email,
                               sizeof(out_tokens->email));
   }

   OLOG_INFO("oauth: token exchange successful for provider '%s'%s%s%s", provider->provider,
             out_tokens->email[0] ? " (" : "", out_tokens->email[0] ? out_tokens->email : "",
             out_tokens->email[0] ? ")" : "");
   return 0;
}

/* =============================================================================
 * Complete a flow from a pasted redirect URL (CLI + future WebUI paste field)
 * ============================================================================= */

int oauth_complete_from_redirect(const oauth_provider_config_t *provider,
                                 int user_id,
                                 const char *redirect_url,
                                 const char *account_key,
                                 char *err,
                                 size_t err_len) {
   if (err && err_len)
      err[0] = '\0';
   if (!provider || !redirect_url || !account_key) {
      if (err)
         snprintf(err, err_len, "internal error");
      return 1;
   }

   /* The pasted URL's base (everything before '?') must EXACTLY match the
    * provider's registered redirect URI — reject a paste from any other origin. */
   const char *q = strchr(redirect_url, '?');
   if (!q) {
      if (err)
         snprintf(err, err_len, "That does not look like a redirect URL (no '?' query).");
      return 1;
   }
   size_t base_len = (size_t)(q - redirect_url);
   if (base_len != strlen(provider->redirect_uri) ||
       strncmp(redirect_url, provider->redirect_uri, base_len) != 0) {
      if (err)
         snprintf(err, err_len, "URL origin/path does not match the configured redirect (%s).",
                  provider->redirect_uri);
      return 1;
   }

   /* Extract raw code= and state= from the query string (ignore other params,
    * e.g. Schwab's extra 'session'). */
   char raw_code[1024] = { 0 }, raw_state[256] = { 0 };
   const char *p = q + 1;
   while (*p) {
      const char *amp = strchr(p, '&');
      size_t seg = amp ? (size_t)(amp - p) : strlen(p);
      if (seg > 5 && strncmp(p, "code=", 5) == 0) {
         size_t vlen = seg - 5;
         if (vlen < sizeof(raw_code)) {
            memcpy(raw_code, p + 5, vlen);
            raw_code[vlen] = '\0';
         }
      } else if (seg > 6 && strncmp(p, "state=", 6) == 0) {
         size_t vlen = seg - 6;
         if (vlen < sizeof(raw_state)) {
            memcpy(raw_state, p + 6, vlen);
            raw_state[vlen] = '\0';
         }
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   if (!raw_code[0] || !raw_state[0]) {
      if (err)
         snprintf(err, err_len, "URL is missing the code or state parameter.");
      sodium_memzero(raw_code, sizeof(raw_code));
      return 1;
   }

   /* URL-decode: the code is percent-encoded (Schwab's ends in %40 → '@'), and
    * oauth_exchange_code re-encodes, so it must arrive decoded. */
   CURL *curl = curl_easy_init();
   if (!curl) {
      if (err)
         snprintf(err, err_len, "internal error");
      sodium_memzero(raw_code, sizeof(raw_code));
      return 1;
   }
   char *dec_code = curl_easy_unescape(curl, raw_code, 0, NULL);
   char *dec_state = curl_easy_unescape(curl, raw_state, 0, NULL);
   sodium_memzero(raw_code, sizeof(raw_code));
   sodium_memzero(raw_state, sizeof(raw_state));

   int rc = 1;
   if (dec_code && dec_state) {
      oauth_token_set_t tokens;
      if (oauth_exchange_code(provider, dec_code, dec_state, user_id, &tokens) != 0) {
         if (err)
            snprintf(err, err_len,
                     "Token exchange failed — the paste may have expired (5 min), or the app "
                     "key/secret/redirect may not match. See the daemon log.");
      } else if (oauth_store_tokens(user_id, provider->provider, account_key, &tokens) != 0) {
         if (err)
            snprintf(err, err_len, "Token exchange succeeded but storing the tokens failed.");
      } else {
         rc = 0;
      }
      sodium_memzero(&tokens, sizeof(tokens));
   } else if (err) {
      snprintf(err, err_len, "internal error");
   }

   if (dec_code) {
      sodium_memzero(dec_code, strlen(dec_code));
      curl_free(dec_code);
   }
   if (dec_state) {
      sodium_memzero(dec_state, strlen(dec_state));
      curl_free(dec_state);
   }
   curl_easy_cleanup(curl);
   return rc;
}

/* =============================================================================
 * Token Refresh
 * ============================================================================= */

int oauth_refresh(const oauth_provider_config_t *provider, oauth_token_set_t *tokens) {
   /* Clear thread-local revoked flag at entry; on the failure path we
    * may set it back if invalid_grant is detected.  On the success path
    * it stays cleared. */
   oauth_set_last_revoked(0, NULL);

   if (!provider || !tokens || !tokens->refresh_token[0])
      return 1;

   char post_body[1024];
   char enc_refresh[768];
   url_encode(tokens->refresh_token, enc_refresh, sizeof(enc_refresh));

   int off = snprintf(post_body, sizeof(post_body), "grant_type=refresh_token&refresh_token=%s",
                      enc_refresh);
   if (off < 0 || (size_t)off >= sizeof(post_body)) {
      sodium_memzero(enc_refresh, sizeof(enc_refresh));
      return 1;
   }

   /* Body creds only for non-Basic providers; Basic providers (Schwab) set them
    * via CURLOPT_USERNAME/PASSWORD below. */
   if (!provider->token_auth_basic) {
      char enc_client_id[256], enc_client_secret[512];
      url_encode(provider->client_id, enc_client_id, sizeof(enc_client_id));
      url_encode(provider->client_secret, enc_client_secret, sizeof(enc_client_secret));
      int n = snprintf(post_body + off, sizeof(post_body) - (size_t)off,
                       "&client_id=%s&client_secret=%s", enc_client_id, enc_client_secret);
      sodium_memzero(enc_client_secret, sizeof(enc_client_secret));
      if (n < 0 || (size_t)n >= sizeof(post_body) - (size_t)off) {
         sodium_memzero(post_body, sizeof(post_body));
         sodium_memzero(enc_refresh, sizeof(enc_refresh));
         return 1;
      }
      off += n;
   }

   CURL *curl = curl_easy_init();
   if (!curl)
      return 1;

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, OAUTH_MAX_RESPONSE_SIZE);
   curl_easy_setopt(curl, CURLOPT_URL, provider->token_endpoint);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
   if (provider->token_auth_basic) {
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
      curl_easy_setopt(curl, CURLOPT_USERNAME, provider->client_id);
      curl_easy_setopt(curl, CURLOPT_PASSWORD, provider->client_secret);
   }
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)OAUTH_CURL_TIMEOUT);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");

   CURLcode cres = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   sodium_memzero(post_body, sizeof(post_body));
   sodium_memzero(enc_refresh, sizeof(enc_refresh));

   if (cres != CURLE_OK || http_code != 200 || resp.truncated) {
      if (resp.truncated) {
         OLOG_ERROR("oauth: refresh response exceeded %d byte cap; rejecting",
                    OAUTH_MAX_RESPONSE_SIZE);
      } else {
         OLOG_ERROR("oauth: refresh failed (HTTP %ld, curl %d)", http_code, cres);
      }
      if (resp.data) {
         /* Log up to 512 bytes for diagnostics on HTTP failure (skip on
          * truncated: payload may be a corrupted partial response). */
         if (!resp.truncated) {
            OLOG_ERROR("oauth: server response: %.*s", (int)(resp.size < 512 ? resp.size : 512),
                       resp.data);
         }
         /* Detect a dead refresh token so downstream tool wrappers can surface a
          * "re-link this account" message instead of a generic "network error".
          * Google returns "invalid_grant" when the token is revoked; Schwab
          * returns "invalid_client" when its fixed-lifetime (7-day) refresh
          * token lapses.  Substring match is acceptable here because the token
          * endpoint response is server-controlled JSON already capped at 1 MB. */
         if (!resp.truncated && http_code == 400 && resp.size > 0 &&
             (strstr(resp.data, "\"invalid_grant\"") != NULL ||
              strstr(resp.data, "\"invalid_client\"") != NULL)) {
            OLOG_ERROR("oauth: refresh token rejected by provider "
                       "(invalid_grant/invalid_client). User must re-link account.");
            oauth_set_last_revoked(1, NULL);
         }
         sodium_memzero(resp.data, resp.size);
      }
      curl_buffer_free(&resp);
      return 1;
   }

   json_object *root = json_tokener_parse(resp.data);
   sodium_memzero(resp.data, resp.size);
   curl_buffer_free(&resp);
   if (!root) {
      OLOG_ERROR("oauth: failed to parse refresh response");
      return 1;
   }

   json_object *j_access, *j_expires, *j_refresh;
   if (json_object_object_get_ex(root, "access_token", &j_access))
      snprintf(tokens->access_token, sizeof(tokens->access_token), "%s",
               json_object_get_string(j_access));
   if (json_object_object_get_ex(root, "expires_in", &j_expires))
      tokens->expires_at = time(NULL) + json_object_get_int64(j_expires);
   /* Google may return a new refresh token */
   if (json_object_object_get_ex(root, "refresh_token", &j_refresh))
      snprintf(tokens->refresh_token, sizeof(tokens->refresh_token), "%s",
               json_object_get_string(j_refresh));

   json_object_put(root);
   return 0;
}

/* =============================================================================
 * Token Serialization (JSON for encryption)
 * ============================================================================= */

static int tokens_to_json(const oauth_token_set_t *tokens, char *buf, size_t buf_len) {
   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "access_token", json_object_new_string(tokens->access_token));
   json_object_object_add(obj, "refresh_token", json_object_new_string(tokens->refresh_token));
   json_object_object_add(obj, "expires_at", json_object_new_int64(tokens->expires_at));
   json_object_object_add(obj, "linked_at", json_object_new_int64(tokens->linked_at));
   json_object_object_add(obj, "scopes", json_object_new_string(tokens->scopes));
   json_object_object_add(obj, "email", json_object_new_string(tokens->email));

   const char *json_str = json_object_to_json_string(obj);
   int written = snprintf(buf, buf_len, "%s", json_str);
   json_object_put(obj);

   return (written >= 0 && (size_t)written < buf_len) ? 0 : 1;
}

static int json_to_tokens(const char *json_str, size_t len, oauth_token_set_t *tokens) {
   /* Parse with explicit length since decrypted buffer may not be NUL-terminated */
   json_tokener *tok = json_tokener_new();
   json_object *root = json_tokener_parse_ex(tok, json_str, (int)len);
   json_tokener_free(tok);
   if (!root)
      return 1;

   memset(tokens, 0, sizeof(*tokens));

   json_object *j;
   if (json_object_object_get_ex(root, "access_token", &j))
      snprintf(tokens->access_token, sizeof(tokens->access_token), "%s", json_object_get_string(j));
   if (json_object_object_get_ex(root, "refresh_token", &j))
      snprintf(tokens->refresh_token, sizeof(tokens->refresh_token), "%s",
               json_object_get_string(j));
   if (json_object_object_get_ex(root, "expires_at", &j))
      tokens->expires_at = json_object_get_int64(j);
   if (json_object_object_get_ex(root, "linked_at", &j))
      tokens->linked_at = json_object_get_int64(j);
   if (json_object_object_get_ex(root, "scopes", &j))
      snprintf(tokens->scopes, sizeof(tokens->scopes), "%s", json_object_get_string(j));
   if (json_object_object_get_ex(root, "email", &j))
      snprintf(tokens->email, sizeof(tokens->email), "%s", json_object_get_string(j));

   json_object_put(root);
   return 0;
}

/* =============================================================================
 * Encrypted DB Persistence
 * ============================================================================= */

int oauth_store_tokens(int user_id,
                       const char *provider,
                       const char *account_key,
                       const oauth_token_set_t *tokens) {
   if (!provider || !account_key || !tokens)
      return 1;

   /* Serialize to JSON */
   char json_buf[4096];
   if (tokens_to_json(tokens, json_buf, sizeof(json_buf)) != 0)
      return 1;

   /* Encrypt */
   size_t pt_len = strlen(json_buf);
   size_t enc_max = pt_len + 64; /* nonce + MAC overhead */
   unsigned char *encrypted = malloc(enc_max);
   if (!encrypted)
      return 1;

   size_t enc_len = 0;
   if (crypto_store_encrypt(json_buf, pt_len, encrypted, enc_max, &enc_len) != 0) {
      free(encrypted);
      sodium_memzero(json_buf, sizeof(json_buf));
      return 1;
   }
   sodium_memzero(json_buf, sizeof(json_buf));

   /* Store in DB */
   AUTH_DB_LOCK_OR_RETURN(1);

   sqlite3_stmt *st = s_db.stmt_oauth_store;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, account_key, -1, SQLITE_STATIC);
   sqlite3_bind_blob(st, 4, encrypted, (int)enc_len, SQLITE_STATIC);
   sqlite3_bind_int(st, 5, (int)enc_len);
   sqlite3_bind_text(st, 6, tokens->scopes, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 7, (int64_t)time(NULL));
   sqlite3_bind_int64(st, 8, (int64_t)time(NULL));

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   if (result != 0)
      OLOG_ERROR("oauth: failed to store tokens: %s", sqlite3_errmsg(s_db.db));
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   free(encrypted);
   return result;
}

int oauth_load_tokens(int user_id,
                      const char *provider,
                      const char *account_key,
                      oauth_token_set_t *out) {
   if (!provider || !account_key || !out)
      return 1;

   memset(out, 0, sizeof(*out));

   AUTH_DB_LOCK_OR_RETURN(1);

   sqlite3_stmt *st = s_db.stmt_oauth_load;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, account_key, -1, SQLITE_STATIC);

   int result = 1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const void *blob = sqlite3_column_blob(st, 0);
      int blob_len = sqlite3_column_bytes(st, 0);

      if (blob && blob_len > 0) {
         /* Decrypt */
         char decrypted[4096];
         size_t dec_len = 0;
         if (crypto_store_decrypt((const unsigned char *)blob, (size_t)blob_len, decrypted,
                                  sizeof(decrypted) - 1, &dec_len) == 0) {
            decrypted[dec_len] = '\0';
            if (json_to_tokens(decrypted, dec_len, out) == 0)
               result = 0;
            sodium_memzero(decrypted, sizeof(decrypted));
         }
      }
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int oauth_delete_tokens(int user_id, const char *provider, const char *account_key) {
   if (!provider || !account_key)
      return 1;

   AUTH_DB_LOCK_OR_RETURN(1);

   sqlite3_stmt *st = s_db.stmt_oauth_delete;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, account_key, -1, SQLITE_STATIC);

   int result = (sqlite3_step(st) == SQLITE_DONE) ? 0 : 1;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

bool oauth_has_tokens(int user_id, const char *provider, const char *account_key) {
   if (!provider || !account_key)
      return false;

   AUTH_DB_LOCK_OR_RETURN(false);

   sqlite3_stmt *st = s_db.stmt_oauth_exists;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, account_key, -1, SQLITE_STATIC);

   bool exists = false;
   if (sqlite3_step(st) == SQLITE_ROW)
      exists = sqlite3_column_int(st, 0) > 0;
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return exists;
}

/* =============================================================================
 * Get Valid Access Token (auto-refresh)
 * ============================================================================= */

int oauth_get_access_token(const oauth_provider_config_t *provider,
                           int user_id,
                           const char *account_key,
                           char *token_buf,
                           size_t token_len) {
   if (!provider || !account_key || !token_buf)
      return 1;

   /* Defense in depth: clear the per-thread revoked flag at entry.
    * oauth_refresh() already resets it on every refresh attempt, but
    * this function may return successfully via the cached-token path
    * WITHOUT calling oauth_refresh — in which case a stale "revoked"
    * stamp from a prior account's failure could linger on the thread
    * and trip a re-link prompt on a subsequent unrelated failure for
    * a different account.  Not currently exploitable (success paths
    * already clear it), but the cost is two writes and the safety is
    * absolute. */
   oauth_set_last_revoked(0, NULL);

   /* Per-account lock to avoid thundering herd */
   unsigned int midx = acct_mutex_index(account_key);
   pthread_mutex_lock(&s_acct_mutexes[midx]);

   /* Load tokens from DB (another thread may have refreshed) */
   oauth_token_set_t tokens;
   if (oauth_load_tokens(user_id, provider->provider, account_key, &tokens) != 0) {
      pthread_mutex_unlock(&s_acct_mutexes[midx]);
      return 1;
   }

   /* Check if refresh needed (per-account mutex prevents concurrent refresh) */
   time_t now = time(NULL);
   if (tokens.expires_at > 0 && (tokens.expires_at - now) < OAUTH_REFRESH_MARGIN_SEC) {
      if (oauth_refresh(provider, &tokens) != 0) {
         OLOG_ERROR("oauth: refresh failed for account '%s'", account_key);
         /* If oauth_refresh detected invalid_grant, stamp the account_key
          * onto the thread-local so downstream wrappers can quote it
          * back to the user via oauth_was_last_refresh_revoked(). */
         if (oauth_was_last_refresh_revoked(NULL, 0)) {
            oauth_set_last_revoked(1, account_key);
         }
         sodium_memzero(&tokens, sizeof(tokens));
         pthread_mutex_unlock(&s_acct_mutexes[midx]);
         return 1;
      }

      /* Store refreshed tokens */
      oauth_store_tokens(user_id, provider->provider, account_key, &tokens);
      OLOG_INFO("oauth: refreshed token for '%s'", account_key);
   }

   snprintf(token_buf, token_len, "%s", tokens.access_token);
   sodium_memzero(&tokens, sizeof(tokens));
   pthread_mutex_unlock(&s_acct_mutexes[midx]);
   return 0;
}

/* =============================================================================
 * Revoke and Delete
 * ============================================================================= */

int oauth_revoke_and_delete(const oauth_provider_config_t *provider,
                            int user_id,
                            const char *account_key) {
   if (!provider || !account_key)
      return 1;

   /* Load tokens for revocation. Skip the HTTP revoke entirely for providers
    * with no revocation endpoint (e.g. Schwab) — local deletion below still runs. */
   oauth_token_set_t tokens;
   if (provider->revoke_endpoint[0] &&
       oauth_load_tokens(user_id, provider->provider, account_key, &tokens) == 0 &&
       (tokens.refresh_token[0] || tokens.access_token[0])) {
      /* Revoke refresh token (invalidates both); fall back to access token */
      char post_body[2560];
      const char *revoke_token = tokens.refresh_token[0] ? tokens.refresh_token
                                                         : tokens.access_token;
      char enc_revoke[2048];
      url_encode(revoke_token, enc_revoke, sizeof(enc_revoke));
      snprintf(post_body, sizeof(post_body), "token=%s", enc_revoke);
      sodium_memzero(enc_revoke, sizeof(enc_revoke));

      CURL *curl = curl_easy_init();
      if (curl) {
         curl_buffer_t resp;
         curl_buffer_init_with_max(&resp, OAUTH_MAX_RESPONSE_SIZE);
         curl_easy_setopt(curl, CURLOPT_URL, provider->revoke_endpoint);
         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
         curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
         curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
         curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)OAUTH_CURL_TIMEOUT);
         curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
         curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
         DAWN_CURL_SET_PROTOCOLS(curl, "https");

         CURLcode cres = curl_easy_perform(curl);
         long http_code = 0;
         curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
         curl_easy_cleanup(curl);
         if (resp.data) {
            sodium_memzero(resp.data, resp.size);
         }
         curl_buffer_free(&resp);

         /* Revocation responses are bodyless on success — no need to
          * gate on resp.truncated for diagnostics here.  We log the
          * outcome based on HTTP status only and always proceed to
          * local deletion. */
         if (cres != CURLE_OK || http_code != 200) {
            OLOG_WARNING("oauth: revocation failed (HTTP %ld) — delete locally anyway. "
                         "User can revoke manually at myaccount.google.com/permissions",
                         http_code);
         } else {
            OLOG_INFO("oauth: revoked token at provider for '%s'", account_key);
         }
      }

      sodium_memzero(post_body, sizeof(post_body));
      sodium_memzero(&tokens, sizeof(tokens));
   }

   /* Always delete locally */
   return oauth_delete_tokens(user_id, provider->provider, account_key);
}

/* =============================================================================
 * Scope Merging
 * ============================================================================= */

int oauth_merge_scopes(const char *existing, const char *requested, char *out, size_t out_len) {
   if (!out || out_len < 2)
      return 1;
   out[0] = '\0';

   /* Collect unique scopes from both inputs */
   const char *all_scopes[32];
   int count = 0;

   /* Parse existing scopes */
   char buf1[1024] = { 0 };
   if (existing && existing[0]) {
      snprintf(buf1, sizeof(buf1), "%s", existing);
      char *saveptr = NULL;
      char *tok = strtok_r(buf1, " ", &saveptr);
      while (tok && count < 32) {
         if (tok[0])
            all_scopes[count++] = tok;
         tok = strtok_r(NULL, " ", &saveptr);
      }
   }

   /* Parse requested scopes, skipping duplicates */
   char buf2[1024] = { 0 };
   if (requested && requested[0]) {
      snprintf(buf2, sizeof(buf2), "%s", requested);
      char *saveptr = NULL;
      char *tok = strtok_r(buf2, " ", &saveptr);
      while (tok && count < 32) {
         if (tok[0]) {
            bool dup = false;
            for (int i = 0; i < count; i++) {
               if (strcmp(all_scopes[i], tok) == 0) {
                  dup = true;
                  break;
               }
            }
            if (!dup)
               all_scopes[count++] = tok;
         }
         tok = strtok_r(NULL, " ", &saveptr);
      }
   }

   /* Join with spaces */
   size_t pos = 0;
   for (int i = 0; i < count && pos < out_len - 1; i++) {
      if (i > 0 && pos < out_len - 1)
         out[pos++] = ' ';
      size_t slen = strlen(all_scopes[i]);
      if (pos + slen >= out_len)
         break;
      memcpy(out + pos, all_scopes[i], slen);
      pos += slen;
   }
   out[pos] = '\0';
   return 0;
}

/* =============================================================================
 * Scope Checking
 * ============================================================================= */

bool oauth_has_scopes(const char *stored_scopes, const char *required_scopes) {
   if (!required_scopes || !required_scopes[0])
      return true;
   if (!stored_scopes || !stored_scopes[0])
      return false;

   /* Tokenize stored_scopes once into an array for O(n+m) lookup */
   char stored_buf[1024];
   snprintf(stored_buf, sizeof(stored_buf), "%s", stored_scopes);

   const char *stored_tokens[32];
   int stored_count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(stored_buf, " ", &saveptr);
   while (tok && stored_count < 32) {
      if (tok[0] != '\0')
         stored_tokens[stored_count++] = tok;
      tok = strtok_r(NULL, " ", &saveptr);
   }

   /* Check each required scope against pre-parsed stored tokens */
   char req_buf[1024];
   snprintf(req_buf, sizeof(req_buf), "%s", required_scopes);

   char *saveptr2 = NULL;
   char *req_token = strtok_r(req_buf, " ", &saveptr2);
   while (req_token) {
      if (req_token[0] != '\0') {
         bool found = false;
         for (int i = 0; i < stored_count; i++) {
            if (strcmp(stored_tokens[i], req_token) == 0) {
               found = true;
               break;
            }
         }
         if (!found)
            return false;
      }
      req_token = strtok_r(NULL, " ", &saveptr2);
   }
   return true;
}

/* =============================================================================
 * Account Listing
 * ============================================================================= */

int oauth_list_accounts(int user_id, const char *provider, char accounts[][256], int max_accounts) {
   if (!provider || !accounts || max_accounts <= 0)
      return 0;

   AUTH_DB_LOCK_OR_RETURN(0);

   sqlite3_stmt *st = s_db.stmt_oauth_list_accounts;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);

   int count = 0;
   while (sqlite3_step(st) == SQLITE_ROW && count < max_accounts) {
      const char *key = (const char *)sqlite3_column_text(st, 0);
      if (key) {
         snprintf(accounts[count], 256, "%s", key);
         count++;
      }
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return count;
}
