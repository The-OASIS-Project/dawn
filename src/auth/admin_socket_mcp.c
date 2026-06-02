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
 * Admin-socket handlers for the MCP bridge (ADMIN_MSG_MCP_* 0xB0-0xB4):
 * list/status, grant/revoke per-user access, and reset (reconnect).
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "auth/admin_socket.h"
#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "auth/auth_db_mcp.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/mcp_bridge.h"

/* Parse a "<username>\0<alias>" payload into separate buffers. */
static int parse_user_alias(const char *payload,
                            uint16_t len,
                            char *user,
                            size_t user_sz,
                            char *alias,
                            size_t alias_sz) {
   if (payload == NULL) {
      return FAILURE;
   }
   size_t sep = 0;
   while (sep < len && payload[sep] != '\0') {
      sep++;
   }
   if (sep == 0 || sep >= len || sep >= user_sz) {
      return FAILURE; /* empty username or no separator */
   }
   memcpy(user, payload, sep);
   user[sep] = '\0';

   size_t alias_len = (size_t)len - sep - 1;
   if (alias_len == 0 || alias_len >= alias_sz) {
      return FAILURE;
   }
   memcpy(alias, payload + sep + 1, alias_len);
   alias[alias_len] = '\0';
   return SUCCESS;
}

int handle_mcp_list(int client_fd, const char *payload, uint16_t payload_len) {
   (void)payload;
   (void)payload_len;
   char buf[2048];
   mcp_bridge_status_text(buf, sizeof(buf), NULL);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

int handle_mcp_status(int client_fd, const char *payload, uint16_t payload_len) {
   return handle_mcp_list(client_fd, payload, payload_len);
}

int handle_mcp_grant(int client_fd, const char *payload, uint16_t payload_len) {
   char user[AUTH_USERNAME_MAX];
   char alias[MCP_SERVER_ALIAS_MAX];
   if (parse_user_alias(payload, payload_len, user, sizeof(user), alias, sizeof(alias)) !=
       SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Usage: mcp grant <user> <alias>");
   }
   auth_user_t u;
   if (auth_db_get_user(user, &u) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }
   if (auth_db_mcp_grant(u.id, alias) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Grant failed");
   }
   char msg[AUTH_USERNAME_MAX + MCP_SERVER_ALIAS_MAX + 48];
   snprintf(msg, sizeof(msg), "Granted '%s' access to MCP server '%s'", user, alias);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

int handle_mcp_revoke(int client_fd, const char *payload, uint16_t payload_len) {
   char user[AUTH_USERNAME_MAX];
   char alias[MCP_SERVER_ALIAS_MAX];
   if (parse_user_alias(payload, payload_len, user, sizeof(user), alias, sizeof(alias)) !=
       SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Usage: mcp revoke <user> <alias>");
   }
   auth_user_t u;
   if (auth_db_get_user(user, &u) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }
   if (auth_db_mcp_revoke(u.id, alias) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Revoke failed");
   }
   char msg[AUTH_USERNAME_MAX + MCP_SERVER_ALIAS_MAX + 48];
   snprintf(msg, sizeof(msg), "Revoked '%s' access to MCP server '%s'", user, alias);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

int handle_mcp_reset(int client_fd, const char *payload, uint16_t payload_len) {
   (void)payload;
   (void)payload_len;
   int connected = 0;
   mcp_bridge_reconnect(&connected);
   char msg[96];
   snprintf(msg, sizeof(msg), "MCP reset: %d server(s) connected", connected);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}
