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
 * WebUI Settings Handlers - Personal user settings management
 *
 * This module handles WebSocket messages for:
 * - get_my_settings (get user's personal settings)
 * - set_my_settings (update user's personal settings)
 */

#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "logging.h"
#include "memory/memory_db_aliases.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

/**
 * @brief Get current user's personal settings
 */
void handle_get_my_settings(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_my_settings_response"));
   json_object *resp_payload = json_object_new_object();

   auth_user_settings_t settings;
   int result = auth_db_get_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      /* Include base persona (from config or dynamic default) for UI display */
      char base_persona_buf[2048];
      const char *base_persona;
      if (g_config.persona.description[0] != '\0') {
         base_persona = g_config.persona.description;
      } else {
         /* Build dynamic persona with configured AI name */
         const char *ai_name = g_config.general.ai_name[0] != '\0' ? g_config.general.ai_name
                                                                   : AI_NAME;

         /* Capitalize first letter for proper noun */
         char capitalized_name[64];
         snprintf(capitalized_name, sizeof(capitalized_name), "%s", ai_name);
         if (capitalized_name[0] >= 'a' && capitalized_name[0] <= 'z') {
            capitalized_name[0] -= 32;
         }

         snprintf(base_persona_buf, sizeof(base_persona_buf),
                  AI_PERSONA_NAME_TEMPLATE " " AI_PERSONA_TRAITS, capitalized_name);
         base_persona = base_persona_buf;
      }
      json_object_object_add(resp_payload, "base_persona", json_object_new_string(base_persona));

      /* User's custom settings */
      json_object_object_add(resp_payload, "persona_description",
                             json_object_new_string(settings.persona_description));
      json_object_object_add(resp_payload, "persona_mode",
                             json_object_new_string(settings.persona_mode));
      json_object_object_add(resp_payload, "location", json_object_new_string(settings.location));
      json_object_object_add(resp_payload, "timezone", json_object_new_string(settings.timezone));
      json_object_object_add(resp_payload, "units", json_object_new_string(settings.units));
      json_object_object_add(resp_payload, "theme", json_object_new_string(settings.theme));

      /* v44 identity fields — empty strings when unset. */
      auth_user_identity_t identity;
      memset(&identity, 0, sizeof(identity));
      auth_db_get_user_identity(conn->auth_user_id, &identity);
      json_object_object_add(resp_payload, "real_name", json_object_new_string(identity.real_name));
      json_object_object_add(resp_payload, "preferred_address",
                             json_object_new_string(identity.preferred_address));
      json_object_object_add(resp_payload, "identity_aliases",
                             json_object_new_string(identity.identity_aliases));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Update current user's personal settings
 */
void handle_set_my_settings(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_my_settings_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get current settings as defaults */
   auth_user_settings_t settings;
   auth_db_get_user_settings(conn->auth_user_id, &settings);

   /* Update with any provided fields */
   json_object *field_obj;

   if (json_object_object_get_ex(payload, "persona_description", &field_obj)) {
      safe_strscpy(settings.persona_description, json_object_get_string(field_obj));
   }

   if (json_object_object_get_ex(payload, "persona_mode", &field_obj)) {
      const char *mode = json_object_get_string(field_obj);
      /* Validate mode value */
      if (strcmp(mode, "append") == 0 || strcmp(mode, "replace") == 0) {
         safe_strscpy(settings.persona_mode, mode);
      }
   }

   if (json_object_object_get_ex(payload, "location", &field_obj)) {
      safe_strscpy(settings.location, json_object_get_string(field_obj));
   }

   if (json_object_object_get_ex(payload, "timezone", &field_obj)) {
      safe_strscpy(settings.timezone, json_object_get_string(field_obj));
   }

   if (json_object_object_get_ex(payload, "units", &field_obj)) {
      const char *units = json_object_get_string(field_obj);
      /* Validate units value */
      if (strcmp(units, "metric") == 0 || strcmp(units, "imperial") == 0) {
         safe_strscpy(settings.units, units);
      }
   }

   if (json_object_object_get_ex(payload, "theme", &field_obj)) {
      const char *theme = json_object_get_string(field_obj);
      /* Validate theme value - check for NULL first */
      if (theme && (strcmp(theme, "cyan") == 0 || strcmp(theme, "purple") == 0 ||
                    strcmp(theme, "green") == 0 || strcmp(theme, "orange") == 0 ||
                    strcmp(theme, "red") == 0 || strcmp(theme, "blue") == 0 ||
                    strcmp(theme, "terminal") == 0)) {
         safe_strscpy(settings.theme, theme);
      }
   }

   /* v44 identity fields — read existing values, overlay any provided
    * fields, write back.  Persisted on the users table (not user_settings)
    * via auth_db_set_user_identity which NULLIFs empty strings. */
   auth_user_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   auth_db_get_user_identity(conn->auth_user_id, &identity);
   if (json_object_object_get_ex(payload, "real_name", &field_obj)) {
      const char *v = json_object_get_string(field_obj);
      if (v) {
         safe_strscpy(identity.real_name, v);
      }
   }
   if (json_object_object_get_ex(payload, "preferred_address", &field_obj)) {
      const char *v = json_object_get_string(field_obj);
      if (v) {
         safe_strscpy(identity.preferred_address, v);
      }
   }
   if (json_object_object_get_ex(payload, "identity_aliases", &field_obj)) {
      const char *v = json_object_get_string(field_obj);
      if (v) {
         safe_strscpy(identity.identity_aliases, v);
      }
   }
   auth_db_set_user_identity(conn->auth_user_id, &identity);

   /* Phase 2 entity-merge: if real_name is now set and no user-self
    * anchor exists yet for this user, sweep existing canonicals and
    * auto-promote a match.  Makes the CLI `dawn-admin memory entity
    * link-user-self` unnecessary for the common "operator sets their
    * name in Settings" flow.  No-op when already promoted or when no
    * canonical matches yet (extraction-time hook catches it later).
    *
    * TODO(architecture): this is the first known "user-identity-changed"
    * cross-subsystem reactor (WebUI handler → memory subsystem).  If a
    * second consumer arrives (e.g. speaker-ID re-cluster, conversation-
    * anchor re-stamp, per-user prompt cache invalidation), extract a
    * Layer-1 notifier hook so `auth_db_set_user_identity` becomes the
    * publisher and each subsystem registers as a listener.  Don't add
    * a third direct call site here without doing that refactor first. */
   if (identity.real_name[0] != '\0') {
      /* Both helpers return MEMORY_DB_SUCCESS on all benign no-ops per
       * their docstrings.  A FAILURE here is a real SQLite-level error
       * worth logging; the settings save itself still proceeds either
       * way (the identity write is already done above, and the user
       * shouldn't lose their settings just because a sweep DB call
       * tripped). */
      int prom_rc = memory_db_entity_auto_promote_user_self_by_real_name(conn->auth_user_id, NULL);
      if (prom_rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("webui_settings: auto-promote-user-self-by-real-name DB error (user_id=%d "
                      "rc=%d)",
                      conn->auth_user_id, prom_rc);
      }
      /* Follow-up sweep: any pre-existing abstract "user" canonical left
       * from earlier extractions (before the anchor existed) gets
       * attached to the newly-promoted user_self.  Order matters —
       * promote first so the anchor is in place before the sweep
       * looks for it. */
      int sweep_rc = memory_db_entity_alias_existing_user_to_self(conn->auth_user_id, NULL);
      if (sweep_rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("webui_settings: alias-existing-user-to-self DB error (user_id=%d rc=%d)",
                      conn->auth_user_id, sweep_rc);
      }
   }

   /* Save settings */
   int result = auth_db_set_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Settings saved"));

      /* Refresh active session's system prompt immediately (preserves conversation) */
      if (conn->session) {
         /* Phase 1f: SESSION_START builder boundary — clear dedup state. */
         session_injected_set_clear(conn->session);
         char *new_prompt = session_manager_build_system_prompt_string(conn->auth_user_id);
         if (new_prompt) {
            session_update_system_prompt(conn->session, new_prompt);
            OLOG_INFO("WebUI: Refreshed system prompt for user %s", conn->username);

            /* Queue updated prompt to client so debug view refreshes.
             * Must go through queue — the settings response below is already
             * a direct write in this callback, so a second write would corrupt
             * WebSocket framing. */
            json_object *prompt_msg = json_object_new_object();
            json_object_object_add(prompt_msg, "type",
                                   json_object_new_string("system_prompt_response"));
            json_object *prompt_payload = json_object_new_object();
            json_object_object_add(prompt_payload, "success", json_object_new_boolean(1));
            json_object_object_add(prompt_payload, "prompt", json_object_new_string(new_prompt));
            json_object_object_add(prompt_payload, "length",
                                   json_object_new_int((int)strlen(new_prompt)));
            json_object_object_add(prompt_msg, "payload", prompt_payload);
            const char *json_str = json_object_to_json_string(prompt_msg);
            ws_response_t resp_q = { 0 };
            resp_q.session = conn->session;
            resp_q.type = WS_RESP_JSON;
            resp_q.generic_json.json = strdup(json_str);
            queue_response(&resp_q);
            json_object_put(prompt_msg);

            free(new_prompt);
         }
      }

      /* Log event */
      auth_db_log_event("SETTINGS_UPDATED", conn->username, conn->client_ip, "Personal settings");
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
