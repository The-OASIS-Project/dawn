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
 * Weather geocoding without the network: a location parsed into the geocoder
 * query, and the result chosen from a canned geocoder answer.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "dawn_error.h"
#include "tools/weather_geocode.h"
#include "unity.h"

/* Rows copied from live Open-Meteo /v1/search answers, in its order (fields trimmed).
 * "Saint Paul" with countryCode=US, then the Alberta row of the "St. Paul" answer. */
#define US_ROW(n, a, p)                                                      \
   "{\"name\":\"" n "\",\"admin1\":\"" a "\",\"country\":\"United States\"," \
   "\"country_code\":\"US\",\"population\":" #p "}"
static const char *SAINT_PAUL_RESULTS = "[" US_ROW("Saint Paul", "Minnesota", 303176) "," US_ROW(
    "Saint Paul",
    "Nebraska",
    2290) "," US_ROW("Saint Paul",
                     "Indiana",
                     1031) "," US_ROW("Saint Paul",
                                      "Virginia",
                                      970) "," US_ROW("Saint Paul",
                                                      "Kansas",
                                                      629) "," US_ROW("Saint Paul",
                                                                      "Alaska",
                                                                      479) ",{\"name\":\"St. "
                                                                           "Paul\",\"admin1\":"
                                                                           "\"Alberta\","
                                                                           "\"country\":\"Canada\","
                                                                           "\"country_code\":"
                                                                           "\"CA\",\"population\":"
                                                                           "5728}]";

/* "St. Paul" with no country: the spelled-out Alaska row comes first. */
static const char *ST_PAUL_TYPED = "[" US_ROW(
    "Saint Paul",
    "Alaska",
    479) ",{\"name\":\"St. Paul\",\"admin1\":\"Alberta\",\"country\":\"Canada\","
         "\"country_code\":\"CA\",\"population\":5728}]";
static const char *SAINT_PAUL_ANY = "[" US_ROW(
    "Saint Paul",
    "Minnesota",
    303176) ",{\"name\":\"Saint-Paul\",\"admin1\":\"Reunion\",\"country_code\":\"RE\","
            "\"population\":105240}," US_ROW("Saint Paul", "Nebraska", 2290) "]";

static struct json_object *s_results;

void setUp(void) {
   s_results = json_tokener_parse(SAINT_PAUL_RESULTS);
}

void tearDown(void) {
   json_object_put(s_results);
   s_results = NULL;
}

/* Field of the result picked across canned answers (each a JSON results array). */
static char s_picked[128];
static const char *picked_from(const char *field,
                               const weather_geo_query_t *q,
                               const char *a,
                               const char *b) {
   struct json_object *sets[2] = { json_tokener_parse(a), b ? json_tokener_parse(b) : NULL };
   struct json_object *r = weather_geocode_pick(sets, b ? 2 : 1, q);
   struct json_object *v;
   const char *out = NULL;
   if (r && json_object_object_get_ex(r, field, &v)) {
      snprintf(s_picked, sizeof(s_picked), "%s", json_object_get_string(v));
      out = s_picked;
   }
   json_object_put(sets[0]);
   json_object_put(sets[1]);
   return out;
}

static const char *picked_admin1(const weather_geo_query_t *q) {
   struct json_object *r = weather_geocode_pick(&s_results, 1, q);
   struct json_object *v;
   if (!r || !json_object_object_get_ex(r, "admin1", &v)) {
      return NULL;
   }
   return json_object_get_string(v);
}

static void test_saint_abbrev_and_state_code(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, MN", &q));
   TEST_ASSERT_EQUAL_STRING("Saint Paul", q.city);
   TEST_ASSERT_EQUAL_STRING("St. Paul", q.city_given);
   TEST_ASSERT_EQUAL_STRING("Minnesota", q.region);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);
   TEST_ASSERT_FALSE(q.bare_state);
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_admin1(&q));
}

static void test_fort_and_mount(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Ft. Worth, TX", &q));
   TEST_ASSERT_EQUAL_STRING("Fort Worth", q.city);
   TEST_ASSERT_EQUAL_STRING("Texas", q.region);

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Mt. Vernon, NY", &q));
   TEST_ASSERT_EQUAL_STRING("Mount Vernon", q.city);
   TEST_ASSERT_EQUAL_STRING("New York", q.region);

   /* Only a whole leading word: Stone Mountain stays as given. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Stone Mountain, GA", &q));
   TEST_ASSERT_EQUAL_STRING("Stone Mountain", q.city);
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("st paul", &q));
   TEST_ASSERT_EQUAL_STRING("Saint paul", q.city);
}

static void test_city_state_country(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, Minnesota, United States", &q));
   TEST_ASSERT_EQUAL_STRING("Saint Paul", q.city);
   TEST_ASSERT_EQUAL_STRING("Minnesota", q.region);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);
   TEST_ASSERT_EQUAL_STRING("", q.country);
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_admin1(&q));

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Atlanta, GA, USA", &q));
   TEST_ASSERT_EQUAL_STRING("Atlanta", q.city);
   TEST_ASSERT_EQUAL_STRING("Georgia", q.region);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Atlanta, USA", &q));
   TEST_ASSERT_EQUAL_STRING("", q.region);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);
}

static void test_bare_state(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Montana", &q));
   TEST_ASSERT_TRUE(q.bare_state);
   TEST_ASSERT_EQUAL_STRING("Helena", q.city);
   TEST_ASSERT_EQUAL_STRING("Montana", q.region);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("NJ", &q));
   TEST_ASSERT_TRUE(q.bare_state);
   TEST_ASSERT_EQUAL_STRING("Trenton", q.city);
   TEST_ASSERT_EQUAL_STRING("New Jersey", q.region);

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("new york", &q));
   TEST_ASSERT_EQUAL_STRING("New York", q.city);
   TEST_ASSERT_EQUAL_STRING("New York", q.region);

   /* A city is not a state. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Paris", &q));
   TEST_ASSERT_FALSE(q.bare_state);
   TEST_ASSERT_EQUAL_STRING("", q.country_code);
}

static void test_no_match_never_falls_back(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, Oregon", &q));
   TEST_ASSERT_NULL(weather_geocode_pick(&s_results, 1, &q));

   /* The first row in the state, though four others come before it. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, VA", &q));
   TEST_ASSERT_EQUAL_STRING("Virginia", picked_admin1(&q));

   /* A non-US country excludes every US row. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, Alberta, Canada", &q));
   TEST_ASSERT_EQUAL_STRING("Alberta", picked_admin1(&q));
   /* A non-state region that matches nothing is dropped; the country filter stays. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, Alberta, US", &q));
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_admin1(&q));

   TEST_ASSERT_NULL(weather_geocode_pick(NULL, 0, &q));
}

/* The geocoder stores some places abbreviated: "Saint Louis" finds Senegal first, "St.
 * Louis" finds the city.  Each answer's first row is a candidate; the larger one wins. */
static const char *SAINT_LOUIS_SPELLED =
    "[{\"name\":\"Saint-Louis\",\"admin1\":\"Saint-Louis\",\"country\":\"Senegal\","
    "\"country_code\":\"SN\",\"population\":254171},"
    "{\"name\":\"Saint-Louis\",\"admin1\":\"Grand Est\",\"country\":\"France\","
    "\"country_code\":\"FR\",\"population\":20871}]";
static const char *SAINT_LOUIS_TYPED = "[" US_ROW(
    "St Louis",
    "Missouri",
    279695) ",{\"name\":\"St. Louis\",\"admin1\":\"Saskatchewan\",\"country\":\"Canada\","
            "\"country_code\":\"CA\"}]";

static void test_abbreviated_storage_both_spellings(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Louis", &q));
   TEST_ASSERT_EQUAL_STRING("Saint Louis", q.city);
   TEST_ASSERT_EQUAL_STRING("St Louis",
                            picked_from("name", &q, SAINT_LOUIS_SPELLED, SAINT_LOUIS_TYPED));

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Louis, MO", &q));
   TEST_ASSERT_EQUAL_STRING("St Louis",
                            picked_from("name", &q, SAINT_LOUIS_SPELLED, SAINT_LOUIS_TYPED));

   /* "St. Paul": Minnesota from the spelled-out answer beats Alaska from the typed one. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul", &q));
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_from("admin1", &q, SAINT_PAUL_ANY, ST_PAUL_TYPED));
   /* The same row in both answers, a missing answer, an empty one. */
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_from("admin1", &q, SAINT_PAUL_ANY, SAINT_PAUL_ANY));
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_from("admin1", &q, SAINT_PAUL_ANY, "[]"));
   struct json_object *sets[2] = { NULL, json_tokener_parse(ST_PAUL_TYPED) };
   struct json_object *r = weather_geocode_pick(sets, 2, &q);
   struct json_object *v;
   TEST_ASSERT_TRUE(r && json_object_object_get_ex(r, "admin1", &v));
   TEST_ASSERT_EQUAL_STRING("Alaska", json_object_get_string(v));
   json_object_put(sets[1]);
}

/* The geocoder's order is kept within an answer: it folds accents and ranks by prominence,
 * so neither Vancouver Island (more populous) nor an unaccented namesake wins. */
static const char *VANCOUVER =
    "[{\"name\":\"Vancouver\",\"admin1\":\"British Columbia\",\"country\":\"Canada\","
    "\"country_code\":\"CA\",\"population\":662248}," US_ROW(
        "Vancouver",
        "Washington",
        196442) ",{\"name\":\"Vancouver Island\",\"admin1\":\"British "
                "Columbia\",\"country\":\"Canada\","
                "\"country_code\":\"CA\",\"population\":748937}]";
static const char *CANCUN =
    "[{\"name\":\"Canc\xc3\xban\",\"admin1\":\"Quintana Roo\",\"country\":\"Mexico\","
    "\"country_code\":\"MX\",\"population\":888797},"
    "{\"name\":\"Cancun\",\"admin1\":\"Guangxi\",\"country\":\"China\",\"country_code\":\"CN\"}]";

static void test_geocoder_order_within_answer(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Vancouver", &q));
   TEST_ASSERT_EQUAL_STRING("British Columbia", picked_from("admin1", &q, VANCOUVER, NULL));
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Vancouver, WA", &q));
   TEST_ASSERT_EQUAL_STRING("Washington", picked_from("admin1", &q, VANCOUVER, NULL));

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Cancun", &q));
   TEST_ASSERT_EQUAL_STRING("Quintana Roo", picked_from("admin1", &q, CANCUN, NULL));
}

static void test_non_state_region_falls_back(void) {
   static const char *LONDON =
       "[{\"name\":\"London\",\"admin1\":\"England\",\"country\":\"United Kingdom\","
       "\"country_code\":\"GB\",\"population\":8961989},"
       "{\"name\":\"London\",\"admin1\":\"Ontario\",\"country\":\"Canada\","
       "\"country_code\":\"CA\",\"population\":422324}," US_ROW("London", "Ohio", 10060) "]";
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("London, UK", &q));
   TEST_ASSERT_FALSE(q.region_is_us_state);
   TEST_ASSERT_EQUAL_STRING("England", picked_from("admin1", &q, LONDON, NULL));

   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("London, OH", &q));
   TEST_ASSERT_EQUAL_STRING("Ohio", picked_from("admin1", &q, LONDON, NULL));
   /* A US state stays strict: no London in Maine means no answer. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("London, Maine", &q));
   TEST_ASSERT_NULL(picked_from("admin1", &q, LONDON, NULL));
}

static void test_zip_after_state(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Sugar Hill, GA 30518", &q));
   TEST_ASSERT_EQUAL_STRING("Sugar Hill", q.city);
   TEST_ASSERT_EQUAL_STRING("Georgia", q.region);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Sugar Hill, GA 30518-1234", &q));
   TEST_ASSERT_EQUAL_STRING("Georgia", q.region);
   /* As its own part, a ZIP is not a country. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Sugar Hill, GA, 30518", &q));
   TEST_ASSERT_EQUAL_STRING("Georgia", q.region);
   TEST_ASSERT_EQUAL_STRING("", q.country);
   TEST_ASSERT_EQUAL_STRING("US", q.country_code);
}

static void test_abbrev_inside_name(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Port St. Lucie, FL", &q));
   TEST_ASSERT_EQUAL_STRING("Port Saint Lucie", q.city);
   TEST_ASSERT_EQUAL_STRING("Port St. Lucie", q.city_given);
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("East St Louis, IL", &q));
   TEST_ASSERT_EQUAL_STRING("East Saint Louis", q.city);
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St.Paul", &q));
   TEST_ASSERT_EQUAL_STRING("Saint Paul", q.city);
   /* A trailing "St." has nothing to abbreviate. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Main St.", &q));
   TEST_ASSERT_EQUAL_STRING("Main St.", q.city);
}

static void test_country_code_only_letters(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Paris, x, &a", &q));
   TEST_ASSERT_EQUAL_STRING("", q.country_code);
   TEST_ASSERT_EQUAL_STRING("&a", q.country);
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Paris, Ile-de-France, fr", &q));
   TEST_ASSERT_EQUAL_STRING("FR", q.country_code);
   /* "UK" is how people write it; the geocoder's rows say GB. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("London, England, UK", &q));
   TEST_ASSERT_EQUAL_STRING("GB", q.country_code);
}

static void test_bare_washington_is_dc(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Washington", &q));
   TEST_ASSERT_TRUE(q.bare_state);
   TEST_ASSERT_EQUAL_STRING("Washington", q.city);
   TEST_ASSERT_EQUAL_STRING("District of Columbia", q.region);
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("WA", &q));
   TEST_ASSERT_EQUAL_STRING("Olympia", q.city);
}

static void test_unfiltered_and_country_region(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("Saint Paul", &q));
   TEST_ASSERT_EQUAL_STRING("Minnesota", picked_admin1(&q));

   /* A region that names a country ("Tokyo, Japan") matches the country field. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, weather_geocode_parse("St. Paul, Canada", &q));
   TEST_ASSERT_EQUAL_STRING("Alberta", picked_admin1(&q));
}

static void test_bad_input(void) {
   weather_geo_query_t q;
   TEST_ASSERT_EQUAL_INT(FAILURE, weather_geocode_parse(NULL, &q));
   TEST_ASSERT_EQUAL_INT(FAILURE, weather_geocode_parse("  , GA", &q));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_saint_abbrev_and_state_code);
   RUN_TEST(test_abbreviated_storage_both_spellings);
   RUN_TEST(test_geocoder_order_within_answer);
   RUN_TEST(test_non_state_region_falls_back);
   RUN_TEST(test_zip_after_state);
   RUN_TEST(test_abbrev_inside_name);
   RUN_TEST(test_country_code_only_letters);
   RUN_TEST(test_bare_washington_is_dc);
   RUN_TEST(test_fort_and_mount);
   RUN_TEST(test_city_state_country);
   RUN_TEST(test_bare_state);
   RUN_TEST(test_no_match_never_falls_back);
   RUN_TEST(test_unfiltered_and_country_region);
   RUN_TEST(test_bad_input);
   return UNITY_END();
}
