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
 * HUD Tools - MQTT-based HUD control for OASIS armor system
 *
 * These tools communicate with the OASIS helmet HUD via MQTT.
 * All commands are sent to the "hud" topic.
 */

#include "tools/hud_tools.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/hud_discovery.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *mqtt_only_stub_callback(const char *action, char *value, int *should_respond);

/* ========== Shared Stub Callback ========== */

/**
 * @brief Stub callback for MQTT-only HUD tools.
 *
 * HUD commands are normally routed to the helmet over MQTT by the LLM
 * tool dispatcher (llm_tools.c).  If this fires directly, MQTT routing
 * dropped the call — typically because the helmet hardware isn't
 * currently connected or the MQTT broker is unreachable.  Returning a
 * concrete-cause message lets the LLM tell the user what's actually
 * wrong instead of opaque framework language.
 */
static char *mqtt_only_stub_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;

   OLOG_WARNING("MQTT-only HUD tool callback called directly - should use MQTT execution");
   *should_respond = 1;
   return strdup("HUD command could not be delivered — the helmet hardware doesn't appear to "
                 "be connected right now.  Let the user know the HUD is offline.");
}

/* =============================================================================
 * HUD Control Meta-tool
 * ============================================================================= */

static const treg_param_t hud_control_params[] = {
   {
       .name = "element",
       .description = "The HUD element to control.  Valid values are discovered "
                      "from the connected helmet/HUD at runtime — only call this "
                      "tool when the LLM has seen a non-empty enum_values list "
                      "in the schema (gated by is_available below).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_DEVICE,
       .enum_values = { NULL }, /* Populated by discovery */
       .enum_count = 0,
   },
   {
       .name = "action",
       .description = "Whether to show or hide the element",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "enable", "disable" },
       .enum_count = 2,
   },
};

/* Hide hud_control from the LLM schema until discovery has populated
 * the `element` enum.  Otherwise the LLM sees a tool with an empty
 * enum array, infers no valid input exists, and either skips silently
 * or fabricates element names ("hud", "main", etc.).  Discovery
 * normally completes within seconds of MQTT connection; until it does,
 * the tool stays hidden (there are no valid elements to control). */
static bool hud_control_is_available(void) {
   return hud_discovery_get_element_count() > 0;
}

/* Device map not needed - element names are used directly as device strings */

static const tool_metadata_t hud_control_metadata = {
   .name = "hud_control",
   .device_string = "hud_control",
   .topic = "hud",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Control HUD (Heads-Up Display) elements. Enable or disable display overlays "
                  "like the armor display, minimap, object detection, or info panel.",
   .params = hud_control_params,
   .param_count = 2,

   .device_map = NULL,
   .device_map_count = 0,

   .device_type = TOOL_DEVICE_TYPE_BOOLEAN,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = true,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = mqtt_only_stub_callback,

   .is_available = hud_control_is_available,
};

int hud_control_tool_register(void) {
   return tool_registry_register(&hud_control_metadata);
}

/* =============================================================================
 * HUD Mode Tool
 * ============================================================================= */

static const treg_param_t hud_mode_params[] = {
   {
       .name = "mode",
       .description = "The HUD mode to switch to.  Valid values are discovered "
                      "from the connected helmet at runtime; the enum below is "
                      "the live set this user's HUD supports (not a fixed list).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
       .enum_values = { "default" },
       .enum_count = 1,
   },
};

/* Hide hud_mode until at least one mode has been discovered.  Unlike
 * hud_control (which truly has no valid value until elements are
 * known), hud_mode CAN always fall back to "default" — but the tool's
 * intent is mode-switching, and a one-element enum reads as broken to
 * the LLM ("why ask if there's only one option?").  Gate on > 1
 * discovered modes so the tool only appears when there's a real
 * choice to make. */
static bool hud_mode_is_available(void) {
   return hud_discovery_get_mode_count() > 1;
}

static const tool_metadata_t hud_mode_metadata = {
   .name = "hud_mode",
   .device_string = "hud",
   .topic = "hud",
   .aliases = { "display", "screen" },
   .alias_count = 2,

   .description = "Switch the HUD display mode.  Available modes are discovered "
                  "from the connected helmet — see the `mode` parameter's "
                  "enum_values for the live set.",
   .params = hud_mode_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = true,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = mqtt_only_stub_callback,

   .is_available = hud_mode_is_available,
};

int hud_mode_tool_register(void) {
   return tool_registry_register(&hud_mode_metadata);
}

/* =============================================================================
 * Faceplate Tool
 * ============================================================================= */

static const treg_param_t faceplate_params[] = {
   {
       .name = "action",
       .description = "Whether to open or close the faceplate",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "enable", "disable" },
       .enum_count = 2,
   },
};

static const tool_metadata_t faceplate_metadata = {
   .name = "faceplate",
   .device_string = "faceplate",
   .topic = "helmet",
   .aliases = { "face plate", "mask", "helmet", "visor" },
   .alias_count = 4,

   .description = "Control the helmet faceplate/visor. Open or close the faceplate.",
   .params = faceplate_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_BOOLEAN,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = true,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = mqtt_only_stub_callback,
};

int faceplate_tool_register(void) {
   return tool_registry_register(&faceplate_metadata);
}

/* =============================================================================
 * Recording Meta-tool
 * ============================================================================= */

static const treg_param_t recording_params[] = {
   {
       .name = "mode",
       .description = "What to control",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_DEVICE,
       .enum_values = { "record", "stream", "record_and_stream" },
       .enum_count = 3,
   },
   {
       .name = "action",
       .description = "Whether to start or stop",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "enable", "disable" },
       .enum_count = 2,
   },
};

static const tool_device_map_t recording_device_map[] = {
   { "record", "record" },
   { "stream", "stream" },
   { "record_and_stream", "record and stream" },
};

static const tool_metadata_t recording_metadata = {
   .name = "recording",
   .device_string = "recording",
   .topic = "hud",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Control video recording and streaming. Start or stop recording, streaming, "
                  "or both simultaneously.",
   .params = recording_params,
   .param_count = 2,

   .device_map = recording_device_map,
   .device_map_count = 3,

   .device_type = TOOL_DEVICE_TYPE_BOOLEAN,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = true,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = mqtt_only_stub_callback,
};

int recording_tool_register(void) {
   return tool_registry_register(&recording_metadata);
}

/* =============================================================================
 * Visual Offset Tool
 * ============================================================================= */

static const treg_param_t visual_offset_params[] = {
   {
       .name = "pixels",
       .description = "Offset in pixels (positive or negative)",
       .type = TOOL_PARAM_TYPE_INT,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t visual_offset_metadata = {
   .name = "visual_offset",
   .device_string = "visual offset",
   .topic = "hud",
   .aliases = { "3d offset", "eye offset" },
   .alias_count = 2,

   .description = "Adjust the 3D visual offset for stereoscopic display alignment.",
   .params = visual_offset_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = true,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = mqtt_only_stub_callback,
};

int visual_offset_tool_register(void) {
   return tool_registry_register(&visual_offset_metadata);
}
