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
 * WebUI Calendar Account Management — per-user CalDAV account CRUD.
 * Each user manages their own CalDAV accounts (not admin-only).
 */

#include "webui/webui_calendar.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/calendar_db.h"
#include "tools/calendar_service.h"
#include "webui/webui_internal.h"

/* Upcoming-events pull (read a window of occurrences for a calendar panel). */
#define CAL_UPCOMING_MAX 256                      /* result cap; ordered by start, drops farthest */
#define CAL_UPCOMING_DEFAULT_DAYS 7               /* {days} convenience-path default */
#define CAL_UPCOMING_MAX_DAYS 90                  /* {days} convenience-path clamp */
#define CAL_UPCOMING_MAX_SPAN_SEC (366L * 86400L) /* explicit {start,end} span ceiling */

/* =============================================================================
 * Helper: verify account belongs to the requesting user
 * ============================================================================= */

static bool verify_account_owner(ws_connection_t *conn,
                                 int64_t account_id,
                                 calendar_account_t *out) {
   if (calendar_db_account_get(account_id, out) != 0)
      return false;
   return out->user_id == conn->auth_user_id;
}

/* =============================================================================
 * List Accounts
 * ============================================================================= */

void handle_calendar_list_accounts(ws_connection_t *conn) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_list_accounts_response"));

   json_object *resp_payload = json_object_new_object();

   calendar_account_t accounts[32];
   int count = 0;
   int list_rc = calendar_db_account_list(conn->auth_user_id, accounts, 32, &count);

   if (list_rc != 0) {
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
         json_object_object_add(obj, "caldav_url", json_object_new_string(accounts[i].caldav_url));
         json_object_object_add(obj, "username", json_object_new_string(accounts[i].username));
         json_object_object_add(obj, "has_password",
                                json_object_new_boolean(accounts[i].encrypted_password_len > 0));
         json_object_object_add(obj, "auth_type", json_object_new_string(accounts[i].auth_type));
         json_object_object_add(obj, "oauth_account_key",
                                json_object_new_string(accounts[i].oauth_account_key));
         json_object_object_add(obj, "enabled", json_object_new_boolean(accounts[i].enabled));
         json_object_object_add(obj, "read_only", json_object_new_boolean(accounts[i].read_only));
         json_object_object_add(obj, "last_sync",
                                json_object_new_int64((int64_t)accounts[i].last_sync));
         json_object_object_add(obj, "principal_url",
                                json_object_new_string(accounts[i].principal_url));
         json_object_object_add(obj, "calendar_home_url",
                                json_object_new_string(accounts[i].calendar_home_url));
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

void handle_calendar_add_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_add_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *name_obj, *url_obj, *user_obj, *pass_obj;
   json_object *auth_type_obj = NULL, *oauth_key_obj = NULL;
   bool has_name = json_object_object_get_ex(payload, "name", &name_obj);
   bool has_url = json_object_object_get_ex(payload, "caldav_url", &url_obj);
   bool has_user = json_object_object_get_ex(payload, "username", &user_obj);
   bool has_pass = json_object_object_get_ex(payload, "password", &pass_obj);
   json_object_object_get_ex(payload, "auth_type", &auth_type_obj);
   json_object_object_get_ex(payload, "oauth_account_key", &oauth_key_obj);

   const char *auth_type = auth_type_obj ? json_object_get_string(auth_type_obj) : "basic";
   bool is_oauth = strcmp(auth_type, "oauth") == 0;

   /* Reject password over non-TLS */
   if (has_pass && !g_config.webui.https) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "Cannot send password over unencrypted connection. "
                                 "Enable HTTPS in webui configuration."));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* OAuth accounts need name + url; basic accounts need all four fields */
   bool valid = has_name && has_url && (is_oauth || (has_user && has_pass));

   if (!valid) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 is_oauth ? "Missing required fields: name, caldav_url"
                                          : "Missing required fields: name, caldav_url, "
                                            "username, password"));
   } else {
      json_object *ro_obj;
      bool read_only = false;
      if (json_object_object_get_ex(payload, "read_only", &ro_obj))
         read_only = json_object_get_boolean(ro_obj);

      const char *oauth_key = oauth_key_obj ? json_object_get_string(oauth_key_obj) : "";

      int rc = calendar_service_add_account(conn->auth_user_id, json_object_get_string(name_obj),
                                            json_object_get_string(url_obj),
                                            has_user ? json_object_get_string(user_obj) : "",
                                            has_pass ? json_object_get_string(pass_obj) : "",
                                            read_only, auth_type, oauth_key);
      if (rc == 2) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account already exists"));
      } else if (rc != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Failed to add account"));
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Remove Account
 * ============================================================================= */

void handle_calendar_remove_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_remove_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account id"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      /* Verify user owns this account */
      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         /* ON DELETE CASCADE handles calendar/event cleanup */
         int rc = calendar_service_remove_account(account_id);
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
 * Test Account Connection
 * ============================================================================= */

void handle_calendar_test_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_test_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account id"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = calendar_service_test_connection(account_id);
         if (rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Connection test failed — check URL, "
                                                          "username, and password"));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "message",
                                   json_object_new_string(
                                       "Connection successful, calendars discovered"));
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Sync Account
 * ============================================================================= */

void handle_calendar_sync_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_sync_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account id"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = calendar_service_sync_now(account_id);
         if (rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error", json_object_new_string("Sync failed"));
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
 * List Calendars (for an account)
 * ============================================================================= */

void handle_calendar_list_calendars(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_list_calendars_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "account_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account_id"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         calendar_calendar_t cals[32];
         int count = 0;
         int cal_list_rc = calendar_db_calendar_list(account_id, cals, 32, &count);

         if (cal_list_rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Failed to list calendars"));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

            json_object *arr = json_object_new_array();
            for (int i = 0; i < count; i++) {
               json_object *obj = json_object_new_object();
               json_object_object_add(obj, "id", json_object_new_int64(cals[i].id));
               json_object_object_add(obj, "display_name",
                                      json_object_new_string(cals[i].display_name));
               json_object_object_add(obj, "color", json_object_new_string(cals[i].color));
               json_object_object_add(obj, "is_active", json_object_new_boolean(cals[i].is_active));
               json_object_object_add(obj, "caldav_path",
                                      json_object_new_string(cals[i].caldav_path));
               json_object_array_add(arr, obj);
            }
            json_object_object_add(resp_payload, "calendars", arr);
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Toggle Calendar Active State
 * ============================================================================= */

void handle_calendar_toggle_calendar(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_toggle_calendar_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *active_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_active", &active_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing id or is_active"));
   } else {
      int64_t cal_id = json_object_get_int64(id_obj);
      bool active = json_object_get_boolean(active_obj);

      /* Verify the calendar belongs to an account owned by this user */
      calendar_calendar_t cal;
      if (calendar_db_calendar_get(cal_id, &cal) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Calendar not found"));
      } else {
         calendar_account_t acct;
         if (!verify_account_owner(conn, cal.account_id, &acct)) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error", json_object_new_string("Access denied"));
         } else {
            int rc = calendar_db_calendar_set_active(cal_id, active);
            if (rc != 0) {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
               json_object_object_add(resp_payload, "error",
                                      json_object_new_string("Failed to toggle calendar"));
            } else {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            }
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Edit Account
 * ============================================================================= */

void handle_calendar_edit_account(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_edit_account_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *name_obj, *url_obj, *user_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "name", &name_obj) ||
       !json_object_object_get_ex(payload, "caldav_url", &url_obj) ||
       !json_object_object_get_ex(payload, "username", &user_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "Missing required fields: id, name, caldav_url, username"));
   } else {
      int64_t account_id = json_object_get_int64(id_obj);

      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         /* Update basic fields */
         snprintf(acct.name, sizeof(acct.name), "%s", json_object_get_string(name_obj));
         snprintf(acct.caldav_url, sizeof(acct.caldav_url), "%s", json_object_get_string(url_obj));
         snprintf(acct.username, sizeof(acct.username), "%s", json_object_get_string(user_obj));

         /* Only re-encrypt password if a new one was provided */
         bool encrypt_ok = true;
         json_object *pass_obj;
         if (json_object_object_get_ex(payload, "password", &pass_obj)) {
            if (!g_config.webui.https) {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
               json_object_object_add(resp_payload, "error",
                                      json_object_new_string(
                                          "Cannot send password over unencrypted "
                                          "connection. Enable HTTPS."));
               json_object_object_add(response, "payload", resp_payload);
               send_json_response(conn, response);
               json_object_put(response);
               return;
            }
            const char *new_pass = json_object_get_string(pass_obj);
            if (new_pass && new_pass[0]) {
               if (calendar_encrypt_password(new_pass, &acct) != 0) {
                  json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
                  json_object_object_add(resp_payload, "error",
                                         json_object_new_string("Failed to encrypt password"));
                  encrypt_ok = false;
               }
            }
         }

         if (encrypt_ok) {
            int rc = calendar_db_account_update(&acct);
            if (rc != 0) {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
               json_object_object_add(resp_payload, "error",
                                      json_object_new_string("Failed to update account"));
            } else {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            }
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Toggle Account Read-Only
 * ============================================================================= */

void handle_calendar_toggle_read_only(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_toggle_read_only_response"));

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

      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = calendar_db_account_set_read_only(account_id, read_only);
         if (rc != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Failed to update read-only flag"));
         } else {
            /* Re-read from DB to echo actual state (not just the request) */
            calendar_account_t updated;
            if (calendar_db_account_get(account_id, &updated) == 0) {
               json_object_object_add(resp_payload, "read_only",
                                      json_object_new_boolean(updated.read_only));
            } else {
               json_object_object_add(resp_payload, "read_only",
                                      json_object_new_boolean(read_only));
            }
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

void handle_calendar_set_enabled(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_set_enabled_response"));

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

      calendar_account_t acct;
      if (!verify_account_owner(conn, account_id, &acct)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Account not found or access denied"));
      } else {
         int rc = calendar_db_account_set_enabled(account_id, enabled);
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

/* =============================================================================
 * List My Calendars (flat, across all accounts — the id->{name,color} map a
 * panel needs to group/color events by their calendar_id in ONE call, instead
 * of list_accounts + per-account list_calendars)
 * ============================================================================= */

void handle_calendar_list_my_calendars(ws_connection_t *conn) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_list_my_calendars_response"));
   json_object *resp_payload = json_object_new_object();

   /* Active calendars only — matches exactly the set the pull draws events from,
    * so every calendar_id an event can carry is present in this map. */
   calendar_calendar_t cals[32];
   int count = 0;
   int rc = calendar_db_active_calendars_for_user(conn->auth_user_id, cals, 32, &count);

   if (rc != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list calendars"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object *arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *o = json_object_new_object();
         json_object_object_add(o, "id", json_object_new_int64(cals[i].id));
         json_object_object_add(o, "account_id", json_object_new_int64(cals[i].account_id));
         json_object_object_add(o, "name", json_object_new_string(cals[i].display_name));
         json_object_object_add(o, "color", json_object_new_string(cals[i].color));
         json_object_array_add(arr, o);
      }
      json_object_object_add(resp_payload, "calendars", arr);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Upcoming Events (read-only pull for a calendar panel)
 * ============================================================================= */

/* Send a {success:false, error} response and clean up. */
static void upcoming_send_error(ws_connection_t *conn,
                                json_object *response,
                                json_object *resp_payload,
                                const char *msg) {
   json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
   json_object_object_add(resp_payload, "error", json_object_new_string(msg));
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_calendar_upcoming_events(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("calendar_upcoming_events_response"));
   json_object *resp_payload = json_object_new_object();

   /* Resolve the window: explicit {start,end} epoch (power path) wins over {days}
    * (convenience path). Exactly one of start/end present is a client bug. */
   time_t start = 0;
   time_t end = 0;
   json_object *jstart = NULL;
   json_object *jend = NULL;
   bool has_start = payload && json_object_object_get_ex(payload, "start", &jstart);
   bool has_end = payload && json_object_object_get_ex(payload, "end", &jend);

   if (has_start != has_end) {
      upcoming_send_error(conn, response, resp_payload,
                          "Provide both 'start' and 'end', or neither");
      return;
   }

   if (has_start && has_end) {
      start = (time_t)json_object_get_int64(jstart);
      end = (time_t)json_object_get_int64(jend);
      if (start <= 0 || end <= 0 || start >= end) {
         upcoming_send_error(conn, response, resp_payload,
                             "Invalid range: require 0 < start < end");
         return;
      }
      if (end - start > CAL_UPCOMING_MAX_SPAN_SEC) {
         upcoming_send_error(conn, response, resp_payload, "Range too large (max 366 days)");
         return;
      }
   } else {
      int days = CAL_UPCOMING_DEFAULT_DAYS;
      json_object *jdays = NULL;
      if (payload && json_object_object_get_ex(payload, "days", &jdays))
         days = json_object_get_int(jdays);
      if (days < 1)
         days = 1;
      if (days > CAL_UPCOMING_MAX_DAYS)
         days = CAL_UPCOMING_MAX_DAYS;
      start = time(NULL);
      end = start + (time_t)days * 86400;
   }

   /* Optional calendar_name filter (empty/omitted = all active calendars). */
   const char *cal_name = NULL;
   json_object *jcal = NULL;
   if (payload && json_object_object_get_ex(payload, "calendar_name", &jcal)) {
      const char *s = json_object_get_string(jcal);
      if (s && s[0])
         cal_name = s;
   }

   /* ~1 KB per occurrence * 256 — heap, not stack. */
   calendar_occurrence_t *occ = calloc(CAL_UPCOMING_MAX, sizeof(calendar_occurrence_t));
   if (!occ) {
      upcoming_send_error(conn, response, resp_payload, "Out of memory");
      return;
   }

   int count = calendar_service_range(conn->auth_user_id, start, end, cal_name, occ,
                                      CAL_UPCOMING_MAX);

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "start", json_object_new_int64((int64_t)start));
   json_object_object_add(resp_payload, "end", json_object_new_int64((int64_t)end));
   json_object_object_add(resp_payload, "truncated",
                          json_object_new_boolean(count >= CAL_UPCOMING_MAX));

   json_object *arr = json_object_new_array();
   for (int i = 0; i < count; i++) {
      json_object *o = json_object_new_object();
      json_object_object_add(o, "id", json_object_new_int64(occ[i].id));
      json_object_object_add(o, "calendar_id", json_object_new_int64(occ[i].calendar_id));
      json_object_object_add(o, "uid", json_object_new_string(occ[i].event_uid));
      json_object_object_add(o, "summary", json_object_new_string(occ[i].summary));
      json_object_object_add(o, "location", json_object_new_string(occ[i].location));
      json_object_object_add(o, "start", json_object_new_int64((int64_t)occ[i].dtstart));
      json_object_object_add(o, "end", json_object_new_int64((int64_t)occ[i].dtend));
      json_object_object_add(o, "all_day", json_object_new_boolean(occ[i].all_day));
      json_object_object_add(o, "start_date", json_object_new_string(occ[i].dtstart_date));
      json_object_object_add(o, "end_date", json_object_new_string(occ[i].dtend_date));
      json_object_object_add(o, "cancelled", json_object_new_boolean(occ[i].is_cancelled));
      json_object_object_add(o, "is_override", json_object_new_boolean(occ[i].is_override));
      json_object_array_add(arr, o);
   }
   json_object_object_add(resp_payload, "events", arr);

   free(occ);
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
