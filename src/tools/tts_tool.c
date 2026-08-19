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
 * TTS Tool - Speak text aloud using text-to-speech
 */

#include "tools/tts_tool.h"

#include <stddef.h>

#include "logging.h"
#include "tools/tool_registry.h"
#include "tts/text_to_speech.h"

/* ========== Forward Declarations ========== */

static char *tts_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Parameter Definitions ========== */

static const treg_param_t tts_tool_params[] = {
   {
       .name = "text",
       .description = "The text to speak aloud",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t tts_tool_metadata = {
   .name = "tts",
   .device_string = "text to speech",
   .topic = "dawn",
   .aliases = { "tts", "speak" },
   .alias_count = 2,

   .description = "Speak text aloud using text-to-speech.",
   .params = tts_tool_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = true,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = tts_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *tts_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action;

   if (should_respond != NULL) {
      *should_respond = 0;
   }

   if (!value || value[0] == '\0') {
      OLOG_WARNING("TTS: empty or NULL text, ignoring");
      /* Override the default no-respond so the LLM sees an explicit error
       * rather than a silent NULL — otherwise it can't tell the user why
       * nothing was spoken. */
      if (should_respond != NULL) {
         *should_respond = 1;
      }
      return strdup("TTS skipped: empty text. Provide non-empty text to speak.");
   }

   OLOG_INFO("TTS: \"%s\"", value);
   text_to_speech(value);
   return NULL;
}

/* ========== Public API ========== */

int tts_tool_register(void) {
   return tool_registry_register(&tts_tool_metadata);
}
