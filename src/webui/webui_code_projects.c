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
 * WebUI Code Projects handlers (coding harness): list / import / refresh /
 * delete over the WebSocket. All access is scoped to the requesting user
 * (conn->auth_user_id); import obeys [code_projects] import_user_required and
 * only admins may create global projects or act on projects they do not own.
 */

#include "webui/webui_code_projects.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/code_project_db.h"
#include "tools/code_project_service.h"

/* Quiet admin check (no error response sent) — mirrors webui_doc_library.c. */
static bool conn_check_admin_quiet(ws_connection_t *conn) {
   if (!conn->authenticated) {
      return false;
   }
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      return false;
   }
   return session.is_admin;
}

/* Send a { ok, message } action result of the given response type. */
static void send_action_result(ws_connection_t *conn, const char *type, bool ok, const char *msg) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string(type));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "ok", json_object_new_boolean(ok));
   json_object_object_add(payload, "message", json_object_new_string(msg ? msg : ""));
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_code_projects_list(ws_connection_t *conn, json_object *payload) {
   (void)payload;
   if (!conn_require_auth(conn)) {
      return;
   }

   code_project_t list[CODE_PROJECTS_MAX];
   int n = 0;
   bool ok = (code_project_db_list_visible(conn->auth_user_id, list, CODE_PROJECTS_MAX, &n) ==
              AUTH_DB_SUCCESS);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("code_projects_list_response"));
   json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "ok", json_object_new_boolean(ok));

   json_object *projects = json_object_new_array();
   for (int i = 0; ok && i < n; i++) {
      json_object *p = json_object_new_object();
      json_object_object_add(p, "id", json_object_new_int64(list[i].id));
      json_object_object_add(p, "name", json_object_new_string(list[i].name));
      json_object_object_add(p, "status", json_object_new_string(list[i].status));
      json_object_object_add(p, "status_msg", json_object_new_string(list[i].status_msg));
      json_object_object_add(p, "is_global", json_object_new_boolean(list[i].is_global));
      json_object_object_add(p, "source_url", json_object_new_string(list[i].source_url));
      json_object_object_add(p, "indexed_at", json_object_new_int64(list[i].indexed_at));
      json_object_object_add(p, "owned",
                             json_object_new_boolean(list[i].user_id == conn->auth_user_id));
      json_object_array_add(projects, p);
   }
   json_object_object_add(resp_payload, "projects", projects);
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_code_projects_import(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   const dawn_config_t *cfg = config_get();
   if (cfg == NULL || !cfg->code_projects.enabled) {
      send_action_result(conn, "code_projects_import_response", false,
                         "Code projects are disabled.");
      return;
   }
   /* import_user_required = "admin" gates imports to admins. */
   if (strcmp(cfg->code_projects.import_user_required, "admin") == 0 &&
       !conn_check_admin_quiet(conn)) {
      send_action_result(conn, "code_projects_import_response", false,
                         "Importing projects is restricted to administrators.");
      return;
   }

   const char *url = NULL;
   const char *name = NULL;
   bool global = false;
   if (payload) {
      json_object *o;
      if (json_object_object_get_ex(payload, "url", &o)) {
         url = json_object_get_string(o);
      }
      if (json_object_object_get_ex(payload, "name", &o)) {
         name = json_object_get_string(o);
      }
      if (json_object_object_get_ex(payload, "global", &o)) {
         global = json_object_get_boolean(o);
      }
   }
   if (url == NULL || url[0] == '\0') {
      send_action_result(conn, "code_projects_import_response", false,
                         "A repository URL is required.");
      return;
   }
   /* Only admins may create projects shared with all users. */
   if (global && !conn_check_admin_quiet(conn)) {
      global = false;
   }

   /* Derive a name from the URL's last path component (strip .git) when absent. */
   char derived[CODE_PROJECT_NAME_MAX] = { 0 };
   if (name == NULL || name[0] == '\0') {
      const char *slash = strrchr(url, '/');
      const char *base = (slash != NULL) ? slash + 1 : url;
      snprintf(derived, sizeof(derived), "%s", base);
      char *dot = strstr(derived, ".git");
      if (dot != NULL) {
         *dot = '\0';
      }
      name = derived;
   }

   int64_t id = 0;
   if (code_project_import(conn->auth_user_id, url, name, global, &id) != SUCCESS) {
      send_action_result(conn, "code_projects_import_response", false,
                         "Import failed (invalid name/URL, duplicate, or blocked host).");
      return;
   }
   char msg[160];
   snprintf(msg, sizeof(msg), "Importing '%s'; clone and indexing started.", name);
   send_action_result(conn, "code_projects_import_response", true, msg);
}

/* Shared resolve-by-name + owner/admin gate for refresh/delete. Returns true and
 * fills *out when the caller may act on the named project; otherwise sends an
 * error result and returns false. */
static bool resolve_owned_or_admin(ws_connection_t *conn,
                                   json_object *payload,
                                   const char *resp_type,
                                   code_project_t *out) {
   const char *name = NULL;
   if (payload) {
      json_object *o;
      if (json_object_object_get_ex(payload, "name", &o)) {
         name = json_object_get_string(o);
      }
   }
   if (name == NULL || name[0] == '\0') {
      send_action_result(conn, resp_type, false, "A project name is required.");
      return false;
   }
   if (code_project_db_get_by_name(name, out) != AUTH_DB_SUCCESS) {
      send_action_result(conn, resp_type, false, "No such project.");
      return false;
   }
   if (out->user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
      send_action_result(conn, resp_type, false, "You can only manage projects you imported.");
      return false;
   }
   return true;
}

void handle_code_projects_refresh(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   code_project_t p;
   if (!resolve_owned_or_admin(conn, payload, "code_projects_action_response", &p)) {
      return;
   }
   bool ok = (code_project_refresh(p.id) == SUCCESS);
   send_action_result(conn, "code_projects_action_response", ok,
                      ok ? "Re-index queued." : "Refresh failed.");
}

void handle_code_projects_delete(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   code_project_t p;
   if (!resolve_owned_or_admin(conn, payload, "code_projects_action_response", &p)) {
      return;
   }
   bool ok = (code_project_delete(p.id) == SUCCESS);
   send_action_result(conn, "code_projects_action_response", ok,
                      ok ? "Project deleted." : "Delete failed.");
}
