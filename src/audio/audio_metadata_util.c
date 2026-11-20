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
#include <strings.h>

#include "audio/audio_decoder.h"
#include "utils/string_utils.h"

/* Digits read from a track or disc number: real ones are small */
#define AUDIO_METADATA_NUMBER_DIGITS 4

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

uint32_t audio_metadata_parse_number(const char *s) {
   if (!s) {
      return 0;
   }
   while (*s == ' ') {
      s++;
   }
   uint32_t n = 0;
   int digits = 0;
   for (; *s >= '0' && *s <= '9' && digits < AUDIO_METADATA_NUMBER_DIGITS; s++, digits++) {
      n = n * 10 + (uint32_t)(*s - '0');
   }
   return n;
}

void audio_metadata_take_vorbis_comment(audio_metadata_t *metadata, const char *entry, size_t len) {
   if (!metadata || !entry || len == 0) {
      return;
   }
   const char *eq = memchr(entry, '=', len);
   if (!eq || eq == entry) {
      return;
   }
   size_t key_len = (size_t)(eq - entry);
   size_t value_len = len - key_len - 1;
   if (value_len == 0) {
      return;
   }
   char value[AUDIO_METADATA_STRING_MAX];
   size_t copy = value_len < sizeof(value) - 1 ? value_len : sizeof(value) - 1;
   memcpy(value, eq + 1, copy);
   value[copy] = '\0';

#define VORBIS_KEY(name) (key_len == sizeof(name) - 1 && strncasecmp(entry, name, key_len) == 0)
   if (VORBIS_KEY("TITLE")) {
      if (!metadata->title[0]) { /* a repeated tag: the first wins */
         safe_strncpy(metadata->title, value, AUDIO_METADATA_STRING_MAX);
      }
   } else if (VORBIS_KEY("ARTIST")) {
      if (!metadata->artist[0]) { /* a repeated tag: the first wins */
         safe_strncpy(metadata->artist, value, AUDIO_METADATA_STRING_MAX);
      }
   } else if (VORBIS_KEY("ALBUM")) {
      if (!metadata->album[0]) { /* a repeated tag: the first wins */
         safe_strncpy(metadata->album, value, AUDIO_METADATA_STRING_MAX);
      }
   } else if (VORBIS_KEY("ALBUMARTIST") || VORBIS_KEY("ALBUM ARTIST") ||
              VORBIS_KEY("ALBUM_ARTIST")) {
      if (!metadata->album_artist[0]) { /* a repeated tag: the first wins */
         safe_strncpy(metadata->album_artist, value, AUDIO_METADATA_STRING_MAX);
      }
   } else if (VORBIS_KEY("GENRE")) {
      audio_metadata_append_genre(metadata, value);
   } else if (VORBIS_KEY("DATE")) {
      uint32_t year = audio_metadata_parse_year(value);
      if (year) {
         metadata->year = year; /* a full date wins over YEAR */
      }
   } else if (VORBIS_KEY("YEAR")) {
      if (metadata->year == 0) {
         metadata->year = audio_metadata_parse_year(value);
      }
   } else if (VORBIS_KEY("TRACKNUMBER")) {
      metadata->track_no = audio_metadata_parse_number(value);
   } else if (VORBIS_KEY("DISCNUMBER")) {
      metadata->disc_no = audio_metadata_parse_number(value);
   }
#undef VORBIS_KEY
}
