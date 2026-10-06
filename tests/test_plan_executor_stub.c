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
 * Stubs for plan_executor unit tests — provides symbols for tool_registry
 * and llm_tools functions that plan_executor.c references but that aren't
 * needed for the pure-logic tests (set, log, if, loop, conditions, vars).
 */

#include "llm/llm_tools.h"
#include "tools/tool_registry.h"

const tool_metadata_t *tool_registry_lookup(const char *name) {
   (void)name;
   return NULL; /* No tools registered in test */
}

bool tool_registry_is_enabled(const char *name) {
   (void)name;
   return false;
}

int llm_tools_execute(const tool_call_t *call, tool_result_t *result) {
   (void)call;
   if (result) {
      result->success = false;
      snprintf(result->result, sizeof(result->result), "Error: stub — no tools in test");
   }
   return 0;
}

/* tool_result_content() is static inline in llm_tools.h — no stub needed */

/* Session manager stubs. */
#include "core/session_manager.h"

session_t *session_get_command_context(void) {
   return NULL;
}

void webui_broadcast_plan_progress(session_t *s, const char *json) {
   (void)s;
   (void)json;
}
