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

#ifndef MUSIC_OPEN_H
#define MUSIC_OPEN_H

#include <stdatomic.h>
#include <stdbool.h>

#include "audio/audio_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a path names a track in the music library
 *
 * A local path must resolve inside [paths] music_dir; a plex: path must be a
 * well-formed Plex part key (no authority, query, fragment, encoded or literal
 * traversal) with Plex configured.  Logs why a path is refused.
 */
bool music_path_is_valid(const char *path);

/**
 * @brief Open a library track for decoding
 *
 * Checks the path with music_path_is_valid().  A plex: path is downloaded to a
 * temp file first; the file is unlinked once the decoder has it open (the
 * decoder keeps its descriptor), so nothing is left on disk.  Blocks for the
 * whole download: call it on a playback thread, with no lock held.
 *
 * @param path        Library path (local file or plex:/library/...)
 * @param cancel      Aborts a Plex download once set (NULL = not cancellable)
 * @param unreachable Optional output: set when the Plex server couldn't be
 *                    reached, so the next Plex track would fail the same way
 * @return Decoder, or NULL if the path, the download or the open failed
 */
audio_decoder_t *music_open_track(const char *path, const atomic_bool *cancel, bool *unreachable);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_OPEN_H */
