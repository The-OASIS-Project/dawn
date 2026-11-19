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
 * Unit tests for dawn-admin against a daemon speaking another admin protocol
 * version: the client must fail, never report success or a session list.
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
   RUN_TEST(test_list_sessions_fails_on_older_daemon);
   RUN_TEST(test_revoke_rejects_other_version_reply);
   return UNITY_END();
}
