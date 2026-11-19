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
 * Audio Capture Thread - Runtime Backend Selection
 *
 * Uses the audio_backend abstraction for runtime selection between
 * ALSA and PulseAudio backends.
 */

#ifndef AUDIO_CAPTURE_THREAD_H
#define AUDIO_CAPTURE_THREAD_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// Note: <stdatomic.h> is intentionally NOT included here to avoid conflicts
// with C++ code. The atomic_bool type is defined as volatile int for the struct.
#ifdef __cplusplus
// C++ uses std::atomic in implementation
typedef volatile int atomic_bool_t;
#else
#include <stdatomic.h>
typedef atomic_bool atomic_bool_t;
#endif

#include "audio/audio_backend.h"
#include "audio/ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio capture thread context
 *
 * Manages a dedicated thread for continuous audio capture that runs
 * independently of the main application loop. Audio is written to a
 * ring buffer for consumption by the main thread.
 *
 * Uses the audio_backend abstraction for runtime selection between
 * ALSA and PulseAudio backends.
 */
typedef struct {
   pthread_t thread;           /**< Capture thread handle */
   ring_buffer_t *ring_buffer; /**< Ring buffer for audio data */
   atomic_bool_t running;      /**< Thread running flag */
   int use_realtime_priority;  /**< Enable realtime scheduling */

   /* Audio backend handle (runtime-selected ALSA or PulseAudio) */
   audio_stream_capture_handle_t *capture_handle; /**< Backend capture handle; NULL while
                                                       the device is missing */
   audio_stream_params_t params;                  /**< Requested parameters (to reopen) */
   audio_hw_params_t hw_params;                   /**< Actual hardware parameters */

   char *pcm_device;   /**< Device name */
   char *read_buffer;  /**< One period as read from the device */
   size_t buffer_size; /**< Size of capture buffer in bytes */
   size_t frames;      /**< Frames per read (period size) */

   // Resampler for 48kHz → 16kHz (always needed for ASR)
   void *downsample_resampler; /**< Resampler for 48kHz → 16kHz (opaque, resampler_t*) */
   int16_t *asr_buffer;        /**< Downsampled buffer for ASR (16kHz) */
   size_t asr_buffer_size;     /**< ASR buffer size in samples */

#ifdef ENABLE_AEC
   int16_t *aec_buffer;    /**< Pre-allocated AEC output buffer (48kHz) */
   size_t aec_buffer_size; /**< AEC buffer size in samples */
   int aec_rate_mismatch;  /**< True if device rate != AEC_SAMPLE_RATE */
#endif
} audio_capture_context_t;

/**
 * @brief Create and start audio capture thread
 *
 * Opens the audio device, creates ring buffer, spawns capture thread,
 * and optionally sets realtime priority for low-latency operation.  A device
 * that isn't there yet doesn't fail it: the thread writes silence and opens the
 * device when it appears (and again whenever it goes away and comes back).
 *
 * @param pcm_device Audio device name (e.g., "plughw:CARD=S3,DEV=0" for ALSA,
 *                   or PulseAudio source name for PulseAudio backend)
 * @param ring_buffer_size Size of ring buffer in bytes (recommend 65536 = 2 seconds at 16kHz)
 * @param use_realtime_priority If non-zero, set SCHED_FIFO realtime priority
 * @return Pointer to capture context, or NULL if it couldn't be allocated or started
 */
audio_capture_context_t *audio_capture_start(const char *pcm_device,
                                             size_t ring_buffer_size,
                                             int use_realtime_priority);

/**
 * @brief Stop audio capture thread and clean up resources
 *
 * Signals thread to stop, waits for it to exit, closes audio device,
 * and frees all allocated resources.
 *
 * @param ctx Capture context to stop and free
 */
void audio_capture_stop(audio_capture_context_t *ctx);

/**
 * @brief Read audio data from capture thread's ring buffer
 *
 * Non-blocking read from the ring buffer filled by the capture thread.
 * Returns immediately with whatever data is available.
 *
 * @param ctx Capture context
 * @param data Destination buffer
 * @param len Maximum bytes to read
 * @return Number of bytes actually read (0 if buffer empty)
 */
size_t audio_capture_read(audio_capture_context_t *ctx, char *data, size_t len);

/**
 * @brief Wait for audio data to become available
 *
 * Blocks until at least min_bytes are available in the ring buffer
 * or timeout occurs. Useful for synchronizing with audio capture.
 *
 * @param ctx Capture context
 * @param min_bytes Minimum bytes to wait for
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return Number of bytes available
 */
size_t audio_capture_wait_for_data(audio_capture_context_t *ctx, size_t min_bytes, int timeout_ms);

/**
 * @brief Get number of bytes available in ring buffer
 *
 * @param ctx Capture context
 * @return Number of bytes ready to read
 */
size_t audio_capture_bytes_available(audio_capture_context_t *ctx);

/**
 * @brief Check if capture thread is still running
 *
 * @param ctx Capture context
 * @return Non-zero if thread is running, 0 if stopped
 */
int audio_capture_is_running(audio_capture_context_t *ctx);

/**
 * @brief Clear all data from ring buffer
 *
 * Useful for discarding old audio data before capturing fresh samples.
 *
 * @param ctx Capture context
 */
void audio_capture_clear(audio_capture_context_t *ctx);

// ============================================================================
// Mic Recording API for Debugging (works with or without AEC)
// ============================================================================

/**
 * @brief Set directory for mic recording output files
 *
 * @param dir Directory path (default: /tmp)
 */
void mic_set_recording_dir(const char *dir);

/**
 * @brief Enable or disable mic recording capability
 *
 * Must be called with true before mic_start_recording() will work.
 *
 * @param enable true to enable, false to disable
 */
void mic_enable_recording(bool enable);

/**
 * @brief Check if mic recording is currently active
 *
 * @return true if actively recording, false otherwise
 */
bool mic_is_recording(void);

/**
 * @brief Check if mic recording capability is enabled
 *
 * @return true if recording is enabled, false otherwise
 */
bool mic_is_recording_enabled(void);

/**
 * @brief Start recording mic input to WAV file
 *
 * Creates a WAV file with timestamped name:
 * - mic_capture_YYYYMMDD_HHMMSS.wav - What VAD sees (16kHz mono)
 *
 * Recording must be enabled first with mic_enable_recording(true).
 *
 * @return 0 on success, non-zero on error
 */
int mic_start_recording(void);

/**
 * @brief Stop recording and finalize WAV file
 *
 * Closes recording file and updates WAV header with final size.
 * Safe to call even if not recording.
 */
void mic_stop_recording(void);

/**
 * @brief Record samples to mic recording file (internal use)
 *
 * Called by capture thread to record samples going to ring buffer.
 *
 * @param samples Audio samples (16-bit)
 * @param num_samples Number of samples
 */
void mic_record_samples(const int16_t *samples, size_t num_samples);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_CAPTURE_THREAD_H
