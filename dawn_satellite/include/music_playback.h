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
 * Music Playback Engine - Opus decode + PCM ring buffer + ALSA output
 */

#ifndef MUSIC_PLAYBACK_H
#define MUSIC_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_playback.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   MUSIC_PB_IDLE = 0,
   MUSIC_PB_BUFFERING,
   MUSIC_PB_PLAYING,
   MUSIC_PB_PAUSED,
} music_pb_state_t;

typedef struct music_playback music_playback_t;

/**
 * Create music playback context.
 * Allocates OpusDecoder (48kHz stereo), ring buffer, and starts consumer thread.
 *
 * @param audio Shared ALSA playback context (not owned, must outlive this object)
 * @return New context, or NULL on failure
 */
music_playback_t *music_playback_create(audio_playback_t *audio);

/**
 * Destroy music playback context. Stops consumer thread, frees all resources.
 */
void music_playback_destroy(music_playback_t *ctx);

/**
 * Push an Opus frame for decoding and playback.
 * Decodes to PCM and pushes into ring buffer. Thread-safe.
 * Blocks if ring buffer is full (backpressure to network).
 *
 * @param ctx Playback context
 * @param opus_data Raw Opus frame bytes
 * @param opus_len Length of Opus frame
 * @return Number of PCM frames decoded, or -1 on decode error
 */
int music_playback_push_opus(music_playback_t *ctx, const uint8_t *opus_data, int opus_len);

/**
 * Stop playback and flush ring buffer. Transitions to IDLE.
 */
void music_playback_stop(music_playback_t *ctx);

/**
 * Flush ring buffer without stopping (for seek/skip).
 * Consumer thread continues running but ring is cleared.
 */
void music_playback_flush(music_playback_t *ctx);

/**
 * Pause playback (consumer thread stops draining to ALSA).
 * Ring buffer continues accumulating from network.
 */
void music_playback_pause(music_playback_t *ctx);

/**
 * Resume playback after pause.
 */
void music_playback_resume(music_playback_t *ctx);

/**
 * Set volume (0-100). Default is 80.
 */
void music_playback_set_volume(music_playback_t *ctx, int volume);

/**
 * Get current volume (0-100).
 */
int music_playback_get_volume(music_playback_t *ctx);

/**
 * Get current playback state.
 */
music_pb_state_t music_playback_get_state(music_playback_t *ctx);

/**
 * Check if currently playing audio (state == PLAYING).
 */
bool music_playback_is_playing(music_playback_t *ctx);

/**
 * Get total buffered audio in milliseconds (ring buffer + ALSA output buffer).
 * Used to compensate server-reported position for accurate display.
 */
int music_playback_get_buffered_ms(music_playback_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYBACK_H */
