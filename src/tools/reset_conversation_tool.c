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
 * Reset Conversation Tool - Clear conversation history and start fresh
 */

#include "tools/reset_conversation_tool.h"

#include <stdlib.h>
#include <string.h>

#include "conversation_manager.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "webui/webui_server.h"

/* ========== Forward Declarations ========== */

static char *reset_conversation_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Tool Metadata ========== */

static const tool_metadata_t reset_conversation_metadata = {
   .name = "reset_conversation",
   .device_string = "reset conversation",
   .topic = "dawn",
   .aliases = { "reset context", "clear conversation", "clear context", "new conversation" },
   .alias_count = 4,

   .description = "Clear the conversation history and start fresh. Use when the user wants to "
                  "change topics completely or start a new conversation.",
   .params = NULL,
   .param_count = 0,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = false,
   .skip_followup = true, /* Must be true - conversation history is invalidated after reset */
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = reset_conversation_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *reset_conversation_tool_callback(const char *action,
                                              char *value,
                                              int *should_respond) {
   (void)action;
   (void)value;

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   session_t *local_session = session_get_local();

   if (!session) {
      session = local_session;
   }

   bool is_local = (session == local_session);
   LOG_INFO("Resetting conversation context for %s session via reset_conversation tool.",
            is_local ? "local" : "remote");

   /* For local session, use the legacy reset which handles global state */
   if (is_local) {
      reset_conversation();
   } else {
      /* For remote sessions, reset with appropriate system prompt
       * Note: WebUI sessions will have memory context rebuilt on next message */
      const char *system_prompt = (session->type == SESSION_TYPE_DAP2) ? get_remote_command_prompt()
                                                                       : get_local_command_prompt();
      session_init_system_prompt(session, system_prompt);

      /* Re-append room context for DAP2 satellites */
      if (session->type == SESSION_TYPE_DAP2) {
         session_append_room_context(session, session->identity.location);
      }
   }

   /* For WebUI sessions, send notification to clear the frontend display */
   if (session && session->type == SESSION_TYPE_WEBSOCKET) {
      webui_send_conversation_reset(session);
   }

   *should_respond = 1;
   return strdup("Conversation context has been reset. Starting fresh.");
}

/* ========== Public API ========== */

int reset_conversation_tool_register(void) {
   return tool_registry_register(&reset_conversation_metadata);
}
