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
 * Opening a library track for playback, from any source: the library path
 * check and the Plex download, shared by the WebUI stream and the local
 * speaker.
 */

#include "audio/music_open.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "audio/music_source.h"
#include "audio/plex_client.h"
#include "config/dawn_config.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"

/* Plex part keys name a track under one of these */
#define PLEX_PART_PREFIX "/library/parts/"
#define PLEX_METADATA_PREFIX "/library/metadata/"

/* True if s holds a percent-encoded '.', '/' or '\\' (%2e, %2f, %5c, any case) */
static bool has_encoded_path_char(const char *s) {
   for (const char *p = strchr(s, '%'); p; p = strchr(p + 1, '%')) {
      char hi = p[1];
      char lo = (char)tolower((unsigned char)(hi ? p[2] : '\0'));
      if ((hi == '2' && (lo == 'e' || lo == 'f')) || (hi == '5' && lo == 'c')) {
         return true;
      }
   }
   return false;
}

/* A Plex part key is sent to the server with the admin's token, so it must
 * stay a plain path under /library: nothing that changes the host, adds a
 * query, or walks out of it once the server decodes it */
static bool plex_part_key_is_valid(const char *path, const char *part_key) {
   if (!plex_client_is_configured()) {
      OLOG_WARNING("Music: Plex path rejected (Plex not configured): %s", path);
      return false;
   }
   if (strncmp(part_key, PLEX_PART_PREFIX, strlen(PLEX_PART_PREFIX)) != 0 &&
       strncmp(part_key, PLEX_METADATA_PREFIX, strlen(PLEX_METADATA_PREFIX)) != 0) {
      OLOG_WARNING("Music: Invalid Plex part key: %s", part_key);
      return false;
   }
   if (strchr(part_key, '@') != NULL) {
      OLOG_WARNING("Music: Authority injection in Plex path: %s", path);
      return false;
   }
   if (strchr(part_key, '?') != NULL || strchr(part_key, '#') != NULL) {
      OLOG_WARNING("Music: Query/fragment injection in Plex path: %s", path);
      return false;
   }
   /* The server decodes "%2e%2e" into ".." */
   if (has_encoded_path_char(part_key) || path_has_traversal(part_key)) {
      OLOG_WARNING("Music: Traversal in Plex path: %s", path);
      return false;
   }
   return true;
}

bool music_path_is_valid(const char *path) {
   if (!path || path[0] == '\0') {
      return false;
   }

   if (music_source_from_path(path) == MUSIC_SOURCE_PLEX) {
      return plex_part_key_is_valid(path,
                                    path + strlen(music_source_path_prefix(MUSIC_SOURCE_PLEX)));
   }

   /* Library paths are absolute (the scanner stores them so); anything else
    * is a track name, not a path, and never touches the filesystem */
   if (path[0] != '/') {
      return false;
   }
   if (path_has_traversal(path)) {
      OLOG_WARNING("Music: Path traversal detected in: %s", path);
      return false;
   }
   if (g_config.paths.music_dir_resolved[0] == '\0') {
      OLOG_WARNING("Music: No music directory configured");
      return false;
   }
   /* Resolves symlinks: the file itself must sit inside the library */
   if (!path_is_within_root(path, g_config.paths.music_dir_resolved)) {
      OLOG_WARNING("Music: Not a file in the music library: %s", path);
      return false;
   }
   return true;
}

audio_decoder_t *music_open_track(const char *path, const atomic_bool *cancel, bool *unreachable) {
   if (unreachable) {
      *unreachable = false;
   }
   if (!music_path_is_valid(path)) {
      OLOG_WARNING("Music: Refusing to open %s (not in the music library)", path);
      return NULL;
   }

   if (music_source_from_path(path) != MUSIC_SOURCE_PLEX) {
      return audio_decoder_open(path);
   }

   char local_path[PATH_MAX];
   const char *part_key = path + strlen(music_source_path_prefix(MUSIC_SOURCE_PLEX));
   int rc = plex_client_download_track(part_key, cancel, local_path, sizeof(local_path));
   if (rc != SUCCESS) {
      OLOG_ERROR("Music: Plex download failed for: %s", path);
      if (unreachable && rc == PLEX_DOWNLOAD_UNREACHABLE) {
         *unreachable = true;
      }
      return NULL;
   }
   audio_decoder_t *decoder = audio_decoder_open(local_path);
   unlink(local_path);
   return decoder;
}
