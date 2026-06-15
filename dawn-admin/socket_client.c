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
 * Socket client implementation for dawn-admin CLI.
 */

#include "socket_client.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int admin_client_connect(void) {
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0) {
      fprintf(stderr, "Error: Failed to create socket: %s\n", strerror(errno));
      return -1;
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;

   /* Use abstract socket namespace (Linux-specific) */
   addr.sun_path[0] = '\0';
   strncpy(addr.sun_path + 1, ADMIN_SOCKET_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);

   socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 +
                        strlen(ADMIN_SOCKET_ABSTRACT_NAME);

   if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
      if (errno == ECONNREFUSED) {
         fprintf(stderr, "Error: Dawn daemon is not running or admin socket not available\n");
      } else if (errno == ENOENT) {
         fprintf(stderr, "Error: Dawn admin socket not found - is the daemon running?\n");
      } else {
         fprintf(stderr, "Error: Failed to connect to daemon: %s\n", strerror(errno));
      }
      close(fd);
      return -1;
   }

   return fd;
}

void admin_client_disconnect(int fd) {
   if (fd >= 0) {
      close(fd);
   }
}

static int send_message(int fd, admin_msg_type_t type, const void *payload, uint16_t payload_len) {
   admin_msg_header_t header = { 0 };
   header.version = ADMIN_PROTOCOL_VERSION;
   header.msg_type = (uint8_t)type;
   header.payload_len = payload_len;

   /* Send header */
   ssize_t sent = write(fd, &header, sizeof(header));
   if (sent != sizeof(header)) {
      fprintf(stderr, "Error: Failed to send message header\n");
      return -1;
   }

   /* Send payload if present */
   if (payload_len > 0 && payload != NULL) {
      sent = write(fd, payload, payload_len);
      if (sent != payload_len) {
         fprintf(stderr, "Error: Failed to send message payload\n");
         return -1;
      }
   }

   return 0;
}

static int recv_response(int fd, admin_msg_response_t *resp) {
   ssize_t n = read(fd, resp, sizeof(*resp));
   if (n != sizeof(*resp)) {
      if (n == 0) {
         fprintf(stderr, "Error: Daemon closed connection\n");
      } else if (n < 0) {
         fprintf(stderr, "Error: Failed to read response: %s\n", strerror(errno));
      } else {
         fprintf(stderr, "Error: Incomplete response (got %zd bytes)\n", n);
      }
      return -1;
   }

   /* Validate protocol version */
   if (resp->version != ADMIN_PROTOCOL_VERSION) {
      fprintf(stderr, "Error: Protocol version mismatch (got 0x%02x, expected 0x%02x)\n",
              resp->version, ADMIN_PROTOCOL_VERSION);
      return -1;
   }

   return 0;
}

int admin_client_ping(int fd) {
   if (send_message(fd, ADMIN_MSG_PING, NULL, 0) != 0) {
      return -1;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return -1;
   }

   return (resp.response_code == ADMIN_RESP_SUCCESS) ? 0 : -1;
}

admin_resp_code_t admin_client_validate_token(int fd, const char *token) {
   if (!token) {
      return ADMIN_RESP_FAILURE;
   }

   size_t token_len = strlen(token);
   if (token_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_VALIDATE_SETUP_TOKEN, token, (uint16_t)token_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_create_user(int fd,
                                           const char *token,
                                           const char *username,
                                           const char *password,
                                           bool is_admin) {
   if (!token || !username || !password) {
      return ADMIN_RESP_FAILURE;
   }

   size_t token_len = strlen(token);
   size_t username_len = strlen(username);
   size_t password_len = strlen(password);

   /* Validate lengths */
   if (token_len != SETUP_TOKEN_LENGTH - 1) {
      fprintf(stderr, "Error: Invalid token format\n");
      return ADMIN_RESP_FAILURE;
   }

   if (username_len == 0 || username_len > ADMIN_USERNAME_MAX_LEN) {
      fprintf(stderr, "Error: Username must be 1-%d characters\n", ADMIN_USERNAME_MAX_LEN);
      return ADMIN_RESP_FAILURE;
   }

   if (password_len < ADMIN_PASSWORD_MIN_LEN || password_len > ADMIN_PASSWORD_MAX_LEN) {
      fprintf(stderr, "Error: Password must be %d-%d characters\n", ADMIN_PASSWORD_MIN_LEN,
              ADMIN_PASSWORD_MAX_LEN);
      return ADMIN_RESP_FAILURE;
   }

   /* Build payload:
    * - setup_token (24 bytes, no null terminator)
    * - username_len (1 byte)
    * - password_len (1 byte)
    * - is_admin (1 byte)
    * - username (username_len bytes)
    * - password (password_len bytes)
    */
   size_t payload_size = sizeof(admin_create_user_payload_t) + username_len + password_len;
   if (payload_size > ADMIN_MSG_MAX_PAYLOAD) {
      fprintf(stderr, "Error: Payload too large\n");
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   memset(payload, 0, sizeof(payload));

   admin_create_user_payload_t *hdr = (admin_create_user_payload_t *)payload;
   memcpy(hdr->setup_token, token, SETUP_TOKEN_LENGTH - 1);
   hdr->username_len = (uint8_t)username_len;
   hdr->password_len = (uint8_t)password_len;
   hdr->is_admin = is_admin ? 1 : 0;

   /* Copy username and password after header */
   char *data_ptr = payload + sizeof(admin_create_user_payload_t);
   memcpy(data_ptr, username, username_len);
   memcpy(data_ptr + username_len, password, password_len);

   if (send_message(fd, ADMIN_MSG_CREATE_USER, payload, (uint16_t)payload_size) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

/**
 * @brief Receive extended list response.
 */
static int recv_list_response(int fd,
                              admin_list_response_t *resp,
                              char *buffer,
                              size_t buffer_size) {
   /* Read the full list response header (8 bytes).
    * If the daemon rejected early (e.g. unauthorized), it sends a simple
    * 4-byte admin_msg_response_t instead. Handle both cases. */
   memset(resp, 0, sizeof(*resp));
   ssize_t n = read(fd, resp, sizeof(*resp));

   if (n == (ssize_t)sizeof(admin_msg_response_t)) {
      /* Got a 4-byte simple response — this is an error (unauthorized, etc.) */
      if (resp->response_code != ADMIN_RESP_SUCCESS) {
         fprintf(stderr, "Error: %s\n",
                 admin_resp_strerror((admin_resp_code_t)resp->response_code));
         return -1;
      }
      /* Success but short — shouldn't happen for list commands */
      fprintf(stderr, "Error: Unexpected short response from daemon\n");
      return -1;
   }

   if (n != (ssize_t)sizeof(*resp)) {
      if (n == 0) {
         fprintf(stderr, "Error: Daemon closed connection\n");
      } else if (n < 0) {
         fprintf(stderr, "Error: Failed to read response: %s\n", strerror(errno));
      } else {
         fprintf(stderr, "Error: Incomplete response (got %zd bytes)\n", n);
      }
      return -1;
   }

   if (resp->version != ADMIN_PROTOCOL_VERSION) {
      fprintf(stderr, "Error: Protocol version mismatch\n");
      return -1;
   }

   /* Check for error in the list response itself */
   if (resp->response_code != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", admin_resp_strerror((admin_resp_code_t)resp->response_code));
      return -1;
   }

   if (resp->payload_len > 0) {
      if (resp->payload_len > buffer_size) {
         fprintf(stderr, "Error: Response payload too large\n");
         return -1;
      }
      n = read(fd, buffer, resp->payload_len);
      if (n != resp->payload_len) {
         fprintf(stderr, "Error: Failed to read response data\n");
         return -1;
      }
   }

   return 0;
}

/**
 * @brief Build admin auth prefix.
 */
static size_t build_auth_prefix(char *buffer, const char *admin_user, const char *admin_password) {
   size_t ulen = strlen(admin_user);
   size_t plen = strlen(admin_password);

   admin_auth_prefix_t *prefix = (admin_auth_prefix_t *)buffer;
   prefix->admin_username_len = (uint8_t)ulen;
   prefix->admin_password_len = (uint8_t)plen;

   char *ptr = buffer + sizeof(admin_auth_prefix_t);
   memcpy(ptr, admin_user, ulen);
   memcpy(ptr + ulen, admin_password, plen);

   return sizeof(admin_auth_prefix_t) + ulen + plen;
}

admin_resp_code_t admin_client_list_users(int fd, admin_user_callback_t callback, void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_LIST_USERS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[4096];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed user entries */
   const char *p = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && p < end; i++) {
      /* Each entry: 4 bytes id, 1 byte uname_len, 1 byte is_admin,
       * 1 byte is_locked, 4 bytes failed_attempts, N bytes username */
      if (p + 11 > end)
         break;

      admin_user_entry_t entry = { 0 };

      uint32_t id;
      memcpy(&id, p, 4);
      entry.id = (int)id;
      p += 4;

      uint8_t uname_len = (uint8_t)*p++;
      entry.is_admin = (*p++) != 0;
      entry.is_locked = (*p++) != 0;

      uint32_t failed;
      memcpy(&failed, p, 4);
      entry.failed_attempts = (int)failed;
      p += 4;

      if (p + uname_len > end)
         break;
      size_t copy_len = (uname_len < 63) ? uname_len : 63;
      memcpy(entry.username, p, copy_len);
      p += uname_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_delete_user(int fd,
                                           const char *admin_user,
                                           const char *admin_password,
                                           const char *target_user) {
   if (!admin_user || !admin_password || !target_user) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);
   size_t target_len = strlen(target_user);

   if (auth_len + target_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, target_user, target_len);

   if (send_message(fd, ADMIN_MSG_DELETE_USER, payload, (uint16_t)(auth_len + target_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_change_password(int fd,
                                               const char *admin_user,
                                               const char *admin_password,
                                               const char *target_user,
                                               const char *new_password) {
   if (!admin_user || !admin_password || !target_user || !new_password) {
      return ADMIN_RESP_FAILURE;
   }

   size_t target_len = strlen(target_user);
   size_t newpass_len = strlen(new_password);

   if (target_len == 0 || target_len > ADMIN_USERNAME_MAX_LEN) {
      return ADMIN_RESP_FAILURE;
   }
   if (newpass_len < ADMIN_PASSWORD_MIN_LEN || newpass_len > ADMIN_PASSWORD_MAX_LEN) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* After auth: 1 byte target_uname_len, 1 byte new_pass_len, username, password */
   size_t remaining = 2 + target_len + newpass_len;
   if (auth_len + remaining > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = (uint8_t)target_len;
   *ptr++ = (uint8_t)newpass_len;
   memcpy(ptr, target_user, target_len);
   memcpy(ptr + target_len, new_password, newpass_len);

   if (send_message(fd, ADMIN_MSG_CHANGE_PASSWORD, payload, (uint16_t)(auth_len + remaining)) !=
       0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_unlock_user(int fd,
                                           const char *admin_user,
                                           const char *admin_password,
                                           const char *target_user) {
   if (!admin_user || !admin_password || !target_user) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);
   size_t target_len = strlen(target_user);

   if (auth_len + target_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, target_user, target_len);

   if (send_message(fd, ADMIN_MSG_UNLOCK_USER, payload, (uint16_t)(auth_len + target_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_list_sessions(int fd, admin_session_callback_t callback, void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_LIST_SESSIONS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[8192];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed session entries */
   const char *p = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && p < end; i++) {
      /* Each entry: 8 bytes token_prefix, 1 byte uname_len, 8 bytes created,
       * 8 bytes last_activity, 1 byte ip_len, N bytes username, M bytes ip */
      if (p + 26 > end)
         break;

      admin_session_entry_t entry = { 0 };

      memcpy(entry.token_prefix, p, 8);
      entry.token_prefix[8] = '\0';
      p += 8;

      uint8_t uname_len = (uint8_t)*p++;

      memcpy(&entry.created_at, p, 8);
      p += 8;

      memcpy(&entry.last_activity, p, 8);
      p += 8;

      uint8_t ip_len = (uint8_t)*p++;

      if (p + uname_len + ip_len > end)
         break;

      size_t copy_len = (uname_len < 63) ? uname_len : 63;
      memcpy(entry.username, p, copy_len);
      p += uname_len;

      copy_len = (ip_len < 63) ? ip_len : 63;
      memcpy(entry.ip_address, p, copy_len);
      p += ip_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_revoke_session(int fd,
                                              const char *admin_user,
                                              const char *admin_password,
                                              const char *token_prefix) {
   if (!admin_user || !admin_password || !token_prefix) {
      return ADMIN_RESP_FAILURE;
   }

   size_t prefix_len = strlen(token_prefix);
   if (prefix_len < 8) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build payload: auth_prefix + token_prefix_len + token_prefix */
   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* Add token prefix length and token prefix */
   if (auth_len + 1 + 8 > sizeof(payload)) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = 8;
   memcpy(ptr, token_prefix, 8);

   if (send_message(fd, ADMIN_MSG_REVOKE_SESSION, payload, (uint16_t)(auth_len + 1 + 8)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_revoke_user_sessions(int fd,
                                                    const char *admin_user,
                                                    const char *admin_password,
                                                    const char *target_user) {
   if (!admin_user || !admin_password || !target_user) {
      return ADMIN_RESP_FAILURE;
   }

   size_t target_len = strlen(target_user);
   if (target_len < 1 || target_len > 63) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build payload: auth_prefix + username_len + username */
   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* Add target username */
   if (auth_len + 1 + target_len > sizeof(payload)) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = (uint8_t)target_len;
   memcpy(ptr, target_user, target_len);

   if (send_message(fd, ADMIN_MSG_REVOKE_USER_SESSIONS, payload,
                    (uint16_t)(auth_len + 1 + target_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_get_stats(int fd, admin_db_stats_t *stats) {
   if (!stats) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_GET_STATS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   /* Receive extended response with stats data */
   admin_list_response_t resp;
   char buffer[256];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Copy stats from buffer */
   if (resp.payload_len >= sizeof(admin_db_stats_t)) {
      memcpy(stats, buffer, sizeof(admin_db_stats_t));
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_db_compact(int fd,
                                          const char *admin_user,
                                          const char *admin_password) {
   if (!admin_user || !admin_password) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   if (send_message(fd, ADMIN_MSG_DB_COMPACT, payload, (uint16_t)auth_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_db_backup(int fd,
                                         const char *admin_user,
                                         const char *admin_password,
                                         const char *dest_path) {
   if (!admin_user || !admin_password || !dest_path) {
      return ADMIN_RESP_FAILURE;
   }

   size_t path_len = strlen(dest_path);
   if (path_len < 1 || path_len > 255) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   /* Add path: 1 byte length + path */
   if (auth_len + 1 + path_len > sizeof(payload)) {
      return ADMIN_RESP_FAILURE;
   }

   char *ptr = payload + auth_len;
   *ptr++ = (uint8_t)path_len;
   memcpy(ptr, dest_path, path_len);

   if (send_message(fd, ADMIN_MSG_DB_BACKUP, payload, (uint16_t)(auth_len + 1 + path_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_query_log(int fd,
                                         const admin_log_filter_t *filter,
                                         admin_log_callback_t callback,
                                         void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build filter payload */
   char payload[ADMIN_MSG_MAX_PAYLOAD];
   char *p = payload;

   /* Since and until (int64_t each) */
   int64_t since = filter ? filter->since : 0;
   int64_t until = filter ? filter->until : 0;
   memcpy(p, &since, 8);
   p += 8;
   memcpy(p, &until, 8);
   p += 8;

   /* Event and username lengths */
   size_t event_len = (filter && filter->event) ? strlen(filter->event) : 0;
   size_t user_len = (filter && filter->username) ? strlen(filter->username) : 0;
   if (event_len > 31)
      event_len = 31;
   if (user_len > 63)
      user_len = 63;

   *p++ = (uint8_t)event_len;
   *p++ = (uint8_t)user_len;

   /* Limit and offset */
   uint16_t limit_val = filter ? (uint16_t)filter->limit : 0;
   uint16_t offset_val = filter ? (uint16_t)filter->offset : 0;
   memcpy(p, &limit_val, 2);
   p += 2;
   memcpy(p, &offset_val, 2);
   p += 2;

   /* Event and username strings */
   if (event_len > 0) {
      memcpy(p, filter->event, event_len);
      p += event_len;
   }
   if (user_len > 0) {
      memcpy(p, filter->username, user_len);
      p += user_len;
   }

   size_t payload_len = p - payload;

   if (send_message(fd, ADMIN_MSG_QUERY_LOG, payload, (uint16_t)payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   /* Receive extended response */
   admin_list_response_t resp;
   char buffer[16384];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse log entries */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      /* Each entry: 8 bytes timestamp, 4 length bytes, variable strings */
      if (rp + 12 > end)
         break;

      admin_log_entry_t entry = { 0 };

      memcpy(&entry.timestamp, rp, 8);
      rp += 8;

      uint8_t event_l = (uint8_t)*rp++;
      uint8_t user_l = (uint8_t)*rp++;
      uint8_t ip_l = (uint8_t)*rp++;
      uint8_t details_l = (uint8_t)*rp++;

      if (rp + event_l + user_l + ip_l + details_l > end)
         break;

      size_t copy_len = (event_l < 31) ? event_l : 31;
      memcpy(entry.event, rp, copy_len);
      rp += event_l;

      copy_len = (user_l < 63) ? user_l : 63;
      memcpy(entry.username, rp, copy_len);
      rp += user_l;

      copy_len = (ip_l < 63) ? ip_l : 63;
      memcpy(entry.ip_address, rp, copy_len);
      rp += ip_l;

      copy_len = (details_l < 255) ? details_l : 255;
      memcpy(entry.details, rp, copy_len);
      rp += details_l;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_list_blocked_ips(int fd, admin_ip_callback_t callback, void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   if (send_message(fd, ADMIN_MSG_LIST_BLOCKED_IPS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[4096];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed IP entries */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      admin_ip_entry_t entry = { 0 };

      /* Unpack ip_len */
      uint8_t ip_len = (uint8_t)*rp++;

      /* Unpack failed_attempts (little-endian) */
      int32_t attempts;
      memcpy(&attempts, rp, 4);
      rp += 4;
      entry.failed_attempts = attempts;

      /* Unpack last_attempt (little-endian) */
      int64_t ts;
      memcpy(&ts, rp, 8);
      rp += 8;
      entry.last_attempt = ts;

      /* Unpack ip_address */
      size_t copy_len = (ip_len < 63) ? ip_len : 63;
      memcpy(entry.ip_address, rp, copy_len);
      rp += ip_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_unblock_ip(int fd,
                                          const char *admin_user,
                                          const char *admin_password,
                                          const char *ip_address) {
   if (!admin_user || !admin_password || !ip_address) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);
   size_t ip_len = strlen(ip_address);

   if (auth_len + ip_len > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, ip_address, ip_len);

   if (send_message(fd, ADMIN_MSG_UNBLOCK_IP, payload, (uint16_t)(auth_len + ip_len)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

const char *admin_resp_strerror(admin_resp_code_t code) {
   switch (code) {
      case ADMIN_RESP_SUCCESS:
         return "Success";
      case ADMIN_RESP_FAILURE:
         return "Operation failed";
      case ADMIN_RESP_RATE_LIMITED:
         return "Too many failed attempts - please wait and try again";
      case ADMIN_RESP_SERVICE_ERROR:
         return "Internal service error";
      case ADMIN_RESP_VERSION_MISMATCH:
         return "Protocol version mismatch - update dawn-admin";
      case ADMIN_RESP_UNAUTHORIZED:
         return "Unauthorized - run as root, the daemon user, or a member of the daemon's group";
      case ADMIN_RESP_LAST_ADMIN:
         return "Cannot delete the last admin user";
      case ADMIN_RESP_NOT_FOUND:
         return "User or session not found";
      default:
         return "Unknown error";
   }
}

/* =============================================================================
 * Phase 3: Session Metrics
 * =============================================================================
 */

admin_resp_code_t admin_client_list_metrics(int fd,
                                            const admin_metrics_filter_t *filter,
                                            admin_metrics_callback_t callback,
                                            void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build filter payload: 4 bytes user_id, 4 bytes limit, 1 byte type_len, N bytes type */
   char payload[64] = { 0 };
   char *p = payload;

   int32_t user_id = filter ? filter->user_id : 0;
   memcpy(p, &user_id, 4);
   p += 4;

   int32_t limit = filter ? (filter->limit > 0 ? filter->limit : 20) : 20;
   memcpy(p, &limit, 4);
   p += 4;

   uint8_t type_len = 0;
   if (filter && filter->type) {
      type_len = (uint8_t)strlen(filter->type);
      if (type_len > 15)
         type_len = 15;
   }
   *p++ = type_len;

   if (type_len > 0) {
      memcpy(p, filter->type, type_len);
      p += type_len;
   }

   uint16_t payload_len = (uint16_t)(p - payload);

   if (send_message(fd, ADMIN_MSG_LIST_METRICS, payload, payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[8192];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed metrics entries
    * Format: 8 bytes id, 4 bytes session_id, 4 bytes user_id, 1 byte type_len,
    *         N bytes type, 8 bytes started_at, 8 bytes ended_at, 4 bytes queries_total,
    *         4 bytes queries_cloud, 4 bytes queries_local, 4 bytes errors,
    *         8 bytes avg_llm_ms (double)
    */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      if (rp + 53 > end)
         break;

      admin_metrics_entry_t entry = { 0 };

      memcpy(&entry.id, rp, 8);
      rp += 8;
      memcpy(&entry.session_id, rp, 4);
      rp += 4;
      memcpy(&entry.user_id, rp, 4);
      rp += 4;

      uint8_t tlen = (uint8_t)*rp++;
      if (rp + tlen > end)
         break;
      size_t copy_len = (tlen < 15) ? tlen : 15;
      memcpy(entry.session_type, rp, copy_len);
      rp += tlen;

      memcpy(&entry.started_at, rp, 8);
      rp += 8;
      memcpy(&entry.ended_at, rp, 8);
      rp += 8;
      memcpy(&entry.queries_total, rp, 4);
      rp += 4;
      memcpy(&entry.queries_cloud, rp, 4);
      rp += 4;
      memcpy(&entry.queries_local, rp, 4);
      rp += 4;
      memcpy(&entry.errors_count, rp, 4);
      rp += 4;
      memcpy(&entry.avg_llm_total_ms, rp, 8);
      rp += 8;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_get_metrics_totals(int fd,
                                                  const admin_metrics_filter_t *filter,
                                                  admin_metrics_totals_t *totals) {
   if (!totals) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build filter payload: 4 bytes user_id, 1 byte type_len, N bytes type */
   char payload[32] = { 0 };
   char *p = payload;

   int32_t user_id = filter ? filter->user_id : 0;
   memcpy(p, &user_id, 4);
   p += 4;

   uint8_t type_len = 0;
   if (filter && filter->type) {
      type_len = (uint8_t)strlen(filter->type);
      if (type_len > 15)
         type_len = 15;
   }
   *p++ = type_len;

   if (type_len > 0) {
      memcpy(p, filter->type, type_len);
      p += type_len;
   }

   uint16_t payload_len = (uint16_t)(p - payload);

   if (send_message(fd, ADMIN_MSG_GET_METRICS_TOTALS, payload, payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   /* Response is a fixed structure:
    * 4 bytes response header + 4 bytes session_count + 8 bytes queries_total +
    * 8 bytes queries_cloud + 8 bytes queries_local + 8 bytes errors_total +
    * 8 bytes avg_llm_ms
    */
   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Read the totals data */
   char data[48];
   ssize_t n = read(fd, data, sizeof(data));
   if (n != sizeof(data)) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   const char *dp = data;
   memcpy(&totals->session_count, dp, 4);
   dp += 4;
   memcpy(&totals->queries_total, dp, 8);
   dp += 8;
   memcpy(&totals->queries_cloud, dp, 8);
   dp += 8;
   memcpy(&totals->queries_local, dp, 8);
   dp += 8;
   memcpy(&totals->errors_total, dp, 8);
   dp += 8;
   memcpy(&totals->avg_llm_ms, dp, 8);

   return ADMIN_RESP_SUCCESS;
}

/* =============================================================================
 * Phase 4: Conversation Management
 * =============================================================================
 */

admin_resp_code_t admin_client_list_conversations(int fd,
                                                  const admin_conversation_filter_t *filter,
                                                  admin_conversation_callback_t callback,
                                                  void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   /* Build filter payload: 4 bytes user_id, 4 bytes limit, 1 byte include_archived */
   char payload[9] = { 0 };

   int32_t user_id = filter ? filter->user_id : 0;
   memcpy(payload, &user_id, 4);

   int32_t limit = filter ? (filter->limit > 0 ? filter->limit : 20) : 20;
   memcpy(payload + 4, &limit, 4);

   payload[8] = filter ? (filter->include_archived ? 1 : 0) : 0;

   if (send_message(fd, ADMIN_MSG_LIST_CONVERSATIONS, payload, 9) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[8192];
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed conversation entries
    * Format: 8 bytes id, 1 byte title_len, N bytes title, 8 bytes created_at,
    *         8 bytes updated_at, 4 bytes message_count, 1 byte uname_len, N bytes username
    */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      if (rp + 30 > end)
         break;

      admin_conversation_entry_t entry = { 0 };

      memcpy(&entry.id, rp, 8);
      rp += 8;

      uint8_t title_len = (uint8_t)*rp++;
      if (rp + title_len > end)
         break;
      size_t copy_len = (title_len < 127) ? title_len : 127;
      memcpy(entry.title, rp, copy_len);
      rp += title_len;

      memcpy(&entry.created_at, rp, 8);
      rp += 8;
      memcpy(&entry.updated_at, rp, 8);
      rp += 8;
      memcpy(&entry.message_count, rp, 4);
      rp += 4;

      uint8_t uname_len = (uint8_t)*rp++;
      if (rp + uname_len > end)
         break;
      copy_len = (uname_len < 63) ? uname_len : 63;
      memcpy(entry.username, rp, copy_len);
      rp += uname_len;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_get_conversation(int fd,
                                                int64_t conv_id,
                                                admin_message_callback_t callback,
                                                void *ctx) {
   if (!callback) {
      return ADMIN_RESP_FAILURE;
   }

   /* Send conv_id as payload */
   if (send_message(fd, ADMIN_MSG_GET_CONVERSATION, &conv_id, 8) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_list_response_t resp;
   char buffer[32768]; /* Messages can be large */
   if (recv_list_response(fd, &resp, buffer, sizeof(buffer)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.response_code != ADMIN_RESP_SUCCESS) {
      return (admin_resp_code_t)resp.response_code;
   }

   /* Parse packed message entries
    * Format: 1 byte role_len, N bytes role, 2 bytes content_len, N bytes content,
    *         8 bytes created_at
    */
   const char *rp = buffer;
   const char *end = buffer + resp.payload_len;

   for (uint16_t i = 0; i < resp.item_count && rp < end; i++) {
      if (rp + 11 > end)
         break;

      admin_message_entry_t entry = { 0 };

      uint8_t role_len = (uint8_t)*rp++;
      if (rp + role_len > end)
         break;
      size_t copy_len = (role_len < 15) ? role_len : 15;
      memcpy(entry.role, rp, copy_len);
      rp += role_len;

      uint16_t content_len;
      memcpy(&content_len, rp, 2);
      rp += 2;
      if (rp + content_len > end)
         break;
      copy_len = (content_len < ADMIN_MSG_CONTENT_MAX) ? content_len : ADMIN_MSG_CONTENT_MAX;
      memcpy(entry.content, rp, copy_len);
      rp += content_len;

      memcpy(&entry.created_at, rp, 8);
      rp += 8;

      if (callback(&entry, ctx) != 0)
         break;
   }

   return ADMIN_RESP_SUCCESS;
}

admin_resp_code_t admin_client_delete_conversation(int fd,
                                                   const char *admin_user,
                                                   const char *admin_password,
                                                   int64_t conv_id) {
   if (!admin_user || !admin_password) {
      return ADMIN_RESP_FAILURE;
   }

   char payload[ADMIN_MSG_MAX_PAYLOAD];
   size_t auth_len = build_auth_prefix(payload, admin_user, admin_password);

   if (auth_len + 8 > ADMIN_MSG_MAX_PAYLOAD) {
      return ADMIN_RESP_FAILURE;
   }

   memcpy(payload + auth_len, &conv_id, 8);

   if (send_message(fd, ADMIN_MSG_DELETE_CONVERSATION, payload, (uint16_t)(auth_len + 8)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   admin_msg_response_t resp;
   if (recv_response(fd, &resp) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }

   return (admin_resp_code_t)resp.response_code;
}

/* =============================================================================
 * Phase 5: Music Database
 * =============================================================================
 */

static admin_resp_code_t recv_text_response(int fd, char *response, size_t resp_len) {
   admin_msg_response_t resp;
   ssize_t n = read(fd, &resp, sizeof(resp));
   if (n != sizeof(resp)) {
      if (n == 0) {
         fprintf(stderr, "Error: Daemon closed connection\n");
      } else if (n < 0) {
         fprintf(stderr, "Error: Failed to read response: %s\n", strerror(errno));
      }
      return ADMIN_RESP_SERVICE_ERROR;
   }

   if (resp.version != ADMIN_PROTOCOL_VERSION) {
      fprintf(stderr, "Error: Protocol version mismatch\n");
      return ADMIN_RESP_SERVICE_ERROR;
   }

   /* reserved field contains text length */
   uint16_t text_len = resp.reserved;
   if (text_len > 0 && response && resp_len > 0) {
      size_t to_read = (text_len < resp_len - 1) ? text_len : resp_len - 1;
      n = read(fd, response, to_read);
      if (n > 0) {
         response[n] = '\0';
      } else {
         response[0] = '\0';
      }
      /* Discard any remaining data if buffer was too small */
      if (text_len > to_read) {
         char discard[256];
         size_t remaining = text_len - to_read;
         while (remaining > 0) {
            size_t chunk = (remaining < sizeof(discard)) ? remaining : sizeof(discard);
            ssize_t got = read(fd, discard, chunk);
            if (got <= 0) {
               break; /* connection closed or error — stop draining */
            }
            remaining -= (size_t)got;
         }
      }
   } else if (response && resp_len > 0) {
      response[0] = '\0';
   }

   return (admin_resp_code_t)resp.response_code;
}

admin_resp_code_t admin_client_music_stats(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_MUSIC_STATS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_music_search(int fd,
                                            const char *query,
                                            char *response,
                                            size_t resp_len) {
   if (!query) {
      return ADMIN_RESP_FAILURE;
   }

   size_t query_len = strlen(query);
   if (query_len > 200) {
      query_len = 200;
   }

   if (send_message(fd, ADMIN_MSG_MUSIC_SEARCH, query, (uint16_t)query_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_music_list(int fd, int limit, char *response, size_t resp_len) {
   char payload[16];
   int payload_len = snprintf(payload, sizeof(payload), "%d", limit);

   if (send_message(fd, ADMIN_MSG_MUSIC_LIST, payload, (uint16_t)payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_music_rescan(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_MUSIC_RESCAN, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_memory_recategorize(int fd,
                                                   const char *username,
                                                   char *response,
                                                   size_t resp_len) {
   if (!username || !username[0]) {
      return ADMIN_RESP_FAILURE;
   }
   uint16_t len = (uint16_t)strlen(username);
   if (send_message(fd, ADMIN_MSG_MEMORY_RECATEGORIZE, username, len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_memory_backfill_note_glosses(int fd,
                                                            const char *username,
                                                            char *response,
                                                            size_t resp_len) {
   if (!username || !username[0]) {
      return ADMIN_RESP_FAILURE;
   }
   uint16_t len = (uint16_t)strlen(username);
   if (send_message(fd, ADMIN_MSG_MEMORY_BACKFILL_NOTE_GLOSSES, username, len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_memory_rebuild_document_fts(int fd,
                                                           char *response,
                                                           size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_MEMORY_REBUILD_DOCUMENT_FTS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

#ifndef ADMIN_MEM_CLEANUP_FLAG_DRY_RUN
#define ADMIN_MEM_CLEANUP_FLAG_DRY_RUN 0x01
#endif

admin_resp_code_t admin_client_memory_cleanup_meta_facts(int fd,
                                                         const char *username,
                                                         bool dry_run,
                                                         char *response,
                                                         size_t resp_len) {
   if (!username || !username[0])
      return ADMIN_RESP_FAILURE;
   size_t ulen = strlen(username);
   if (ulen == 0 || ulen >= 64)
      return ADMIN_RESP_FAILURE;

   /* Wire format: byte 0 = flags, bytes 1..N = username. */
   uint8_t buf[128];
   buf[0] = (uint8_t)(dry_run ? ADMIN_MEM_CLEANUP_FLAG_DRY_RUN : 0);
   memcpy(buf + 1, username, ulen);
   uint16_t total = (uint16_t)(1 + ulen);

   if (send_message(fd, ADMIN_MSG_MEMORY_CLEANUP_META_FACTS, (const char *)buf, total) != 0)
      return ADMIN_RESP_SERVICE_ERROR;
   return recv_text_response(fd, response, resp_len);
}

/* Server uses the same dry-run bit position as cleanup-meta-facts; keep the
 * #define explicit here so the wire-format bytes are obvious. */
#ifndef ADMIN_MEM_SUMMARIZE_FLAG_DRY_RUN
#define ADMIN_MEM_SUMMARIZE_FLAG_DRY_RUN 0x01
#endif

admin_resp_code_t admin_client_memory_summarize_missing(int fd,
                                                        const char *username,
                                                        bool dry_run,
                                                        uint32_t max_count,
                                                        char *response,
                                                        size_t resp_len) {
   if (!username || !username[0])
      return ADMIN_RESP_FAILURE;
   size_t ulen = strlen(username);
   if (ulen == 0 || ulen >= 64)
      return ADMIN_RESP_FAILURE;

   /* Wire format: byte 0 = flags, bytes 1..4 = max_count (LE), bytes 5..N = username. */
   uint8_t buf[128];
   buf[0] = (uint8_t)(dry_run ? ADMIN_MEM_SUMMARIZE_FLAG_DRY_RUN : 0);
   memcpy(buf + 1, &max_count, sizeof(max_count));
   memcpy(buf + 5, username, ulen);
   uint16_t total = (uint16_t)(5 + ulen);

   if (send_message(fd, ADMIN_MSG_MEMORY_SUMMARIZE_MISSING, (const char *)buf, total) != 0)
      return ADMIN_RESP_SERVICE_ERROR;
   return recv_text_response(fd, response, resp_len);
}

/* Encode the binary reextract payload (see admin_socket.h
 * "MEMORY_REEXTRACT payload" block).  Returns the populated payload
 * length, or 0 on validation failure. */
static uint16_t encode_reextract_payload(uint8_t flags,
                                         const char *backup_path,
                                         double max_cost_usd,
                                         const char *username,
                                         uint8_t *out, /* size >= ADMIN_MSG_MAX_PAYLOAD */
                                         size_t out_size) {
   if (!username || !username[0])
      return 0;
   size_t ulen = strlen(username);
   if (ulen == 0 || ulen > ADMIN_REEXTRACT_USERNAME_MAX)
      return 0;
   size_t bplen = backup_path ? strlen(backup_path) : 0;
   if (bplen > ADMIN_REEXTRACT_BACKUP_PATH_MAX)
      return 0;
   /* Cost cap → micros, clamp to uint32 max. */
   uint32_t max_micros = 0;
   if (max_cost_usd > 0.0) {
      double micros = max_cost_usd * 1.0e6;
      if (micros >= (double)UINT32_MAX) {
         max_micros = UINT32_MAX;
      } else {
         max_micros = (uint32_t)micros;
      }
   }
   /* Total = 1 + 2 + bplen + 4 + 1 + ulen */
   size_t total = 1 + 2 + bplen + 4 + 1 + ulen;
   if (total > out_size || total > ADMIN_MSG_MAX_PAYLOAD)
      return 0;

   size_t off = 0;
   out[off++] = flags;
   uint16_t bplen_le = (uint16_t)bplen;
   memcpy(out + off, &bplen_le, sizeof(bplen_le));
   off += sizeof(bplen_le);
   if (bplen > 0) {
      memcpy(out + off, backup_path, bplen);
      off += bplen;
   }
   memcpy(out + off, &max_micros, sizeof(max_micros));
   off += sizeof(max_micros);
   out[off++] = (uint8_t)ulen;
   memcpy(out + off, username, ulen);
   off += ulen;
   return (uint16_t)off;
}

admin_resp_code_t admin_client_memory_reextract(int fd,
                                                const char *username,
                                                bool confirm,
                                                bool keep_summaries,
                                                const char *backup_path,
                                                double max_cost_usd,
                                                char *response,
                                                size_t resp_len) {
   uint8_t payload[ADMIN_MSG_MAX_PAYLOAD];
   uint8_t flags = 0;
   if (confirm)
      flags |= ADMIN_REEXTRACT_FLAG_CONFIRM;
   if (keep_summaries)
      flags |= ADMIN_REEXTRACT_FLAG_KEEP_SUMMARIES;

   uint16_t payload_len = encode_reextract_payload(flags, backup_path, max_cost_usd, username,
                                                   payload, sizeof(payload));
   if (payload_len == 0) {
      fprintf(stderr,
              "Error: invalid reextract arguments (username 1-%d bytes, "
              "backup-path <= %d bytes)\n",
              ADMIN_REEXTRACT_USERNAME_MAX, ADMIN_REEXTRACT_BACKUP_PATH_MAX);
      return ADMIN_RESP_FAILURE;
   }
   if (send_message(fd, ADMIN_MSG_MEMORY_REEXTRACT, payload, payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_memory_reextract_status(int fd,
                                                       const char *username,
                                                       char *response,
                                                       size_t resp_len) {
   uint8_t payload[ADMIN_MSG_MAX_PAYLOAD];
   uint8_t flags = ADMIN_REEXTRACT_FLAG_STATUS_QUERY;
   uint16_t payload_len = encode_reextract_payload(flags, NULL, 0.0, username, payload,
                                                   sizeof(payload));
   if (payload_len == 0) {
      fprintf(stderr, "Error: invalid username for reextract-status\n");
      return ADMIN_RESP_FAILURE;
   }
   if (send_message(fd, ADMIN_MSG_MEMORY_REEXTRACT_STATUS, payload, payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

/* =============================================================================
 * MEMORY_ENTITY_* subcommands (v43, dawn-admin memory entity *)
 *
 * One shared encoder + six thin send/recv wrappers.  All six subcommands use
 * admin_memory_entity_payload_t and reply with text via recv_text_response.
 * ============================================================================= */

static uint16_t encode_entity_payload(uint8_t flags,
                                      int64_t arg1,
                                      int64_t arg2,
                                      const char *username,
                                      const char *reason,
                                      uint8_t *out,
                                      size_t out_size) {
   if (!out || !username || !*username)
      return 0;
   size_t ulen = strlen(username);
   if (ulen == 0 || ulen > ADMIN_MEM_ENTITY_USERNAME_MAX)
      return 0;
   size_t rlen = (reason && *reason) ? strlen(reason) : 0;
   if (rlen > ADMIN_MEM_ENTITY_REASON_MAX)
      return 0;

   size_t total = sizeof(admin_memory_entity_payload_t) + ulen + rlen;
   if (total > out_size || total > ADMIN_MSG_MAX_PAYLOAD)
      return 0;

   admin_memory_entity_payload_t header = { 0 };
   header.flags = flags;
   header.username_len = (uint8_t)ulen;
   header.arg1 = arg1;
   header.arg2 = arg2;
   header.reason_len = (uint8_t)rlen;
   memcpy(out, &header, sizeof(header));
   memcpy(out + sizeof(header), username, ulen);
   if (rlen > 0)
      memcpy(out + sizeof(header) + ulen, reason, rlen);
   return (uint16_t)total;
}

static admin_resp_code_t entity_send_recv(int fd,
                                          admin_msg_type_t opcode,
                                          uint8_t flags,
                                          int64_t arg1,
                                          int64_t arg2,
                                          const char *username,
                                          const char *reason,
                                          char *response,
                                          size_t resp_len) {
   uint8_t payload[ADMIN_MSG_MAX_PAYLOAD];
   uint16_t payload_len = encode_entity_payload(flags, arg1, arg2, username, reason, payload,
                                                sizeof(payload));
   if (payload_len == 0) {
      fprintf(stderr, "Error: invalid entity-subcommand arguments\n");
      return ADMIN_RESP_FAILURE;
   }
   if (send_message(fd, opcode, payload, payload_len) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_memory_entity_merge(int fd,
                                                   const char *username,
                                                   int64_t source_id,
                                                   int64_t target_id,
                                                   const char *reason,
                                                   char *response,
                                                   size_t resp_len) {
   return entity_send_recv(fd, ADMIN_MSG_MEMORY_ENTITY_MERGE, 0, source_id, target_id, username,
                           reason, response, resp_len);
}

admin_resp_code_t admin_client_memory_entity_split(int fd,
                                                   const char *username,
                                                   int64_t link_id,
                                                   const char *reason,
                                                   char *response,
                                                   size_t resp_len) {
   return entity_send_recv(fd, ADMIN_MSG_MEMORY_ENTITY_SPLIT, 0, link_id, 0, username, reason,
                           response, resp_len);
}

admin_resp_code_t admin_client_memory_entity_aliases(int fd,
                                                     const char *username,
                                                     int64_t entity_id,
                                                     char *response,
                                                     size_t resp_len) {
   return entity_send_recv(fd, ADMIN_MSG_MEMORY_ENTITY_ALIASES, 0, entity_id, 0, username, NULL,
                           response, resp_len);
}

admin_resp_code_t admin_client_memory_entity_history(int fd,
                                                     const char *username,
                                                     int64_t entity_id,
                                                     char *response,
                                                     size_t resp_len) {
   return entity_send_recv(fd, ADMIN_MSG_MEMORY_ENTITY_HISTORY, 0, entity_id, 0, username, NULL,
                           response, resp_len);
}

admin_resp_code_t admin_client_memory_entity_list(int fd,
                                                  const char *username,
                                                  bool include_aliases,
                                                  char *response,
                                                  size_t resp_len) {
   uint8_t flags = include_aliases ? ADMIN_MEM_ENTITY_FLAG_INCLUDE_ALIASES : 0;
   return entity_send_recv(fd, ADMIN_MSG_MEMORY_ENTITY_LIST, flags, 0, 0, username, NULL, response,
                           resp_len);
}

admin_resp_code_t admin_client_memory_entity_link_user_self(int fd,
                                                            const char *username,
                                                            bool dry_run,
                                                            char *response,
                                                            size_t resp_len) {
   uint8_t flags = dry_run ? ADMIN_MEM_ENTITY_FLAG_DRY_RUN : 0;
   return entity_send_recv(fd, ADMIN_MSG_MEMORY_ENTITY_LINK_USER_SELF, flags, 0, 0, username, NULL,
                           response, resp_len);
}

admin_resp_code_t admin_client_messaging_generate_link_code(int fd,
                                                            const char *username,
                                                            const char *provider_hint,
                                                            char *response,
                                                            size_t resp_len) {
   if (!username || !username[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t ulen = strlen(username);
   if (ulen == 0 || ulen >= ADMIN_MESSAGING_USERNAME_MAX) {
      return ADMIN_RESP_FAILURE;
   }
   size_t hlen = (provider_hint && provider_hint[0]) ? strlen(provider_hint) : 0;
   if (hlen > ADMIN_MESSAGING_PROVIDER_HINT_MAX) {
      return ADMIN_RESP_FAILURE;
   }

   /* Wire format: byte 0 = provider_hint_len, bytes 1..1+H = hint,
    * bytes 1+H..end = username. */
   uint8_t buf[1 + ADMIN_MESSAGING_PROVIDER_HINT_MAX + ADMIN_MESSAGING_USERNAME_MAX];
   buf[0] = (uint8_t)hlen;
   if (hlen > 0) {
      memcpy(buf + 1, provider_hint, hlen);
   }
   memcpy(buf + 1 + hlen, username, ulen);
   uint16_t total = (uint16_t)(1 + hlen + ulen);

   if (send_message(fd, ADMIN_MSG_MESSAGING_GENERATE_LINK_CODE, (const char *)buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_messaging_list_channels(int fd,
                                                       const char *username,
                                                       char *response,
                                                       size_t resp_len) {
   if (!username || !username[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t ulen = strlen(username);
   if (ulen >= ADMIN_MESSAGING_USERNAME_MAX) {
      return ADMIN_RESP_FAILURE;
   }
   /* Payload is the raw username. */
   if (send_message(fd, ADMIN_MSG_MESSAGING_LIST_CHANNELS, username, (uint16_t)ulen) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_messaging_unlink_channel(int fd,
                                                        const char *username,
                                                        const char *display_name,
                                                        char *response,
                                                        size_t resp_len) {
   if (!username || !username[0] || !display_name || !display_name[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t ulen = strlen(username);
   size_t nlen = strlen(display_name);
   if (ulen == 0 || ulen >= ADMIN_MESSAGING_USERNAME_MAX || nlen == 0 ||
       nlen >= ADMIN_MESSAGING_DISPLAY_NAME_MAX) {
      return ADMIN_RESP_FAILURE;
   }

   /* Wire: byte 0 = username_len, [username], [display_name]. */
   uint8_t buf[1 + ADMIN_MESSAGING_USERNAME_MAX + ADMIN_MESSAGING_DISPLAY_NAME_MAX];
   buf[0] = (uint8_t)ulen;
   memcpy(buf + 1, username, ulen);
   memcpy(buf + 1 + ulen, display_name, nlen);
   uint16_t total = (uint16_t)(1 + ulen + nlen);

   if (send_message(fd, ADMIN_MSG_MESSAGING_UNLINK_CHANNEL, (const char *)buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_messaging_reenable_channel(int fd,
                                                          const char *username,
                                                          const char *display_name,
                                                          char *response,
                                                          size_t resp_len) {
   if (!username || !username[0] || !display_name || !display_name[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t ulen = strlen(username);
   size_t nlen = strlen(display_name);
   if (ulen == 0 || ulen >= ADMIN_MESSAGING_USERNAME_MAX || nlen == 0 ||
       nlen >= ADMIN_MESSAGING_DISPLAY_NAME_MAX) {
      return ADMIN_RESP_FAILURE;
   }

   /* Wire: byte 0 = username_len, [username], [display_name] (same as unlink). */
   uint8_t buf[1 + ADMIN_MESSAGING_USERNAME_MAX + ADMIN_MESSAGING_DISPLAY_NAME_MAX];
   buf[0] = (uint8_t)ulen;
   memcpy(buf + 1, username, ulen);
   memcpy(buf + 1 + ulen, display_name, nlen);
   uint16_t total = (uint16_t)(1 + ulen + nlen);

   if (send_message(fd, ADMIN_MSG_MESSAGING_REENABLE_CHANNEL, (const char *)buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_messaging_link_attempts(int fd,
                                                       const char *provider_filter,
                                                       int limit,
                                                       char *response,
                                                       size_t resp_len) {
   size_t plen = (provider_filter && provider_filter[0]) ? strlen(provider_filter) : 0;
   if (plen > ADMIN_MESSAGING_PROVIDER_HINT_MAX) {
      return ADMIN_RESP_FAILURE;
   }
   if (limit < 0) {
      limit = 0;
   }

   /* Wire: byte 0 = provider_len, [provider], then uint16 LE limit. */
   uint8_t buf[1 + ADMIN_MESSAGING_PROVIDER_HINT_MAX + 2];
   buf[0] = (uint8_t)plen;
   if (plen > 0) {
      memcpy(buf + 1, provider_filter, plen);
   }
   uint16_t lim = (uint16_t)(limit > 0xFFFF ? 0xFFFF : limit);
   buf[1 + plen] = (uint8_t)(lim & 0xFF);
   buf[1 + plen + 1] = (uint8_t)((lim >> 8) & 0xFF);
   uint16_t total = (uint16_t)(1 + plen + 2);

   if (send_message(fd, ADMIN_MSG_MESSAGING_LINK_ATTEMPTS, (const char *)buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

/* =============================================================================
 * OTA updates (dawn-admin ota *)
 * ============================================================================= */

admin_resp_code_t admin_client_ota_list(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_OTA_LIST, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

/* ===== MCP bridge (0xB0-0xB4) ===== */

admin_resp_code_t admin_client_mcp_list(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_MCP_LIST, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_ota_rescan(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_OTA_RESCAN, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_mcp_reset(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_MCP_RESET, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_ota_push(int fd,
                                        const char *uuid,
                                        const char *version,
                                        bool allow_downgrade,
                                        char *response,
                                        size_t resp_len) {
   if (!uuid || !uuid[0] || !version || !version[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t ulen = strlen(uuid);
   size_t vlen = strlen(version);
   if (ulen == 0 || ulen > ADMIN_OTA_UUID_MAX || vlen == 0 || vlen > ADMIN_OTA_VERSION_MAX) {
      return ADMIN_RESP_FAILURE;
   }

   /* Wire: byte 0 = flags, byte 1 = uuid_len, [uuid], [version]. */
   uint8_t buf[2 + ADMIN_OTA_UUID_MAX + ADMIN_OTA_VERSION_MAX];
   buf[0] = (uint8_t)(allow_downgrade ? ADMIN_OTA_FLAG_ALLOW_DOWNGRADE : 0);
   buf[1] = (uint8_t)ulen;
   memcpy(buf + 2, uuid, ulen);
   memcpy(buf + 2 + ulen, version, vlen);
   uint16_t total = (uint16_t)(2 + ulen + vlen);

   if (send_message(fd, ADMIN_MSG_OTA_PUSH, (const char *)buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

/* Wire format for grant/revoke: "<username>\0<alias>". */
static admin_resp_code_t mcp_grant_revoke(int fd,
                                          uint8_t opcode,
                                          const char *username,
                                          const char *alias,
                                          char *response,
                                          size_t resp_len) {
   if (!username || !username[0] || !alias || !alias[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t ul = strlen(username);
   size_t al = strlen(alias);
   char buf[256];
   if (ul + 1 + al > sizeof(buf)) {
      return ADMIN_RESP_FAILURE;
   }
   memcpy(buf, username, ul);
   buf[ul] = '\0';
   memcpy(buf + ul + 1, alias, al);
   uint16_t total = (uint16_t)(ul + 1 + al);
   if (send_message(fd, opcode, buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_ota_push_all(int fd,
                                            int tier,
                                            const char *version,
                                            bool allow_downgrade,
                                            char *response,
                                            size_t resp_len) {
   if (!version || !version[0] || (tier != 1 && tier != 2)) {
      return ADMIN_RESP_FAILURE;
   }
   size_t vlen = strlen(version);
   if (vlen == 0 || vlen > ADMIN_OTA_VERSION_MAX) {
      return ADMIN_RESP_FAILURE;
   }
   /* Wire: byte 0 = flags, byte 1 = tier, [version]. */
   uint8_t buf[2 + ADMIN_OTA_VERSION_MAX];
   buf[0] = (uint8_t)(allow_downgrade ? ADMIN_OTA_FLAG_ALLOW_DOWNGRADE : 0);
   buf[1] = (uint8_t)tier;
   memcpy(buf + 2, version, vlen);
   uint16_t total = (uint16_t)(2 + vlen);

   if (send_message(fd, ADMIN_MSG_OTA_PUSH_ALL, (const char *)buf, total) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_mcp_grant(int fd,
                                         const char *username,
                                         const char *alias,
                                         char *response,
                                         size_t resp_len) {
   return mcp_grant_revoke(fd, ADMIN_MSG_MCP_GRANT, username, alias, response, resp_len);
}

admin_resp_code_t admin_client_mcp_revoke(int fd,
                                          const char *username,
                                          const char *alias,
                                          char *response,
                                          size_t resp_len) {
   return mcp_grant_revoke(fd, ADMIN_MSG_MCP_REVOKE, username, alias, response, resp_len);
}

/* ===== Code projects (0xD0-0xD6) ===== */

/* Staging buffer for multi-field code-project payloads (flags + name + url +
 * branch, or name + path) — larger than a single field since they pack several
 * NUL-separated values (name 63 + url 511 + branch 128 ≈ 704 worst case). */
#define CP_ADMIN_PAYLOAD_BUF 1024

admin_resp_code_t admin_client_code_proj_list(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_CODE_PROJ_LIST, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_code_proj_import(int fd,
                                                const char *url,
                                                const char *name,
                                                const char *branch,
                                                bool global,
                                                char *response,
                                                size_t resp_len) {
   if (!url || !url[0] || !name || !name[0]) {
      return ADMIN_RESP_FAILURE;
   }
   /* Wire: flags(1) + "name\0url[\0branch]". */
   size_t nl = strlen(name);
   size_t ul = strlen(url);
   size_t bl = (branch != NULL) ? strlen(branch) : 0;
   uint8_t buf[CP_ADMIN_PAYLOAD_BUF];
   if (1 + nl + 1 + ul + (bl ? 1 + bl : 0) > sizeof(buf)) {
      return ADMIN_RESP_FAILURE;
   }
   size_t off = 0;
   buf[off++] = (uint8_t)(global ? 0x01 : 0x00);
   memcpy(buf + off, name, nl);
   off += nl;
   buf[off++] = '\0';
   memcpy(buf + off, url, ul);
   off += ul;
   if (bl) {
      buf[off++] = '\0';
      memcpy(buf + off, branch, bl);
      off += bl;
   }
   if (send_message(fd, ADMIN_MSG_CODE_PROJ_IMPORT, (const char *)buf, (uint16_t)off) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

/* Send a "first\0second" pair payload under @p opcode. */
static admin_resp_code_t code_proj_pair(int fd,
                                        uint8_t opcode,
                                        const char *first,
                                        const char *second,
                                        char *response,
                                        size_t resp_len) {
   /* @first may be empty (e.g. link with a derived name); @second is required. */
   if (first == NULL || !second || !second[0]) {
      return ADMIN_RESP_FAILURE;
   }
   size_t fl = strlen(first);
   size_t sl = strlen(second);
   uint8_t buf[CP_ADMIN_PAYLOAD_BUF];
   if (fl + 1 + sl > sizeof(buf)) {
      return ADMIN_RESP_FAILURE;
   }
   memcpy(buf, first, fl);
   buf[fl] = '\0';
   memcpy(buf + fl + 1, second, sl);
   if (send_message(fd, opcode, (const char *)buf, (uint16_t)(fl + 1 + sl)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_ota_rollout_status(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_OTA_ROLLOUT_STATUS, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

static admin_resp_code_t code_proj_by_name(int fd,
                                           uint8_t opcode,
                                           const char *name,
                                           char *response,
                                           size_t resp_len) {
   if (!name || !name[0]) {
      return ADMIN_RESP_FAILURE;
   }
   if (send_message(fd, opcode, name, (uint16_t)strlen(name)) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_ota_rollout_abort(int fd, char *response, size_t resp_len) {
   if (send_message(fd, ADMIN_MSG_OTA_ROLLOUT_ABORT, NULL, 0) != 0) {
      return ADMIN_RESP_SERVICE_ERROR;
   }
   return recv_text_response(fd, response, resp_len);
}

admin_resp_code_t admin_client_code_proj_refresh(int fd,
                                                 const char *name,
                                                 char *response,
                                                 size_t resp_len) {
   return code_proj_by_name(fd, ADMIN_MSG_CODE_PROJ_REFRESH, name, response, resp_len);
}

admin_resp_code_t admin_client_code_proj_delete(int fd,
                                                const char *name,
                                                char *response,
                                                size_t resp_len) {
   return code_proj_by_name(fd, ADMIN_MSG_CODE_PROJ_DELETE, name, response, resp_len);
}

admin_resp_code_t admin_client_code_proj_rebuild(int fd,
                                                 const char *name,
                                                 char *response,
                                                 size_t resp_len) {
   return code_proj_by_name(fd, ADMIN_MSG_CODE_PROJ_REBUILD, name, response, resp_len);
}

admin_resp_code_t admin_client_code_proj_set_branch(int fd,
                                                    const char *name,
                                                    const char *branch,
                                                    char *response,
                                                    size_t resp_len) {
   return code_proj_pair(fd, ADMIN_MSG_CODE_PROJ_SET_BRANCH, name, branch, response, resp_len);
}

admin_resp_code_t admin_client_code_proj_link(int fd,
                                              const char *path,
                                              const char *name,
                                              char *response,
                                              size_t resp_len) {
   /* Wire: "name\0path"; name may be empty (daemon derives it from the basename). */
   return code_proj_pair(fd, ADMIN_MSG_CODE_PROJ_LINK, name != NULL ? name : "", path, response,
                         resp_len);
}
