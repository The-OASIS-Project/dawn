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
 * WebUI messaging-channels handlers — user-scoped channel management
 * (list / create-link-code / unlink / rename) for the Settings panel.
 *
 * Transport is WebSocket RPC (mirrors the satellite admin handlers), but
 * these are USER-scoped: conn_require_auth + conn->auth_user_id, so a
 * user manages only their own channels.  Operator / cross-user channel
 * management lives on the dawn-admin unix socket instead.  All four wrap
 * the shared messaging_engine_* functions (the single source of truth);
 * mutations key on the stable row id, not the mutable display_name.
 * See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 6.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "logging.h"
#include "messaging/messaging_engine.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * list_channels — { } → { channels: [ {id,name,provider,enabled,last_used_at} ] }
 * ============================================================================= */

void handle_list_channels(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   char *json = messaging_engine_list_channels_json(conn->auth_user_id);
   /* Parse the engine's JSON array string into an object we can embed.
    * On NULL / parse failure, fall back to an empty array so the panel
    * always renders. */
   struct json_object *channels = json ? json_tokener_parse(json) : NULL;
   if (json) {
      free(json);
   }
   if (!channels || !json_object_is_type(channels, json_type_array)) {
      if (channels) {
         json_object_put(channels);
      }
      channels = json_object_new_array();
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_channels_response"));
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "channels", channels);
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * create_link_code — { provider? } → { code, ttl_seconds, provider }
 * ============================================================================= */

void handle_create_link_code(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   const char *provider = NULL;
   struct json_object *prov_obj = NULL;
   if (payload && json_object_object_get_ex(payload, "provider", &prov_obj)) {
      const char *p = json_object_get_string(prov_obj);
      if (p && p[0] != '\0') {
         if (!messaging_engine_provider_known(p)) {
            send_error_impl(conn->wsi, "INVALID_PARAM",
                            "Unknown provider (use telegram|discord|slack|sms)");
            return;
         }
         provider = p;
      }
   }

   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   int rc = messaging_engine_generate_link_code(conn->auth_user_id, provider, code, sizeof(code));
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to generate link code");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("create_link_code_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "code", json_object_new_string(code));
   json_object_object_add(payload_out, "ttl_seconds",
                          json_object_new_int(MESSAGING_LINK_TTL_SECONDS));
   json_object_object_add(payload_out, "provider",
                          json_object_new_string(provider ? provider : ""));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * unlink_channel — { id } → { success, id }
 * ============================================================================= */

void handle_unlink_channel(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *id_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'id'");
      return;
   }
   int64_t channel_id = json_object_get_int64(id_obj);
   if (channel_id <= 0) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid 'id'");
      return;
   }

   int rc = messaging_engine_unlink_channel_by_id(conn->auth_user_id, channel_id);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such channel");
      return;
   }
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to unlink channel");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("unlink_channel_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "id", json_object_new_int64(channel_id));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: user %d unlinked messaging channel id %lld", conn->auth_user_id,
             (long long)channel_id);
}

/* =============================================================================
 * rename_channel — { id, name } → { success, id, name }
 * ============================================================================= */

void handle_rename_channel(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *id_obj = NULL;
   struct json_object *name_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "name", &name_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'id' or 'name'");
      return;
   }
   int64_t channel_id = json_object_get_int64(id_obj);
   const char *new_name = json_object_get_string(name_obj);
   if (channel_id <= 0 || !new_name || new_name[0] == '\0') {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid 'id' or empty 'name'");
      return;
   }
   if (strlen(new_name) >= MESSAGING_DISPLAY_NAME_MAX) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Name must be under 64 characters");
      return;
   }

   int rc = messaging_engine_rename_channel_by_id(conn->auth_user_id, channel_id, new_name);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such channel");
      return;
   }
   if (rc == MESSAGING_NAME_TAKEN) {
      send_error_impl(conn->wsi, "NAME_TAKEN", "A channel with that name already exists");
      return;
   }
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to rename channel");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("rename_channel_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "id", json_object_new_int64(channel_id));
   json_object_object_add(payload_out, "name", json_object_new_string(new_name));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: user %d renamed messaging channel id %lld", conn->auth_user_id,
             (long long)channel_id);
}

/* =============================================================================
 * reenable_channel — { id } → { success, id }  (inverse of unlink)
 * ============================================================================= */

void handle_reenable_channel(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *id_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'id'");
      return;
   }
   int64_t channel_id = json_object_get_int64(id_obj);
   if (channel_id <= 0) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid 'id'");
      return;
   }

   int rc = messaging_engine_reenable_channel_by_id(conn->auth_user_id, channel_id);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such unlinked channel");
      return;
   }
   if (rc == MESSAGING_NAME_TAKEN) {
      send_error_impl(conn->wsi, "NAME_TAKEN", "An enabled channel with that name already exists");
      return;
   }
   if (rc != MESSAGING_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to re-enable channel");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("reenable_channel_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "id", json_object_new_int64(channel_id));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: user %d re-enabled messaging channel id %lld", conn->auth_user_id,
             (long long)channel_id);
}

/* =============================================================================
 * set_channel_llm — { conversation_id, llm_type, cloud_provider, model,
 *                     thinking_mode, reasoning_effort } → { success }
 *
 * Writes the channel's forever-conversation row (the same conversations.llm_*
 * columns the WebUI per-conversation control uses).  Applies on the channel's
 * NEXT messaging session (daemon picks it up at session (re)creation), or in
 * chat immediately via the switch_llm tool.  Ownership is enforced by
 * conv_db_get / conv_db_update_llm_settings (both filter on user_id).
 * ============================================================================= */

/* Empty (inherit) or a member of the NULL-terminated allowlist. */
static bool set_channel_llm_field_valid(const char *value, const char *const *allowed) {
   if (!value || value[0] == '\0') {
      return true; /* "" = inherit the global default */
   }
   for (const char *const *p = allowed; *p; p++) {
      if (strcmp(value, *p) == 0) {
         return true;
      }
   }
   return false;
}

void handle_set_channel_llm(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   struct json_object *conv_obj = NULL;
   if (!payload || !json_object_object_get_ex(payload, "conversation_id", &conv_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'conversation_id'");
      return;
   }
   int64_t conv_id = json_object_get_int64(conv_obj);
   if (conv_id <= 0) {
      send_error_impl(conn->wsi, "INVALID_PARAM",
                      "Channel has no conversation yet (send a message first)");
      return;
   }

   /* Optional string fields; absent = empty (= inherit the global default). */
   struct json_object *v = NULL;
   const char *llm_type = json_object_object_get_ex(payload, "llm_type", &v)
                              ? json_object_get_string(v)
                              : "";
   const char *cloud_provider = json_object_object_get_ex(payload, "cloud_provider", &v)
                                    ? json_object_get_string(v)
                                    : "";
   const char *model = json_object_object_get_ex(payload, "model", &v) ? json_object_get_string(v)
                                                                       : "";
   const char *thinking_mode = json_object_object_get_ex(payload, "thinking_mode", &v)
                                   ? json_object_get_string(v)
                                   : "";
   const char *reasoning_effort = json_object_object_get_ex(payload, "reasoning_effort", &v)
                                      ? json_object_get_string(v)
                                      : "";

   /* Bound lengths to the conversation column sizes (mirror
    * handle_lock_conversation_llm) so an overlong value can't be stored. */
   if (strlen(llm_type) > 15 || strlen(cloud_provider) > 15 || strlen(model) > 63 ||
       strlen(thinking_mode) > 15 || strlen(reasoning_effort) > 15) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Field value too long");
      return;
   }

   /* Edge enum validation (defense-in-depth — the dropdowns already constrain
    * these; downstream falls back gracefully on a bad value, but fail fast). */
   if (!set_channel_llm_field_valid(llm_type, (const char *[]){ "local", "cloud", NULL }) ||
       !set_channel_llm_field_valid(cloud_provider,
                                    (const char *[]){ "openai", "claude", "anthropic", "gemini",
                                                      "openrouter", NULL }) ||
       !set_channel_llm_field_valid(thinking_mode,
                                    (const char *[]){ "disabled", "enabled", "auto", NULL }) ||
       !set_channel_llm_field_valid(reasoning_effort,
                                    (const char *[]){ "none", "minimal", "low", "medium", "high",
                                                      "xhigh", NULL })) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid LLM setting value");
      return;
   }

   /* Fetch the current row to (a) verify ownership and (b) confirm it's a
    * messaging conversation — this endpoint is for messaging channels, not
    * arbitrary owned conversations. */
   conversation_t conv;
   if (conv_db_get(conv_id, conn->auth_user_id, &conv) != AUTH_DB_SUCCESS) {
      send_error_impl(conn->wsi, "NOT_FOUND", "No such conversation");
      return;
   }
   if (strncmp(conv.origin, "messaging:", 10) != 0) {
      conv_free(&conv);
      send_error_impl(conn->wsi, "INVALID_PARAM", "Not a messaging conversation");
      return;
   }
   /* tools_mode column is retired (dead) — pass empty. */
   int rc = conv_db_update_llm_settings(conv_id, conn->auth_user_id, llm_type, cloud_provider,
                                        model, "", thinking_mode, reasoning_effort);
   conv_free(&conv);

   if (rc != AUTH_DB_SUCCESS) {
      send_error_impl(conn->wsi, "SERVICE_ERROR", "Failed to update channel model");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_channel_llm_response"));
   struct json_object *payload_out = json_object_new_object();
   json_object_object_add(payload_out, "success", json_object_new_boolean(1));
   json_object_object_add(payload_out, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(response, "payload", payload_out);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO(
       "WebUI: user %d set LLM for messaging conversation %lld (type=%s provider=%s model=%s)",
       conn->auth_user_id, (long long)conv_id, llm_type[0] ? llm_type : "(inherit)",
       cloud_provider[0] ? cloud_provider : "(inherit)", model[0] ? model : "(inherit)");
}
