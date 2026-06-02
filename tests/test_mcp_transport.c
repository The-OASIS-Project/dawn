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
 * Unit tests for the MCP HTTP+SSE transport. A loopback HTTP+SSE server runs
 * in an in-test pthread so the transport exercises real libcurl I/O against a
 * controllable peer (frame parse, POST roundtrip, drop + restart).
 */

#define _GNU_SOURCE /* strcasestr, MSG_NOSIGNAL */

#include <arpa/inet.h>
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
#include "tools/mcp_transport.h"
#include "tools/mcp_transport_http_sse.h"
#include "unity.h"

/* --------------------------------------------------------------------------
 * Loopback HTTP+SSE test server
 * -------------------------------------------------------------------------- */

typedef struct {
   int listen_fd;
   int port;
   pthread_t thread;
   volatile sig_atomic_t stop;

   pthread_mutex_t mtx;
   int sse_fd;           /* current SSE client fd, -1 if none */
   int want_drop_sse;    /* test asks the server to drop the SSE stream */
   int push_pending;     /* test asks the server to push push_msg */
   char push_msg[2048];  /* JSON-RPC text to push over SSE */
   char last_post[4096]; /* body of the most recent POST */
   int post_count;
   int sse_connect_count; /* number of SSE GETs accepted */
} loop_server_t;

static void server_send_sse(loop_server_t *s, int fd) {
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
   s->sse_connect_count++;
   pthread_mutex_unlock(&s->mtx);
}

static void server_handle_post(loop_server_t *s, int fd, const char *buf, ssize_t got) {
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

   pthread_mutex_lock(&s->mtx);
   snprintf(s->last_post, sizeof(s->last_post), "%s", body);
   s->post_count++;
   pthread_mutex_unlock(&s->mtx);

   const char *resp = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
   send(fd, resp, strlen(resp), MSG_NOSIGNAL);
   close(fd);
}

static void server_service_flags(loop_server_t *s) {
   pthread_mutex_lock(&s->mtx);
   if (s->want_drop_sse && s->sse_fd >= 0) {
      close(s->sse_fd);
      s->sse_fd = -1;
      s->want_drop_sse = 0;
   }
   if (s->push_pending && s->sse_fd >= 0) {
      char frame[2200];
      snprintf(frame, sizeof(frame), "event: message\ndata: %s\n\n", s->push_msg);
      if (send(s->sse_fd, frame, strlen(frame), MSG_NOSIGNAL) < 0) {
         close(s->sse_fd);
         s->sse_fd = -1;
      }
      s->push_pending = 0;
   }
   pthread_mutex_unlock(&s->mtx);
}

static void *server_main(void *arg) {
   loop_server_t *s = (loop_server_t *)arg;
   while (!s->stop) {
      server_service_flags(s);

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
         server_send_sse(s, fd); /* keeps fd open */
      } else if (strncmp(buf, "POST", 4) == 0) {
         server_handle_post(s, fd, buf, got);
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

static int server_start(loop_server_t *s) {
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
   addr.sin_port = 0; /* ephemeral */
   if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(s->listen_fd, 8) < 0) {
      close(s->listen_fd);
      return FAILURE;
   }
   socklen_t alen = sizeof(addr);
   getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen);
   s->port = ntohs(addr.sin_port);

   if (pthread_create(&s->thread, NULL, server_main, s) != 0) {
      close(s->listen_fd);
      return FAILURE;
   }
   return SUCCESS;
}

static void server_stop(loop_server_t *s) {
   s->stop = 1;
   pthread_join(s->thread, NULL);
   close(s->listen_fd);
   pthread_mutex_destroy(&s->mtx);
}

static int server_sse_count(loop_server_t *s) {
   pthread_mutex_lock(&s->mtx);
   int v = s->sse_connect_count;
   pthread_mutex_unlock(&s->mtx);
   return v;
}

static int server_post_count(loop_server_t *s) {
   pthread_mutex_lock(&s->mtx);
   int v = s->post_count;
   pthread_mutex_unlock(&s->mtx);
   return v;
}

static void server_get_post(loop_server_t *s, char *out, size_t n) {
   pthread_mutex_lock(&s->mtx);
   snprintf(out, n, "%s", s->last_post);
   pthread_mutex_unlock(&s->mtx);
}

static void server_push(loop_server_t *s, const char *msg) {
   pthread_mutex_lock(&s->mtx);
   snprintf(s->push_msg, sizeof(s->push_msg), "%s", msg);
   s->push_pending = 1;
   pthread_mutex_unlock(&s->mtx);
}

static void server_drop_sse(loop_server_t *s) {
   pthread_mutex_lock(&s->mtx);
   s->want_drop_sse = 1;
   pthread_mutex_unlock(&s->mtx);
}

/* --------------------------------------------------------------------------
 * Transport callback recorder
 * -------------------------------------------------------------------------- */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_connected;
static int g_disconnected;
static int g_errored;
static int g_msg_count;
static char g_last_msg[4096];

static void on_message(void *user, const char *json, size_t len) {
   (void)user;
   (void)len;
   pthread_mutex_lock(&g_mtx);
   snprintf(g_last_msg, sizeof(g_last_msg), "%s", json);
   g_msg_count++;
   pthread_mutex_unlock(&g_mtx);
}

static void on_state(void *user, mcp_transport_event_t ev) {
   (void)user;
   pthread_mutex_lock(&g_mtx);
   if (ev == MCP_TRANSPORT_CONNECTED) {
      g_connected++;
   } else if (ev == MCP_TRANSPORT_DISCONNECTED) {
      g_disconnected++;
   } else {
      g_errored++;
   }
   pthread_mutex_unlock(&g_mtx);
}

static int rec_connected(void) {
   pthread_mutex_lock(&g_mtx);
   int v = g_connected;
   pthread_mutex_unlock(&g_mtx);
   return v;
}

static int rec_disconnected(void) {
   pthread_mutex_lock(&g_mtx);
   int v = g_disconnected;
   pthread_mutex_unlock(&g_mtx);
   return v;
}

static int rec_msg_count(void) {
   pthread_mutex_lock(&g_mtx);
   int v = g_msg_count;
   pthread_mutex_unlock(&g_mtx);
   return v;
}

static void rec_get_last(char *out, size_t n) {
   pthread_mutex_lock(&g_mtx);
   snprintf(out, n, "%s", g_last_msg);
   pthread_mutex_unlock(&g_mtx);
}

/* Poll a condition up to timeout_ms (5ms granularity). */
#define WAIT_FOR(cond, timeout_ms)                \
   do {                                           \
      int _waited = 0;                            \
      while (!(cond) && _waited < (timeout_ms)) { \
         usleep(5000);                            \
         _waited += 5;                            \
      }                                           \
   } while (0)

/* --------------------------------------------------------------------------
 * Fixtures
 * -------------------------------------------------------------------------- */

static loop_server_t g_srv;
static char g_url[128];

void setUp(void) {
   pthread_mutex_lock(&g_mtx);
   g_connected = 0;
   g_disconnected = 0;
   g_errored = 0;
   g_msg_count = 0;
   g_last_msg[0] = '\0';
   pthread_mutex_unlock(&g_mtx);

   TEST_ASSERT_EQUAL_INT(SUCCESS, server_start(&g_srv));
   snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d/sse", g_srv.port);
}

void tearDown(void) {
   server_stop(&g_srv);
}

static mcp_transport_t *make_transport(void) {
   mcp_transport_opts_t opts = {
      .url = g_url,
      .bearer_token = NULL,
      .tls_verify = false,
      .connect_timeout_seconds = 5,
      .idle_close_seconds = 0,
      .on_message = on_message,
      .on_state = on_state,
      .user = NULL,
   };
   return mcp_transport_http_sse_create(&opts);
}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

/* Endpoint event -> CONNECTED; server push -> on_message with the payload. */
static void test_connect_and_message(void) {
   mcp_transport_t *t = make_transport();
   TEST_ASSERT_NOT_NULL(t);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_transport_start(t));

   WAIT_FOR(rec_connected() >= 1, 3000);
   TEST_ASSERT_TRUE(rec_connected() >= 1);
   TEST_ASSERT_EQUAL_INT(1, server_sse_count(&g_srv));

   server_push(&g_srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"hello\"}");
   WAIT_FOR(rec_msg_count() >= 1, 3000);
   TEST_ASSERT_TRUE(rec_msg_count() >= 1);

   char last[4096];
   rec_get_last(last, sizeof(last));
   TEST_ASSERT_NOT_NULL(strstr(last, "hello"));

   mcp_transport_stop(t);
   mcp_transport_destroy(t);
}

/* send() POSTs to the resolved endpoint; server receives the exact body. */
static void test_post_roundtrip(void) {
   mcp_transport_t *t = make_transport();
   TEST_ASSERT_NOT_NULL(t);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_transport_start(t));
   WAIT_FOR(rec_connected() >= 1, 3000);
   TEST_ASSERT_TRUE(rec_connected() >= 1);

   const char *req = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}";
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_transport_send(t, req, strlen(req)));

   WAIT_FOR(server_post_count(&g_srv) >= 1, 3000);
   TEST_ASSERT_TRUE(server_post_count(&g_srv) >= 1);
   char body[4096];
   server_get_post(&g_srv, body, sizeof(body));
   TEST_ASSERT_NOT_NULL(strstr(body, "\"method\":\"ping\""));

   /* The actual JSON-RPC response arrives over SSE, not the POST reply. */
   server_push(&g_srv, "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":\"pong\"}");
   WAIT_FOR(rec_msg_count() >= 1, 3000);
   char last[4096];
   rec_get_last(last, sizeof(last));
   TEST_ASSERT_NOT_NULL(strstr(last, "pong"));

   mcp_transport_stop(t);
   mcp_transport_destroy(t);
}

/* Drop the SSE stream -> DISCONNECTED; client policy = stop + start again. */
static void test_reconnect_on_drop(void) {
   mcp_transport_t *t = make_transport();
   TEST_ASSERT_NOT_NULL(t);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_transport_start(t));
   WAIT_FOR(rec_connected() >= 1, 3000);
   TEST_ASSERT_TRUE(rec_connected() >= 1);

   server_drop_sse(&g_srv);
   WAIT_FOR(rec_disconnected() >= 1, 3000);
   TEST_ASSERT_TRUE(rec_disconnected() >= 1);

   mcp_transport_stop(t);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_transport_start(t));
   WAIT_FOR(rec_connected() >= 2, 3000);
   TEST_ASSERT_TRUE(rec_connected() >= 2);
   TEST_ASSERT_TRUE(server_sse_count(&g_srv) >= 2);

   mcp_transport_stop(t);
   mcp_transport_destroy(t);
}

/* send() before the endpoint is known must fail cleanly, not crash. */
static void test_send_before_connect_fails(void) {
   mcp_transport_t *t = make_transport();
   TEST_ASSERT_NOT_NULL(t);
   const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1}";
   TEST_ASSERT_EQUAL_INT(FAILURE, mcp_transport_send(t, req, strlen(req)));
   mcp_transport_destroy(t);
}

int main(void) {
   signal(SIGPIPE, SIG_IGN);
   UNITY_BEGIN();
   RUN_TEST(test_connect_and_message);
   RUN_TEST(test_post_roundtrip);
   RUN_TEST(test_reconnect_on_drop);
   RUN_TEST(test_send_before_connect_fails);
   return UNITY_END();
}
