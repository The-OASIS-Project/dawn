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
 * Stateless streamable-HTTP transport: one POST per JSON-RPC message, the
 * reply in the POST's JSON body.  The peer may be on another machine, so the
 * reply is treated as untrusted: size-capped, JSON only, and accepted only
 * as the answer to the request just sent (its id must match).
 */

#define _GNU_SOURCE /* explicit_bzero */

#include "tools/mcp_transport_http.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/curl_buffer.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/mcp_transport.h"
#include "utils/string_utils.h"

#define MCP_HTTP_USER_AGENT "DAWN-MCP/1.0"
#define MCP_HTTP_DEFAULT_TIMEOUT 30L
#define MCP_HTTP_VERSION_MAX 32

/** Concrete transport. `base` MUST be first (see mcp_transport.h). */
typedef struct {
   mcp_transport_t base;
   char *url;
   char *auth_header; /* "Authorization: Bearer <token>", or NULL */
   bool tls_verify;
   long timeout_seconds;
   mcp_transport_on_message_fn on_message;
   mcp_transport_on_state_fn on_state;
   void *user;
   /* stop() sets `stopped` (sends refuse, POSTs in flight abort) until the
    * next start(), and bumps `gen` so a POST begun before a stop+start still
    * aborts: a stop can't be undone under a POST that was already running. */
   atomic_bool stopped;
   atomic_uint gen;
   pthread_mutex_t mtx; /* protocol_version */
   char protocol_version[MCP_HTTP_VERSION_MAX];
} mcp_http_t;

/** One POST's identity, for the progress callback. */
typedef struct {
   mcp_http_t *t;
   unsigned gen;
} mcp_http_call_t;

static const mcp_transport_vtable_t s_http_vtable;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/** The request's JSON-RPC id and method; false for a notification (no id). */
static bool request_info(const char *json, int64_t *id, char *method, size_t method_len) {
   struct json_object *root = json_tokener_parse(json);
   if (root == NULL) {
      return false;
   }
   struct json_object *o = NULL;
   bool has_id = json_object_object_get_ex(root, "id", &o) && json_object_is_type(o, json_type_int);
   *id = has_id ? json_object_get_int64(o) : 0;
   method[0] = '\0';
   if (json_object_object_get_ex(root, "method", &o) && json_object_is_type(o, json_type_string)) {
      safe_strncpy(method, json_object_get_string(o), method_len);
   }
   json_object_put(root);
   return has_id;
}

/** True if the reply is a JSON-RPC message answering request `id`. Remembers
 *  the protocol version an initialize reply names, for later requests. */
static bool reply_matches(mcp_http_t *t, const char *body, int64_t id, const char *method) {
   struct json_object *root = json_tokener_parse(body);
   if (root == NULL || !json_object_is_type(root, json_type_object)) {
      json_object_put(root);
      return false;
   }
   struct json_object *o = NULL;
   bool ok = json_object_object_get_ex(root, "id", &o) && json_object_is_type(o, json_type_int) &&
             json_object_get_int64(o) == id;

   struct json_object *res = NULL;
   if (ok && strcmp(method, "initialize") == 0 && json_object_object_get_ex(root, "result", &res) &&
       json_object_object_get_ex(res, "protocolVersion", &o) &&
       json_object_is_type(o, json_type_string)) {
      const char *v = json_object_get_string(o);
      /* A header value: dates like 2025-11-25 only. */
      if (strlen(v) < MCP_HTTP_VERSION_MAX && strspn(v, "0123456789-") == strlen(v)) {
         pthread_mutex_lock(&t->mtx);
         safe_strscpy(t->protocol_version, v);
         pthread_mutex_unlock(&t->mtx);
      }
   }
   json_object_put(root);
   return ok;
}

/** libcurl progress callback (about once a second): abort a stopped POST. */
static int abort_if_stopped(void *clientp,
                            curl_off_t dltotal,
                            curl_off_t dlnow,
                            curl_off_t ultotal,
                            curl_off_t ulnow) {
   (void)dltotal;
   (void)dlnow;
   (void)ultotal;
   (void)ulnow;
   const mcp_http_call_t *call = clientp;
   return atomic_load(&call->t->stopped) || atomic_load(&call->t->gen) != call->gen;
}

/** True unless the URL's host is this machine; reports whether it's plain http. */
static bool url_leaves_host(const char *url, bool *plain_http) {
   CURLU *u = curl_url();
   *plain_http = true;
   if (u == NULL || curl_url_set(u, CURLUPART_URL, url, 0) != CURLUE_OK) {
      curl_url_cleanup(u);
      return true; /* unparseable: assume the worst */
   }
   char *scheme = NULL;
   char *host = NULL;
   curl_url_get(u, CURLUPART_SCHEME, &scheme, 0);
   curl_url_get(u, CURLUPART_HOST, &host, 0);
   *plain_http = scheme == NULL || strcasecmp(scheme, "https") != 0;
   bool leaves = host == NULL || (strcmp(host, "127.0.0.1") != 0 &&
                                  strcasecmp(host, "localhost") != 0 && strcmp(host, "[::1]") != 0);
   curl_free(scheme);
   curl_free(host);
   curl_url_cleanup(u);
   return leaves;
}

/** Request headers; the caller frees the list. */
static struct curl_slist *request_headers(mcp_http_t *t, const char *method) {
   struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
   /* The spec has clients accept both; a reply that streams is refused below. */
   h = curl_slist_append(h, "Accept: application/json, text/event-stream");
   if (t->auth_header != NULL) {
      h = curl_slist_append(h, t->auth_header);
   }
   /* Not on initialize: that's where the server says which version it speaks now. */
   pthread_mutex_lock(&t->mtx);
   if (t->protocol_version[0] != '\0' && strcmp(method, "initialize") != 0) {
      char v[64];
      snprintf(v, sizeof(v), "MCP-Protocol-Version: %s", t->protocol_version);
      h = curl_slist_append(h, v);
   }
   pthread_mutex_unlock(&t->mtx);
   return h;
}

/** Check a 2xx reply to request `id` and deliver it; SUCCESS if delivered. */
static int deliver_reply(mcp_http_t *t,
                         const curl_buffer_t *resp,
                         const char *ctype,
                         int64_t id,
                         const char *method) {
   if (resp->truncated || resp->data == NULL || strlen(resp->data) != resp->size) {
      OLOG_WARNING("MCP http: %s reply too large or not text; dropped", method);
   } else if (ctype != NULL && strncasecmp(ctype, "text/event-stream", 17) == 0) {
      OLOG_WARNING("MCP http: %s reply is a stream; this server needs transport \"http+sse\"",
                   method);
   } else if (ctype == NULL || strncasecmp(ctype, "application/json", 16) != 0) {
      OLOG_WARNING("MCP http: %s reply is not JSON (%s); dropped", method,
                   ctype ? ctype : "no type");
   } else if (!reply_matches(t, resp->data, id, method)) {
      OLOG_WARNING("MCP http: %s reply doesn't answer request %lld; dropped", method,
                   (long long)id);
   } else {
      t->on_message(t->user, resp->data, resp->size);
      return SUCCESS;
   }
   return FAILURE;
}

/* --------------------------------------------------------------------------
 * Vtable
 * -------------------------------------------------------------------------- */

static int http_start(mcp_transport_t *base) {
   mcp_http_t *t = (mcp_http_t *)base;
   atomic_store(&t->stopped, false);
   /* No stream to open: ready now. The client waits for this report. */
   if (t->on_state != NULL) {
      t->on_state(t->user, MCP_TRANSPORT_CONNECTED);
   }
   return SUCCESS;
}

static int http_send(mcp_transport_t *base, const char *json, size_t len) {
   mcp_http_t *t = (mcp_http_t *)base;
   mcp_http_call_t call = { t, atomic_load(&t->gen) };
   if (atomic_load(&t->stopped)) {
      return FAILURE; /* stopped and not restarted: send nothing */
   }
   int64_t id = 0;
   char method[64];
   bool has_id = request_info(json, &id, method, sizeof(method));

   CURL *easy = curl_easy_init();
   if (easy == NULL) {
      return FAILURE;
   }
   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, MCP_HTTP_REPLY_MAX);
   struct curl_slist *hdrs = request_headers(t, method);

   curl_easy_setopt(easy, CURLOPT_URL, t->url);
   DAWN_CURL_SET_PROTOCOLS(easy, "http,https");
   curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
   curl_apply_dawn_defaults(easy, MCP_HTTP_USER_AGENT, t->timeout_seconds, &resp);
   curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, t->tls_verify ? 1L : 0L);
   curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, t->tls_verify ? 2L : 0L);
   curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
   curl_easy_setopt(easy, CURLOPT_POST, 1L);
   curl_easy_setopt(easy, CURLOPT_POSTFIELDS, json);
   curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)len);
   curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
   curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abort_if_stopped);
   curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &call);

   CURLcode rc = curl_easy_perform(easy);
   long code = 0;
   char *ctype = NULL;
   curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
   curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &ctype);

   int ret = FAILURE;
   if (rc != CURLE_OK || code < 200 || code >= 300) {
      if (rc != CURLE_ABORTED_BY_CALLBACK) {
         OLOG_WARNING("MCP http: %s failed (curl: %s, http: %ld)", method[0] ? method : "POST",
                      curl_easy_strerror(rc), code);
      }
   } else if (!has_id) {
      ret = SUCCESS; /* a notification: the reply (202, no body) carries nothing */
   } else {
      ret = deliver_reply(t, &resp, ctype, id, method);
   }

   curl_slist_free_all(hdrs);
   curl_buffer_free(&resp);
   curl_easy_cleanup(easy);
   return ret;
}

static void http_stop(mcp_transport_t *base) {
   mcp_http_t *t = (mcp_http_t *)base;
   atomic_store(&t->stopped, true);
   atomic_fetch_add(&t->gen, 1);
}

static void http_destroy(mcp_transport_t *base) {
   mcp_http_t *t = (mcp_http_t *)base;
   if (t == NULL) {
      return;
   }
   if (t->auth_header != NULL) {
      explicit_bzero(t->auth_header, strlen(t->auth_header));
      free(t->auth_header);
   }
   free(t->url);
   pthread_mutex_destroy(&t->mtx);
   free(t);
}

static const mcp_transport_vtable_t s_http_vtable = {
   .name = "http",
   .start = http_start,
   .send = http_send,
   .stop = http_stop,
   .destroy = http_destroy,
};

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */

mcp_transport_t *mcp_transport_http_create(const mcp_transport_opts_t *opts) {
   if (opts == NULL || opts->url == NULL || opts->on_message == NULL) {
      OLOG_ERROR("MCP http: create requires url and on_message");
      return NULL;
   }
   bool token = opts->bearer_token != NULL && opts->bearer_token[0] != '\0';
   bool plain_http = true;
   /* A token leaves this machine only over verified TLS. */
   if (token && url_leaves_host(opts->url, &plain_http) && (plain_http || !opts->tls_verify)) {
      OLOG_ERROR("MCP http: refusing to send a bearer token to another host without verified "
                 "https (%s)",
                 opts->url);
      return NULL;
   }

   mcp_http_t *t = calloc(1, sizeof(*t));
   if (t == NULL) {
      return NULL;
   }
   t->base.vt = &s_http_vtable;
   t->tls_verify = opts->tls_verify;
   t->timeout_seconds = opts->connect_timeout_seconds > 0 ? opts->connect_timeout_seconds
                                                          : MCP_HTTP_DEFAULT_TIMEOUT;
   t->on_message = opts->on_message;
   t->on_state = opts->on_state;
   t->user = opts->user;
   atomic_init(&t->stopped, false);
   atomic_init(&t->gen, 0);
   pthread_mutex_init(&t->mtx, NULL);

   t->url = strdup(opts->url);
   if (t->url == NULL) {
      http_destroy((mcp_transport_t *)t);
      return NULL;
   }
   if (token) {
      size_t n = strlen("Authorization: Bearer ") + strlen(opts->bearer_token) + 1;
      t->auth_header = malloc(n);
      if (t->auth_header == NULL) {
         http_destroy((mcp_transport_t *)t);
         return NULL;
      }
      snprintf(t->auth_header, n, "Authorization: Bearer %s", opts->bearer_token);
   }
   return (mcp_transport_t *)t;
}
