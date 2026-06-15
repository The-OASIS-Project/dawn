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
 * HTTP+SSE concrete transport for the MCP bridge. A reader thread owns a
 * long-lived SSE GET (driven by curl_multi_poll, never busy-looping); the
 * first `endpoint` event yields the URL that send() POSTs JSON-RPC requests
 * to. Server responses arrive asynchronously over the SSE stream.
 */

#define _GNU_SOURCE /* explicit_bzero */

#include "tools/mcp_transport_http_sse.h"

#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "core/curl_buffer.h"
#include "dawn_error.h"
#include "llm/sse_parser.h"
#include "logging.h"
#include "tools/mcp_transport.h"

#define MCP_SSE_USER_AGENT "DAWN-MCP/1.0"
#define MCP_SSE_POLL_TIMEOUT_MS 1000
#define MCP_SSE_POST_RESP_MAX (64 * 1024)
#define MCP_SSE_DEFAULT_POST_TIMEOUT 30L

/** Concrete HTTP+SSE transport. `base` MUST be first (see mcp_transport.h). */
typedef struct {
   mcp_transport_t base;

   /* Configuration (owned copies). */
   char *url;
   char *auth_header; /* "Authorization: Bearer <token>" or NULL. */
   bool tls_verify;
   long connect_timeout_seconds;
   long idle_close_seconds;
   mcp_transport_on_message_fn on_message;
   mcp_transport_on_state_fn on_state;
   void *user;

   /* SSE inbound. */
   struct curl_slist *sse_headers;
   sse_parser_t *parser;
   CURLM *multi;
   pthread_t reader;
   bool reader_running;
   volatile sig_atomic_t stop_flag;

   /* POST endpoint, published by the reader once the `endpoint` event lands. */
   pthread_mutex_t ep_mtx;
   pthread_cond_t ep_cv;
   char *endpoint_url;
   bool connected;
} mcp_http_sse_t;

/* Forward declarations for the vtable + internal use. */
static int http_sse_start(mcp_transport_t *base);
static int http_sse_send(mcp_transport_t *base, const char *json, size_t len);
static void http_sse_stop(mcp_transport_t *base);
static void http_sse_destroy(mcp_transport_t *base);

static const mcp_transport_vtable_t s_http_sse_vtable = {
   .name = "http+sse",
   .start = http_sse_start,
   .send = http_sse_send,
   .stop = http_sse_stop,
   .destroy = http_sse_destroy,
};

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/**
 * Resolve the server-advertised endpoint (often a path like
 * "/messages?sessionId=...") against the SSE base URL into an absolute URL.
 * Uses libcurl's RFC 3986 relative-resolution. Caller frees the result.
 */
/* True if the two CURLU handles share scheme + host + (effective) port. */
static bool curlu_same_origin(CURLU *a, CURLU *b) {
   char *as = NULL, *bs = NULL, *ah = NULL, *bh = NULL, *ap = NULL, *bp = NULL;
   bool same = false;
   if (curl_url_get(a, CURLUPART_SCHEME, &as, 0) == CURLUE_OK &&
       curl_url_get(b, CURLUPART_SCHEME, &bs, 0) == CURLUE_OK &&
       curl_url_get(a, CURLUPART_HOST, &ah, 0) == CURLUE_OK &&
       curl_url_get(b, CURLUPART_HOST, &bh, 0) == CURLUE_OK &&
       curl_url_get(a, CURLUPART_PORT, &ap, CURLU_DEFAULT_PORT) == CURLUE_OK &&
       curl_url_get(b, CURLUPART_PORT, &bp, CURLU_DEFAULT_PORT) == CURLUE_OK) {
      same = (strcasecmp(as, bs) == 0 && strcasecmp(ah, bh) == 0 && strcmp(ap, bp) == 0);
   }
   curl_free(as);
   curl_free(bs);
   curl_free(ah);
   curl_free(bh);
   curl_free(ap);
   curl_free(bp);
   return same;
}

static char *resolve_endpoint(const char *base_url, const char *endpoint_field) {
   CURLU *base = curl_url();
   CURLU *h = curl_url();
   if (base == NULL || h == NULL) {
      curl_url_cleanup(base);
      curl_url_cleanup(h);
      return NULL;
   }

   char *result = NULL;
   if (curl_url_set(base, CURLUPART_URL, base_url, 0) == CURLUE_OK &&
       curl_url_set(h, CURLUPART_URL, base_url, 0) == CURLUE_OK &&
       curl_url_set(h, CURLUPART_URL, endpoint_field, 0) == CURLUE_OK) {
      /* The `endpoint` event is server-controlled. curl resolves an absolute
       * endpoint by REPLACING scheme/host, so a malicious or MITM'd server could
       * point our authenticated POSTs (bearer Authorization header) at another
       * origin — SSRF / token exfiltration. Require the resolved endpoint to stay
       * same-origin as the operator-configured base URL, and reject embedded
       * credentials. */
      char *user = NULL;
      bool has_user = (curl_url_get(h, CURLUPART_USER, &user, 0) == CURLUE_OK && user != NULL);
      curl_free(user);
      if (has_user || !curlu_same_origin(base, h)) {
         OLOG_ERROR("MCP transport: rejecting cross-origin or credentialed SSE endpoint");
      } else {
         char *full = NULL;
         if (curl_url_get(h, CURLUPART_URL, &full, 0) == CURLUE_OK) {
            result = strdup(full);
            curl_free(full);
         }
      }
   }
   curl_url_cleanup(base);
   curl_url_cleanup(h);
   return result;
}

/* SSE write callback: feed raw bytes to the parser, which fires sse_event(). */
static size_t sse_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)userp;
   if (size != 0 && nmemb > SIZE_MAX / size) {
      return 0; /* overflow guard */
   }
   size_t total = size * nmemb;
   sse_parser_feed(t->parser, (const char *)contents, total);
   return total;
}

/* Parser callback: dispatch one complete SSE event. */
static void sse_event(const char *event_type, const char *event_data, void *userdata) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)userdata;

   if (event_type != NULL && strcmp(event_type, "endpoint") == 0) {
      char *ep = resolve_endpoint(t->url, event_data != NULL ? event_data : "");

      pthread_mutex_lock(&t->ep_mtx);
      free(t->endpoint_url);
      t->endpoint_url = ep;
      t->connected = (ep != NULL);
      pthread_cond_broadcast(&t->ep_cv);
      pthread_mutex_unlock(&t->ep_mtx);

      if (ep != NULL) {
         OLOG_INFO("MCP transport: endpoint ready");
         if (t->on_state != NULL) {
            t->on_state(t->user, MCP_TRANSPORT_CONNECTED);
         }
      } else {
         OLOG_ERROR("MCP transport: failed to resolve endpoint URL");
         if (t->on_state != NULL) {
            t->on_state(t->user, MCP_TRANSPORT_ERROR);
         }
      }
      return;
   }

   /* "message" (or a default unnamed event) carries a JSON-RPC payload. */
   if (event_data != NULL && (event_type == NULL || strcmp(event_type, "message") == 0)) {
      if (t->on_message != NULL) {
         t->on_message(t->user, event_data, strlen(event_data));
      }
   }
   /* Other events (e.g. "ping") are ignored. */
}

/* Apply the SSE-specific setopts to the long-lived GET handle. */
static void configure_sse_handle(mcp_http_sse_t *t, CURL *easy) {
   curl_easy_setopt(easy, CURLOPT_URL, t->url);
   DAWN_CURL_SET_PROTOCOLS(easy, "http,https");
   curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L); /* sec-H3: no redirects */
   curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
   curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, 60L);
   curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, 30L);
   curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, t->tls_verify ? 1L : 0L);
   curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, t->tls_verify ? 2L : 0L);
   curl_easy_setopt(easy, CURLOPT_USERAGENT, MCP_SSE_USER_AGENT);
   if (t->connect_timeout_seconds > 0) {
      curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, t->connect_timeout_seconds);
   }
   /* No overall CURLOPT_TIMEOUT: the SSE stream is intentionally long-lived. */
   curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sse_write_cb);
   curl_easy_setopt(easy, CURLOPT_WRITEDATA, t);
   curl_easy_setopt(easy, CURLOPT_HTTPHEADER, t->sse_headers);
}

/* --------------------------------------------------------------------------
 * Reader thread
 * -------------------------------------------------------------------------- */

static void *reader_thread(void *arg) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)arg;

   CURL *easy = curl_easy_init();
   if (easy == NULL) {
      OLOG_ERROR("MCP transport: curl_easy_init failed");
      if (t->on_state != NULL) {
         t->on_state(t->user, MCP_TRANSPORT_ERROR);
      }
      return NULL;
   }
   configure_sse_handle(t, easy);
   curl_multi_add_handle(t->multi, easy);

   int still_running = 0;
   do {
      CURLMcode mc = curl_multi_perform(t->multi, &still_running);
      if (mc != CURLM_OK) {
         OLOG_ERROR("MCP transport: curl_multi_perform: %s", curl_multi_strerror(mc));
         break;
      }
      if (still_running) {
         /* Blocks up to the timeout; curl_multi_wakeup() (from stop) breaks it. */
         mc = curl_multi_poll(t->multi, NULL, 0, MCP_SSE_POLL_TIMEOUT_MS, NULL);
         if (mc != CURLM_OK) {
            OLOG_ERROR("MCP transport: curl_multi_poll: %s", curl_multi_strerror(mc));
            break;
         }
      }
   } while (still_running != 0 && t->stop_flag == 0);

   curl_multi_remove_handle(t->multi, easy);
   curl_easy_cleanup(easy);

   pthread_mutex_lock(&t->ep_mtx);
   t->connected = false;
   pthread_cond_broadcast(&t->ep_cv); /* wake any sender waiting on connect */
   pthread_mutex_unlock(&t->ep_mtx);

   if (t->on_state != NULL) {
      t->on_state(t->user, MCP_TRANSPORT_DISCONNECTED);
   }
   return NULL;
}

/* --------------------------------------------------------------------------
 * vtable implementation
 * -------------------------------------------------------------------------- */

static int http_sse_start(mcp_transport_t *base) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)base;
   if (t->reader_running) {
      return SUCCESS;
   }

   t->stop_flag = 0;
   sse_parser_reset(t->parser);

   t->multi = curl_multi_init();
   if (t->multi == NULL) {
      OLOG_ERROR("MCP transport: curl_multi_init failed");
      return FAILURE;
   }

   if (pthread_create(&t->reader, NULL, reader_thread, t) != 0) {
      OLOG_ERROR("MCP transport: failed to start reader thread");
      curl_multi_cleanup(t->multi);
      t->multi = NULL;
      return FAILURE;
   }
   t->reader_running = true;
   return SUCCESS;
}

static int http_sse_send(mcp_transport_t *base, const char *json, size_t len) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)base;

   /* Snapshot the endpoint under lock; the client only sends after CONNECTED. */
   pthread_mutex_lock(&t->ep_mtx);
   char *endpoint = (t->connected && t->endpoint_url != NULL) ? strdup(t->endpoint_url) : NULL;
   pthread_mutex_unlock(&t->ep_mtx);

   if (endpoint == NULL) {
      OLOG_ERROR("MCP transport: send() before endpoint is ready");
      return FAILURE;
   }

   CURL *easy = curl_easy_init();
   if (easy == NULL) {
      free(endpoint);
      return FAILURE;
   }

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, MCP_SSE_POST_RESP_MAX);

   struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
   if (t->auth_header != NULL) {
      hdrs = curl_slist_append(hdrs, t->auth_header);
   }

   long timeout = t->connect_timeout_seconds > 0 ? t->connect_timeout_seconds
                                                 : MCP_SSE_DEFAULT_POST_TIMEOUT;
   curl_easy_setopt(easy, CURLOPT_URL, endpoint);
   DAWN_CURL_SET_PROTOCOLS(easy, "http,https");
   curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L); /* sec-H3 */
   curl_apply_dawn_defaults(easy, MCP_SSE_USER_AGENT, timeout, &resp);
   /* Override TLS verification per config (dev_mode may disable it). */
   curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, t->tls_verify ? 1L : 0L);
   curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, t->tls_verify ? 2L : 0L);
   curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
   curl_easy_setopt(easy, CURLOPT_POST, 1L);
   curl_easy_setopt(easy, CURLOPT_POSTFIELDS, json);
   curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)len);

   CURLcode rc = curl_easy_perform(easy);
   long http_code = 0;
   curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

   int ret = (rc == CURLE_OK && http_code >= 200 && http_code < 300) ? SUCCESS : FAILURE;
   if (ret != SUCCESS) {
      OLOG_ERROR("MCP transport: POST failed (curl: %s, http: %ld)", curl_easy_strerror(rc),
                 http_code);
   }

   curl_slist_free_all(hdrs);
   curl_buffer_free(&resp);
   curl_easy_cleanup(easy);
   free(endpoint);
   return ret;
}

static void http_sse_stop(mcp_transport_t *base) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)base;
   if (!t->reader_running) {
      return;
   }

   t->stop_flag = 1;
   if (t->multi != NULL) {
      curl_multi_wakeup(t->multi); /* break curl_multi_poll immediately */
   }
   pthread_join(t->reader, NULL);
   t->reader_running = false;

   if (t->multi != NULL) {
      curl_multi_cleanup(t->multi);
      t->multi = NULL;
   }

   pthread_mutex_lock(&t->ep_mtx);
   free(t->endpoint_url);
   t->endpoint_url = NULL;
   t->connected = false;
   pthread_mutex_unlock(&t->ep_mtx);
}

static void http_sse_destroy(mcp_transport_t *base) {
   mcp_http_sse_t *t = (mcp_http_sse_t *)base;
   if (t == NULL) {
      return;
   }

   http_sse_stop(base); /* ensure the reader thread is joined */

   if (t->parser != NULL) {
      sse_parser_free(t->parser);
   }
   if (t->sse_headers != NULL) {
      curl_slist_free_all(t->sse_headers);
   }
   if (t->auth_header != NULL) {
      /* Best-effort: scrub our copy of the bearer token. The definitive
       * secrets-buffer scrub happens upstream where the token is read. */
      explicit_bzero(t->auth_header, strlen(t->auth_header));
      free(t->auth_header);
   }
   free(t->url);
   free(t->endpoint_url);
   pthread_mutex_destroy(&t->ep_mtx);
   pthread_cond_destroy(&t->ep_cv);
   free(t);
}

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */

mcp_transport_t *mcp_transport_http_sse_create(const mcp_transport_opts_t *opts) {
   if (opts == NULL || opts->url == NULL || opts->on_message == NULL) {
      OLOG_ERROR("MCP transport: create requires url and on_message");
      return NULL;
   }

   mcp_http_sse_t *t = calloc(1, sizeof(*t));
   if (t == NULL) {
      return NULL;
   }

   t->base.vt = &s_http_sse_vtable;
   t->tls_verify = opts->tls_verify;
   t->connect_timeout_seconds = opts->connect_timeout_seconds;
   t->idle_close_seconds = opts->idle_close_seconds;
   t->on_message = opts->on_message;
   t->on_state = opts->on_state;
   t->user = opts->user;
   pthread_mutex_init(&t->ep_mtx, NULL);
   pthread_cond_init(&t->ep_cv, NULL);

   t->url = strdup(opts->url);
   t->parser = sse_parser_create(sse_event, t);
   if (t->url == NULL || t->parser == NULL) {
      http_sse_destroy((mcp_transport_t *)t);
      return NULL;
   }

   t->sse_headers = curl_slist_append(NULL, "Accept: text/event-stream");
   t->sse_headers = curl_slist_append(t->sse_headers, "Cache-Control: no-cache");
   if (opts->bearer_token != NULL && opts->bearer_token[0] != '\0') {
      size_t n = strlen("Authorization: Bearer ") + strlen(opts->bearer_token) + 1;
      t->auth_header = malloc(n);
      if (t->auth_header == NULL) {
         /* A token was explicitly requested; failing silently would start an
          * unauthenticated connection that the server rejects (sec-S8). */
         OLOG_ERROR("MCP transport: failed to allocate auth header");
         http_sse_destroy((mcp_transport_t *)t);
         return NULL;
      }
      snprintf(t->auth_header, n, "Authorization: Bearer %s", opts->bearer_token);
      t->sse_headers = curl_slist_append(t->sse_headers, t->auth_header);
   }

   return (mcp_transport_t *)t;
}
