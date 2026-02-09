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
 * TTS Playback Queue - Decouples TTS synthesis from audio playback
 *
 * Producer (WS thread): synthesizes TTS audio, pushes buffers into queue.
 * Consumer (playback thread): dequeues buffers and plays them sequentially.
 * This allows sentence N+1 to be synthesized while sentence N is playing.
 */

#ifndef TTS_PLAYBACK_QUEUE_H
#define TTS_PLAYBACK_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_playback.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tts_playback_queue tts_playback_queue_t;

/**
 * @brief Create a playback queue and start its consumer thread
 *
 * @param playback Audio playback handle (shared, not owned)
 * @param stop_flag Atomic flag checked by audio_playback_play to abort
 * @return Allocated queue, or NULL on failure
 */
tts_playback_queue_t *tts_playback_queue_create(audio_playback_t *playback, atomic_int *stop_flag);

/**
 * @brief Destroy queue, join playback thread, free resources
 *
 * Flushes any remaining entries before joining.
 */
void tts_playback_queue_destroy(tts_playback_queue_t *q);

/**
 * @brief Push synthesized audio into the queue
 *
 * Takes ownership of the audio pointer (will be freed by the consumer).
 * Blocks if the queue is full (back-pressure on producer).
 *
 * @param q Queue
 * @param audio PCM samples (16-bit mono, heap-allocated)
 * @param len Number of samples
 * @param sample_rate Sample rate of the audio
 * @return 0 on success, 1 if queue is shutting down
 */
int tts_playback_queue_push(tts_playback_queue_t *q, int16_t *audio, size_t len, int sample_rate);

/**
 * @brief Signal that no more entries will be added
 *
 * The playback thread will drain remaining entries and stop.
 * Call this after sentence_buffer_flush() on stream_end.
 */
void tts_playback_queue_finish(tts_playback_queue_t *q);

/**
 * @brief Discard all queued audio (for cancel/barge-in)
 *
 * Frees queued buffers and wakes the playback thread so it can exit.
 * The caller should also set stop_flag to interrupt current playback.
 */
void tts_playback_queue_flush(tts_playback_queue_t *q);

/**
 * @brief Check if the queue has work (playing or entries queued)
 *
 * @return true if audio is playing or entries are waiting
 */
bool tts_playback_queue_is_active(tts_playback_queue_t *q);

/**
 * @brief Reset queue for a new interaction
 *
 * Waits for the previous playback thread to finish, then starts a fresh one.
 * Call this before the first push of a new response.
 */
void tts_playback_queue_reset(tts_playback_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* TTS_PLAYBACK_QUEUE_H */
