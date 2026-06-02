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
 * Admin-socket handlers for code projects (ADMIN_MSG_CODE_PROJ_* 0xB5-0xB8):
 * list / import / refresh / delete. The admin socket is a privileged local
 * channel, so these are operator-level (no per-user gate).
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "auth/admin_socket.h"
#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "dawn_error.h"
#include "tools/code_project_db.h"
#include "tools/code_project_service.h"

#define CP_IMPORT_FLAG_GLOBAL 0x01

int handle_code_proj_list(int client_fd, const char *payload, uint16_t payload_len) {
   (void)payload;
   (void)payload_len;
   code_project_t list[CODE_PROJECTS_MAX];
   int n = 0;
   if (code_project_db_list_all(list, CODE_PROJECTS_MAX, &n) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Failed to list projects");
   }
   if (n == 0) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No code projects.");
   }
   char buf[3072];
   int off = 0;
   for (int i = 0; i < n && off < (int)sizeof(buf); i++) {
      off += snprintf(buf + off, sizeof(buf) - off, "%s\t%s%s\t%s\n", list[i].name, list[i].status,
                      list[i].is_global ? "\t[global]" : "", list[i].source_url);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

int handle_code_proj_import(int client_fd, const char *payload, uint16_t payload_len) {
   /* Wire format: byte 0 = flags, then "name\0url". */
   if (payload == NULL || payload_len < 4) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid import payload");
   }
   uint8_t flags = (uint8_t)payload[0];
   const char *body = payload + 1;
   uint16_t body_len = payload_len - 1;

   size_t sep = 0;
   while (sep < body_len && body[sep] != '\0') {
      sep++;
   }
   if (sep == 0 || sep >= body_len) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Usage: import <url> [--name n]");
   }
   char name[CODE_PROJECT_NAME_MAX];
   char url[CODE_PROJECT_URL_MAX];
   if (sep >= sizeof(name)) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Project name too long");
   }
   memcpy(name, body, sep);
   name[sep] = '\0';
   snprintf(url, sizeof(url), "%.*s", (int)(body_len - sep - 1), body + sep + 1);

   bool global = (flags & CP_IMPORT_FLAG_GLOBAL) != 0;
   int64_t id = 0;
   /* requester 0: operator import (imported_by recorded as NULL). */
   if (code_project_import(0, url, name, global, &id) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Import failed (invalid name/URL, duplicate, or disabled)");
   }
   char msg[160];
   snprintf(msg, sizeof(msg), "Importing '%s' (id %lld); clone + index queued.", name,
            (long long)id);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

static int by_name_op(int client_fd, const char *payload, uint16_t payload_len, bool is_delete) {
   if (payload == NULL || payload_len == 0 || payload_len >= CODE_PROJECT_NAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid project name");
   }
   char name[CODE_PROJECT_NAME_MAX];
   memcpy(name, payload, payload_len);
   name[payload_len] = '\0';

   code_project_t p;
   if (code_project_db_get_by_name(name, &p) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "No such project");
   }
   int rc = is_delete ? code_project_delete(p.id) : code_project_refresh(p.id);
   if (rc != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                is_delete ? "Delete failed" : "Refresh failed");
   }
   char msg[128];
   snprintf(msg, sizeof(msg), "%s '%s'.", is_delete ? "Deleted" : "Refresh queued for", name);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

int handle_code_proj_refresh(int client_fd, const char *payload, uint16_t payload_len) {
   return by_name_op(client_fd, payload, payload_len, false);
}

int handle_code_proj_delete(int client_fd, const char *payload, uint16_t payload_len) {
   return by_name_op(client_fd, payload, payload_len, true);
}
