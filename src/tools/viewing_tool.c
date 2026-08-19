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
 * Viewing Tool - Analyze camera view (MQTT-based vision system)
 *
 * This tool communicates with the OASIS helmet camera system via MQTT.
 * Commands are sent to the "hud" topic and responses are received asynchronously.
 */

#include "tools/viewing_tool.h"

#include <stdlib.h>
#include <string.h>

#include "llm/llm_command_parser.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *viewing_tool_callback(const char *action, char *value, int *should_respond);
static bool viewing_tool_is_available(void);

/* ========== Parameter Definitions ========== */

static const treg_param_t viewing_params[] = {
   {
       .name = "query",
       .description = "What to look for or question about the view (e.g., 'what do you see?', "
                      "'read the text', 'is anyone there?')",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t viewing_metadata = {
   .name = "viewing",
   .device_string = "viewing",
   .topic = "hud",
   .aliases = { "looking at", "seeing" },
   .alias_count = 2,

   .description = "Analyze what the camera sees. Takes a photo and describes the scene, identifies "
                  "objects, reads text, or answers questions about the view. The captured image "
                  "stays visible in conversation history for a bounded number of follow-up turns "
                  "(older captures are automatically replaced by newer ones). If no image is "
                  "attached in your current context, say so plainly instead of guessing or "
                  "claiming a capture happened — call this tool again for a fresh look.",
   .params = viewing_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = true, /* Commands sent via MQTT to external vision system */
   .sync_wait = true, /* Wait for MQTT response */
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .is_available = viewing_tool_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = viewing_tool_callback,
};

/* ========== Availability Check ========== */

static bool viewing_tool_is_available(void) {
   return is_vision_enabled_for_current_llm();
}

/* ========== Callback Implementation ========== */

/**
 * @brief Stub callback for the viewing tool.
 *
 * Viewing commands are normally routed to the vision subsystem over
 * MQTT by the LLM tool dispatcher (llm_tools.c).  If this fires
 * directly, MQTT routing dropped the call — typically because the
 * vision hardware isn't currently connected or the MQTT broker is
 * unreachable.  Returning a concrete-cause message lets the LLM tell
 * the user what's actually wrong instead of opaque framework language.
 */
static char *viewing_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;

   OLOG_WARNING("viewing_tool_callback called directly - should use MQTT execution");
   *should_respond = 1;
   return strdup("Viewing command could not be delivered — the vision hardware doesn't appear "
                 "to be connected right now.  Let the user know the camera/vision system is "
                 "offline.");
}

/* ========== Public API ========== */

int viewing_tool_register(void) {
   return tool_registry_register(&viewing_metadata);
}
