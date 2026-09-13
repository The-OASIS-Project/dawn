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
 * WebUI Email Account Management — per-user IMAP/SMTP account CRUD.
 */

#include "webui/webui_email.h"

#include <json-c/json.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/email_db.h"
#include "tools/email_service.h"
#include "tools/oauth_client.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Helper: verify account belongs to the requesting user
 * ============================================================================= */

static bool verify_account_owner(ws_connection_t *conn, int64_t account_id, email_account_t *out) {
   if (email_db_account_get(account_id, out) != 0)
      return false;
   return out->user_id == conn->auth_user_id;
}

/* =============================================================================
 * List Accounts
 * ============================================================================= */

void handle_email_list_accounts(ws_connection_t *conn) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("email_list_accounts_response"));

   json_object *resp_payload = json_object_new_object();

   email_account_t accounts[32];
   int count = email_service_list_accounts(conn->auth_user_id, accounts, 32);

   if (count < 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list accounts"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      json_object *arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *obj = json_object_new_object();
         json_object_object_add(obj, "id", json_object_new_int64(accounts[i].id));
         json_object_object_add(obj, "name", json_object_new_string(accounts[i].name));
         json_object_object_add(obj, "imap_server",
                                json_object_new_string(accounts[i].imap_server));
         json_object_object_add(obj, "imap_port", json_object_new_int(accounts[i].imap_port));
         json_object_object_add(obj, "imap_ssl", json_object_new_boolean(accounts[i].imap_ssl));
         json_object_object_add(obj, "smtp_server",
                                json_object_new_string(accounts[i].smtp_server));
         json_object_object_add(obj, "smtp_port", json_object_new_int(accounts[i].smtp_port));
         json_object_object_add(obj, "smtp_ssl", json_object_new_boolean(accounts[i].smtp_ssl));
         json_object_object_add(obj, "username", json_object_new_string(accounts[i].username));
         json_object_object_add(obj, "display_name",
                                json_object_new_string(accounts[i].display_name));
         json_object_object_add(obj, "has_password",
                                json_object_new_boolean(accounts[i].encrypted_password_len > 0));
         json_object_object_add(obj, "auth_type", json_object_new_string(accounts[i].auth_type));
         json_object_object_add(obj, "oauth_account_key",
                                json_object_new_string(accounts[i].oauth_account_key));
         json_object_object_add(obj, "enabled", json_object_new_boolean(accounts[i].enabled));
         json_object_object_add(obj, "read_only", json_object_new_boolean(accounts[i].read_only));
         json_object_object_add(obj, "max_recent", json_object_new_int(accounts[i].max_recent));
         json_object_object_add(obj, "max_body_chars",
                                json_object_new_int(accounts[i].max_body_chars));
         json_object_array_add(arr, obj);
      }
      json_object_object_add(resp_payload, "accounts", arr);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Add Account
 * ============================================================================= */

void handle_email_add_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("email_add_account_response"));

   json_object *resp_payload = json_object_new_object();

   /* Extract auth_type first to determine required fields */
   json_object *auth_type_obj = NULL;
   json_object_object_get_ex(payload, "auth_type", &auth_type_obj);
   const char *auth_type = auth_type_obj ? json_object_get_string(auth_type_obj) : "app_password";
   bool is_oauth = strcmp(auth_type, "oauth") == 0;

   json_object *name_obj, *imap_obj, *smtp_obj, *user_obj, *pass_obj;
   bool has_name = json_object_object_get_ex(payload, "name", &name_obj);
   bool has_imap = json_object_object_get_ex(payload, "imap_server", &imap_obj);
   bool has_smtp = json_object_object_get_ex(payload, "smtp_server", &smtp_obj);
   bool has_user = json_object_object_get_ex(payload, "username", &user_obj);
   bool has_pass = json_object_object_get_ex(payload, "password", &pass_obj);

   /* Reject password over non-TLS */
   if (has_pass && !g_config.webui.https) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "Cannot send password over unencrypted connection. "
                                 "Enable HTTPS in webui configuration."));
      goto done;
   }

   bool valid = has_name && (is_oauth || (has_imap && has_smtp && has_user && has_pass));

   if (!valid) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 is_oauth ? "Missing required field: name"
                                          : "Missing required fields: name, imap_server, "
                                            "smtp_server, username, password"));
   } else {
      json_object *ro_obj, *imap_port_obj, *imap_ssl_obj;
      json_object *smtp_port_obj, *smtp_ssl_obj, *display_name_obj, *oauth_key_obj;

      bool read_only = false;
      if (json_object_object_get_ex(payload, "read_only", &ro_obj))
         read_only = json_object_get_boolean(ro_obj);

      int imap_port = 993;
      if (json_object_object_get_ex(payload, "imap_port", &imap_port_obj))
         imap_port = json_object_get_int(imap_port_obj);

      bool imap_ssl = true;
      if (json_object_object_get_ex(payload, "imap_ssl", &imap_ssl_obj))
         imap_ssl = json_object_get_boolean(imap_ssl_obj);

      int smtp_port = 465;
      if (json_object_object_get_ex(payload, "smtp_port", &smtp_port_obj))
         smtp_port = json_object_get_int(smtp_port_obj);

      bool smtp_ssl = true;
      if (json_object_object_get_ex(payload, "smtp_ssl", &smtp_ssl_obj))
         smtp_ssl = json_object_get_boolean(smtp_ssl_obj);

      const char *display_name = "";
      if (json_object_object_get_ex(payload, "display_name", &display_name_obj))
         display_name = json_object_get_string(display_name_obj);

      const char *oauth_key = "";
      if (json_object_object_get_ex(payload, "oauth_account_key", &oauth_key_obj))
         oauth_key = json_object_get_string(oauth_key_obj);

      int rc = email_service_add_account(
          conn->auth_user_id, json_object_get_string(name_obj),
          has_imap ? json_object_get_string(imap_obj) : "", imap_port, imap_ssl,
          has_smtp ? json_object_get_string(smtp_obj) : "", smtp_port, smtp_ssl,
          has_user ? json_object_get_string(user_obj) : "", display_name,
          has_pass ? json_object_get_string(pass_obj) : "", read_only, auth_type, oauth_key);

      if (rc == EMAIL_ADD_RC_DUPLICATE) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account already exists"));
      } else if (rc != EMAIL_RC_OK) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Failed to add account"));
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      }
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Update Account
 * ============================================================================= */

void handle_email_update_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("email_update_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account id"));
      goto done;
   }

   int64_t account_id = json_object_get_int64(id_obj);

   email_account_t acct;
   if (!verify_account_owner(conn, account_id, &acct)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Account not found or access denied"));
      goto done;
   }

   /* Update fields if provided */
   json_object *val;
   if (json_object_object_get_ex(payload, "name", &val))
      snprintf(acct.name, sizeof(acct.name), "%s", json_object_get_string(val));
   if (json_object_object_get_ex(payload, "imap_server", &val))
      snprintf(acct.imap_server, sizeof(acct.imap_server), "%s", json_object_get_string(val));
   if (json_object_object_get_ex(payload, "imap_port", &val))
      acct.imap_port = json_object_get_int(val);
   if (json_object_object_get_ex(payload, "imap_ssl", &val))
      acct.imap_ssl = json_object_get_boolean(val);
   if (json_object_object_get_ex(payload, "smtp_server", &val))
      snprintf(acct.smtp_server, sizeof(acct.smtp_server), "%s", json_object_get_string(val));
   if (json_object_object_get_ex(payload, "smtp_port", &val))
      acct.smtp_port = json_object_get_int(val);
   if (json_object_object_get_ex(payload, "smtp_ssl", &val))
      acct.smtp_ssl = json_object_get_boolean(val);
   if (json_object_object_get_ex(payload, "username", &val))
      snprintf(acct.username, sizeof(acct.username), "%s", json_object_get_string(val));
   if (json_object_object_get_ex(payload, "display_name", &val))
      snprintf(acct.display_name, sizeof(acct.display_name), "%s", json_object_get_string(val));
   if (json_object_object_get_ex(payload, "max_recent", &val)) {
      int mr = json_object_get_int(val);
      if (mr >= 1 && mr <= 50)
         acct.max_recent = mr;
   }
   if (json_object_object_get_ex(payload, "max_body_chars", &val)) {
      int mb = json_object_get_int(val);
      if (mb >= EMAIL_MIN_READ_BODY_LEN && mb <= EMAIL_MAX_READ_BODY_LEN)
         acct.max_body_chars = mb;
   }

   /* Only re-encrypt password if a new one was provided */
   json_object *pass_obj;
   if (json_object_object_get_ex(payload, "password", &pass_obj)) {
      if (!g_config.webui.https) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Cannot send password over unencrypted "
                                                       "connection. Enable HTTPS."));
         goto done;
      }
      const char *new_pass = json_object_get_string(pass_obj);
      if (new_pass && new_pass[0]) {
         if (email_encrypt_password(new_pass, &acct) != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Failed to encrypt password"));
            goto done;
         }
      }
   }

   if (email_db_account_update(&acct) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to update account"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Remove Account
 * ============================================================================= */

void handle_email_remove_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("email_remove_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account id"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      email_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = email_service_remove_account(account_id);
         if (rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Failed to remove account"));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Test Connection
 * ============================================================================= */

void handle_email_test_connection(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("email_test_connection_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account id"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      email_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         bool imap_ok = false, smtp_ok = false;
         email_service_test_connection(account_id, &imap_ok, &smtp_ok);

         json_object_object_add(resp_payload, "imap_ok", json_object_new_boolean(imap_ok));
         json_object_object_add(resp_payload, "smtp_ok", json_object_new_boolean(smtp_ok));

         if (imap_ok && smtp_ok) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            char err[256];
            if (!imap_ok && !smtp_ok)
               snprintf(err, sizeof(err), "Both IMAP and SMTP connection failed");
            else if (!imap_ok)
               snprintf(err, sizeof(err), "IMAP connection failed (SMTP OK)");
            else
               snprintf(err, sizeof(err), "SMTP connection failed (IMAP OK)");
            json_object_object_add(resp_payload, "error", json_object_new_string(err));
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Set Read-Only
 * ============================================================================= */

void handle_email_set_read_only(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("email_set_read_only_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *ro_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "read_only", &ro_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing id or read_only"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);
      bool read_only = json_object_get_boolean(ro_obj);

      email_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = email_db_account_set_read_only(account_id, read_only);
         if (rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Failed to update read-only flag"));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Set Enabled
 * ============================================================================= */

void handle_email_set_enabled(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("email_set_enabled_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *en_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "enabled", &en_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing id or enabled"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);
      bool enabled = json_object_get_boolean(en_obj);

      email_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = email_db_account_set_enabled(account_id, enabled);
         if (rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Failed to update enabled flag"));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
