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
 * Weather Tool - Weather forecasts via Open-Meteo API
 *
 * Supports actions: today, tomorrow, week (with optional location)
 */

#include "tools/weather_tool.h"

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "tools/weather_service.h"

/* ========== Constants ========== */

#define WEATHER_RESULT_BUFFER_SIZE 2048 /* Sized for week forecast */

/* ========== Forward Declarations ========== */

static char *weather_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t weather_params[] = {
   {
       .name = "action",
       .description = "The forecast type: 'today' (current conditions), "
                      "'tomorrow' (today and tomorrow), 'week' (7-day forecast)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "today", "tomorrow", "week", "get" },
       .enum_count = 4,
   },
   {
       .name = "location",
       .description = "Location for weather (e.g., 'New York City' or 'Paris, France'). "
                      "Optional - uses default from config if not provided.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t weather_metadata = {
   .name = "weather",
   .device_string = "weather",
   .topic = "dawn",
   .aliases = { "forecast" },
   .alias_count = 1,

   .description = "Get weather forecasts for any location. Use 'today' for current conditions, "
                  "'tomorrow' for today and tomorrow, or 'week' for a 7-day forecast. "
                  "Location is optional if a default is configured.",
   .params = weather_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SCHEDULABLE | TOOL_CAP_INFORMATIONAL,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = weather_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *weather_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1; /* Always return results to LLM */

   if (!action)
      action = "today";

   /* Determine forecast type from action */
   forecast_type_t forecast = FORECAST_TODAY; /* Default */

   if (strcmp(action, "get") == 0 || strcmp(action, "today") == 0) {
      forecast = FORECAST_TODAY;
   } else if (strcmp(action, "tomorrow") == 0) {
      forecast = FORECAST_TOMORROW;
   } else if (strcmp(action, "week") == 0) {
      forecast = FORECAST_WEEK;
   } else {
      return strdup("Unknown weather action. Use: 'today', 'tomorrow', or 'week' with a location.");
   }

   /* Use provided location or fall back to config default */
   const char *location = value;
   if (value == NULL || strlen(value) == 0) {
      if (g_config.localization.location[0] != '\0') {
         location = g_config.localization.location;
         OLOG_INFO("weather_tool_callback: Using default location from config: %s", location);
      } else {
         OLOG_WARNING("weather_tool_callback: No location provided and no default configured");
         return strdup("Please specify a location for the weather request.");
      }
   }

   OLOG_INFO("weather_tool_callback: Fetching %s weather for '%s'",
             forecast == FORECAST_WEEK ? "week"
                                       : (forecast == FORECAST_TOMORROW ? "tomorrow" : "today"),
             location);

   weather_response_t *response = weather_get(location, forecast);
   if (response) {
      if (response->error) {
         OLOG_ERROR("weather_tool_callback: Weather error: %s", response->error);
         char *result = malloc(256);
         if (result) {
            snprintf(result, 256, TOOL_RESULT_ERROR_MARK "Weather lookup failed: %s",
                     response->error);
         }
         weather_free_response(response);
         return result ? result : strdup(TOOL_RESULT_ERROR_MARK "Weather lookup failed.");
      }

      char *result = malloc(WEATHER_RESULT_BUFFER_SIZE);
      if (result) {
         int formatted_len = 0;
         if (weather_format_for_llm(response, result, WEATHER_RESULT_BUFFER_SIZE, &formatted_len) !=
                 SUCCESS ||
             formatted_len <= 0 || (size_t)formatted_len >= WEATHER_RESULT_BUFFER_SIZE) {
            OLOG_ERROR("weather_tool_callback: Weather data truncated (needed %d bytes, have %d)",
                       formatted_len, WEATHER_RESULT_BUFFER_SIZE);
            free(result);
            weather_free_response(response);
            return strdup(TOOL_RESULT_ERROR_MARK "Weather data too large to format.");
         }
         OLOG_INFO("weather_tool_callback: Weather data retrieved successfully (%d bytes)",
                   formatted_len);
      }
      weather_free_response(response);
      return result ? result : strdup(TOOL_RESULT_ERROR_MARK "Failed to format weather data.");
   }

   return strdup(TOOL_RESULT_ERROR_MARK "Weather request failed.");
}

/* ========== Public API ========== */

int weather_tool_register(void) {
   return tool_registry_register(&weather_metadata);
}
