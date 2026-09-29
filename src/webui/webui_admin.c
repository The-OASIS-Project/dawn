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
 * WebUI Admin Handlers - User management endpoints (admin-only)
 *
 * This module handles admin-only WebSocket messages for user management:
 * - list_users, create_user, delete_user
 * - change_password, unlock_user
 */

#include <string.h>
#include <time.h>

#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "auth/auth_db_withdraw.h"
#include "core/session_prefix.h"
#include "logging.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * User Management Handlers (Admin-only)
 * ============================================================================= */

/* Callback for user list enumeration */
static int list_users_callback(const auth_user_summary_t *user, void *context) {
   json_object *users_array = (json_object *)context;
   json_object *user_obj = json_object_new_object();
   json_object_object_add(user_obj, "id", json_object_new_int(user->id));
   json_object_object_add(user_obj, "username", json_object_new_string(user->username));
   json_object_object_add(user_obj, "is_admin", json_object_new_boolean(user->is_admin));
   json_object_object_add(user_obj, "created_at", json_object_new_int64(user->created_at));
   json_object_object_add(user_obj, "last_login", json_object_new_int64(user->last_login));
   json_object_object_add(user_obj, "failed_attempts", json_object_new_int(user->failed_attempts));
   json_object_object_add(user_obj, "is_locked",
                          json_object_new_boolean(user->lockout_until > time(NULL)));
   json_object_array_add(users_array, user_obj);
   return 0;
}

/**
 * @brief List all users (admin only)
 */
void handle_list_users(ws_connection_t *conn) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_users_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *users_array = json_object_new_array();

   int result = auth_db_list_users(list_users_callback, users_array);
   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "users", users_array);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Failed to list users"));
      json_object_put(users_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Create a new user (admin only)
 */
void handle_create_user(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("create_user_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get required fields */
   json_object *username_obj, *password_obj, *is_admin_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj) ||
       !json_object_object_get_ex(payload, "password", &password_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing username or password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);
   const char *password = json_object_get_string(password_obj);
   bool is_admin = json_object_object_get_ex(payload, "is_admin", &is_admin_obj)
                       ? json_object_get_boolean(is_admin_obj)
                       : false;

   /* Validate username format */
   if (auth_db_validate_username(username) != AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid username format"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Validate password length */
   if (!password || strlen(password) < 8) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Password must be at least 8 characters"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Hash password */
   char hash[AUTH_HASH_LEN];
   if (auth_hash_password(password, hash) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to hash password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Create user */
   int result = auth_db_create_user(username, hash, is_admin);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("User created"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Created user '%s' (admin=%s) by '%s'", username,
               is_admin ? "yes" : "no", conn->username);
      auth_db_log_event("USER_CREATED", username, conn->client_ip, details);
      OLOG_INFO("WebUI: %s", details);
   } else if (result == AUTH_DB_DUPLICATE) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Username already exists"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to create user"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a user (admin only)
 */
void handle_delete_user(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_user_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *username_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing username"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);

   /* Prevent self-deletion */
   if (strcmp(username, conn->username) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot delete your own account"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* The account's documents (a shared one is in other users' conversations)
    * and memories go with it: removed on its behalf, and withdrawn. */
   auth_user_t target;
   const int target_id = auth_db_get_user(username, &target) == AUTH_DB_SUCCESS ? target.id : 0;
   conv_db_withdraw_intent_begin(target_id);
   int result = auth_db_delete_user(username);
   conv_db_withdraw_intent_end();
   if (result == AUTH_DB_SUCCESS && target_id > 0) {
      session_withdraw_forgotten_async(target_id, false);
   }

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("User deleted"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Deleted by '%s'", conn->username);
      auth_db_log_event("USER_DELETED", username, conn->client_ip, details);
      OLOG_INFO("WebUI: User '%s' deleted by '%s'", username, conn->username);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("User not found"));
   } else if (result == AUTH_DB_LAST_ADMIN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot delete last admin user"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete user"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Change user password (admin for any user, or user for self with current password)
 */
void handle_change_password(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("change_password_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *username_obj, *new_password_obj, *current_password_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj) ||
       !json_object_object_get_ex(payload, "new_password", &new_password_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing username or new_password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);
   const char *new_password = json_object_get_string(new_password_obj);
   bool is_self_change = (strcmp(username, conn->username) == 0);

   /* Check permissions: admin can change any password, user can only change own */
   auth_session_t session;
   bool is_admin = false;
   if (auth_db_get_session(conn->auth_session_token, &session) == AUTH_DB_SUCCESS) {
      is_admin = session.is_admin;
   }

   if (!is_admin && !is_self_change) {
      /* Non-admin trying to change someone else's password */
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot change another user's password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Non-admin self-change requires current password verification */
   if (!is_admin && is_self_change) {
      if (!json_object_object_get_ex(payload, "current_password", &current_password_obj)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Current password required"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      const char *current_password = json_object_get_string(current_password_obj);
      auth_user_t user;
      if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS ||
          auth_verify_password(current_password, user.password_hash) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Current password incorrect"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }
   }

   /* Validate new password length */
   if (!new_password || strlen(new_password) < 8) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("New password must be at least 8 characters"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Hash new password */
   char hash[AUTH_HASH_LEN];
   if (auth_hash_password(new_password, hash) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to hash password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Update password (this also invalidates all sessions) */
   int result = auth_db_update_password(username, hash);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Password changed"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Password changed by '%s'", conn->username);
      auth_db_log_event("PASSWORD_CHANGED", username, conn->client_ip, details);
      OLOG_INFO("WebUI: Password changed for '%s' by '%s'", username, conn->username);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("User not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to change password"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Unlock a locked user account (admin only)
 */
void handle_unlock_user(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("unlock_user_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *username_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing username"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);

   /* Unlock user and reset failed attempts */
   int result = auth_db_unlock_user(username);
   if (result == AUTH_DB_SUCCESS) {
      auth_db_reset_failed_attempts(username);
   }

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("User unlocked"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Unlocked by '%s'", conn->username);
      auth_db_log_event("USER_UNLOCKED", username, conn->client_ip, details);
      OLOG_INFO("WebUI: User '%s' unlocked by '%s'", username, conn->username);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("User not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to unlock user"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
