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
 * the project author(s).
 *
 * Weather geocoding without the network: a spoken location parsed into what
 * Open-Meteo's geocoder is asked, and the result chosen from its answer.
 */

#ifndef WEATHER_GEOCODE_H
#define WEATHER_GEOCODE_H

#include <stdbool.h>
#include <stddef.h>

struct json_object;

#ifdef __cplusplus
extern "C" {
#endif

/** Size of each name field of a parsed query. */
#define WEATHER_GEO_FIELD_MAX 128

/** What a location string asks for: "city[, state or region][, country]". */
typedef struct {
   char city[WEATHER_GEO_FIELD_MAX];       /**< Name to query, St./Ft./Mt. spelled out */
   char city_given[WEATHER_GEO_FIELD_MAX]; /**< The name as given (trimmed) */
   char region[WEATHER_GEO_FIELD_MAX];     /**< State or region; a US state code spelled out */
   bool region_is_us_state;                /**< region is a US state: admin1 must equal it */
   char country[64];                       /**< Country filter as given; empty when none */
   char country_code[3];                   /**< ISO alpha-2 for countryCode; empty if none */
   bool bare_state;                        /**< Whole input was a US state (city stands in) */
} weather_geo_query_t;

/**
 * @brief Parse a location string into a geocoder query.
 *
 * A US state code in the region ("MN") becomes its name and sets country_code "US", as
 * does a US country part ("USA", "United States", "US"); a ZIP after the state is dropped.
 * A whole input that is a US state ("Montana", "NJ") queries that state's representative
 * city (its capital; New York's is New York City, Washington's is Washington, D.C.).
 * St./Ft./Mt. starting any word of the city are spelled out in city; city_given keeps
 * the name as typed, since some places are stored abbreviated ("St. Louis").
 *
 * @param location Location as the model passed it
 * @param out      Parsed query (zeroed first)
 * @return SUCCESS, or FAILURE when location is NULL or has no city part
 */
int weather_geocode_parse(const char *location, weather_geo_query_t *out);

/**
 * @brief Choose the geocoder result for a query from one or more answers.
 *
 * Keeps results whose admin1 or country names the region, and whose country matches the
 * country part.  Each answer keeps the geocoder's order (accents folded, then prominence):
 * its first passing row is a candidate, and the more populous candidate wins (the first
 * on a tie).  When nothing matches a region that is not a US state, the region is dropped
 * and the rest applies; a US state never yields another state's result.
 *
 * @param sets  "results" arrays of the geocoder answers (entries may be NULL)
 * @param nsets Number of arrays
 * @param q     Parsed query
 * @return Borrowed pointer into one of the arrays, or NULL when nothing passes
 */
struct json_object *weather_geocode_pick(struct json_object *const *sets,
                                         size_t nsets,
                                         const weather_geo_query_t *q);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_GEOCODE_H */
