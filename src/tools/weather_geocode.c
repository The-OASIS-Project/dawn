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

#include "tools/weather_geocode.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "dawn_error.h"
#include "utils/string_utils.h"

/* US states: code, name, and the city a bare state name is looked up as.  The geocoder
 * has no state-level entries (a search for "Montana" returns towns named Montana in
 * other states and countries), so a bare state resolves to its capital, except New York,
 * whose bare name means New York City, and Washington, whose bare name is taken as
 * Washington, D.C. (as before this table existed; "Seattle, Washington" still works). */
static const struct {
   const char *code;
   const char *name;
   const char *city;
} us_states[] = {
   { "AL", "Alabama", "Montgomery" },
   { "AK", "Alaska", "Juneau" },
   { "AZ", "Arizona", "Phoenix" },
   { "AR", "Arkansas", "Little Rock" },
   { "CA", "California", "Sacramento" },
   { "CO", "Colorado", "Denver" },
   { "CT", "Connecticut", "Hartford" },
   { "DE", "Delaware", "Dover" },
   { "FL", "Florida", "Tallahassee" },
   { "GA", "Georgia", "Atlanta" },
   { "HI", "Hawaii", "Honolulu" },
   { "ID", "Idaho", "Boise" },
   { "IL", "Illinois", "Springfield" },
   { "IN", "Indiana", "Indianapolis" },
   { "IA", "Iowa", "Des Moines" },
   { "KS", "Kansas", "Topeka" },
   { "KY", "Kentucky", "Frankfort" },
   { "LA", "Louisiana", "Baton Rouge" },
   { "ME", "Maine", "Augusta" },
   { "MD", "Maryland", "Annapolis" },
   { "MA", "Massachusetts", "Boston" },
   { "MI", "Michigan", "Lansing" },
   { "MN", "Minnesota", "Saint Paul" },
   { "MS", "Mississippi", "Jackson" },
   { "MO", "Missouri", "Jefferson City" },
   { "MT", "Montana", "Helena" },
   { "NE", "Nebraska", "Lincoln" },
   { "NV", "Nevada", "Carson City" },
   { "NH", "New Hampshire", "Concord" },
   { "NJ", "New Jersey", "Trenton" },
   { "NM", "New Mexico", "Santa Fe" },
   { "NY", "New York", "New York" },
   { "NC", "North Carolina", "Raleigh" },
   { "ND", "North Dakota", "Bismarck" },
   { "OH", "Ohio", "Columbus" },
   { "OK", "Oklahoma", "Oklahoma City" },
   { "OR", "Oregon", "Salem" },
   { "PA", "Pennsylvania", "Harrisburg" },
   { "RI", "Rhode Island", "Providence" },
   { "SC", "South Carolina", "Columbia" },
   { "SD", "South Dakota", "Pierre" },
   { "TN", "Tennessee", "Nashville" },
   { "TX", "Texas", "Austin" },
   { "UT", "Utah", "Salt Lake City" },
   { "VT", "Vermont", "Montpelier" },
   { "VA", "Virginia", "Richmond" },
   { "WA", "Washington", "Olympia" },
   { "WV", "West Virginia", "Charleston" },
   { "WI", "Wisconsin", "Madison" },
   { "WY", "Wyoming", "Cheyenne" },
   { "DC", "District of Columbia", "Washington" },
};
#define US_STATE_COUNT (sizeof(us_states) / sizeof(us_states[0]))

/* Abbreviations the geocoder doesn't expand ("St. Paul" finds Saint Paul, Alaska and never
 * Minnesota's; "Port St. Lucie" finds nothing).  Some places are stored abbreviated
 * ("St. Louis"), so both spellings are asked for. */
static const struct {
   const char *abbrev;
   const char *word;
} name_abbrevs[] = {
   { "St", "Saint" },
   { "Ft", "Fort" },
   { "Mt", "Mount" },
};

static const char *const us_country_names[] = {
   "US", "USA", "U.S.", "U.S.A.", "United States", "United States of America", "America",
};

/* Index into us_states of a state code or name, or -1. */
static int find_us_state(const char *s, bool allow_code) {
   for (size_t i = 0; i < US_STATE_COUNT; i++) {
      if ((allow_code && strcasecmp(s, us_states[i].code) == 0) ||
          strcasecmp(s, us_states[i].name) == 0) {
         return (int)i;
      }
   }
   return -1;
}

static bool is_us_country(const char *s) {
   for (size_t i = 0; i < sizeof(us_country_names) / sizeof(us_country_names[0]); i++) {
      if (strcasecmp(s, us_country_names[i]) == 0) {
         return true;
      }
   }
   return false;
}

static bool is_ascii_letter(char c) {
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool contains_ci(const char *haystack, const char *needle) {
   size_t hay_len = strlen(haystack);
   size_t needle_len = strlen(needle);

   for (size_t i = 0; needle_len <= hay_len && i <= hay_len - needle_len; i++) {
      if (strncasecmp(haystack + i, needle, needle_len) == 0) {
         return true;
      }
   }
   return false;
}

/* Trim spaces in place; returns the first non-space character. */
static char *trim(char *s) {
   while (*s == ' ' || *s == '\t') {
      s++;
   }
   size_t len = strlen(s);
   while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
      s[--len] = '\0';
   }
   return s;
}

static void append(char *out, size_t out_len, size_t *used, const char *s, size_t n) {
   if (*used + n >= out_len) {
      n = out_len - 1 - *used;
   }
   memcpy(out + *used, s, n);
   *used += n;
   out[*used] = '\0';
}

/* Spell out St./Ft./Mt. at the start of any word ("Port St. Lucie" -> "Port Saint Lucie",
 * "St.Paul" -> "Saint Paul").  A bare "St" or "St." must have a word after it. */
static void expand_name(const char *given, char *out, size_t out_len) {
   size_t used = 0;
   out[0] = '\0';
   for (const char *p = given; *p;) {
      size_t wlen = strcspn(p, " ");
      size_t keep_from = 0;
      for (size_t i = 0; i < sizeof(name_abbrevs) / sizeof(name_abbrevs[0]); i++) {
         size_t n = strlen(name_abbrevs[i].abbrev);
         if (wlen < n || strncasecmp(p, name_abbrevs[i].abbrev, n) != 0) {
            continue;
         }
         bool whole = wlen == n || (wlen == n + 1 && p[n] == '.');
         if (whole && p[wlen] == ' ') {
            keep_from = wlen;
         } else if (wlen > n + 1 && p[n] == '.') {
            keep_from = n + 1;
         } else {
            continue;
         }
         const char *word = name_abbrevs[i].word;
         append(out, out_len, &used, word, strlen(word));
         if (keep_from < wlen) {
            append(out, out_len, &used, " ", 1);
         }
         break;
      }
      size_t gap = strspn(p + wlen, " ");
      append(out, out_len, &used, p + keep_from, wlen - keep_from + gap);
      p += wlen + gap;
   }
}

int weather_geocode_parse(const char *location, weather_geo_query_t *out) {
   if (!location || !out) {
      return FAILURE;
   }
   memset(out, 0, sizeof(*out));

   char buf[WEATHER_GEO_FIELD_MAX * 3];
   safe_strscpy(buf, location);

   /* city[, region][, country] */
   char *parts[3] = { NULL, NULL, NULL };
   int nparts = 0;
   char *cursor = buf;
   while (nparts < 3 && cursor) {
      char *comma = (nparts < 2) ? strchr(cursor, ',') : NULL;
      if (comma) {
         *comma = '\0';
      }
      parts[nparts++] = trim(cursor);
      cursor = comma ? comma + 1 : NULL;
   }
   if (!parts[0][0]) {
      return FAILURE;
   }

   char *region = (nparts >= 2) ? parts[1] : NULL;
   const char *country = (nparts == 3) ? parts[2] : NULL;
   if (country && strspn(country, "0123456789-") == strlen(country)) {
      country = NULL; /* "Sugar Hill, GA, 30518": a ZIP, not a country */
   }
   if (nparts == 2 && is_us_country(parts[1])) {
      region = NULL;
      country = parts[1];
   }
   if (region) {
      /* A ZIP or postcode after the state ("GA 30518", "GA 30518-1234"). */
      size_t len = strlen(region);
      while (len > 0 && (isdigit((unsigned char)region[len - 1]) || region[len - 1] == '-' ||
                         region[len - 1] == ' ')) {
         region[--len] = '\0';
      }
   }

   if (country && country[0]) {
      if (is_us_country(country)) {
         safe_strscpy(out->country_code, "US");
      } else if (strlen(country) == 2 && is_ascii_letter(country[0]) &&
                 is_ascii_letter(country[1])) {
         /* Only two ASCII letters go into the URL; anything else is matched locally. */
         out->country_code[0] = (char)toupper((unsigned char)country[0]);
         out->country_code[1] = (char)toupper((unsigned char)country[1]);
         if (strcmp(out->country_code, "UK") == 0) {
            /* The geocoder knows the United Kingdom only by its ISO code. */
            safe_strscpy(out->country_code, "GB");
         }
      } else {
         safe_strscpy(out->country, country);
      }
   }
   bool us_or_none = !out->country[0] &&
                     (!out->country_code[0] || strcmp(out->country_code, "US") == 0);

   if (region && region[0]) {
      int st = us_or_none ? find_us_state(region, true) : -1;
      if (st >= 0) {
         safe_strscpy(out->region, us_states[st].name);
         out->region_is_us_state = true;
         if (strlen(region) == 2) {
            safe_strscpy(out->country_code, "US");
         }
      } else {
         safe_strscpy(out->region, region);
      }
   } else if (us_or_none) {
      int st = find_us_state(parts[0], true);
      if (st >= 0 && strcasecmp(parts[0], "Washington") == 0) {
         st = find_us_state("DC", true);
      }
      if (st >= 0) {
         out->bare_state = true;
         out->region_is_us_state = true;
         safe_strscpy(out->region, us_states[st].name);
         safe_strscpy(out->country_code, "US");
         safe_strscpy(out->city, us_states[st].city);
         safe_strscpy(out->city_given, us_states[st].city);
         return SUCCESS;
      }
   }

   safe_strscpy(out->city_given, parts[0]);
   expand_name(parts[0], out->city, sizeof(out->city));
   return SUCCESS;
}

static const char *field_str(struct json_object *obj, const char *key) {
   struct json_object *v;
   return json_object_object_get_ex(obj, key, &v) ? json_object_get_string(v) : NULL;
}

static bool result_matches(struct json_object *r, const weather_geo_query_t *q, bool use_region) {
   const char *admin1 = field_str(r, "admin1");
   const char *country = field_str(r, "country");
   const char *code = field_str(r, "country_code");

   if (use_region && q->region[0]) {
      bool admin1_ok = admin1 && (q->region_is_us_state ? strcasecmp(admin1, q->region) == 0
                                                        : contains_ci(admin1, q->region));
      if (!admin1_ok && !(country && contains_ci(country, q->region))) {
         return false;
      }
   }
   if (q->country_code[0]) {
      return code && strcasecmp(code, q->country_code) == 0;
   }
   if (q->country[0]) {
      return (country && contains_ci(country, q->country)) ||
             (code && strcasecmp(code, q->country) == 0);
   }
   return true;
}

static int64_t population(struct json_object *r) {
   struct json_object *v;
   return json_object_object_get_ex(r, "population", &v) ? json_object_get_int64(v) : 0;
}

/* Each answer in the geocoder's own order (name relevance, accents folded, then
 * prominence): its first row that passes the filters.  Between answers, the more
 * populous candidate wins ("St. Paul" -> Minnesota, not Alaska's spelled-out row). */
static struct json_object *pick_pass(struct json_object *const *sets,
                                     size_t nsets,
                                     const weather_geo_query_t *q,
                                     bool use_region) {
   struct json_object *best = NULL;
   for (size_t s = 0; s < nsets; s++) {
      if (!sets[s] || !json_object_is_type(sets[s], json_type_array)) {
         continue;
      }
      size_t n = json_object_array_length(sets[s]);
      for (size_t i = 0; i < n; i++) {
         struct json_object *r = json_object_array_get_idx(sets[s], i);
         if (r && result_matches(r, q, use_region)) {
            if (!best || population(r) > population(best)) {
               best = r;
            }
            break;
         }
      }
   }
   return best;
}

struct json_object *weather_geocode_pick(struct json_object *const *sets,
                                         size_t nsets,
                                         const weather_geo_query_t *q) {
   if (!sets || !q) {
      return NULL;
   }
   struct json_object *best = pick_pass(sets, nsets, q, true);
   /* A US state is strict: another state's town is a wrong answer, not a near one.  Any
    * other region ("UK", "BC", "NSW") may be written in a way admin1 never matches, so the
    * name decides, inside the country filter. */
   if (!best && q->region[0] && !q->region_is_us_state) {
      best = pick_pass(sets, nsets, q, false);
   }
   return best;
}
