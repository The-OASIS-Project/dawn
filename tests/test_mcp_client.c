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
 * Unit tests for the MCP JSON-RPC client. An in-test "mock MCP server" speaks
 * HTTP+SSE and auto-responds to JSON-RPC requests so the client's handshake,
 * call/response, timeout, progress, reconnect, and shutdown paths are exercised
 * end to end against real libcurl I/O.
 *
 * NOTE: the loopback socket scaffolding here is intentionally self-contained
 * and overlaps test_mcp_transport.c's simpler server. If a third consumer
 * appears, extract a shared tests/loopback_http_sse.{c,h} helper with a
 * request-handler callback (matches the project's "extract on third consumer"
 * convention).
 */

#define _GNU_SOURCE /* strcasestr, MSG_NOSIGNAL */

#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dawn_error.h"
#include "tools/mcp_client.h"
#include "tools/mcp_transport_http_sse.h"
#include "unity.h"

/* --------------------------------------------------------------------------
 * Mock MCP server (HTTP+SSE, auto-responding JSON-RPC)
 * -------------------------------------------------------------------------- */

typedef struct {
   int listen_fd;
   int port;
   pthread_t thread;
   volatile sig_atomic_t stop;

   pthread_mutex_t mtx;
   int sse_fd;
   int want_drop_sse;
} mock_t;

static void mock_sse_write(mock_t *s, const char *json) {
   char frame[4096];
   snprintf(frame, sizeof(frame), "event: message\ndata: %s\n\n", json);
   pthread_mutex_lock(&s->mtx);
   if (s->sse_fd >= 0) {
      if (send(s->sse_fd, frame, strlen(frame), MSG_NOSIGNAL) < 0) {
         close(s->sse_fd);
         s->sse_fd = -1;
      }
   }
   pthread_mutex_unlock(&s->mtx);
}

static void mock_open_sse(mock_t *s, int fd) {
   const char *headers = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Cache-Control: no-cache\r\n"
                         "Connection: keep-alive\r\n\r\n";
   send(fd, headers, strlen(headers), MSG_NOSIGNAL);
   const char *endpoint = "event: endpoint\ndata: /rpc\n\n";
   send(fd, endpoint, strlen(endpoint), MSG_NOSIGNAL);
   pthread_mutex_lock(&s->mtx);
   if (s->sse_fd >= 0) {
      close(s->sse_fd);
   }
   s->sse_fd = fd;
   pthread_mutex_unlock(&s->mtx);
}

/* Craft and push the JSON-RPC response(s) for one request. */
static void mock_respond(mock_t *s, struct json_object *req) {
   struct json_object *id_obj = NULL;
   if (!json_object_object_get_ex(req, "id", &id_obj)) {
      return; /* notification: no response */
   }
   int64_t id = json_object_get_int64(id_obj);

   struct json_object *m_obj = NULL;
   const char *method = json_object_object_get_ex(req, "method", &m_obj)
                            ? json_object_get_string(m_obj)
                            : "";
   char resp[2048];

   if (strcmp(method, "initialize") == 0) {
      snprintf(resp, sizeof(resp),
               "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"protocolVersion\":\"2024-11-05\","
               "\"capabilities\":{},\"serverInfo\":{\"name\":\"mock\",\"version\":\"1\"}}}",
               (long long)id);
      mock_sse_write(s, resp);
   } else if (strcmp(method, "tools/list") == 0) {
      snprintf(resp, sizeof(resp),
               "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"tools\":[{\"name\":\"echo\","
               "\"description\":\"echo\",\"inputSchema\":{\"type\":\"object\"}}]}}",
               (long long)id);
      mock_sse_write(s, resp);
   } else if (strcmp(method, "withprogress") == 0) {
      int64_t token = id;
      struct json_object *params = NULL;
      struct json_object *meta = NULL;
      struct json_object *tok = NULL;
      if (json_object_object_get_ex(req, "params", &params) &&
          json_object_object_get_ex(params, "_meta", &meta) &&
          json_object_object_get_ex(meta, "progressToken", &tok)) {
         token = json_object_get_int64(tok);
      }
      char prog[512];
      snprintf(prog, sizeof(prog),
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":"
               "{\"progressToken\":%lld,\"progress\":50,\"total\":100,\"message\":\"halfway\"}}",
               (long long)token);
      mock_sse_write(s, prog);
      snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"ok\":true}}",
               (long long)id);
      mock_sse_write(s, resp);
   } else if (strcmp(method, "ignore") == 0) {
      /* Intentionally no response: drives the timeout path. */
   } else {
      snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"echoed\":true}}",
               (long long)id);
      mock_sse_write(s, resp);
   }
}

static void mock_handle_post(mock_t *s, int fd, const char *buf, ssize_t got) {
   int content_length = 0;
   const char *cl = strcasestr(buf, "content-length:");
   if (cl != NULL) {
      content_length = atoi(cl + strlen("content-length:"));
   }
   char body[4096];
   body[0] = '\0';
   int have = 0;
   const char *split = strstr(buf, "\r\n\r\n");
   if (split != NULL) {
      split += 4;
      have = (int)(got - (split - buf));
      if (have < 0) {
         have = 0;
      }
      if (have > (int)sizeof(body) - 1) {
         have = (int)sizeof(body) - 1;
      }
      memcpy(body, split, have);
      body[have] = '\0';
   }
   while (have < content_length && have < (int)sizeof(body) - 1) {
      ssize_t r = recv(fd, body + have, sizeof(body) - 1 - have, 0);
      if (r <= 0) {
         break;
      }
      have += (int)r;
      body[have] = '\0';
   }

   const char *ok = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
   send(fd, ok, strlen(ok), MSG_NOSIGNAL);
   close(fd);

   struct json_object *req = json_tokener_parse(body);
   if (req != NULL) {
      mock_respond(s, req);
      json_object_put(req);
   }
}

static void *mock_main(void *arg) {
   mock_t *s = (mock_t *)arg;
   while (!s->stop) {
      pthread_mutex_lock(&s->mtx);
      if (s->want_drop_sse && s->sse_fd >= 0) {
         close(s->sse_fd);
         s->sse_fd = -1;
         s->want_drop_sse = 0;
      }
      pthread_mutex_unlock(&s->mtx);

      struct pollfd pfd = { .fd = s->listen_fd, .events = POLLIN };
      if (poll(&pfd, 1, 20) <= 0) {
         continue;
      }
      int fd = accept(s->listen_fd, NULL, NULL);
      if (fd < 0) {
         continue;
      }
      char buf[8192];
      ssize_t got = recv(fd, buf, sizeof(buf) - 1, 0);
      if (got <= 0) {
         close(fd);
         continue;
      }
      buf[got] = '\0';
      if (strncmp(buf, "GET", 3) == 0) {
         mock_open_sse(s, fd);
      } else if (strncmp(buf, "POST", 4) == 0) {
         mock_handle_post(s, fd, buf, got);
      } else {
         close(fd);
      }
   }
   pthread_mutex_lock(&s->mtx);
   if (s->sse_fd >= 0) {
      close(s->sse_fd);
      s->sse_fd = -1;
   }
   pthread_mutex_unlock(&s->mtx);
   return NULL;
}

static int mock_start(mock_t *s) {
   memset(s, 0, sizeof(*s));
   s->sse_fd = -1;
   pthread_mutex_init(&s->mtx, NULL);
   s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   if (s->listen_fd < 0) {
      return FAILURE;
   }
   int yes = 1;
   setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(s->listen_fd, 8) < 0) {
      close(s->listen_fd);
      return FAILURE;
   }
   socklen_t alen = sizeof(addr);
   getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen);
   s->port = ntohs(addr.sin_port);
   if (pthread_create(&s->thread, NULL, mock_main, s) != 0) {
      close(s->listen_fd);
      return FAILURE;
   }
   return SUCCESS;
}

static void mock_stop(mock_t *s) {
   s->stop = 1;
   pthread_join(s->thread, NULL);
   close(s->listen_fd);
   pthread_mutex_destroy(&s->mtx);
}

static void mock_drop_sse(mock_t *s) {
   pthread_mutex_lock(&s->mtx);
   s->want_drop_sse = 1;
   pthread_mutex_unlock(&s->mtx);
}

/* --------------------------------------------------------------------------
 * Fixtures
 * -------------------------------------------------------------------------- */

static mock_t g_srv;
static char g_url[128];

/* Progress recorder. */
static pthread_mutex_t g_pmtx = PTHREAD_MUTEX_INITIALIZER;
static int g_progress_calls;
static int g_last_percent;
static char g_last_progress_msg[256];

static void progress_cb(void *user, int percent, const char *message) {
   (void)user;
   pthread_mutex_lock(&g_pmtx);
   g_progress_calls++;
   g_last_percent = percent;
   snprintf(g_last_progress_msg, sizeof(g_last_progress_msg), "%s", message != NULL ? message : "");
   pthread_mutex_unlock(&g_pmtx);
}

void setUp(void) {
   pthread_mutex_lock(&g_pmtx);
   g_progress_calls = 0;
   g_last_percent = 0;
   g_last_progress_msg[0] = '\0';
   pthread_mutex_unlock(&g_pmtx);

   TEST_ASSERT_EQUAL_INT(SUCCESS, mock_start(&g_srv));
   snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d/sse", g_srv.port);
}

void tearDown(void) {
   mock_stop(&g_srv);
}

static mcp_client_t *make_client(void) {
   mcp_client_opts_t opts = {
      .name = "mock",
      .transport_factory = mcp_transport_http_sse_create,
      .url = g_url,
      .bearer_token = NULL,
      .tls_verify = false,
      .connect_timeout_seconds = 5,
      .idle_close_seconds = 0,
      .request_timeout_ms = 3000,
      .connect_wait_ms = 3000,
      .client_name = "DAWN-test",
      .client_version = "1.0",
   };
   return mcp_client_create(&opts);
}

#define WAIT_FOR(cond, timeout_ms)                \
   do {                                           \
      int _waited = 0;                            \
      while (!(cond) && _waited < (timeout_ms)) { \
         usleep(5000);                            \
         _waited += 5;                            \
      }                                           \
   } while (0)

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

static void test_handshake_connects(void) {
   mcp_client_t *c = make_client();
   TEST_ASSERT_NOT_NULL(c);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(c));
   TEST_ASSERT_EQUAL_INT(MCP_STATE_CONNECTED, mcp_client_state(c));

   const char *tools = mcp_client_tools_json(c);
   TEST_ASSERT_NOT_NULL(tools);
   TEST_ASSERT_NOT_NULL(strstr(tools, "echo"));

   mcp_client_destroy(c);
}

static void test_call_echo(void) {
   mcp_client_t *c = make_client();
   TEST_ASSERT_NOT_NULL(c);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(c));

   char *result = NULL;
   int rc = mcp_client_call(c, "echo", "{\"v\":1}", 3000, NULL, NULL, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_NOT_NULL(strstr(result, "echoed"));
   free(result);

   mcp_client_destroy(c);
}

static void test_call_timeout(void) {
   mcp_client_t *c = make_client();
   TEST_ASSERT_NOT_NULL(c);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(c));

   char *result = NULL;
   int rc = mcp_client_call(c, "ignore", NULL, 300, NULL, NULL, &result);
   TEST_ASSERT_EQUAL_INT(MCP_ERR_TIMEOUT, rc);
   TEST_ASSERT_NULL(result);

   mcp_client_destroy(c);
}

static void test_progress_dispatch(void) {
   mcp_client_t *c = make_client();
   TEST_ASSERT_NOT_NULL(c);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(c));

   char *result = NULL;
   int rc = mcp_client_call(c, "withprogress", NULL, 3000, progress_cb, NULL, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   free(result);

   pthread_mutex_lock(&g_pmtx);
   int calls = g_progress_calls;
   int percent = g_last_percent;
   char msg[256];
   snprintf(msg, sizeof(msg), "%s", g_last_progress_msg);
   pthread_mutex_unlock(&g_pmtx);

   TEST_ASSERT_TRUE(calls >= 1);
   TEST_ASSERT_EQUAL_INT(50, percent);
   TEST_ASSERT_NOT_NULL(strstr(msg, "halfway"));

   mcp_client_destroy(c);
}

static void test_reconnect_after_drop(void) {
   mcp_client_t *c = make_client();
   TEST_ASSERT_NOT_NULL(c);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(c));

   mock_drop_sse(&g_srv);
   WAIT_FOR(mcp_client_state(c) == MCP_STATE_DISCONNECTED, 3000);
   TEST_ASSERT_EQUAL_INT(MCP_STATE_DISCONNECTED, mcp_client_state(c));

   /* A call lazily reconnects (transport stop+start + fresh handshake). */
   char *result = NULL;
   int rc = mcp_client_call(c, "echo", NULL, 3000, NULL, NULL, &result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MCP_STATE_CONNECTED, mcp_client_state(c));
   free(result);

   mcp_client_destroy(c);
}

/* A blocking call must be unblocked promptly when shutdown() is invoked. */
typedef struct {
   mcp_client_t *client;
   int rc;
} shutdown_call_ctx_t;

static void *blocking_call_thread(void *arg) {
   shutdown_call_ctx_t *ctx = (shutdown_call_ctx_t *)arg;
   char *result = NULL;
   /* "ignore" never gets a response; a 30s timeout ensures the wakeup comes
    * from shutdown(), not from the timeout firing. */
   ctx->rc = mcp_client_call(ctx->client, "ignore", NULL, 30000, NULL, NULL, &result);
   free(result);
   return NULL;
}

static void test_shutdown_unblocks_pending(void) {
   mcp_client_t *c = make_client();
   TEST_ASSERT_NOT_NULL(c);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(c));

   shutdown_call_ctx_t ctx = { .client = c, .rc = -999 };
   pthread_t th;
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, blocking_call_thread, &ctx));

   usleep(300000); /* let the call register and park in cond_wait */
   mcp_client_shutdown(c);

   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(MCP_ERR_SHUTDOWN, ctx.rc);
   TEST_ASSERT_EQUAL_INT(MCP_STATE_OFF, mcp_client_state(c));

   mcp_client_destroy(c);
}

int main(void) {
   signal(SIGPIPE, SIG_IGN);
   UNITY_BEGIN();
   RUN_TEST(test_handshake_connects);
   RUN_TEST(test_call_echo);
   RUN_TEST(test_call_timeout);
   RUN_TEST(test_progress_dispatch);
   RUN_TEST(test_reconnect_after_drop);
   RUN_TEST(test_shutdown_unblocks_pending);
   return UNITY_END();
}
