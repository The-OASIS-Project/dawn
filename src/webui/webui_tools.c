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
 * WebUI Tools Handlers - Tool configuration management
 *
 * This module handles WebSocket messages for:
 * - get_tools_config (get tool enabled states)
 * - set_tools_config (update tool enabled states)
 */

#include <string.h>

#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

void handle_get_tools_config(ws_connection_t *conn) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_tools_config_response"));

   json_object *payload = json_object_new_object();
   json_object *tools_array = json_object_new_array();

   /* Get all tools with their enable states */
   tool_info_t tools[LLM_TOOLS_MAX_TOOLS];
   int count = llm_tools_get_all(tools, LLM_TOOLS_MAX_TOOLS);

   for (int i = 0; i < count; i++) {
      json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "name", json_object_new_string(tools[i].name));
      json_object_object_add(tool_obj, "description", json_object_new_string(tools[i].description));
      json_object_object_add(tool_obj, "available", json_object_new_boolean(tools[i].enabled));
      json_object_object_add(tool_obj, "local", json_object_new_boolean(tools[i].enabled_local));
      json_object_object_add(tool_obj, "remote", json_object_new_boolean(tools[i].enabled_remote));
      json_object_object_add(tool_obj, "armor_feature",
                             json_object_new_boolean(tools[i].armor_feature));
      json_object_array_add(tools_array, tool_obj);
   }

   json_object_object_add(payload, "tools", tools_array);

   /* Add token estimates */
   json_object *estimates = json_object_new_object();
   json_object_object_add(estimates, "local",
                          json_object_new_int(llm_tools_estimate_tokens(false)));
   json_object_object_add(estimates, "remote",
                          json_object_new_int(llm_tools_estimate_tokens(true)));
   json_object_object_add(payload, "token_estimate", estimates);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Sent tools config (%d tools)", count);
}

/**
 * @brief Validate a tool name for safe processing.
 *
 * Tool names must be non-empty, under LLM_TOOL_NAME_MAX length,
 * and contain only alphanumeric characters, underscores, and hyphens.
 *
 * @param name The tool name to validate
 * @return true if valid, false otherwise
 */
static bool is_valid_tool_name(const char *name) {
   if (!name || name[0] == '\0') {
      return false;
   }

   size_t len = strlen(name);
   if (len >= LLM_TOOL_NAME_MAX) {
      return false;
   }

   for (size_t i = 0; i < len; i++) {
      char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')) {
         return false;
      }
   }

   return true;
}

void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload) {
   /* Admin-only operation */
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_tools_config_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *tools_array;
   if (!json_object_object_get_ex(payload, "tools", &tools_array)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing 'tools' array"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int updated = 0;
   int skipped = 0;
   int len = json_object_array_length(tools_array);
   for (int i = 0; i < len; i++) {
      json_object *tool_obj = json_object_array_get_idx(tools_array, i);

      json_object *name_obj, *local_obj, *remote_obj;
      if (json_object_object_get_ex(tool_obj, "name", &name_obj) &&
          json_object_object_get_ex(tool_obj, "local", &local_obj) &&
          json_object_object_get_ex(tool_obj, "remote", &remote_obj)) {
         const char *name = json_object_get_string(name_obj);

         /* Validate tool name before processing */
         if (!is_valid_tool_name(name)) {
            LOG_WARNING("WebUI: Skipping invalid tool name: '%s'", name ? name : "(null)");
            skipped++;
            continue;
         }

         bool local = json_object_get_boolean(local_obj);
         bool remote = json_object_get_boolean(remote_obj);

         if (llm_tools_set_enabled(name, local, remote) == 0) {
            updated++;
         }
      }
   }

   /* Update config arrays for persistence - requires write lock on g_config */
   tool_info_t tools[LLM_TOOLS_MAX_TOOLS];
   int count = llm_tools_get_all(tools, LLM_TOOLS_MAX_TOOLS);

   pthread_rwlock_wrlock(&s_config_rwlock);

   /* Mark both as explicitly configured (even if empty - empty means none enabled) */
   g_config.llm.tools.local_enabled_configured = true;
   g_config.llm.tools.remote_enabled_configured = true;
   g_config.llm.tools.local_enabled_count = 0;
   g_config.llm.tools.remote_enabled_count = 0;

   for (int i = 0; i < count; i++) {
      if (tools[i].enabled_local &&
          g_config.llm.tools.local_enabled_count < LLM_TOOLS_MAX_CONFIGURED) {
         safe_strncpy(g_config.llm.tools.local_enabled[g_config.llm.tools.local_enabled_count++],
                      tools[i].name, LLM_TOOL_NAME_MAX);
      }
      if (tools[i].enabled_remote &&
          g_config.llm.tools.remote_enabled_count < LLM_TOOLS_MAX_CONFIGURED) {
         safe_strncpy(g_config.llm.tools.remote_enabled[g_config.llm.tools.remote_enabled_count++],
                      tools[i].name, LLM_TOOL_NAME_MAX);
      }
   }

   /* Save to TOML */
   const char *config_path = config_get_loaded_path();
   if (!config_path || strcmp(config_path, "(none - using defaults)") == 0) {
      config_path = "./dawn.toml";
   }
   config_write_toml(&g_config, config_path);

   pthread_rwlock_unlock(&s_config_rwlock);

   /* Invalidate cached prompts so they rebuild with new tool enabled states.
    * This affects both native tool schemas and legacy <command> tag prompts. */
   if (updated > 0) {
      invalidate_system_instructions();
      LOG_INFO("WebUI: Tool states changed, prompt cache invalidated");

      /* Update current session's system prompt */
      if (conn->session) {
         char *new_prompt = build_user_prompt(conn->auth_user_id);
         if (new_prompt) {
            session_update_system_prompt(conn->session, new_prompt);
            LOG_INFO("WebUI: Updated session prompt for tool state changes");
            free(new_prompt);
         }
      }
   }

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "updated", json_object_new_int(updated));

   /* Include updated token estimates */
   json_object *estimates = json_object_new_object();
   json_object_object_add(estimates, "local",
                          json_object_new_int(llm_tools_estimate_tokens(false)));
   json_object_object_add(estimates, "remote",
                          json_object_new_int(llm_tools_estimate_tokens(true)));
   json_object_object_add(resp_payload, "token_estimate", estimates);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Updated %d tool enable states", updated);
}
