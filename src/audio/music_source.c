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
 * Music Source Abstraction - name/prefix lookups and path identification
 */

#include "audio/music_source.h"

#include <string.h>

/* =============================================================================
 * Source Name and Prefix Tables
 * ============================================================================= */

static const char *source_names[MUSIC_SOURCE_COUNT] = {
   [MUSIC_SOURCE_LOCAL] = "local",
   [MUSIC_SOURCE_PLEX] = "plex",
};

static const char *source_prefixes[MUSIC_SOURCE_COUNT] = {
   [MUSIC_SOURCE_LOCAL] = "",     /* No prefix - bare filesystem path */
   [MUSIC_SOURCE_PLEX] = "plex:", /* plex:/library/parts/... */
};

/* Compile-time check: ensure lookup tables are updated when new sources are added */
_Static_assert(sizeof(source_names) / sizeof(source_names[0]) == MUSIC_SOURCE_COUNT,
               "source_names[] must have an entry for each music_source_t value");
_Static_assert(sizeof(source_prefixes) / sizeof(source_prefixes[0]) == MUSIC_SOURCE_COUNT,
               "source_prefixes[] must have an entry for each music_source_t value");

/* =============================================================================
 * Public API
 * ============================================================================= */

const char *music_source_name(music_source_t source) {
   if (source >= 0 && source < MUSIC_SOURCE_COUNT)
      return source_names[source];
   return "unknown";
}

const char *music_source_path_prefix(music_source_t source) {
   if (source >= 0 && source < MUSIC_SOURCE_COUNT)
      return source_prefixes[source];
   return "";
}

music_source_t music_source_from_path(const char *path) {
   if (!path)
      return MUSIC_SOURCE_LOCAL;

   /* Check remote sources (highest enum value first) for prefix match */
   for (int i = MUSIC_SOURCE_COUNT - 1; i > 0; i--) {
      const char *prefix = source_prefixes[i];
      if (prefix[0] && strncmp(path, prefix, strlen(prefix)) == 0)
         return (music_source_t)i;
   }
   return MUSIC_SOURCE_LOCAL;
}

bool music_source_is_remote(music_source_t source) {
   return source != MUSIC_SOURCE_LOCAL;
}
