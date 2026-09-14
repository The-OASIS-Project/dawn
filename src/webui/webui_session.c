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
 * WebUI Session Handlers - User session management
 *
 * This module handles WebSocket messages for:
 * - list_my_sessions (list user's active sessions)
 * - revoke_session (revoke a session by token prefix)
 */

#include <string.h>

#include "auth/auth_db.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

/* Callback for session enumeration */
static int list_sessions_callback(const auth_session_summary_t *session, void *context) {
   json_object *sessions_array = (json_object *)context;
   json_object *session_obj = json_object_new_object();

   json_object_object_add(session_obj, "token_prefix",
                          json_object_new_string(session->token_prefix));
   json_object_object_add(session_obj, "created_at", json_object_new_int64(session->created_at));
   json_object_object_add(session_obj, "last_activity",
                          json_object_new_int64(session->last_activity));
   json_object_object_add(session_obj, "ip_address", json_object_new_string(session->ip_address));
   json_object_object_add(session_obj, "user_agent", json_object_new_string(session->user_agent));

   json_object_array_add(sessions_array, session_obj);
   return 0;
}

/**
 * @brief List current user's active sessions
 *
 * Returns all sessions for the authenticated user, allowing them to see
 * where they're logged in and identify sessions to revoke.
 */
void handle_list_my_sessions(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_my_sessions_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *sessions_array = json_object_new_array();

   int result = auth_db_list_user_sessions(conn->auth_user_id, list_sessions_callback,
                                           sessions_array);
   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "sessions", sessions_array);

      /* Include current session's token prefix so UI can highlight it */
      char current_prefix[17] = { 0 };
      safe_strscpy(current_prefix, conn->auth_session_token);
      json_object_object_add(resp_payload, "current_session",
                             json_object_new_string(current_prefix));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list sessions"));
      json_object_put(sessions_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Revoke a session by token prefix
 *
 * Users can revoke their own sessions. Admins can revoke any session.
 * Cannot revoke your own current session (use logout instead).
 */
void handle_revoke_session(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("revoke_session_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get token prefix */
   json_object *prefix_obj;
   if (!json_object_object_get_ex(payload, "token_prefix", &prefix_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing token_prefix"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *prefix = json_object_get_string(prefix_obj);

   /* Validate prefix length (16 chars for reduced collision risk) */
   if (!prefix || strlen(prefix) < 16) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Invalid token prefix"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Check if trying to revoke current session */
   if (strncmp(conn->auth_session_token, prefix, 16) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot revoke current session - use logout"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* For non-admins, verify the session belongs to them by checking if it appears
    * in their session list. Admins can revoke any session.
    */
   bool is_admin = false;
   auth_session_t auth_session;
   if (auth_db_get_session(conn->auth_session_token, &auth_session) == AUTH_DB_SUCCESS) {
      is_admin = auth_session.is_admin;
   }

   if (!is_admin) {
      /* Verify ownership with efficient single-query check */
      if (!auth_db_session_belongs_to_user(prefix, conn->auth_user_id)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Session not found or access denied"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }
   }

   /* Delete the session */
   int result = auth_db_delete_session_by_prefix(prefix);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Session revoked"));

      /* Log event */
      char details[128];
      snprintf(details, sizeof(details), "Revoked session: %.8s...", prefix);
      auth_db_log_event("SESSION_REVOKED", conn->username, conn->client_ip, details);

      /* Proactively notify any connected client with this auth token */
      webui_force_logout_by_auth_token(prefix);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Session not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to revoke session"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
