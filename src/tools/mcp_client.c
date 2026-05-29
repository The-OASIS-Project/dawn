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
 * JSON-RPC client + lifecycle FSM for one MCP server. Owns a transport, a
 * lifecycle state machine, and a pending-request table keyed by JSON-RPC id.
 * Responses and notifications arrive on the transport's reader thread and are
 * matched to waiting caller threads via per-request condition variables.
 *
 * Lock order (outer -> inner), per the bridge design:
 *   state_mtx  ->  pending_mtx  ->  pending->mtx
 * A thread never acquires an outer lock while holding an inner one.
 */

#define _GNU_SOURCE

#include "tools/mcp_client.h"

#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dawn_error.h"
#include "logging.h"
#include "tools/mcp_transport.h"

#define MCP_PENDING_MAX 64
#define MCP_FAIL_WINDOW_SEC 60
#define MCP_FAIL_THRESHOLD 3
#define MCP_DEFAULT_TIMEOUT_MS 30000
#define MCP_DEFAULT_CONNECT_WAIT_MS 10000

/** One in-flight request awaiting a response. Address is stable until removed. */
typedef struct {
   uint64_t id;
   pthread_mutex_t mtx;
   pthread_cond_t cv; /* CLOCK_MONOTONIC */
   bool done;
   int status;   /* SUCCESS or MCP_ERR_* */
   char *result; /* malloc'd result/error JSON (ownership moves to caller). */
   mcp_progress_fn progress_cb;
   void *progress_user;
} mcp_pending_t;

struct mcp_client {
   char *name;
   mcp_transport_t *transport;
   long request_timeout_ms;
   long connect_wait_ms;
   char *client_name;
   char *client_version;

   /* Lifecycle state. */
   pthread_mutex_t state_mtx;
   pthread_cond_t state_cv; /* CLOCK_MONOTONIC */
   mcp_client_state_t state;
   bool transport_connected; /* endpoint event seen */
   int64_t fail_ts[MCP_FAIL_THRESHOLD];

   /* Pending table: pointers sorted ascending by id (ids are monotonic). */
   pthread_mutex_t pending_mtx;
   mcp_pending_t *pending[MCP_PENDING_MAX];
   int pending_count;
   _Atomic uint64_t next_id;

   /* tools/list cache. */
   char *tools_json;
};

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */

static int64_t mono_now_sec(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec;
}

static void deadline_in_ms(struct timespec *ts, long ms) {
   clock_gettime(CLOCK_MONOTONIC, ts);
   ts->tv_sec += ms / 1000;
   ts->tv_nsec += (ms % 1000) * 1000000L;
   if (ts->tv_nsec >= 1000000000L) {
      ts->tv_sec += 1;
      ts->tv_nsec -= 1000000000L;
   }
}

static void cond_init_monotonic(pthread_cond_t *cv) {
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(cv, &attr);
   pthread_condattr_destroy(&attr);
}

/* --------------------------------------------------------------------------
 * Pending table (all ops assume pending_mtx is held unless noted)
 * -------------------------------------------------------------------------- */

static mcp_pending_t *pending_add(mcp_client_t *c, uint64_t id, mcp_progress_fn cb, void *user) {
   if (c->pending_count >= MCP_PENDING_MAX) {
      return NULL;
   }
   mcp_pending_t *p = calloc(1, sizeof(*p));
   if (p == NULL) {
      return NULL;
   }
   p->id = id;
   p->status = MCP_ERR_TIMEOUT; /* default outcome if no response ever arrives */
   pthread_mutex_init(&p->mtx, NULL);
   cond_init_monotonic(&p->cv);
   p->progress_cb = cb;
   p->progress_user = user;
   c->pending[c->pending_count++] = p; /* monotonic ids => append keeps sorted */
   return p;
}

static int pending_index(mcp_client_t *c, uint64_t id) {
   int lo = 0;
   int hi = c->pending_count - 1;
   while (lo <= hi) {
      int mid = lo + (hi - lo) / 2;
      uint64_t mid_id = c->pending[mid]->id;
      if (mid_id == id) {
         return mid;
      }
      if (mid_id < id) {
         lo = mid + 1;
      } else {
         hi = mid - 1;
      }
   }
   return -1;
}

static void pending_remove(mcp_client_t *c, mcp_pending_t *p) {
   int idx = pending_index(c, p->id);
   if (idx < 0) {
      return;
   }
   for (int i = idx; i < c->pending_count - 1; i++) {
      c->pending[i] = c->pending[i + 1];
   }
   c->pending_count--;
}

static void pending_destroy(mcp_pending_t *p) {
   pthread_mutex_destroy(&p->mtx);
   pthread_cond_destroy(&p->cv);
   free(p->result);
   free(p);
}

/* Wake every pending request with a terminal status (used on drop/shutdown). */
static void fail_all_pending(mcp_client_t *c, int status) {
   pthread_mutex_lock(&c->pending_mtx);
   for (int i = 0; i < c->pending_count; i++) {
      mcp_pending_t *p = c->pending[i];
      pthread_mutex_lock(&p->mtx);
      if (!p->done) {
         p->done = true;
         p->status = status;
      }
      pthread_cond_signal(&p->cv);
      pthread_mutex_unlock(&p->mtx);
   }
   pthread_mutex_unlock(&c->pending_mtx);
}

/* --------------------------------------------------------------------------
 * Inbound message routing (runs on the transport reader thread)
 * -------------------------------------------------------------------------- */

static void deliver_response(mcp_client_t *c, uint64_t id, char *payload, int status) {
   pthread_mutex_lock(&c->pending_mtx);
   int idx = pending_index(c, id);
   if (idx >= 0) {
      mcp_pending_t *p = c->pending[idx];
      pthread_mutex_lock(&p->mtx);
      if (!p->done) {
         p->result = payload;
         p->status = status;
         p->done = true;
         payload = NULL; /* ownership transferred to the pending entry */
      }
      pthread_cond_signal(&p->cv);
      pthread_mutex_unlock(&p->mtx);
   }
   pthread_mutex_unlock(&c->pending_mtx);
   free(payload); /* no waiter, or already completed */
}

static void dispatch_progress(mcp_client_t *c, uint64_t token, int percent, const char *message) {
   mcp_progress_fn cb = NULL;
   void *user = NULL;
   pthread_mutex_lock(&c->pending_mtx);
   int idx = pending_index(c, token);
   if (idx >= 0) {
      cb = c->pending[idx]->progress_cb;
      user = c->pending[idx]->progress_user;
   }
   pthread_mutex_unlock(&c->pending_mtx);
   if (cb != NULL) {
      cb(user, percent, message);
   }
}

static void handle_progress_notification(mcp_client_t *c, struct json_object *params) {
   if (params == NULL) {
      return;
   }
   struct json_object *tok = NULL;
   struct json_object *prog = NULL;
   struct json_object *total = NULL;
   struct json_object *msg = NULL;
   uint64_t token = 0;
   double progress = 0.0;
   double total_val = 0.0;
   const char *message = NULL;

   if (json_object_object_get_ex(params, "progressToken", &tok)) {
      token = (uint64_t)json_object_get_int64(tok);
   }
   if (json_object_object_get_ex(params, "progress", &prog)) {
      progress = json_object_get_double(prog);
   }
   if (json_object_object_get_ex(params, "total", &total)) {
      total_val = json_object_get_double(total);
   }
   if (json_object_object_get_ex(params, "message", &msg)) {
      message = json_object_get_string(msg);
   }

   int percent = (total_val > 0.0) ? (int)((progress / total_val) * 100.0) : (int)progress;
   dispatch_progress(c, token, percent, message);
}

static void client_on_message(void *user, const char *json, size_t len) {
   mcp_client_t *c = (mcp_client_t *)user;
   (void)len;

   struct json_object *root = json_tokener_parse(json);
   if (root == NULL) {
      OLOG_WARNING("MCP %s: dropping unparseable message", c->name);
      return;
   }

   struct json_object *id_obj = NULL;
   bool has_id = json_object_object_get_ex(root, "id", &id_obj) &&
                 !json_object_is_type(id_obj, json_type_null);

   if (has_id) {
      uint64_t id = (uint64_t)json_object_get_int64(id_obj);
      struct json_object *res = NULL;
      struct json_object *err = NULL;
      if (json_object_object_get_ex(root, "error", &err)) {
         deliver_response(c, id, strdup(json_object_to_json_string(err)), MCP_ERR_RPC);
      } else if (json_object_object_get_ex(root, "result", &res)) {
         deliver_response(c, id, strdup(json_object_to_json_string(res)), SUCCESS);
      } else {
         deliver_response(c, id, strdup("{}"), SUCCESS);
      }
   } else {
      struct json_object *method_obj = NULL;
      if (json_object_object_get_ex(root, "method", &method_obj)) {
         const char *method = json_object_get_string(method_obj);
         if (method != NULL && strcmp(method, "notifications/progress") == 0) {
            struct json_object *params = NULL;
            json_object_object_get_ex(root, "params", &params);
            handle_progress_notification(c, params);
         }
         /* Other server notifications are ignored in Phase 1. */
      }
   }

   json_object_put(root);
}

static void client_on_state(void *user, mcp_transport_event_t ev) {
   mcp_client_t *c = (mcp_client_t *)user;

   pthread_mutex_lock(&c->state_mtx);
   if (ev == MCP_TRANSPORT_CONNECTED) {
      c->transport_connected = true;
   } else {
      c->transport_connected = false;
      if (c->state == MCP_STATE_CONNECTED) {
         c->state = MCP_STATE_DISCONNECTED;
      }
   }
   pthread_cond_broadcast(&c->state_cv);
   pthread_mutex_unlock(&c->state_mtx);

   if (ev != MCP_TRANSPORT_CONNECTED) {
      fail_all_pending(c, MCP_ERR_DISCONNECTED);
   }
}

/* --------------------------------------------------------------------------
 * Request / response
 * -------------------------------------------------------------------------- */

static int send_notification(mcp_client_t *c, const char *method, const char *params_json) {
   struct json_object *n = json_object_new_object();
   json_object_object_add(n, "jsonrpc", json_object_new_string("2.0"));
   json_object_object_add(n, "method", json_object_new_string(method));
   if (params_json != NULL) {
      struct json_object *p = json_tokener_parse(params_json);
      if (p != NULL) {
         json_object_object_add(n, "params", p);
      }
   }
   const char *body = json_object_to_json_string(n);
   int rc = mcp_transport_send(c->transport, body, strlen(body));
   json_object_put(n);
   return rc;
}

/* Build the request JSON, returning a json_object the caller must put(). */
static struct json_object *build_request(uint64_t id,
                                         const char *method,
                                         const char *params_json,
                                         bool with_progress) {
   struct json_object *req = json_object_new_object();
   json_object_object_add(req, "jsonrpc", json_object_new_string("2.0"));
   json_object_object_add(req, "id", json_object_new_int64((int64_t)id));
   json_object_object_add(req, "method", json_object_new_string(method));

   struct json_object *params = NULL;
   if (params_json != NULL) {
      params = json_tokener_parse(params_json);
   }
   if (with_progress) {
      if (params == NULL || !json_object_is_type(params, json_type_object)) {
         if (params != NULL) {
            json_object_put(params);
         }
         params = json_object_new_object();
      }
      struct json_object *meta = json_object_new_object();
      json_object_object_add(meta, "progressToken", json_object_new_int64((int64_t)id));
      json_object_object_add(params, "_meta", meta);
   }
   if (params != NULL) {
      json_object_object_add(req, "params", params);
   }
   return req;
}

static int do_request(mcp_client_t *c,
                      const char *method,
                      const char *params_json,
                      long timeout_ms,
                      mcp_progress_fn cb,
                      void *cb_user,
                      char **result_out) {
   if (result_out != NULL) {
      *result_out = NULL;
   }
   uint64_t id = atomic_fetch_add(&c->next_id, 1) + 1;

   struct json_object *req = build_request(id, method, params_json, cb != NULL);
   const char *body = json_object_to_json_string(req);

   pthread_mutex_lock(&c->pending_mtx);
   mcp_pending_t *p = pending_add(c, id, cb, cb_user);
   pthread_mutex_unlock(&c->pending_mtx);
   if (p == NULL) {
      OLOG_WARNING("MCP %s: pending table full (backpressure)", c->name);
      json_object_put(req);
      return MCP_ERR_BACKPRESSURE;
   }

   int sent = mcp_transport_send(c->transport, body, strlen(body));
   json_object_put(req);
   if (sent != SUCCESS) {
      pthread_mutex_lock(&c->pending_mtx);
      pending_remove(c, p);
      pthread_mutex_unlock(&c->pending_mtx);
      pending_destroy(p);
      return MCP_ERR_DISCONNECTED;
   }

   struct timespec deadline;
   deadline_in_ms(&deadline, timeout_ms > 0 ? timeout_ms : c->request_timeout_ms);

   pthread_mutex_lock(&p->mtx);
   int wrc = 0;
   while (!p->done && wrc != ETIMEDOUT) {
      wrc = pthread_cond_timedwait(&p->cv, &p->mtx, &deadline);
   }
   pthread_mutex_unlock(&p->mtx);

   /* Re-acquire in lock order to capture the final state and remove atomically.
    * A late delivery between the two locks is captured here, not lost. */
   pthread_mutex_lock(&c->pending_mtx);
   pthread_mutex_lock(&p->mtx);
   bool done = p->done;
   int status = done ? p->status : MCP_ERR_TIMEOUT;
   char *result = p->result;
   p->result = NULL;
   pthread_mutex_unlock(&p->mtx);
   pending_remove(c, p);
   pthread_mutex_unlock(&c->pending_mtx);

   if (status == SUCCESS && result_out != NULL) {
      *result_out = result;
      result = NULL;
   }
   free(result);
   pending_destroy(p);
   return status;
}

/* --------------------------------------------------------------------------
 * Handshake + lifecycle
 * -------------------------------------------------------------------------- */

static bool record_failure_locked(mcp_client_t *c) {
   int64_t now = mono_now_sec();
   c->fail_ts[0] = c->fail_ts[1];
   c->fail_ts[1] = c->fail_ts[2];
   c->fail_ts[2] = now;
   return c->fail_ts[0] != 0 && (now - c->fail_ts[0]) <= MCP_FAIL_WINDOW_SEC;
}

static int verify_protocol_version(mcp_client_t *c, const char *init_result) {
   if (init_result == NULL) {
      return SUCCESS; /* nothing to check */
   }
   struct json_object *r = json_tokener_parse(init_result);
   int rc = SUCCESS;
   struct json_object *pv = NULL;
   if (r != NULL && json_object_object_get_ex(r, "protocolVersion", &pv)) {
      const char *v = json_object_get_string(pv);
      if (v != NULL && strcmp(v, MCP_PROTOCOL_VERSION) < 0) {
         OLOG_ERROR("MCP %s: server protocol %s < required %s", c->name, v, MCP_PROTOCOL_VERSION);
         rc = FAILURE;
      }
   }
   if (r != NULL) {
      json_object_put(r);
   }
   return rc;
}

static int client_handshake(mcp_client_t *c) {
   char init_params[512];
   snprintf(init_params, sizeof(init_params),
            "{\"protocolVersion\":\"%s\",\"capabilities\":{},"
            "\"clientInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
            MCP_PROTOCOL_VERSION, c->client_name, c->client_version);

   char *init_res = NULL;
   int rc = do_request(c, "initialize", init_params, c->request_timeout_ms, NULL, NULL, &init_res);
   if (rc != SUCCESS) {
      OLOG_ERROR("MCP %s: initialize failed (%d)", c->name, rc);
      free(init_res);
      return FAILURE;
   }
   if (verify_protocol_version(c, init_res) != SUCCESS) {
      free(init_res);
      return FAILURE;
   }
   free(init_res);

   send_notification(c, "notifications/initialized", NULL);

   char *tools = NULL;
   rc = do_request(c, "tools/list", NULL, c->request_timeout_ms, NULL, NULL, &tools);
   if (rc != SUCCESS) {
      OLOG_ERROR("MCP %s: tools/list failed (%d)", c->name, rc);
      free(tools);
      return FAILURE;
   }
   pthread_mutex_lock(&c->state_mtx);
   free(c->tools_json);
   c->tools_json = tools;
   pthread_mutex_unlock(&c->state_mtx);
   return SUCCESS;
}

int mcp_client_connect(mcp_client_t *c) {
   if (c == NULL) {
      return FAILURE;
   }

   pthread_mutex_lock(&c->state_mtx);
   while (c->state == MCP_STATE_CONNECTING) {
      pthread_cond_wait(&c->state_cv, &c->state_mtx); /* another thread is connecting */
   }
   if (c->state == MCP_STATE_DISABLED) {
      pthread_mutex_unlock(&c->state_mtx);
      return MCP_ERR_DISABLED;
   }
   if (c->state == MCP_STATE_CONNECTED) {
      pthread_mutex_unlock(&c->state_mtx);
      return SUCCESS;
   }
   c->state = MCP_STATE_CONNECTING;
   c->transport_connected = false;
   pthread_mutex_unlock(&c->state_mtx);

   /* Reset the transport before (re)starting: after a dropped stream the reader
    * thread has exited but the transport is still marked running, so start()
    * would no-op. stop() is idempotent (no-op on a fresh transport). */
   mcp_transport_stop(c->transport);
   bool ok = (mcp_transport_start(c->transport) == SUCCESS);

   if (ok) {
      struct timespec dl;
      deadline_in_ms(&dl, c->connect_wait_ms);
      pthread_mutex_lock(&c->state_mtx);
      int wrc = 0;
      while (!c->transport_connected && c->state == MCP_STATE_CONNECTING && wrc != ETIMEDOUT) {
         wrc = pthread_cond_timedwait(&c->state_cv, &c->state_mtx, &dl);
      }
      ok = c->transport_connected && c->state == MCP_STATE_CONNECTING;
      pthread_mutex_unlock(&c->state_mtx);
   }

   if (ok) {
      ok = (client_handshake(c) == SUCCESS);
   }

   if (ok) {
      pthread_mutex_lock(&c->state_mtx);
      c->state = MCP_STATE_CONNECTED;
      memset(c->fail_ts, 0, sizeof(c->fail_ts));
      pthread_cond_broadcast(&c->state_cv);
      pthread_mutex_unlock(&c->state_mtx);
      OLOG_INFO("MCP %s: connected", c->name);
      return SUCCESS;
   }

   mcp_transport_stop(c->transport);
   pthread_mutex_lock(&c->state_mtx);
   bool disable = record_failure_locked(c);
   c->state = disable ? MCP_STATE_DISABLED : MCP_STATE_DISCONNECTED;
   pthread_cond_broadcast(&c->state_cv);
   pthread_mutex_unlock(&c->state_mtx);
   if (disable) {
      OLOG_ERROR("MCP %s: disabled after repeated connect failures", c->name);
   }
   return disable ? MCP_ERR_DISABLED : MCP_ERR_DISCONNECTED;
}

void mcp_client_shutdown(mcp_client_t *c) {
   if (c == NULL) {
      return;
   }
   pthread_mutex_lock(&c->state_mtx);
   if (c->state == MCP_STATE_OFF) {
      pthread_mutex_unlock(&c->state_mtx);
      return;
   }
   c->state = MCP_STATE_SHUTTING_DOWN;
   pthread_cond_broadcast(&c->state_cv);
   pthread_mutex_unlock(&c->state_mtx);

   fail_all_pending(c, MCP_ERR_SHUTDOWN);
   mcp_transport_stop(c->transport);

   pthread_mutex_lock(&c->state_mtx);
   c->state = MCP_STATE_OFF;
   pthread_mutex_unlock(&c->state_mtx);
}

mcp_client_state_t mcp_client_state(mcp_client_t *c) {
   pthread_mutex_lock(&c->state_mtx);
   mcp_client_state_t s = c->state;
   pthread_mutex_unlock(&c->state_mtx);
   return s;
}

int mcp_client_call(mcp_client_t *c,
                    const char *method,
                    const char *params_json,
                    long timeout_ms,
                    mcp_progress_fn progress_cb,
                    void *progress_user,
                    char **result_out) {
   if (c == NULL || method == NULL) {
      return FAILURE;
   }

   pthread_mutex_lock(&c->state_mtx);
   mcp_client_state_t st = c->state;
   pthread_mutex_unlock(&c->state_mtx);

   if (st == MCP_STATE_DISABLED) {
      return MCP_ERR_DISABLED;
   }
   if (st == MCP_STATE_SHUTTING_DOWN) {
      return MCP_ERR_SHUTDOWN;
   }
   if (st != MCP_STATE_CONNECTED) {
      int rc = mcp_client_connect(c); /* lazy (re)connect */
      if (rc != SUCCESS) {
         return rc;
      }
   }
   return do_request(c, method, params_json, timeout_ms, progress_cb, progress_user, result_out);
}

const char *mcp_client_tools_json(mcp_client_t *c) {
   return c != NULL ? c->tools_json : NULL;
}

/* --------------------------------------------------------------------------
 * Construction / teardown
 * -------------------------------------------------------------------------- */

mcp_client_t *mcp_client_create(const mcp_client_opts_t *opts) {
   if (opts == NULL || opts->transport_factory == NULL || opts->url == NULL) {
      OLOG_ERROR("MCP client: create requires transport_factory and url");
      return NULL;
   }

   mcp_client_t *c = calloc(1, sizeof(*c));
   if (c == NULL) {
      return NULL;
   }

   c->name = strdup(opts->name != NULL ? opts->name : "mcp");
   c->client_name = strdup(opts->client_name != NULL ? opts->client_name : "DAWN");
   c->client_version = strdup(opts->client_version != NULL ? opts->client_version : "1.0");
   c->request_timeout_ms = opts->request_timeout_ms > 0 ? opts->request_timeout_ms
                                                        : MCP_DEFAULT_TIMEOUT_MS;
   c->connect_wait_ms = opts->connect_wait_ms > 0 ? opts->connect_wait_ms
                                                  : MCP_DEFAULT_CONNECT_WAIT_MS;
   c->state = MCP_STATE_OFF;
   pthread_mutex_init(&c->state_mtx, NULL);
   cond_init_monotonic(&c->state_cv);
   pthread_mutex_init(&c->pending_mtx, NULL);
   atomic_init(&c->next_id, 0);

   if (c->name == NULL || c->client_name == NULL || c->client_version == NULL) {
      mcp_client_destroy(c);
      return NULL;
   }

   mcp_transport_opts_t topts = {
      .url = opts->url,
      .bearer_token = opts->bearer_token,
      .tls_verify = opts->tls_verify,
      .connect_timeout_seconds = opts->connect_timeout_seconds,
      .idle_close_seconds = opts->idle_close_seconds,
      .on_message = client_on_message,
      .on_state = client_on_state,
      .user = c,
   };
   c->transport = opts->transport_factory(&topts);
   if (c->transport == NULL) {
      OLOG_ERROR("MCP client: transport factory failed");
      mcp_client_destroy(c);
      return NULL;
   }

   return c;
}

void mcp_client_destroy(mcp_client_t *c) {
   if (c == NULL) {
      return;
   }

   if (c->transport != NULL) {
      mcp_client_shutdown(c); /* fail pending + stop reader thread */
      mcp_transport_destroy(c->transport);
   }

   /* Defensive: free any pending entries that outlived their callers (misuse). */
   for (int i = 0; i < c->pending_count; i++) {
      pending_destroy(c->pending[i]);
   }

   free(c->tools_json);
   free(c->name);
   free(c->client_name);
   free(c->client_version);
   pthread_mutex_destroy(&c->state_mtx);
   pthread_cond_destroy(&c->state_cv);
   pthread_mutex_destroy(&c->pending_mtx);
   free(c);
}
