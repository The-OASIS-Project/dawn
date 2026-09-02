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
 * Volume Tool - Session-aware volume control with get/set actions
 *
 * Routes volume commands to the originating client:
 *   - WebUI session: updates conn->volume and syncs to browser
 *   - Satellite: forwards via DAP2 volume_set message
 *   - Daemon local: updates global_volume for local playback
 */

#include "tools/volume_tool.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/flac_playback.h"
#include "dawn.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "tts/text_to_speech.h"
#include "word_to_number.h"

/* Session routing for WebUI/satellite */
#ifdef ENABLE_WEBUI
#include "core/session_manager.h"
#include "webui/webui_internal.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_satellite.h"
#endif

/* ========== Forward Declarations ========== */

static char *volume_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Parameter Definitions ========== */

static const treg_param_t volume_params[] = {
   {
       .name = "action",
       .description = "Action to perform: 'set' to change volume, 'get' to query current level",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "set", "get" },
       .enum_count = 2,
   },
   {
       .name = "level",
       .description = "Volume level 0-100. Required for 'set' action.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t volume_metadata = {
   .name = "volume",
   .device_string = "volume",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Get or set the music volume level.",
   .params = volume_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_SCHEDULABLE,
   .skip_followup = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = volume_tool_callback,
};

/* ========== Volume Parsing Helper ========== */

float parse_volume_level(const char *value) {
   if (!value || !*value)
      return -1.0f;

   float vol = -1.0f;

   /* Try numeric parse first ("100", "50", "0.5") */
   char *endptr;
   double parsed = strtod(value, &endptr);
   if (endptr != value && *endptr == '\0' && isfinite(parsed)) {
      vol = (float)parsed;
   } else {
      /* wordToNumber tokenizes in-place via strtok_r, so copy input */
      char buf[256];
      snprintf(buf, sizeof(buf), "%s", value);
      vol = (float)wordToNumber(buf);
   }

   /* Normalize: values > 2.0 are assumed to be percentages */
   if (vol > 2.0f)
      vol /= 100.0f;

   /* Clamp to 0.0-1.0 (no amplification for remote clients) */
   if (vol < 0.0f)
      return -1.0f;
   if (vol > 1.0f)
      vol = 1.0f;

   return vol;
}

/* ========== Callback Implementation ========== */

static char *volume_tool_callback(const char *action, char *value, int *should_respond) {
   bool is_get = (action && strcmp(action, "get") == 0);
   bool is_set = !is_get; /* default to set for backward compat */

#ifdef ENABLE_WEBUI
   /* Route to originating client session (WebUI or satellite) */
   session_t *session = session_get_command_context();
   if (session && session->client_data) {
      ws_connection_t *conn = (ws_connection_t *)session->client_data;

      if (conn->is_satellite) {
         char *result = satellite_volume_execute_tool(conn, action, value, should_respond);
         return result;
      } else {
         char *result = webui_volume_execute_tool(conn, action, value, should_respond);
         return result;
      }
   }
#endif

   /* Daemon-local fallback (no WebUI session, or ENABLE_WEBUI disabled) */
   if (is_get) {
      float vol = getMusicVolume();
      char *result = malloc(64);
      if (result)
         snprintf(result, 64, "Volume is at %.0f%%", vol * 100.0f);
      *should_respond = 1;
      return result;
   }

   /* Action: set */
   if (!value || !*value) {
      *should_respond = 1;
      return strdup("Error: 'level' parameter is required for 'set' action.");
   }

   float vol = parse_volume_level(value);
   OLOG_INFO("Volume: %s -> %.2f", value, vol);

   if (vol < 0.0f) {
      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         text_to_speech("Invalid volume level requested.");
         *should_respond = 0;
         return NULL;
      }
      char *result = malloc(128);
      if (result)
         snprintf(result, 128, "Invalid volume level '%s'. Use a number 0-100.", value);
      *should_respond = 1;
      return result;
   }

   /* Daemon-local allows amplification up to 2.0x */
   if (is_set) {
      /* Re-parse without clamping for daemon local (allows >1.0) */
      float daemon_vol = vol;
      char *endptr;
      double raw = strtod(value, &endptr);
      if (endptr != value && *endptr == '\0') {
         daemon_vol = (float)raw;
         if (daemon_vol > 2.0f)
            daemon_vol /= 100.0f;
         if (daemon_vol < 0.0f)
            daemon_vol = 0.0f;
         if (daemon_vol > 2.0f)
            daemon_vol = 2.0f;
      }
      setMusicVolume(daemon_vol);

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         *should_respond = 0;
         return NULL;
      }
      char *result = malloc(64);
      if (result)
         snprintf(result, 64, "Volume set to %.0f%%", daemon_vol * 100.0f);
      *should_respond = 1;
      return result;
   }

   *should_respond = 0;
   return NULL;
}

/* ========== Public API ========== */

int volume_tool_register(void) {
   return tool_registry_register(&volume_metadata);
}
