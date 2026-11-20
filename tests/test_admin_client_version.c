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
 * Unit tests for dawn-admin's socket client: it parses the session list in
 * this build's wire layout, and against a daemon speaking another admin
 * protocol version it fails rather than report success or a session list.
 */

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "socket_client.h"
#include "unity.h"

/* An admin protocol version other than this build's (an older daemon). */
#define OTHER_VERSION (ADMIN_PROTOCOL_VERSION - 1)

static int s_client_fd = -1;
static int s_daemon_fd = -1;

void setUp(void) {
   int fds[2];
   TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
   s_client_fd = fds[0];
   s_daemon_fd = fds[1];
}

void tearDown(void) {
   close(s_client_fd);
   close(s_daemon_fd);
}

/* Queue the 4-byte reply a daemon sends before the client asks. */
static void daemon_replies(uint8_t version, uint8_t code) {
   admin_msg_response_t resp = { .version = version, .response_code = code, .reserved = 0 };
   TEST_ASSERT_EQUAL_INT((int)sizeof(resp), (int)write(s_daemon_fd, &resp, sizeof(resp)));
}

static int count_session(const admin_session_entry_t *session, void *ctx) {
   (void)session;
   (*(int *)ctx)++;
   return 0;
}

/* Pack one LIST_SESSIONS entry in the wire layout; returns its length. */
static size_t pack_session(uint8_t *p,
                           const char *prefix,
                           const char *user,
                           int64_t created,
                           int64_t last,
                           const char *ip) {
   const uint8_t *start = p;
   memcpy(p, prefix, ADMIN_SESSION_PREFIX_LEN);
   p += ADMIN_SESSION_PREFIX_LEN;
   *p++ = (uint8_t)strlen(user);
   memcpy(p, &created, 8);
   p += 8;
   memcpy(p, &last, 8);
   p += 8;
   *p++ = (uint8_t)strlen(ip);
   memcpy(p, user, strlen(user));
   p += strlen(user);
   memcpy(p, ip, strlen(ip));
   p += strlen(ip);
   return (size_t)(p - start);
}

typedef struct {
   admin_session_entry_t got[2];
   int count;
} session_capture_t;

static int capture_session(const admin_session_entry_t *session, void *ctx) {
   session_capture_t *cap = ctx;
   if (cap->count < 2) {
      cap->got[cap->count] = *session;
   }
   cap->count++;
   return 0;
}

/* A same-version list: each login's full 16-character prefix (what
 * `session revoke` needs) and the fields after it come through intact. */
static void test_list_sessions_parses_full_prefix(void) {
   uint8_t payload[256];
   size_t len = pack_session(payload, "a1b2c3d4e5f60718", "alice", 1700000000, 1700000600,
                             "192.168.1.20");
   len += pack_session(payload + len, "0f1e2d3c4b5a6978", "bob", 1700001000, 1700001900, "");

   admin_list_response_t hdr = { .version = ADMIN_PROTOCOL_VERSION,
                                 .response_code = ADMIN_RESP_SUCCESS,
                                 .payload_len = (uint16_t)len,
                                 .item_count = 2,
                                 .flags = 0 };
   TEST_ASSERT_EQUAL_INT((int)sizeof(hdr), (int)write(s_daemon_fd, &hdr, sizeof(hdr)));
   TEST_ASSERT_EQUAL_INT((int)len, (int)write(s_daemon_fd, payload, len));

   session_capture_t cap = { 0 };
   TEST_ASSERT_EQUAL_INT(ADMIN_RESP_SUCCESS,
                         admin_client_list_sessions(s_client_fd, capture_session, &cap));
   TEST_ASSERT_EQUAL_INT(2, cap.count);

   TEST_ASSERT_EQUAL_STRING("a1b2c3d4e5f60718", cap.got[0].token_prefix);
   TEST_ASSERT_EQUAL_STRING("alice", cap.got[0].username);
   TEST_ASSERT_EQUAL_INT64(1700000000, cap.got[0].created_at);
   TEST_ASSERT_EQUAL_INT64(1700000600, cap.got[0].last_activity);
   TEST_ASSERT_EQUAL_STRING("192.168.1.20", cap.got[0].ip_address);

   TEST_ASSERT_EQUAL_STRING("0f1e2d3c4b5a6978", cap.got[1].token_prefix);
   TEST_ASSERT_EQUAL_STRING("bob", cap.got[1].username);
   TEST_ASSERT_EQUAL_INT64(1700001000, cap.got[1].created_at);
   TEST_ASSERT_EQUAL_INT64(1700001900, cap.got[1].last_activity);
   TEST_ASSERT_EQUAL_STRING("", cap.got[1].ip_address);
}

/* The client stamps its requests with this build's version. */
static void test_request_carries_protocol_version(void) {
   daemon_replies(ADMIN_PROTOCOL_VERSION, ADMIN_RESP_SUCCESS);
   admin_client_ping(s_client_fd);

   admin_msg_header_t header;
   TEST_ASSERT_EQUAL_INT((int)sizeof(header), (int)read(s_daemon_fd, &header, sizeof(header)));
   TEST_ASSERT_EQUAL_UINT8(ADMIN_PROTOCOL_VERSION, header.version);
   TEST_ASSERT_EQUAL_UINT8(ADMIN_MSG_PING, header.msg_type);
}

/* An older daemon refuses the request: the list fails, showing no sessions. */
static void test_list_sessions_fails_on_older_daemon(void) {
   daemon_replies(OTHER_VERSION, ADMIN_RESP_VERSION_MISMATCH);
   int count = 0;
   TEST_ASSERT_NOT_EQUAL(ADMIN_RESP_SUCCESS,
                         admin_client_list_sessions(s_client_fd, count_session, &count));
   TEST_ASSERT_EQUAL_INT(0, count);
}

/* A reply stamped with another version is never taken as success. */
static void test_revoke_rejects_other_version_reply(void) {
   daemon_replies(OTHER_VERSION, ADMIN_RESP_SUCCESS);
   TEST_ASSERT_EQUAL_INT(ADMIN_RESP_SERVICE_ERROR,
                         admin_client_revoke_session(s_client_fd, "admin", "password123",
                                                     "a1b2c3d4e5f60718"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_request_carries_protocol_version);
   RUN_TEST(test_list_sessions_parses_full_prefix);
   RUN_TEST(test_list_sessions_fails_on_older_daemon);
   RUN_TEST(test_revoke_rejects_other_version_reply);
   return UNITY_END();
}
