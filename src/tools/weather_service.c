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
#include "tools/weather_geocode.h"

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

/* Ask the geocoder for a name.  On SUCCESS *root_out holds the parsed response (caller
 * frees) and *results_out its "results" array, or NULL when it found nothing. */
static int geocode_query(const char *name,
                         const char *country_code,
                         int count,
                         struct json_object **root_out,
                         struct json_object **results_out) {
   *root_out = NULL;
   *results_out = NULL;

   CURL *curl = curl_easy_init();
   if (!curl) {
      OLOG_ERROR("Failed to initialize curl for geocoding");
      return FAILURE;
   }

   char *encoded = curl_easy_escape(curl, name, 0);
   if (!encoded) {
      curl_easy_cleanup(curl);
      return FAILURE;
   }

   char url[512];
   snprintf(url, sizeof(url), "%s?name=%s&count=%d&language=en&format=json%s%s",
            OPEN_METEO_GEOCODING_URL, encoded, count, country_code[0] ? "&countryCode=" : "",
            country_code);
   curl_free(encoded);

   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, CURL_BUFFER_MAX_WEB_SEARCH);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

   CURLcode res = curl_easy_perform(curl);
   curl_easy_cleanup(curl);
   if (res != CURLE_OK) {
      OLOG_ERROR("Geocoding request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buffer);
      return FAILURE;
   }

   *root_out = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);
   if (!*root_out) {
      OLOG_ERROR("Failed to parse geocoding response");
      return FAILURE;
   }
   json_object_object_get_ex(*root_out, "results", results_out);
   return SUCCESS;
}

/* Geocode a location string to lat/lon.  On failure, why (if not empty) says what wasn't
 * found, for the model. */
static int geocode_location(const char *location,
                            double *latitude,
                            double *longitude,
                            char **resolved_name,
                            char *why,
                            size_t why_len) {
   weather_geo_query_t q;
   why[0] = '\0';
   if (weather_geocode_parse(location, &q) != SUCCESS) {
      return FAILURE;
   }

   OLOG_INFO("Geocoding: city='%s', region='%s', country='%s%s'", q.city,
             q.region[0] ? q.region : "(none)", q.country_code, q.country);

   int count = (q.region[0] || q.country[0]) ? WEATHER_GEO_COUNT_FILTERED : WEATHER_GEO_COUNT;
   /* Both spellings when they differ: "Saint Paul" is stored spelled out, "St. Louis" not.
    * The name as given goes first, so it wins when neither answer has a population. */
   const char *names[2] = { q.city_given, q.city };
   size_t nsets = strcmp(names[0], names[1]) == 0 ? 1 : 2;
   struct json_object *roots[2] = { NULL, NULL };
   struct json_object *results[2] = { NULL, NULL };
   for (size_t i = 0; i < nsets; i++) {
      if (geocode_query(names[i], q.country_code, count, &roots[i], &results[i]) != SUCCESS) {
         json_object_put(roots[0]);
         json_object_put(roots[1]);
         return FAILURE;
      }
   }
   struct json_object *best = weather_geocode_pick(results, nsets, &q);

   if (!best) {
      json_object_put(roots[0]);
      json_object_put(roots[1]);
      const char *where = q.region[0] ? q.region : q.country[0] ? q.country : NULL;
      if (where) {
         snprintf(why, why_len, "Couldn't find %s in %s.", q.city_given, where);
      }
      OLOG_WARNING("Geocoding: no result for '%s'%s%s", location, where ? " in " : "",
                   where ? where : "");
      return FAILURE;
   }

   struct json_object *lat_obj = NULL, *lon_obj = NULL, *name_obj = NULL, *admin1_obj = NULL,
                      *country_obj = NULL;
   json_object_object_get_ex(best, "latitude", &lat_obj);
   json_object_object_get_ex(best, "longitude", &lon_obj);
   json_object_object_get_ex(best, "name", &name_obj);
   json_object_object_get_ex(best, "admin1", &admin1_obj);
   json_object_object_get_ex(best, "country", &country_obj);

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

   json_object_put(roots[0]);
   json_object_put(roots[1]);
   return *resolved_name ? SUCCESS : FAILURE;
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

   /* Holds "Couldn't find <city> in <region>." whole, whatever the two fields are. */
   char why[2 * WEATHER_GEO_FIELD_MAX + 32];
   if (geocode_location(location, &latitude, &longitude, &resolved_name, why, sizeof(why)) !=
       SUCCESS) {
      weather_response_t *response = calloc(1, sizeof(weather_response_t));
      if (response) {
         /* Compose an LLM-actionable message: name the input that failed and
          * suggest disambiguation strategies the model can actually try. */
         char msg[512];
         if (why[0]) {
            /* Never another state's place: say this one wasn't found there. */
            snprintf(msg, sizeof(msg),
                     "%s Check the spelling, or ask the user which place they mean.", why);
            response->error = strdup(msg);
            return response;
         }
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
