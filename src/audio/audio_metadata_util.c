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
 * Pure, dependency-free helpers for audio tag metadata (year parsing, genre
 * accumulation). Kept in their own translation unit so they can be unit-tested
 * without linking the codec-backed decoders.
 */

#include <string.h>

#include "audio/audio_decoder.h"
#include "utils/string_utils.h"

void audio_metadata_append_genre(audio_metadata_t *metadata, const char *genre) {
   if (!metadata || !genre || !genre[0]) {
      return;
   }
   size_t cur = strlen(metadata->genre);
   /* First value: plain copy. Subsequent values: ", "-join, bounded. */
   if (cur == 0) {
      safe_strncpy(metadata->genre, genre, AUDIO_GENRE_STRING_MAX);
      return;
   }
   if (cur + 2 >= AUDIO_GENRE_STRING_MAX) {
      return; /* No room for a separator + at least one char */
   }
   metadata->genre[cur++] = ',';
   metadata->genre[cur++] = ' ';
   safe_strncpy(metadata->genre + cur, genre, AUDIO_GENRE_STRING_MAX - cur);
}

uint32_t audio_metadata_parse_year(const char *s) {
   if (!s) {
      return 0;
   }
   /* Tag date fields vary: "1985", "1985-06-01", "1985/06", "06/1985". Take the
    * first standalone run of exactly four digits within a plausible range. */
   for (const char *p = s; *p; p++) {
      if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' &&
          p[3] >= '0' && p[3] <= '9' && !(p[4] >= '0' && p[4] <= '9') &&
          (p == s || !(p[-1] >= '0' && p[-1] <= '9'))) {
         uint32_t year = (uint32_t)((p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 +
                                    (p[3] - '0'));
         if (year >= 1000 && year <= 2999) {
            return year;
         }
      }
   }
   return 0;
}
