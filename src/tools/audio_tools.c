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
 * Audio Tools - Voice amplifier and audio device control
 */

#include "tools/audio_tools.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "audio/mic_passthrough.h"
#include "dawn.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations for Existing Callbacks ========== */

/* From dawn.c */
char *setPcmPlaybackDevice(const char *actionName, char *value, int *should_respond);
char *setPcmCaptureDevice(const char *actionName, char *value, int *should_respond);

/* ========== Voice Amplifier State ========== */

static pthread_t voice_thread = -1;

/* ========== Internal Callbacks ========== */

/**
 * @brief Audio device callback that routes to capture or playback based on action
 *
 * The audio_device meta-tool uses the "device" parameter mapped to action to
 * determine whether to call capture or playback callback.
 */
static char *audio_device_callback(const char *action, char *value, int *should_respond) {
   if (!action) {
      *should_respond = 1;
      return strdup("No device type specified. Use 'capture' or 'playback'.");
   }

   if (strcmp(action, "capture") == 0) {
      return setPcmCaptureDevice("set", value, should_respond);
   } else if (strcmp(action, "playback") == 0) {
      return setPcmPlaybackDevice("set", value, should_respond);
   } else {
      *should_respond = 1;
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128,
                  TOOL_RESULT_ERROR_MARK "Unknown device type '%s'. Use 'capture' or 'playback'.",
                  action);
      }
      return result;
   }
}

/* =============================================================================
 * Voice Amplifier Tool
 * ============================================================================= */

static char *voice_amplifier_callback(const char *actionName, char *value, int *should_respond) {
   (void)value;
   *should_respond = 1;

   if (strcmp(actionName, "enable") == 0) {
      if ((voice_thread != (pthread_t)-1) && (pthread_kill(voice_thread, 0) == 0)) {
         OLOG_WARNING("Voice amplification thread already running.");
         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Voice amplifier is already enabled");
         }
         *should_respond = 0;
         return NULL;
      }

      if (pthread_create(&voice_thread, NULL, voiceAmplificationThread, NULL)) {
         OLOG_ERROR("Error creating voice thread");
         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to enable voice amplifier");
         }
         *should_respond = 0;
         return NULL;
      }

      if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
         return strdup("Voice amplifier enabled");
      }
      *should_respond = 0;
      return NULL;

   } else if (strcmp(actionName, "disable") == 0) {
      if ((voice_thread != (pthread_t)-1) && (pthread_kill(voice_thread, 0) == 0)) {
         setStopVA();
         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Voice amplifier disabled");
         }
      } else {
         OLOG_WARNING("Voice amplification thread not running.");
         if (command_processing_mode != CMD_MODE_DIRECT_ONLY) {
            return strdup("Voice amplifier was not running");
         }
      }
      *should_respond = 0;
      return NULL;
   }

   return NULL;
}

static const treg_param_t voice_amplifier_params[] = {
   {
       .name = "action",
       .description = "Whether to enable or disable voice amplification",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "enable", "disable" },
       .enum_count = 2,
   },
};

static const tool_metadata_t voice_amplifier_metadata = {
   .name = "voice_amplifier",
   .device_string = "voice amplifier",
   .topic = "dawn",
   .aliases = { "pa", "pa system", "bullhorn" },
   .alias_count = 3,

   .description =
       "Control the voice amplifier/PA system for projecting voice through external speakers.",
   .params = voice_amplifier_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_BOOLEAN,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .skip_followup = false,
   .mqtt_only = false,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = voice_amplifier_callback,
};

int voice_amplifier_tool_register(void) {
   return tool_registry_register(&voice_amplifier_metadata);
}

/* =============================================================================
 * Audio Device Meta-tool
 * ============================================================================= */

static const treg_param_t audio_device_params[] = {
   {
       .name = "type",
       .description = "The type of audio device to change",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "capture", "playback" },
       .enum_count = 2,
   },
   {
       .name = "device",
       .description = "Friendly name of a configured audio device, NOT an ALSA "
                      "string.  Match against the names + aliases in dawn.toml "
                      "[[audio.devices]].  Common examples: 'microphone', "
                      "'headphones', 'speakers', 'usb headset'.  Case-"
                      "insensitive.  ALSA-style values like 'hw:1,0' or "
                      "'plughw:CARD=USB' will NOT match — those are the "
                      "underlying targets the friendly name resolves to.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_device_map_t audio_device_map[] = {
   { "capture", "audio capture device" },
   { "playback", "audio playback device" },
};

static const tool_metadata_t audio_device_metadata = {
   .name = "audio_device",
   .device_string = "audio_device",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Switch audio input or output devices.",
   .params = audio_device_params,
   .param_count = 2,

   .device_map = audio_device_map,
   .device_map_count = 2,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .mqtt_only = false,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = audio_device_callback,
};

int audio_device_tool_register(void) {
   return tool_registry_register(&audio_device_metadata);
}
