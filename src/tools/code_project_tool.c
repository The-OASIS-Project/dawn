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
 * Native LLM tool `code_project`: list visible projects, set the per-session
 * active project, or report a project's status. Import/delete are operator-only
 * (admin socket / WebUI), not LLM-callable in Phase 1.
 */

#include "tools/code_project_tool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/code_project_db.h"
#include "tools/tool_registry.h"

static char *cp_list(int64_t uid) {
   code_project_t list[CODE_PROJECTS_MAX];
   int n = 0;
   if (code_project_db_list_visible(uid, list, CODE_PROJECTS_MAX, &n) != AUTH_DB_SUCCESS) {
      return strdup("Could not list code projects.");
   }
   if (n == 0) {
      return strdup("No code projects are available to you.");
   }
   char buf[2048];
   int off = snprintf(buf, sizeof(buf), "Available code projects:\n");
   for (int i = 0; i < n && off < (int)sizeof(buf); i++) {
      off += snprintf(buf + off, sizeof(buf) - off, "- %s (%s)%s\n", list[i].name, list[i].status,
                      list[i].is_global ? " [global]" : "");
   }
   return strdup(buf);
}

static char *cp_set_active(int64_t uid, const char *name) {
   if (name == NULL || name[0] == '\0') {
      return strdup("Specify a project name to activate.");
   }
   bool visible = false;
   if (code_project_db_check_visible(uid, name, &visible) != AUTH_DB_SUCCESS || !visible) {
      return strdup("No such project is available to you.");
   }
   code_project_t p;
   if (code_project_db_get_by_name(name, &p) != AUTH_DB_SUCCESS) {
      return strdup("No such project.");
   }
   session_t *s = session_get_command_context();
   if (s == NULL) {
      return strdup("No active session to set the project on.");
   }
   pthread_mutex_lock(&s->llm_config_mutex);
   s->active_project_id = p.id;
   snprintf(s->active_project_name, sizeof(s->active_project_name), "%s", p.name);
   pthread_mutex_unlock(&s->llm_config_mutex);

   char msg[160];
   snprintf(msg, sizeof(msg), "Active code project set to '%s'.", p.name);
   return strdup(msg);
}

static char *cp_status(int64_t uid, const char *name) {
   char resolved[64] = { 0 };
   if (name != NULL && name[0] != '\0') {
      snprintf(resolved, sizeof(resolved), "%s", name);
   } else {
      session_t *s = session_get_command_context();
      if (s != NULL) {
         pthread_mutex_lock(&s->llm_config_mutex);
         snprintf(resolved, sizeof(resolved), "%s", s->active_project_name);
         pthread_mutex_unlock(&s->llm_config_mutex);
      }
   }
   if (resolved[0] == '\0') {
      return strdup("No active project. Use action 'set_active' first.");
   }
   bool visible = false;
   if (code_project_db_check_visible(uid, resolved, &visible) != AUTH_DB_SUCCESS || !visible) {
      return strdup("No such project is available to you.");
   }
   code_project_t p;
   if (code_project_db_get_by_name(resolved, &p) != AUTH_DB_SUCCESS) {
      return strdup("No such project.");
   }
   char msg[512];
   snprintf(msg, sizeof(msg), "Project '%s': status=%s%s%s", p.name, p.status,
            p.status_msg[0] ? ", " : "", p.status_msg);
   return strdup(msg);
}

static char *code_project_callback(const char *action, char *value, int *should_respond) {
   if (should_respond != NULL) {
      *should_respond = 1;
   }
   int64_t uid = tool_get_current_user_id();

   if (action == NULL || action[0] == '\0' || strcmp(action, "list") == 0) {
      return cp_list(uid);
   }
   if (strcmp(action, "set_active") == 0) {
      return cp_set_active(uid, value);
   }
   if (strcmp(action, "status") == 0) {
      return cp_status(uid, value);
   }
   return strdup("Unknown action. Use 'list', 'set_active', or 'status'.");
}

static const treg_param_t s_params[] = {
   {
       .name = "action",
       .description = "list (show projects), set_active (switch active project), or status",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list", "set_active", "status" },
       .enum_count = 3,
   },
   {
       .name = "name",
       .description = "Project name (required for set_active; optional for status)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t s_meta = {
   .name = "code_project",
   .description = "Manage code projects: list available repos, set the active project for code "
                  "questions, or check indexing status.",
   .params = s_params,
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = true,
   .default_local = true,
   .default_remote = true,
   .callback = code_project_callback,
};

int code_project_tool_register(void) {
   return tool_registry_register(&s_meta);
}
