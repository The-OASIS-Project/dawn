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
 * Date/Time Tools - Get current date and time with timezone support
 */

#include "tools/datetime_tool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dawn.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "tts/text_to_speech.h"

/* ========== Forward Declarations ========== */

static char *date_tool_callback(const char *action, char *value, int *should_respond);
static char *time_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Date Tool Metadata ========== */

static const tool_metadata_t date_metadata = {
   .name = "date",
   .device_string = "date",
   .topic = "dawn",
   .aliases = { "current date", "today" },
   .alias_count = 2,

   .description = "Get the current date. Returns today's date formatted for the user.",
   .params = NULL,
   .param_count = 0,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = date_tool_callback,
};

/* ========== Time Tool Metadata ========== */

static const tool_metadata_t time_metadata = {
   .name = "time",
   .device_string = "time",
   .topic = "dawn",
   .aliases = { "current time", "clock" },
   .alias_count = 2,

   .description = "Get the current time. Returns the current local time.",
   .params = NULL,
   .param_count = 0,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = time_tool_callback,
};

/* ========== Callback Implementations ========== */

static char *date_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;

   time_t current_time;
   char buffer[80];
   char *result = NULL;

   *should_respond = 1;

   time(&current_time);
   struct tm tm_storage;
   struct tm *time_info = localtime_r(&current_time, &tm_storage);
   strftime(buffer, sizeof(buffer), "%A, %B %d, %Y", time_info);

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      /* Direct mode: use TTS with personality */
      srand(time(NULL));
      int choice = rand() % 3;

      result = malloc(256);
      if (!result) {
         OLOG_ERROR("date_tool: malloc failed");
         *should_respond = 0;
         return NULL;
      }

      switch (choice) {
         case 0:
            snprintf(result, 256, "Today's date, dear Sir, is %s. You're welcome.", buffer);
            break;
         case 1:
            snprintf(result, 256, "In case you've forgotten, Sir, it's %s today.", buffer);
            break;
         case 2:
            snprintf(result, 256, "The current date is %s.", buffer);
            break;
      }

      text_to_speech(result);
      free(result);
      *should_respond = 0;
      return NULL;
   } else {
      /* AI modes: return raw data */
      result = malloc(128);
      if (!result) {
         OLOG_ERROR("date_tool: malloc failed");
         *should_respond = 0;
         return NULL;
      }
      snprintf(result, 128, "The current date is %s", buffer);
      return result;
   }
}

static char *time_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;

   time_t current_time;
   char buffer[80];
   char *result = NULL;

   *should_respond = 1;

   time(&current_time);
   struct tm tm_storage;
   struct tm *time_info = localtime_r(&current_time, &tm_storage);
   strftime(buffer, sizeof(buffer), "%I:%M:%S %p %Z", time_info);

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      /* Direct mode: use TTS with personality */
      srand(time(NULL));
      int choice = rand() % 4;

      result = malloc(256);
      if (!result) {
         OLOG_ERROR("time_tool: malloc failed");
         *should_respond = 0;
         return NULL;
      }

      switch (choice) {
         case 0:
            snprintf(result, 256,
                     "The current time, in case your wristwatch has failed you, is %s.", buffer);
            break;
         case 1:
            snprintf(result, 256, "I trust you have something important planned, Sir? It's %s.",
                     buffer);
            break;
         case 2:
            snprintf(result, 256,
                     "Oh, you want to know the time again? It's %s, not that I'm keeping track.",
                     buffer);
            break;
         case 3:
            snprintf(result, 256, "The time is %s.", buffer);
            break;
      }

      text_to_speech(result);
      free(result);
      *should_respond = 0;
      return NULL;
   } else {
      /* AI modes: return raw data */
      result = malloc(96);
      if (!result) {
         OLOG_ERROR("time_tool: malloc failed");
         *should_respond = 0;
         return NULL;
      }
      snprintf(result, 96, "The time is %s", buffer);
      return result;
   }
}

/* ========== Public API ========== */

int date_tool_register(void) {
   return tool_registry_register(&date_metadata);
}

int time_tool_register(void) {
   return tool_registry_register(&time_metadata);
}
