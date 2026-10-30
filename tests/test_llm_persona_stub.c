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
 * Stubs for test_llm_persona: what llm_command_parser.c's prompt rules need
 * (sessions, the LLM, the tool registry), none of which the persona uses.
 */

#include <string.h>

#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "tools/tool_registry.h"

session_t *session_get_command_context(void) {
   return NULL;
}

void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   memset(config, 0, sizeof(*config));
}

llm_type_t llm_get_type(void) {
   return LLM_CLOUD;
}

bool llm_tools_enabled(const llm_resolved_config_t *config) {
   (void)config;
   return false;
}

const tool_metadata_t *tool_registry_find(const char *name_or_alias) {
   (void)name_or_alias;
   return NULL;
}

int tool_registry_count(void) {
   return 0;
}
