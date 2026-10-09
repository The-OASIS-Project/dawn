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
 * Weather service using Open-Meteo API (free, no API key required)
 */

#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <stddef.h>

// Open-Meteo API endpoints
#define OPEN_METEO_GEOCODING_URL "https://geocoding-api.open-meteo.com/v1/search"
#define OPEN_METEO_FORECAST_URL "https://api.open-meteo.com/v1/forecast"

// Weather condition codes (WMO)
typedef enum {
   WEATHER_CLEAR = 0,
   WEATHER_MAINLY_CLEAR = 1,
   WEATHER_PARTLY_CLOUDY = 2,
   WEATHER_OVERCAST = 3,
   WEATHER_FOG = 45,
   WEATHER_DEPOSITING_RIME_FOG = 48,
   WEATHER_DRIZZLE_LIGHT = 51,
   WEATHER_DRIZZLE_MODERATE = 53,
   WEATHER_DRIZZLE_DENSE = 55,
   WEATHER_FREEZING_DRIZZLE_LIGHT = 56,
   WEATHER_FREEZING_DRIZZLE_DENSE = 57,
   WEATHER_RAIN_SLIGHT = 61,
   WEATHER_RAIN_MODERATE = 63,
   WEATHER_RAIN_HEAVY = 65,
   WEATHER_FREEZING_RAIN_LIGHT = 66,
   WEATHER_FREEZING_RAIN_HEAVY = 67,
   WEATHER_SNOW_SLIGHT = 71,
   WEATHER_SNOW_MODERATE = 73,
   WEATHER_SNOW_HEAVY = 75,
   WEATHER_SNOW_GRAINS = 77,
   WEATHER_RAIN_SHOWERS_SLIGHT = 80,
   WEATHER_RAIN_SHOWERS_MODERATE = 81,
   WEATHER_RAIN_SHOWERS_VIOLENT = 82,
   WEATHER_SNOW_SHOWERS_SLIGHT = 85,
   WEATHER_SNOW_SHOWERS_HEAVY = 86,
   WEATHER_THUNDERSTORM = 95,
   WEATHER_THUNDERSTORM_HAIL_SLIGHT = 96,
   WEATHER_THUNDERSTORM_HAIL_HEAVY = 99
} weather_code_t;

// Current weather data
typedef struct {
   double temperature_f;     // Current temperature in Fahrenheit
   double feels_like_f;      // Feels like temperature in Fahrenheit
   double humidity;          // Relative humidity percentage
   double wind_speed_mph;    // Wind speed in mph
   double wind_gusts_mph;    // Wind gusts in mph
   int wind_direction;       // Wind direction in degrees
   double precipitation_in;  // Precipitation in inches
   int weather_code;         // WMO weather code
   int is_day;               // 1 = day, 0 = night
   const char *condition;    // Human-readable condition (static string, do not free)
} current_weather_t;

// Daily forecast data
typedef struct {
   char *date;                   // Date string (YYYY-MM-DD)
   double high_f;                // High temperature in Fahrenheit
   double low_f;                 // Low temperature in Fahrenheit
   double precipitation_chance;  // Precipitation probability percentage
   double precipitation_in;      // Expected precipitation in inches
   int weather_code;             // WMO weather code
   const char *condition;        // Human-readable condition (static string, do not free)
} daily_forecast_t;

// Forecast type for weather requests
typedef enum {
   FORECAST_TODAY,     // Current conditions only
   FORECAST_TOMORROW,  // Today and tomorrow
   FORECAST_WEEK       // 7-day forecast
} forecast_type_t;

#define MAX_FORECAST_DAYS 7

// Full weather response
typedef struct {
   char *location_name;  // Resolved location name
   double latitude;
   double longitude;
   current_weather_t current;
   daily_forecast_t daily[MAX_FORECAST_DAYS];  // Daily forecasts (0=today, 1=tomorrow, etc.)
   int num_days;                               // Number of days in forecast
   char *error;                                // Error message if any
} weather_response_t;

/**
 * Initialize the weather service
 * @return 0 on success, non-zero on failure
 */
int weather_service_init(void);

/**
 * Get weather for a location string (geocodes automatically)
 * @param location Location string (e.g., "Sugar Hill, Georgia")
 * @param forecast Forecast type (FORECAST_TODAY, FORECAST_TOMORROW, FORECAST_WEEK)
 * @return Weather response (caller must free with weather_free_response)
 */
weather_response_t *weather_get(const char *location, forecast_type_t forecast);

/**
 * Get weather for specific coordinates
 * @param latitude Latitude
 * @param longitude Longitude
 * @param location_name Optional location name for display
 * @param forecast Forecast type (FORECAST_TODAY, FORECAST_TOMORROW, FORECAST_WEEK)
 * @return Weather response (caller must free with weather_free_response)
 */
weather_response_t *weather_get_by_coords(double latitude,
                                          double longitude,
                                          const char *location_name,
                                          forecast_type_t forecast);

/**
 * Format weather response for LLM consumption
 * @param response Weather response
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @param bytes_written Number of characters written (output)
 * @return SUCCESS or FAILURE
 */
int weather_format_for_llm(const weather_response_t *response,
                           char *buffer,
                           size_t buffer_size,
                           int *bytes_written);

/**
 * Free a weather response
 * @param response Response to free
 */
void weather_free_response(weather_response_t *response);

/**
 * Cleanup the weather service
 */
void weather_service_cleanup(void);

/**
 * Convert WMO weather code to human-readable string
 * @param code WMO weather code
 * @return Human-readable condition string (static, do not free)
 */
const char *weather_code_to_string(int code);

#endif  // WEATHER_SERVICE_H
