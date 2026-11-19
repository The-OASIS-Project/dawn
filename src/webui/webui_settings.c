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
#include "core/scheduler.h"
#include "core/session_manager.h"
#include "core/tz.h"
#include "core/user_tz.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
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

      /* The server's half of each, which the user's text adds to or replaces */
      char base[CONFIG_DESCRIPTION_MAX];
      llm_persona_half(PERSONA_VALUES, base, sizeof(base));
      json_object_object_add(resp_payload, "base_values", json_object_new_string(base));
      llm_persona_half(PERSONA_VOICE, base, sizeof(base));
      json_object_object_add(resp_payload, "base_voice", json_object_new_string(base));

      /* The user's halves, and the older single text (shown while it's set) */
      json_object_object_add(resp_payload, "persona_values",
                             json_object_new_string(settings.persona_values));
      json_object_object_add(resp_payload, "persona_values_mode",
                             json_object_new_string(settings.persona_values_mode));
      json_object_object_add(resp_payload, "persona_voice",
                             json_object_new_string(settings.persona_voice));
      json_object_object_add(resp_payload, "persona_voice_mode",
                             json_object_new_string(settings.persona_voice_mode));
      json_object_object_add(resp_payload, "persona_description",
                             json_object_new_string(settings.persona_description));
      json_object_object_add(resp_payload, "persona_mode",
                             json_object_new_string(settings.persona_mode));
      json_object_object_add(resp_payload, "location", json_object_new_string(settings.location));
      json_object_object_add(resp_payload, "timezone", json_object_new_string(settings.timezone));
      json_object_object_add(resp_payload, "server_timezone",
                             json_object_new_string(tz_name(tz_daemon())));
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

/* A set_my_settings payload, and why it can't be saved (empty while it can). */
typedef struct {
   json_object *payload;
   char err[128];
} settings_form_t;

/* A text field of the form into @p dest, when present and a string.  Text that
 * doesn't fit leaves @p dest as it was and, for the first such field, names
 * @p label in form->err: the save then fails rather than cut the user's text. */
static void take_text(settings_form_t *form,
                      const char *key,
                      const char *label,
                      char *dest,
                      size_t size) {
   json_object *field = NULL;
   if (!json_object_object_get_ex(form->payload, key, &field) ||
       !json_object_is_type(field, json_type_string)) {
      return;
   }
   const char *text = json_object_get_string(field);
   if (strlen(text) < size) {
      safe_strncpy(dest, text, size);
   } else if (!form->err[0]) {
      snprintf(form->err, sizeof(form->err), "%s is too long (at most %zu bytes)", label, size - 1);
   }
}

/* A persona mode field of @p payload into @p dest, when "append" or "replace". */
static void take_mode(json_object *payload, const char *key, char *dest, size_t size) {
   json_object *field = NULL;
   if (!json_object_object_get_ex(payload, key, &field) ||
       !json_object_is_type(field, json_type_string)) {
      return;
   }
   const char *mode = json_object_get_string(field);
   if (strcmp(mode, "append") == 0 || strcmp(mode, "replace") == 0) {
      safe_strncpy(dest, mode, size);
   }
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

   /* The stored settings and identity are the base: a field the payload leaves
    * out keeps its value.  Without them nothing is saved (a save would blank
    * them). */
   auth_user_settings_t settings;
   auth_user_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   if (auth_db_get_user_settings(conn->auth_user_id, &settings) != AUTH_DB_SUCCESS ||
       auth_db_get_user_identity(conn->auth_user_id, &identity) != AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load settings"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Update with any provided fields */
   json_object *field_obj;
   settings_form_t form = { .payload = payload };

   take_text(&form, "persona_values", "Your values", settings.persona_values,
             sizeof(settings.persona_values));
   take_mode(payload, "persona_values_mode", settings.persona_values_mode,
             sizeof(settings.persona_values_mode));
   take_text(&form, "persona_voice", "Your voice", settings.persona_voice,
             sizeof(settings.persona_voice));
   take_mode(payload, "persona_voice_mode", settings.persona_voice_mode,
             sizeof(settings.persona_voice_mode));
   /* The older single text (clearing it, from this page) and its mode (an
    * older page still sends it). */
   take_text(&form, "persona_description", "Your persona from an earlier version",
             settings.persona_description, sizeof(settings.persona_description));
   take_mode(payload, "persona_mode", settings.persona_mode, sizeof(settings.persona_mode));

   take_text(&form, "location", "Location", settings.location, sizeof(settings.location));

   if (json_object_object_get_ex(payload, "timezone", &field_obj)) {
      /* "" is the server's zone; anything else must be a zone (stored by its
       * current name: "Asia/Calcutta" is kept as "Asia/Kolkata"). */
      const char *tz = json_object_get_string(field_obj);
      const tz_t *zone = tz && tz[0] ? tz_lookup(tz) : NULL;
      if (tz && !tz[0]) {
         settings.timezone[0] = '\0';
      } else if (zone) {
         safe_strscpy(settings.timezone, tz_name(zone));
      } else {
         /* Nothing is saved: the user hears why, not "saved". */
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("That time zone isn't one DAWN knows"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }
   }

   if (json_object_object_get_ex(payload, "units", &field_obj)) {
      const char *units = json_object_get_string(field_obj);
      /* Validate units value */
      if (units && (strcmp(units, "metric") == 0 || strcmp(units, "imperial") == 0)) {
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

   /* v44 identity fields — overlay any provided fields on the stored ones,
    * write back.  Persisted on the users table (not user_settings) via
    * auth_db_set_user_identity which NULLIFs empty strings. */
   take_text(&form, "real_name", "Real name", identity.real_name, sizeof(identity.real_name));
   take_text(&form, "preferred_address", "Ways to address you", identity.preferred_address,
             sizeof(identity.preferred_address));
   take_text(&form, "identity_aliases", "Other names / aliases", identity.identity_aliases,
             sizeof(identity.identity_aliases));

   /* Nothing is saved: the user hears which field, and keeps the edit. */
   if (form.err[0]) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string(form.err));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   if (auth_db_set_user_identity(conn->auth_user_id, &identity) != AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save settings"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

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
   const tz_t *old_zone = user_tz(conn->auth_user_id);
   int result = auth_db_set_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Settings saved"));

      /* A running conversation gets the changed parts of its prompt with its
       * next turn, appended (session_prefix.c). */

      /* Log event */
      auth_db_log_event("SETTINGS_UPDATED", conn->username, conn->client_ip, "Personal settings");

      /* Repeating events follow the user to a new zone. */
      if (user_tz(conn->auth_user_id) != old_zone)
         scheduler_rezone(conn->auth_user_id);

      /* Every open tab shows this user's times in the zone now in force. */
      json_object *tz_frame = json_object_new_object();
      json_object *tz_payload = json_object_new_object();
      json_object_object_add(tz_frame, "type", json_object_new_string("timezone_changed"));
      json_object_object_add(tz_payload, "timezone",
                             json_object_new_string(tz_name(user_tz(conn->auth_user_id))));
      json_object_object_add(tz_frame, "payload", tz_payload);
      webui_broadcast_json_to_user(conn->auth_user_id, tz_frame, true);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
