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
 * Weather service implementation using Open-Meteo API (free, no API key required)
 */

#include "tools/weather_service.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/curl_buffer.h"
#include "dawn_error.h"
#include "logging.h"
#include "utils/string_utils.h"

// Module state (thread-safe with mutex protection)
static int module_initialized = 0;
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;

// Convert Celsius to Fahrenheit
static double celsius_to_fahrenheit(double celsius) {
   return (celsius * 9.0 / 5.0) + 32.0;
}

// Convert km/h to mph
static double kmh_to_mph(double kmh) {
   return kmh * 0.621371;
}

// Get relative day label for forecast (index 0=today, 1=tomorrow, etc.)
// date_str is in "YYYY-MM-DD" format
// Thread-safe: uses thread-local storage for the label buffer
static const char *get_relative_day_label(int day_index, const char *date_str) {
   static __thread char label_buf[32];
   static const char *day_names[] = { "Sunday",   "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday" };

   if (day_index == 0) {
      return "today";
   } else if (day_index == 1) {
      return "tomorrow";
   }

   // For day 2+, get the day name from the date string
   if (date_str && strlen(date_str) >= 10) {
      struct tm tm = { 0 };
      int year, month, day;
      if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) == 3) {
         tm.tm_year = year - 1900;
         tm.tm_mon = month - 1;
         tm.tm_mday = day;
         mktime(&tm);  // Normalizes and fills in tm_wday
         // Bounds check tm_wday (should be 0-6, but validate for safety)
         if (tm.tm_wday >= 0 && tm.tm_wday <= 6) {
            snprintf(label_buf, sizeof(label_buf), "%s", day_names[tm.tm_wday]);
            return label_buf;
         }
      }
   }

   // Fallback
   snprintf(label_buf, sizeof(label_buf), "day %d", day_index);
   return label_buf;
}

// Convert mm to inches
static double mm_to_inches(double mm) {
   return mm * 0.0393701;
}

const char *weather_code_to_string(int code) {
   switch (code) {
      case 0:
         return "Clear sky";
      case 1:
         return "Mainly clear";
      case 2:
         return "Partly cloudy";
      case 3:
         return "Overcast";
      case 45:
         return "Foggy";
      case 48:
         return "Depositing rime fog";
      case 51:
         return "Light drizzle";
      case 53:
         return "Moderate drizzle";
      case 55:
         return "Dense drizzle";
      case 56:
         return "Light freezing drizzle";
      case 57:
         return "Dense freezing drizzle";
      case 61:
         return "Slight rain";
      case 63:
         return "Moderate rain";
      case 65:
         return "Heavy rain";
      case 66:
         return "Light freezing rain";
      case 67:
         return "Heavy freezing rain";
      case 71:
         return "Slight snow";
      case 73:
         return "Moderate snow";
      case 75:
         return "Heavy snow";
      case 77:
         return "Snow grains";
      case 80:
         return "Slight rain showers";
      case 81:
         return "Moderate rain showers";
      case 82:
         return "Violent rain showers";
      case 85:
         return "Slight snow showers";
      case 86:
         return "Heavy snow showers";
      case 95:
         return "Thunderstorm";
      case 96:
         return "Thunderstorm with slight hail";
      case 99:
         return "Thunderstorm with heavy hail";
      default:
         return "Unknown";
   }
}

int weather_service_init(void) {
   pthread_mutex_lock(&module_mutex);

   if (module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      OLOG_WARNING("weather_service: Already initialized");
      return 0;
   }

   // Note: curl_global_init() is called in main() - not here
   module_initialized = 1;
   pthread_mutex_unlock(&module_mutex);
   OLOG_INFO("weather_service: Initialized");
   return 0;
}

void weather_service_cleanup(void) {
   pthread_mutex_lock(&module_mutex);

   if (!module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      return;
   }

   // Note: curl_global_cleanup() is called in main() - not here
   module_initialized = 0;
   pthread_mutex_unlock(&module_mutex);
   OLOG_INFO("weather_service: Cleanup complete");
}

int weather_service_is_initialized(void) {
   // Use atomic load for thread-safe access without full mutex lock
   return __atomic_load_n(&module_initialized, __ATOMIC_ACQUIRE);
}

// US state abbreviation to full name mapping
static const struct {
   const char *abbrev;
   const char *full;
} us_state_map[] = { { "AL", "Alabama" },
                     { "AK", "Alaska" },
                     { "AZ", "Arizona" },
                     { "AR", "Arkansas" },
                     { "CA", "California" },
                     { "CO", "Colorado" },
                     { "CT", "Connecticut" },
                     { "DE", "Delaware" },
                     { "FL", "Florida" },
                     { "GA", "Georgia" },
                     { "HI", "Hawaii" },
                     { "ID", "Idaho" },
                     { "IL", "Illinois" },
                     { "IN", "Indiana" },
                     { "IA", "Iowa" },
                     { "KS", "Kansas" },
                     { "KY", "Kentucky" },
                     { "LA", "Louisiana" },
                     { "ME", "Maine" },
                     { "MD", "Maryland" },
                     { "MA", "Massachusetts" },
                     { "MI", "Michigan" },
                     { "MN", "Minnesota" },
                     { "MS", "Mississippi" },
                     { "MO", "Missouri" },
                     { "MT", "Montana" },
                     { "NE", "Nebraska" },
                     { "NV", "Nevada" },
                     { "NH", "New Hampshire" },
                     { "NJ", "New Jersey" },
                     { "NM", "New Mexico" },
                     { "NY", "New York" },
                     { "NC", "North Carolina" },
                     { "ND", "North Dakota" },
                     { "OH", "Ohio" },
                     { "OK", "Oklahoma" },
                     { "OR", "Oregon" },
                     { "PA", "Pennsylvania" },
                     { "RI", "Rhode Island" },
                     { "SC", "South Carolina" },
                     { "SD", "South Dakota" },
                     { "TN", "Tennessee" },
                     { "TX", "Texas" },
                     { "UT", "Utah" },
                     { "VT", "Vermont" },
                     { "VA", "Virginia" },
                     { "WA", "Washington" },
                     { "WV", "West Virginia" },
                     { "WI", "Wisconsin" },
                     { "WY", "Wyoming" },
                     { "DC", "District of Columbia" },
                     { NULL, NULL } };

/**
 * @brief Expand US state abbreviation to full name
 * @param abbrev State abbreviation (e.g., "GA")
 * @return Full state name (e.g., "Georgia") or NULL if not found
 */
static const char *expand_state_abbrev(const char *abbrev) {
   if (!abbrev || strlen(abbrev) != 2) {
      return NULL;
   }
   // Make uppercase copy for comparison
   char upper[3];
   upper[0] = toupper((unsigned char)abbrev[0]);
   upper[1] = toupper((unsigned char)abbrev[1]);
   upper[2] = '\0';

   for (int i = 0; us_state_map[i].abbrev != NULL; i++) {
      if (strcmp(upper, us_state_map[i].abbrev) == 0) {
         return us_state_map[i].full;
      }
   }
   return NULL;
}

// Case-insensitive substring match
static int str_contains_ci(const char *haystack, const char *needle) {
   if (!haystack || !needle)
      return 0;

   size_t hay_len = strlen(haystack);
   size_t needle_len = strlen(needle);

   if (needle_len > hay_len)
      return 0;

   for (size_t i = 0; i <= hay_len - needle_len; i++) {
      int match = 1;
      for (size_t j = 0; j < needle_len; j++) {
         if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
            match = 0;
            break;
         }
      }
      if (match)
         return 1;
   }
   return 0;
}

// Geocode a location string to lat/lon
static int geocode_location(const char *location,
                            double *latitude,
                            double *longitude,
                            char **resolved_name) {
   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_ERROR("Failed to initialize curl for geocoding");
      return 1;
   }

   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, CURL_BUFFER_MAX_WEB_SEARCH);

   // Parse location into city and optional state/region
   // Supports formats: "Atlanta, Georgia", "Atlanta Georgia", "Atlanta, GA", "Atlanta"
   char city_name[256] = { 0 };
   char state_filter[128] = { 0 };

   snprintf(city_name, sizeof(city_name), "%s", location);

   // Check for comma separator
   char *comma = strchr(city_name, ',');
   if (comma) {
      *comma = '\0';
      // Extract state/region after comma
      const char *state_start = comma + 1;
      while (*state_start == ' ')
         state_start++;
      safe_strscpy(state_filter, state_start);
      // Trim trailing whitespace from state
      char *end = state_filter + strlen(state_filter) - 1;
      while (end > state_filter && *end == ' ')
         *end-- = '\0';

      // Expand US state abbreviation to full name (e.g., "GA" -> "Georgia")
      const char *expanded = expand_state_abbrev(state_filter);
      if (expanded) {
         OLOG_INFO("Geocoding: Expanded state '%s' to '%s'", state_filter, expanded);
         safe_strscpy(state_filter, expanded);
      }
   }

   // Trim whitespace from city name
   char *city_start = city_name;
   while (*city_start == ' ')
      city_start++;
   char *city_end = city_start + strlen(city_start) - 1;
   while (city_end > city_start && *city_end == ' ')
      *city_end-- = '\0';

   // URL encode the city name
   char *encoded_location = curl_easy_escape(curl, city_start, 0);
   if (!encoded_location) {
      curl_easy_cleanup(curl);
      return 1;
   }

   OLOG_INFO("Geocoding: city='%s', state_filter='%s'", city_start,
             state_filter[0] ? state_filter : "(none)");

   // Request multiple results so we can filter by state
   char url[512];
   snprintf(url, sizeof(url), "%s?name=%s&count=10&language=en&format=json",
            OPEN_METEO_GEOCODING_URL, encoded_location);
   curl_free(encoded_location);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

   CURLcode res = curl_easy_perform(curl);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      OLOG_ERROR("Geocoding request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buffer);
      return 1;
   }

   // Parse JSON response
   struct json_object *root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!root) {
      OLOG_ERROR("Failed to parse geocoding response");
      return 1;
   }

   struct json_object *results;
   if (!json_object_object_get_ex(root, "results", &results) ||
       json_object_array_length(results) == 0) {
      OLOG_ERROR("No geocoding results found for: %s", location);
      json_object_put(root);
      return 1;
   }

   // Find the best matching result
   struct json_object *best_result = NULL;
   size_t num_results = json_object_array_length(results);

   if (state_filter[0]) {
      // Filter results by state/admin1
      for (size_t i = 0; i < num_results; i++) {
         struct json_object *result = json_object_array_get_idx(results, i);
         struct json_object *admin1_obj;

         if (json_object_object_get_ex(result, "admin1", &admin1_obj)) {
            const char *admin1 = json_object_get_string(admin1_obj);
            // Check if state filter matches admin1 (case-insensitive, partial match)
            if (admin1 && str_contains_ci(admin1, state_filter)) {
               best_result = result;
               OLOG_INFO("Matched result %zu: admin1='%s' matches filter '%s'", i, admin1,
                         state_filter);
               break;
            }
         }
      }

      if (!best_result) {
         OLOG_WARNING("No results matched state filter '%s', using first result", state_filter);
      }
   }

   // Fall back to first result if no state match
   if (!best_result) {
      best_result = json_object_array_get_idx(results, 0);
   }

   struct json_object *lat_obj, *lon_obj, *name_obj, *admin1_obj, *country_obj;

   json_object_object_get_ex(best_result, "latitude", &lat_obj);
   json_object_object_get_ex(best_result, "longitude", &lon_obj);
   json_object_object_get_ex(best_result, "name", &name_obj);
   json_object_object_get_ex(best_result, "admin1", &admin1_obj);
   json_object_object_get_ex(best_result, "country", &country_obj);

   *latitude = json_object_get_double(lat_obj);
   *longitude = json_object_get_double(lon_obj);

   // Build resolved name
   const char *name = name_obj ? json_object_get_string(name_obj) : "Unknown";
   const char *admin1 = admin1_obj ? json_object_get_string(admin1_obj) : NULL;
   const char *country = country_obj ? json_object_get_string(country_obj) : NULL;

   char name_buf[256];
   if (admin1 && country) {
      snprintf(name_buf, sizeof(name_buf), "%s, %s, %s", name, admin1, country);
   } else if (admin1) {
      snprintf(name_buf, sizeof(name_buf), "%s, %s", name, admin1);
   } else {
      snprintf(name_buf, sizeof(name_buf), "%s", name);
   }
   *resolved_name = strdup(name_buf);

   OLOG_INFO("Resolved location: %s (lat=%.4f, lon=%.4f)", *resolved_name, *latitude, *longitude);

   json_object_put(root);
   return 0;
}

// Fetch weather data from Open-Meteo
static weather_response_t *fetch_weather(double latitude,
                                         double longitude,
                                         const char *location_name,
                                         forecast_type_t forecast) {
   weather_response_t *response = calloc(1, sizeof(weather_response_t));
   if (!response) {
      return NULL;
   }

   response->latitude = latitude;
   response->longitude = longitude;
   if (location_name) {
      response->location_name = strdup(location_name);
   }

   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, CURL_BUFFER_MAX_WEB_SEARCH);

   // Determine number of forecast days based on type
   int forecast_days;
   switch (forecast) {
      case FORECAST_TODAY:
         forecast_days = 1;
         break;
      case FORECAST_TOMORROW:
         forecast_days = 2;
         break;
      case FORECAST_WEEK:
      default:
         forecast_days = 7;
         break;
   }

   // Build forecast URL with all needed parameters
   // Using metric units from API, will convert to imperial
   // Note: 'time' is automatically returned by the API, don't request it explicitly
   char url[1024];
   snprintf(url, sizeof(url),
            "%s?latitude=%.4f&longitude=%.4f"
            "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
            "precipitation,weather_code,wind_speed_10m,wind_direction_10m,wind_gusts_10m,is_day"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
            "precipitation_sum,precipitation_probability_max"
            "&timezone=auto&forecast_days=%d",
            OPEN_METEO_FORECAST_URL, latitude, longitude, forecast_days);

   // Try up to 4 times with exponential backoff on transient-looking
   // failure.  Open-Meteo occasionally serves HTML error pages, 5xx, or
   // empty bodies during load spikes — often at the top of the hour, when
   // scheduled briefings fire and everyone's cron jobs hit it at once.
   // Those spikes can persist for several seconds, so a couple of retries
   // walked out to ~7s ride past them without burning much latency on
   // genuinely persistent failures (5xx come back immediately, not as
   // timeouts).
   //
   // Retry triggers:
   //   - CURLcode != OK (network failure)
   //   - HTTP 5xx (upstream server error)
   //   - JSON parse failure (HTML error page, truncated body, etc.)
   //
   // Diagnostic logging on terminal parse failure (body snippet + HTTP
   // status) is what unblocks future investigation — the prior code
   // surfaced only "Failed to parse weather response" with no context.
   // Open-Meteo doesn't require auth and the weather endpoint takes no
   // user-identifiable data, so logging body excerpts is privacy-safe.
   struct json_object *root = NULL;
   CURLcode res = CURLE_OK;
   long http_code = 0;
   const int max_attempts = 4;

   for (int attempt = 0; attempt < max_attempts; attempt++) {
      if (attempt > 0) {
         /* Exponential backoff: 1s, 2s, 4s before attempts 2/3/4. */
         long backoff_ms = 1000L << (attempt - 1);
         OLOG_INFO("Weather: retrying after transient error (HTTP=%ld curl=%s, attempt %d/%d, "
                   "backoff=%ldms)",
                   http_code, (res != CURLE_OK) ? curl_easy_strerror(res) : "OK", attempt + 1,
                   max_attempts, backoff_ms);
         curl_buffer_reset(&buffer);
         struct timespec ts = { .tv_sec = backoff_ms / 1000,
                                .tv_nsec = (backoff_ms % 1000) * 1000000L };
         nanosleep(&ts, NULL);
      }

      CURL *curl = curl_easy_init();
      if (!curl) {
         response->error = strdup("Failed to initialize curl");
         curl_buffer_free(&buffer);
         return response;
      }

      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

      res = curl_easy_perform(curl);
      http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      curl_easy_cleanup(curl);

      if (res != CURLE_OK) {
         continue;  // Network failure — retry if attempts remain
      }
      if (http_code >= 500 && http_code < 600) {
         continue;  // Upstream 5xx — retry
      }
      // Try to parse.  Success → break; failure → retry (or fall through).
      root = json_tokener_parse(buffer.data);
      if (root != NULL) {
         break;
      }
   }

   if (res != CURLE_OK) {
      response->error = strdup(curl_easy_strerror(res));
      curl_buffer_free(&buffer);
      return response;
   }

   if (!root) {
      // Capture diagnostic on terminal parse failure: HTTP status, body
      // size, first 200 bytes of body (control chars sanitized for log
      // readability).  Body excerpt is privacy-safe per the comment
      // above the retry loop.
      const size_t snippet_cap = 200;
      char snippet[201] = { 0 };
      size_t snippet_len = 0;
      if (buffer.data != NULL && buffer.size > 0) {
         snippet_len = buffer.size < snippet_cap ? buffer.size : snippet_cap;
         memcpy(snippet, buffer.data, snippet_len);
         for (size_t i = 0; i < snippet_len; i++) {
            unsigned char c = (unsigned char)snippet[i];
            if (c == '\n' || c == '\r' || c == '\t')
               snippet[i] = ' ';
            else if (c < 32 || c == 127)
               snippet[i] = '?';
         }
      }
      OLOG_WARNING("Weather parse failed: HTTP=%ld body_size=%zu body[0:%zu]=\"%s%s\"", http_code,
                   buffer.size, snippet_len, snippet, buffer.size > snippet_cap ? "..." : "");

      // Surface HTTP status to the caller so the LLM (and operator) can
      // distinguish upstream error from parse failure.
      char err_buf[160];
      if (http_code >= 400) {
         snprintf(err_buf, sizeof(err_buf), "Weather upstream HTTP %ld", http_code);
      } else if (buffer.size == 0) {
         snprintf(err_buf, sizeof(err_buf), "Weather upstream returned empty body (HTTP %ld)",
                  http_code);
      } else {
         snprintf(err_buf, sizeof(err_buf),
                  "Failed to parse weather response (HTTP %ld, %zu bytes)", http_code, buffer.size);
      }
      response->error = strdup(err_buf);
      curl_buffer_free(&buffer);
      return response;
   }

   curl_buffer_free(&buffer);

   // Parse current weather
   struct json_object *current;
   if (json_object_object_get_ex(root, "current", &current)) {
      struct json_object *val;

      if (json_object_object_get_ex(current, "temperature_2m", &val)) {
         response->current.temperature_f = celsius_to_fahrenheit(json_object_get_double(val));
      }
      if (json_object_object_get_ex(current, "apparent_temperature", &val)) {
         response->current.feels_like_f = celsius_to_fahrenheit(json_object_get_double(val));
      }
      if (json_object_object_get_ex(current, "relative_humidity_2m", &val)) {
         response->current.humidity = json_object_get_double(val);
      }
      if (json_object_object_get_ex(current, "wind_speed_10m", &val)) {
         response->current.wind_speed_mph = kmh_to_mph(json_object_get_double(val));
      }
      if (json_object_object_get_ex(current, "wind_gusts_10m", &val)) {
         response->current.wind_gusts_mph = kmh_to_mph(json_object_get_double(val));
      }
      if (json_object_object_get_ex(current, "wind_direction_10m", &val)) {
         response->current.wind_direction = json_object_get_int(val);
      }
      if (json_object_object_get_ex(current, "precipitation", &val)) {
         response->current.precipitation_in = mm_to_inches(json_object_get_double(val));
      }
      if (json_object_object_get_ex(current, "weather_code", &val)) {
         response->current.weather_code = json_object_get_int(val);
         // No strdup needed - weather_code_to_string returns static string
         response->current.condition = weather_code_to_string(response->current.weather_code);
      }
      if (json_object_object_get_ex(current, "is_day", &val)) {
         response->current.is_day = json_object_get_int(val);
      }
   }

   // Parse daily forecast
   struct json_object *daily;
   if (json_object_object_get_ex(root, "daily", &daily)) {
      struct json_object *temp_max, *temp_min, *weather_codes, *precip_sum, *precip_prob, *times;

      json_object_object_get_ex(daily, "temperature_2m_max", &temp_max);
      json_object_object_get_ex(daily, "temperature_2m_min", &temp_min);
      json_object_object_get_ex(daily, "weather_code", &weather_codes);
      json_object_object_get_ex(daily, "precipitation_sum", &precip_sum);
      json_object_object_get_ex(daily, "precipitation_probability_max", &precip_prob);
      json_object_object_get_ex(daily, "time", &times);

      size_t num_days = temp_max ? json_object_array_length(temp_max) : 0;
      if (num_days > MAX_FORECAST_DAYS) {
         num_days = MAX_FORECAST_DAYS;
      }
      response->num_days = (int)num_days;

      for (size_t i = 0; i < num_days; i++) {
         if (times) {
            const char *date_str = json_object_get_string(json_object_array_get_idx(times, i));
            if (date_str) {
               response->daily[i].date = strdup(date_str);
            }
         }
         if (temp_max) {
            response->daily[i].high_f = celsius_to_fahrenheit(
                json_object_get_double(json_object_array_get_idx(temp_max, i)));
         }
         if (temp_min) {
            response->daily[i].low_f = celsius_to_fahrenheit(
                json_object_get_double(json_object_array_get_idx(temp_min, i)));
         }
         if (weather_codes) {
            response->daily[i].weather_code = json_object_get_int(
                json_object_array_get_idx(weather_codes, i));
            // No strdup needed - weather_code_to_string returns static string
            response->daily[i].condition = weather_code_to_string(response->daily[i].weather_code);
         }
         if (precip_sum) {
            response->daily[i].precipitation_in = mm_to_inches(
                json_object_get_double(json_object_array_get_idx(precip_sum, i)));
         }
         if (precip_prob) {
            response->daily[i].precipitation_chance = json_object_get_double(
                json_object_array_get_idx(precip_prob, i));
         }
      }
   }

   json_object_put(root);
   return response;
}

weather_response_t *weather_get(const char *location, forecast_type_t forecast) {
   double latitude, longitude;
   char *resolved_name = NULL;

   if (geocode_location(location, &latitude, &longitude, &resolved_name) != 0) {
      weather_response_t *response = calloc(1, sizeof(weather_response_t));
      if (response) {
         /* Compose an LLM-actionable message: name the input that failed and
          * suggest disambiguation strategies the model can actually try. */
         char msg[256];
         snprintf(msg, sizeof(msg),
                  "Could not geocode '%s'. Try: add country/state ('%s, USA' or '%s, GA'), "
                  "drop punctuation, or use a major city near the target. If the user said "
                  "an ambiguous name, ask them to disambiguate.",
                  location ? location : "(empty)", location ? location : "city",
                  location ? location : "city");
         response->error = strdup(msg);
      }
      return response;
   }

   weather_response_t *response = fetch_weather(latitude, longitude, resolved_name, forecast);
   free(resolved_name);
   return response;
}

weather_response_t *weather_get_by_coords(double latitude,
                                          double longitude,
                                          const char *location_name,
                                          forecast_type_t forecast) {
   return fetch_weather(latitude, longitude, location_name, forecast);
}

int weather_format_for_llm(const weather_response_t *response,
                           char *buffer,
                           size_t buffer_size,
                           int *bytes_written) {
   if (bytes_written)
      *bytes_written = 0;
   if (!response || !buffer || buffer_size == 0) {
      return FAILURE;
   }

   if (response->error) {
      int n = snprintf(buffer, buffer_size, "{\"error\": \"%s\"}", response->error);
      if (bytes_written)
         *bytes_written = n;
      return SUCCESS;
   }

   int written = snprintf(buffer, buffer_size,
                          "{"
                          "\"location\": \"%s\", "
                          "\"current\": {"
                          "\"temperature_f\": %.1f, "
                          "\"feels_like_f\": %.1f, "
                          "\"humidity\": %.0f, "
                          "\"condition\": \"%s\", "
                          "\"wind_mph\": %.1f, "
                          "\"wind_direction\": %d"
                          "}, "
                          "\"forecast\": [",
                          response->location_name ? response->location_name : "Unknown",
                          response->current.temperature_f, response->current.feels_like_f,
                          response->current.humidity,
                          response->current.condition ? response->current.condition : "Unknown",
                          response->current.wind_speed_mph, response->current.wind_direction);

   if (written < 0 || (size_t)written >= buffer_size) {
      if (bytes_written)
         *bytes_written = written;
      return SUCCESS;
   }

   size_t remaining = buffer_size - (size_t)written;
   for (int i = 0; i < response->num_days && remaining > 1; i++) {
      const char *day_label = get_relative_day_label(i, response->daily[i].date);
      int day_written = snprintf(buffer + written, remaining,
                                 "%s{"
                                 "\"day\": \"%s\", "
                                 "\"date\": \"%s\", "
                                 "\"high_f\": %.1f, "
                                 "\"low_f\": %.1f, "
                                 "\"condition\": \"%s\", "
                                 "\"precipitation_chance\": %.0f"
                                 "}",
                                 (i > 0) ? ", " : "", day_label,
                                 response->daily[i].date ? response->daily[i].date : "Unknown",
                                 response->daily[i].high_f, response->daily[i].low_f,
                                 response->daily[i].condition ? response->daily[i].condition
                                                              : "Unknown",
                                 response->daily[i].precipitation_chance);

      if (day_written < 0) {
         if (bytes_written)
            *bytes_written = written;
         return SUCCESS;
      }
      if ((size_t)day_written >= remaining) {
         written = (int)(buffer_size - 1);
         remaining = 1;
         break;
      }
      written += day_written;
      remaining -= (size_t)day_written;
   }

   if (remaining > 2) {
      int close_written = snprintf(buffer + written, remaining, "]}");
      if (close_written > 0 && (size_t)close_written < remaining) {
         written += close_written;
      }
   }

   if (bytes_written)
      *bytes_written = written;
   return SUCCESS;
}

void weather_free_response(weather_response_t *response) {
   if (!response) {
      return;
   }

   free(response->location_name);
   // Note: current.condition is a static string, do not free

   // Free daily forecast data
   for (int i = 0; i < response->num_days; i++) {
      free(response->daily[i].date);
      // Note: daily[i].condition is a static string, do not free
   }

   free(response->error);
   free(response);
}
