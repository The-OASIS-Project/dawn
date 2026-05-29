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
 * Unit tests for the MCP bridge: registration into the tool registry, dispatch
 * forwarding (tools/call) to an MCP server, per-call MCP-access auth, and the
 * dangerous-tool admin denylist. Drives a compact in-test mock MCP server.
 *
 * NOTE: third in-test loopback-HTTP+SSE mock (after test_mcp_transport and
 * test_mcp_client). This is the trigger to extract a shared
 * tests/loopback_http_sse.{c,h} helper -- left as a follow-up to keep this
 * change focused.
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

#include "auth/auth_db.h"
#include "auth/auth_db_mcp.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "tools/mcp_bridge.h"
#include "tools/mcp_bridge_schema.h"
#include "tools/mcp_client.h"
#include "tools/mcp_transport_http_sse.h"
#include "tools/tool_registry.h"
#include "unity.h"

/* session_manager.c is not linked into this test, but ENABLE_MULTI_CLIENT is
 * defined for the target (so the header uses the extern declarations). Provide
 * the thread-local command context here, mirroring test_session_commands.c. */
static __thread session_t *s_test_cmd_ctx;
void session_set_command_context(session_t *session) {
   s_test_cmd_ctx = session;
}
session_t *session_get_command_context(void) {
   return s_test_cmd_ctx;
}

/* --------------------------------------------------------------------------
 * Mock MCP server (handles initialize / tools/list / tools/call)
 * -------------------------------------------------------------------------- */

typedef struct {
   int listen_fd;
   int port;
   pthread_t thread;
   volatile sig_atomic_t stop;
   pthread_mutex_t mtx;
   int sse_fd;
   int toolscall_count;
} mock_t;

static void mock_write(mock_t *s, const char *json) {
   char frame[4096];
   snprintf(frame, sizeof(frame), "event: message\ndata: %s\n\n", json);
   pthread_mutex_lock(&s->mtx);
   if (s->sse_fd >= 0 && send(s->sse_fd, frame, strlen(frame), MSG_NOSIGNAL) < 0) {
      close(s->sse_fd);
      s->sse_fd = -1;
   }
   pthread_mutex_unlock(&s->mtx);
}

static void mock_respond(mock_t *s, struct json_object *req) {
   struct json_object *id_obj = NULL;
   if (!json_object_object_get_ex(req, "id", &id_obj)) {
      return;
   }
   int64_t id = json_object_get_int64(id_obj);
   struct json_object *m = NULL;
   const char *method = json_object_object_get_ex(req, "method", &m) ? json_object_get_string(m)
                                                                     : "";
   char resp[1024];
   if (strcmp(method, "initialize") == 0) {
      snprintf(resp, sizeof(resp),
               "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"protocolVersion\":\"2024-11-05\","
               "\"capabilities\":{}}}",
               (long long)id);
      mock_write(s, resp);
   } else if (strcmp(method, "tools/list") == 0) {
      snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"tools\":[]}}",
               (long long)id);
      mock_write(s, resp);
   } else if (strcmp(method, "tools/call") == 0) {
      const char *called = "?";
      struct json_object *params = NULL;
      struct json_object *name = NULL;
      if (json_object_object_get_ex(req, "params", &params) &&
          json_object_object_get_ex(params, "name", &name)) {
         called = json_object_get_string(name);
      }
      pthread_mutex_lock(&s->mtx);
      s->toolscall_count++;
      pthread_mutex_unlock(&s->mtx);
      snprintf(resp, sizeof(resp),
               "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"called\":\"%s\"}}", (long long)id,
               called);
      mock_write(s, resp);
   }
}

static void mock_handle_post(mock_t *s, int fd, const char *buf, ssize_t got) {
   int clen = 0;
   const char *cl = strcasestr(buf, "content-length:");
   if (cl != NULL) {
      clen = atoi(cl + strlen("content-length:"));
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
   while (have < clen && have < (int)sizeof(body) - 1) {
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
         const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                           "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
         send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
         const char *ep = "event: endpoint\ndata: /rpc\n\n";
         send(fd, ep, strlen(ep), MSG_NOSIGNAL);
         pthread_mutex_lock(&s->mtx);
         if (s->sse_fd >= 0) {
            close(s->sse_fd);
         }
         s->sse_fd = fd;
         pthread_mutex_unlock(&s->mtx);
      } else if (strncmp(buf, "POST", 4) == 0) {
         mock_handle_post(s, fd, buf, got);
      } else {
         close(fd);
      }
   }
   pthread_mutex_lock(&s->mtx);
   if (s->sse_fd >= 0) {
      close(s->sse_fd);
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
      return -1;
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
      return -1;
   }
   socklen_t alen = sizeof(addr);
   getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen);
   s->port = ntohs(addr.sin_port);
   return pthread_create(&s->thread, NULL, mock_main, s) == 0 ? 0 : -1;
}

static void mock_stop(mock_t *s) {
   s->stop = 1;
   pthread_join(s->thread, NULL);
   close(s->listen_fd);
   pthread_mutex_destroy(&s->mtx);
}

static int mock_toolscall_count(mock_t *s) {
   pthread_mutex_lock(&s->mtx);
   int v = s->toolscall_count;
   pthread_mutex_unlock(&s->mtx);
   return v;
}

/* --------------------------------------------------------------------------
 * Fixtures
 * -------------------------------------------------------------------------- */

static mock_t g_srv;
static char g_url[128];
static mcp_client_t *g_client;
static int64_t g_admin_uid;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, tool_registry_init());
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   /* First user -> id 1, admin. tool_get_current_user_id() returns 1 when no
    * session context is set, so this is the default caller. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("admin", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("admin", &u));
   g_admin_uid = u.id;

   TEST_ASSERT_EQUAL_INT(0, mock_start(&g_srv));
   snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d/sse", g_srv.port);

   mcp_client_opts_t opts = {
      .name = "cbm",
      .transport_factory = mcp_transport_http_sse_create,
      .url = g_url,
      .tls_verify = false,
      .connect_timeout_seconds = 5,
      .request_timeout_ms = 3000,
      .connect_wait_ms = 3000,
   };
   g_client = mcp_client_create(&opts);
   TEST_ASSERT_NOT_NULL(g_client);
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_client_connect(g_client));
}

void tearDown(void) {
   session_set_command_context(NULL);
   mcp_bridge_shutdown();
   tool_registry_shutdown();
   if (g_client != NULL) {
      mcp_client_destroy(g_client);
      g_client = NULL;
   }
   mock_stop(&g_srv);
   auth_db_shutdown();
}

static mcp_param_set_t empty_params(void) {
   mcp_param_set_t p = { 0 };
   return p;
}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

/* Registration lands in the tool registry with the wrapped description + caps. */
static void test_register_into_registry(void) {
   mcp_param_set_t params = empty_params();
   TEST_ASSERT_EQUAL_INT(SUCCESS, mcp_bridge_register_tool(g_client, "cbm", "search_graph",
                                                           "cbm_search_graph", "Search the graph",
                                                           &params, false));
   const tool_metadata_t *meta = tool_registry_find("cbm_search_graph");
   TEST_ASSERT_NOT_NULL(meta);
   TEST_ASSERT_TRUE((meta->capabilities & TOOL_CAP_NETWORK) != 0);
   TEST_ASSERT_FALSE((meta->capabilities & TOOL_CAP_DANGEROUS) != 0);
   TEST_ASSERT_NOT_NULL(meta->description);
   TEST_ASSERT_NOT_NULL(strstr(meta->description, "[BEGIN UNTRUSTED MCP TOOL DESCRIPTION"));
   TEST_ASSERT_NOT_NULL(meta->callback);
}

/* Granted user -> dispatch forwards a tools/call carrying the upstream name. */
static void test_dispatch_forwards_toolscall(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(g_admin_uid, "cbm"));
   mcp_param_set_t params = empty_params();
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         mcp_bridge_register_tool(g_client, "cbm", "search_graph",
                                                  "cbm_search_graph", "desc", &params, false));
   const tool_metadata_t *meta = tool_registry_find("cbm_search_graph");
   TEST_ASSERT_NOT_NULL(meta);

   int should_respond = 0;
   char *result = meta->callback(NULL, "{\"query\":\"mqtt\"}", &should_respond);
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_EQUAL_INT(1, should_respond);
   TEST_ASSERT_TRUE(mock_toolscall_count(&g_srv) >= 1);
   TEST_ASSERT_NOT_NULL(strstr(result, "search_graph")); /* server echoed the name */
   free(result);
}

/* Without a grant, dispatch denies and never forwards. */
static void test_dispatch_denies_without_grant(void) {
   /* No grant for g_admin_uid this test. */
   mcp_param_set_t params = empty_params();
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         mcp_bridge_register_tool(g_client, "cbm", "search_graph",
                                                  "cbm_search_graph", "desc", &params, false));
   const tool_metadata_t *meta = tool_registry_find("cbm_search_graph");
   int should_respond = 0;
   char *result = meta->callback(NULL, "{}", &should_respond);
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_NOT_NULL(strstr(result, "not enabled"));
   TEST_ASSERT_EQUAL_INT(0, mock_toolscall_count(&g_srv));
   free(result);
}

/* Dangerous tools register with TOOL_CAP_DANGEROUS. */
static void test_dangerous_tool_marked(void) {
   mcp_param_set_t params = empty_params();
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         mcp_bridge_register_tool(g_client, "cbm", "index_repository",
                                                  "cbm_index_repository", "desc", &params, true));
   const tool_metadata_t *meta = tool_registry_find("cbm_index_repository");
   TEST_ASSERT_NOT_NULL(meta);
   TEST_ASSERT_TRUE((meta->capabilities & TOOL_CAP_DANGEROUS) != 0);
}

/* A non-admin (but access-granted) caller is blocked from a dangerous tool. */
static void test_dangerous_tool_blocks_non_admin(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("bob", "h", false));
   auth_user_t bob;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("bob", &bob));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_mcp_grant(bob.id, "cbm"));

   mcp_param_set_t params = empty_params();
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         mcp_bridge_register_tool(g_client, "cbm", "index_repository",
                                                  "cbm_index_repository", "desc", &params, true));
   const tool_metadata_t *meta = tool_registry_find("cbm_index_repository");

   /* Make bob the current caller via a minimal session context. */
   session_t *s = calloc(1, sizeof(session_t));
   TEST_ASSERT_NOT_NULL(s);
   s->metrics.user_id = bob.id;
   session_set_command_context(s);

   int should_respond = 0;
   char *result = meta->callback(NULL, "{}", &should_respond);
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_NOT_NULL(strstr(result, "administrators"));
   TEST_ASSERT_EQUAL_INT(0, mock_toolscall_count(&g_srv));
   free(result);

   session_set_command_context(NULL);
   free(s);
}

int main(void) {
   signal(SIGPIPE, SIG_IGN);
   UNITY_BEGIN();
   RUN_TEST(test_register_into_registry);
   RUN_TEST(test_dispatch_forwards_toolscall);
   RUN_TEST(test_dispatch_denies_without_grant);
   RUN_TEST(test_dangerous_tool_marked);
   RUN_TEST(test_dangerous_tool_blocks_non_admin);
   return UNITY_END();
}
