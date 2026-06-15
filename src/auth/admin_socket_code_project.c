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
 * Admin-socket handlers for code projects (ADMIN_MSG_CODE_PROJ_* 0xD0-0xD6):
 * list / import / refresh / delete / rebuild / link / set-branch. The admin
 * socket is a privileged local channel, so these are operator-level (no per-user
 * gate).
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
/* List response buffer: one tab-separated line per project (name + kind + branch
 * + status + source/path); sized for the full visible set at CODE_PROJECTS_MAX. */
#define CP_LIST_BUF_MAX 8192

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
   char buf[CP_LIST_BUF_MAX];
   int off = 0;
   for (int i = 0; i < n && off < (int)sizeof(buf); i++) {
      const char *loc = list[i].source_url[0] != '\0' ? list[i].source_url : list[i].local_path;
      off += snprintf(buf + off, sizeof(buf) - off, "%s\t%s\t%s\t%s%s\t%s\n", list[i].name,
                      list[i].kind, list[i].branch[0] != '\0' ? list[i].branch : "-",
                      list[i].status, list[i].is_global ? "\t[global]" : "", loc);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

int handle_code_proj_import(int client_fd, const char *payload, uint16_t payload_len) {
   /* Wire format: byte 0 = flags, then "name\0url[\0branch]" (branch optional). */
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
   char branch[CODE_PROJECT_BRANCH_MAX] = { 0 };
   if (sep >= sizeof(name)) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Project name too long");
   }
   memcpy(name, body, sep);
   name[sep] = '\0';
   /* url runs to the next NUL (if any), then an optional branch follows. */
   const char *url_start = body + sep + 1;
   uint16_t rest_len = (uint16_t)(body_len - sep - 1);
   size_t usep = 0;
   while (usep < rest_len && url_start[usep] != '\0') {
      usep++;
   }
   snprintf(url, sizeof(url), "%.*s", (int)usep, url_start);
   if (usep < rest_len) {
      snprintf(branch, sizeof(branch), "%.*s", (int)(rest_len - usep - 1), url_start + usep + 1);
   }

   bool global = (flags & CP_IMPORT_FLAG_GLOBAL) != 0;
   int64_t id = 0; /* unused: the row is created by the worker after it validates */
   /* requester 0: operator import (imported_by recorded as NULL). The remote is
    * probed on the worker thread before a row is created, so no id is returned
    * here — the project appears in `list` once the repo is confirmed to exist. */
   if (code_project_import(0, url, name, branch[0] ? branch : NULL, global, &id) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Import rejected (invalid name/URL, duplicate, or disabled)");
   }
   char msg[160];
   snprintf(msg, sizeof(msg),
            "Checking '%s'; if the repo exists it will be cloned + indexed (run `list`).", name);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

typedef enum {
   CP_ADMIN_REFRESH,
   CP_ADMIN_REBUILD,
   CP_ADMIN_DELETE
} cp_admin_op_t;

static int by_name_op(int client_fd, const char *payload, uint16_t payload_len, cp_admin_op_t op) {
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
   int rc;
   const char *verb;
   const char *failmsg;
   switch (op) {
      case CP_ADMIN_DELETE:
         rc = code_project_delete(p.id);
         verb = "Deleted";
         failmsg = "Delete failed";
         break;
      case CP_ADMIN_REBUILD:
         rc = code_project_rebuild(p.id);
         verb = "Rebuild queued for";
         failmsg = "Rebuild failed";
         break;
      default:
         rc = code_project_refresh(p.id);
         verb = "Refresh queued for";
         failmsg = "Refresh failed";
         break;
   }
   if (rc != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, failmsg);
   }
   char msg[128];
   snprintf(msg, sizeof(msg), "%s '%s'.", verb, name);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

int handle_code_proj_refresh(int client_fd, const char *payload, uint16_t payload_len) {
   return by_name_op(client_fd, payload, payload_len, CP_ADMIN_REFRESH);
}

int handle_code_proj_delete(int client_fd, const char *payload, uint16_t payload_len) {
   return by_name_op(client_fd, payload, payload_len, CP_ADMIN_DELETE);
}

int handle_code_proj_rebuild(int client_fd, const char *payload, uint16_t payload_len) {
   return by_name_op(client_fd, payload, payload_len, CP_ADMIN_REBUILD);
}

/* Split a "first\0second" payload into two NUL-terminated parts. Returns false if
 * the separator is missing or the first part overflows @p first_sz. @p second may
 * be empty. */
static bool split_pair(const char *payload,
                       uint16_t payload_len,
                       char *first,
                       size_t first_sz,
                       char *second,
                       size_t second_sz) {
   if (payload == NULL || payload_len == 0) {
      return false;
   }
   size_t sep = 0;
   while (sep < payload_len && payload[sep] != '\0') {
      sep++;
   }
   if (sep >= payload_len || sep >= first_sz) {
      return false; /* no NUL separator, or first part too long */
   }
   memcpy(first, payload, sep);
   first[sep] = '\0';
   snprintf(second, second_sz, "%.*s", (int)(payload_len - sep - 1), payload + sep + 1);
   return true;
}

int handle_code_proj_set_branch(int client_fd, const char *payload, uint16_t payload_len) {
   char name[CODE_PROJECT_NAME_MAX];
   char branch[CODE_PROJECT_BRANCH_MAX];
   if (!split_pair(payload, payload_len, name, sizeof(name), branch, sizeof(branch)) ||
       branch[0] == '\0') {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Usage: set-branch <name> <branch>");
   }
   code_project_t p;
   if (code_project_db_get_by_name(name, &p) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "No such project");
   }
   if (code_project_set_branch(p.id, branch) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Set branch failed (linked repos track their live checkout)");
   }
   /* name (<=63) + branch (<=127) + fixed text — size to hold the longest case. */
   char msg[CODE_PROJECT_NAME_MAX + CODE_PROJECT_BRANCH_MAX + 48];
   snprintf(msg, sizeof(msg), "Branch of '%s' set to '%s'; rebuild queued.", name, branch);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

int handle_code_proj_link(int client_fd, const char *payload, uint16_t payload_len) {
   /* Wire format: "name\0path"; name may be empty (derive from the path basename). */
   char name[CODE_PROJECT_NAME_MAX];
   char path[CODE_PROJECT_PATH_MAX];
   if (!split_pair(payload, payload_len, name, sizeof(name), path, sizeof(path)) ||
       path[0] == '\0') {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Usage: link <path> [--name n]");
   }
   if (name[0] == '\0') {
      const char *slash = strrchr(path, '/');
      const char *base = (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
      snprintf(name, sizeof(name), "%s", base);
   }
   int64_t id = 0;
   if (code_project_link(0, path, name, false, &id) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Link rejected (path not in allowed_local_roots, not a git repo, "
                                "duplicate name, or disabled)");
   }
   char msg[160];
   snprintf(msg, sizeof(msg), "Linked '%s'; indexing (run `list`).", name);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}
