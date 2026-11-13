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
 * Tests for the stateless streamable-HTTP MCP transport against an in-test
 * loopback HTTP server that answers each POST the way the test asks: a
 * matching reply, someone else's reply, the wrong type, too much, an error,
 * or slowly (so stop() must abort it).
 */

#define _GNU_SOURCE /* strcasestr, MSG_NOSIGNAL */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dawn_error.h"
#include "tools/mcp_client.h"
#include "tools/mcp_transport.h"
#include "tools/mcp_transport_http.h"
#include "unity.h"

/* How the server answers the next POST. */
typedef enum {
   REPLY_MATCH,      /* JSON echoing the request id */
   REPLY_OTHER_ID,   /* JSON answering another request */
   REPLY_TEXT,       /* text/plain */
   REPLY_HUGE,       /* JSON over the size cap */
   REPLY_ERROR,      /* HTTP 500 */
   REPLY_ACCEPTED,   /* 202, no body (a notification) */
   REPLY_SLOW,       /* waits before answering */
   REPLY_INITIALIZE, /* initialize result naming a protocol version */
   REPLY_SERVER      /* a small MCP server: initialize, tools/list, tools/call */
} reply_mode_t;

static atomic_int s_slow_until_mode = REPLY_SLOW; /* a slow reply waits while this mode holds */
static int s_listen_fd = -1;
static int s_port;
static pthread_t s_thread;
static atomic_int s_stop;
static atomic_int s_mode;
static char s_last_headers[8192];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* What the transport delivered. */
static char s_delivered[256];
static int s_delivered_count;
static int s_state_events[4];

static void on_message(void *user, const char *json, size_t len) {
   (void)user;
   snprintf(s_delivered, sizeof(s_delivered), "%.*s", (int)(len < 255 ? len : 255), json);
   s_delivered_count++;
}

static void on_state(void *user, mcp_transport_event_t ev) {
   (void)user;
   s_state_events[ev]++;
}

static long long request_id(const char *body) {
   const char *p = strstr(body, "\"id\"");
   return p != NULL ? atoll(strchr(p, ':') + 1) : -1;
}

static void answer(int fd, const char *req) {
   const char *split = strstr(req, "\r\n\r\n");
   const char *body = split ? split + 4 : "";
   long long id = request_id(body);
   char json[256];
   char head[256];
   const char *type = "application/json";
   int code = 200;

   int mode = atomic_load(&s_mode);
   if (mode == REPLY_SERVER) {
      if (strstr(body, "\"initialize\"")) {
         mode = REPLY_INITIALIZE;
      } else if (id < 0) {
         mode = REPLY_ACCEPTED;          /* a notification */
      } else if (strstr(body, "list")) { /* json-c writes "tools\/list" */
         snprintf(json, sizeof(json),
                  "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"tools\":[{\"name\":"
                  "\"ping\",\"inputSchema\":{\"type\":\"object\"}}]}}",
                  id);
         mode = -1;
      } else if (strstr(body, "\"slow\"")) {
         mode = REPLY_SLOW;
      } else {
         snprintf(json, sizeof(json),
                  "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"content\":[{\"type\":"
                  "\"text\",\"text\":\"pong\"}]}}",
                  id);
         mode = -1;
      }
   }
   switch (mode) {
      case -1:
         break; /* json already built */
      case REPLY_OTHER_ID:
         snprintf(json, sizeof(json), "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{}}", id + 7);
         break;
      case REPLY_TEXT:
         snprintf(json, sizeof(json), "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{}}", id);
         type = "text/plain";
         break;
      case REPLY_ERROR:
         snprintf(json, sizeof(json), "oops");
         code = 500;
         break;
      case REPLY_ACCEPTED:
         json[0] = '\0';
         code = 202;
         break;
      case REPLY_INITIALIZE:
         snprintf(json, sizeof(json),
                  "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"protocolVersion\":"
                  "\"2025-11-25\"}}",
                  id);
         break;
      case REPLY_SLOW:
         for (int i = 0; i < 100 && atomic_load(&s_mode) == (int)s_slow_until_mode; i++) {
            usleep(50000); /* 5 s unless the test ends first */
         }
         /* fall through: answer if anyone is still listening */
      default:
         snprintf(json, sizeof(json), "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"ok\":1}}",
                  id);
   }

   if (mode == REPLY_HUGE) {
      size_t n = MCP_HTTP_REPLY_MAX + 4096;
      snprintf(head, sizeof(head),
               "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
               n);
      send(fd, head, strlen(head), MSG_NOSIGNAL);
      char *big = malloc(n);
      memset(big, ' ', n);
      big[0] = '{';
      big[n - 1] = '}';
      send(fd, big, n, MSG_NOSIGNAL);
      free(big);
      return;
   }
   snprintf(head, sizeof(head), "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",
            code, type, strlen(json));
   send(fd, head, strlen(head), MSG_NOSIGNAL);
   send(fd, json, strlen(json), MSG_NOSIGNAL);
}

static void *server(void *arg) {
   (void)arg;
   while (!atomic_load(&s_stop)) {
      struct pollfd p = { .fd = s_listen_fd, .events = POLLIN };
      if (poll(&p, 1, 100) <= 0) {
         continue;
      }
      int fd = accept(s_listen_fd, NULL, NULL);
      if (fd < 0) {
         continue;
      }
      char req[8192];
      ssize_t got = 0;
      /* Read headers and the (small) body. */
      for (int i = 0; i < 20; i++) {
         ssize_t r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
         if (r <= 0) {
            break;
         }
         got += r;
         req[got] = '\0';
         const char *split = strstr(req, "\r\n\r\n");
         const char *cl = strcasestr(req, "content-length:");
         if (split && cl && (got - (split + 4 - req)) >= atoi(cl + 15)) {
            break;
         }
      }
      req[got] = '\0';
      pthread_mutex_lock(&s_mtx);
      snprintf(s_last_headers, sizeof(s_last_headers), "%s", req);
      pthread_mutex_unlock(&s_mtx);
      answer(fd, req);
      close(fd);
   }
   return NULL;
}

static mcp_transport_t *make(const char *token) {
   char url[64];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", s_port);
   mcp_transport_opts_t o = { .url = url,
                              .bearer_token = token,
                              .tls_verify = true,
                              .connect_timeout_seconds = 10,
                              .on_message = on_message,
                              .on_state = on_state };
   return mcp_transport_http_create(&o);
}

#define REQ(id) "{\"jsonrpc\":\"2.0\",\"id\":" #id ",\"method\":\"tools/call\"}"

void setUp(void) {
   atomic_store(&s_slow_until_mode, REPLY_SLOW);
   s_delivered[0] = '\0';
   s_delivered_count = 0;
   memset(s_state_events, 0, sizeof(s_state_events));
   atomic_store(&s_mode, REPLY_MATCH);
}

void tearDown(void) {
}

static void test_start_reports_connected(void) {
   mcp_transport_t *t = make(NULL);
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_start(t));
   TEST_ASSERT_EQUAL(1, s_state_events[MCP_TRANSPORT_CONNECTED]);
   mcp_transport_destroy(t);
}

static void test_reply_delivered_with_headers(void) {
   mcp_transport_t *t = make("tok123");
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, REQ(5), strlen(REQ(5))));
   TEST_ASSERT_EQUAL(1, s_delivered_count);
   TEST_ASSERT_NOT_NULL(strstr(s_delivered, "\"ok\""));
   pthread_mutex_lock(&s_mtx);
   TEST_ASSERT_NOT_NULL(strcasestr(s_last_headers, "Authorization: Bearer tok123"));
   TEST_ASSERT_NOT_NULL(strcasestr(s_last_headers, "Accept: application/json, text/event-stream"));
   TEST_ASSERT_NULL(strcasestr(s_last_headers, "MCP-Protocol-Version"));
   pthread_mutex_unlock(&s_mtx);
   mcp_transport_destroy(t);
}

static void test_protocol_version_sent_after_initialize(void) {
   mcp_transport_t *t = make(NULL);
   atomic_store(&s_mode, REPLY_INITIALIZE);
   const char *init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}";
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, init, strlen(init)));
   atomic_store(&s_mode, REPLY_MATCH);
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, REQ(2), strlen(REQ(2))));
   pthread_mutex_lock(&s_mtx);
   TEST_ASSERT_NOT_NULL(strcasestr(s_last_headers, "MCP-Protocol-Version: 2025-11-25"));
   pthread_mutex_unlock(&s_mtx);
   mcp_transport_destroy(t);
}

/* A reply for another request, the wrong type, too much, or an error: that
 * request fails, nothing is delivered, and the connection isn't reported lost
 * (that would fail every other request in flight). */
static void test_bad_replies_fail_only_that_request(void) {
   mcp_transport_t *t = make(NULL);
   reply_mode_t bad[] = { REPLY_OTHER_ID, REPLY_TEXT, REPLY_HUGE, REPLY_ERROR };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      atomic_store(&s_mode, bad[i]);
      TEST_ASSERT_EQUAL_MESSAGE(FAILURE, mcp_transport_send(t, REQ(9), strlen(REQ(9))),
                                "bad reply accepted");
   }
   TEST_ASSERT_EQUAL(0, s_delivered_count);
   TEST_ASSERT_EQUAL(0, s_state_events[MCP_TRANSPORT_DISCONNECTED]);
   TEST_ASSERT_EQUAL(0, s_state_events[MCP_TRANSPORT_ERROR]);
   mcp_transport_destroy(t);
}

static void test_notification_needs_no_body(void) {
   mcp_transport_t *t = make(NULL);
   atomic_store(&s_mode, REPLY_ACCEPTED);
   const char *n = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, n, strlen(n)));
   TEST_ASSERT_EQUAL(0, s_delivered_count);
   mcp_transport_destroy(t);
}

typedef struct {
   mcp_transport_t *t;
   int rc;
} send_arg_t;

static void *slow_send(void *arg) {
   send_arg_t *a = arg;
   a->rc = mcp_transport_send(a->t, REQ(3), strlen(REQ(3)));
   return NULL;
}

/* stop() aborts a POST the server holds open (a long-poll), well before the
 * server would answer; start() then works again. */
static void test_stop_aborts_a_long_poll(void) {
   mcp_transport_t *t = make(NULL);
   atomic_store(&s_mode, REPLY_SLOW);
   send_arg_t a = { t, -1 };
   pthread_t th;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   pthread_create(&th, NULL, slow_send, &a);
   usleep(300000);
   mcp_transport_stop(t);
   pthread_join(th, NULL);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   TEST_ASSERT_EQUAL(FAILURE, a.rc);
   TEST_ASSERT_LESS_THAN(3, t1.tv_sec - t0.tv_sec);
   atomic_store(&s_mode, REPLY_MATCH);
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_start(t));
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, REQ(4), strlen(REQ(4))));
   mcp_transport_destroy(t);
}

static void test_stopped_transport_refuses_until_started(void) {
   mcp_transport_t *t = make(NULL);
   mcp_transport_stop(t);
   TEST_ASSERT_EQUAL(FAILURE, mcp_transport_send(t, REQ(6), strlen(REQ(6))));
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_start(t));
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, REQ(7), strlen(REQ(7))));
   mcp_transport_destroy(t);
}

/* The server names its version again on every initialize, so the header the
 * last one gave isn't sent with the next. */
static void test_no_protocol_version_on_initialize(void) {
   mcp_transport_t *t = make(NULL);
   atomic_store(&s_mode, REPLY_INITIALIZE);
   const char *init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}";
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, init, strlen(init)));
   TEST_ASSERT_EQUAL(SUCCESS, mcp_transport_send(t, init, strlen(init)));
   pthread_mutex_lock(&s_mtx);
   TEST_ASSERT_NULL(strcasestr(s_last_headers, "MCP-Protocol-Version"));
   pthread_mutex_unlock(&s_mtx);
   mcp_transport_destroy(t);
}

static mcp_client_t *client(void) {
   char url[64];
   snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", s_port);
   mcp_client_opts_t o = { .name = "t",
                           .transport_factory = mcp_transport_http_create,
                           .url = url,
                           .tls_verify = true,
                           .connect_timeout_seconds = 10,
                           .request_timeout_ms = 10000,
                           .connect_wait_ms = 2000 };
   return mcp_client_create(&o);
}

/* The real client over this transport: handshake (initialize, the initialized
 * notification, tools/list), then a tool call. */
static void test_client_handshake_and_call(void) {
   atomic_store(&s_mode, REPLY_SERVER);
   mcp_client_t *c = client();
   TEST_ASSERT_EQUAL(SUCCESS, mcp_client_connect(c));
   char *tools = mcp_client_tools_json_dup(c);
   TEST_ASSERT_NOT_NULL(tools);
   TEST_ASSERT_NOT_NULL(strstr(tools, "ping"));
   free(tools);
   char *out = NULL;
   TEST_ASSERT_EQUAL(SUCCESS,
                     mcp_client_call(c, "tools/call", "{\"name\":\"ping\"}", 0, NULL, NULL, &out));
   TEST_ASSERT_EQUAL_STRING("pong", out);
   free(out);
   mcp_client_destroy(c);
}

typedef struct {
   mcp_client_t *c;
   int rc;
} call_arg_t;

static void *slow_call(void *arg) {
   call_arg_t *a = arg;
   char *out = NULL;
   a->rc = mcp_client_call(a->c, "tools/call", "{\"name\":\"slow\"}", 0, NULL, NULL, &out);
   free(out);
   return NULL;
}

/* Destroying the client while a call is still inside the transport waits for
 * it (it aborts within about a second) instead of freeing under it. */
static void test_destroy_waits_for_a_call_in_flight(void) {
   atomic_store(&s_mode, REPLY_SERVER);
   atomic_store(&s_slow_until_mode, REPLY_SERVER);
   mcp_client_t *c = client();
   TEST_ASSERT_EQUAL(SUCCESS, mcp_client_connect(c));
   call_arg_t a = { c, -1 };
   pthread_t th;
   pthread_create(&th, NULL, slow_call, &a);
   usleep(300000);
   mcp_client_destroy(c);  /* must not return until the call is out */
   pthread_join(th, NULL); /* the call has already returned */
   TEST_ASSERT_NOT_EQUAL(SUCCESS, a.rc);
   atomic_store(&s_mode, REPLY_MATCH); /* release the server's slow reply */
}

static void test_token_refused_over_plain_http_off_host(void) {
   mcp_transport_opts_t o = { .url = "http://192.0.2.1:9760/mcp",
                              .bearer_token = "secret",
                              .on_message = on_message };
   TEST_ASSERT_NULL(mcp_transport_http_create(&o));
   o.url = "https://192.0.2.1:9760/mcp";
   TEST_ASSERT_NULL(mcp_transport_http_create(&o)); /* https, but unverified */
   o.tls_verify = true;
   mcp_transport_t *t = mcp_transport_http_create(&o);
   TEST_ASSERT_NOT_NULL(t);
   mcp_transport_destroy(t);
   o.url = "http://192.0.2.1:9760/mcp";
   o.bearer_token = NULL; /* no token: plain http is the operator's choice */
   t = mcp_transport_http_create(&o);
   TEST_ASSERT_NOT_NULL(t);
   mcp_transport_destroy(t);
}

int main(void) {
   s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   int one = 1;
   setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
   bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr));
   socklen_t len = sizeof(addr);
   getsockname(s_listen_fd, (struct sockaddr *)&addr, &len);
   s_port = ntohs(addr.sin_port);
   listen(s_listen_fd, 8);
   pthread_create(&s_thread, NULL, server, NULL);

   UNITY_BEGIN();
   RUN_TEST(test_start_reports_connected);
   RUN_TEST(test_reply_delivered_with_headers);
   RUN_TEST(test_protocol_version_sent_after_initialize);
   RUN_TEST(test_bad_replies_fail_only_that_request);
   RUN_TEST(test_notification_needs_no_body);
   RUN_TEST(test_stop_aborts_a_long_poll);
   RUN_TEST(test_stopped_transport_refuses_until_started);
   RUN_TEST(test_no_protocol_version_on_initialize);
   RUN_TEST(test_client_handshake_and_call);
   RUN_TEST(test_destroy_waits_for_a_call_in_flight);
   RUN_TEST(test_token_refused_over_plain_http_off_host);
   int rc = UNITY_END();

   atomic_store(&s_stop, 1);
   pthread_join(s_thread, NULL);
   close(s_listen_fd);
   return rc;
}
