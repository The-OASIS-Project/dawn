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
 * DAWN Audio Backend Abstraction
 *
 * Provides a unified interface for audio capture and playback that can use
 * either ALSA (embedded/low-latency) or PulseAudio (desktop) as the backend.
 *
 * Note: Uses "audio_stream_*" prefix to avoid conflict with the higher-level
 * "audio_capture_*" API in audio_capture_thread.h which manages ring buffers.
 *
 * Usage:
 *   1. Call audio_backend_init() with desired backend type
 *   2. Use audio_stream_capture_*() for microphone input
 *   3. Use audio_stream_playback_*() for speaker output
 *   4. Call audio_backend_cleanup() on shutdown
 *
 * Thread Safety:
 *   - audio_backend_init() is thread-safe (uses internal mutex)
 *   - audio_backend_cleanup() is thread-safe but MUST NOT be called while
 *     handles are still open (close all handles first, then cleanup)
 *   - Handle operations are NOT thread-safe; use one handle per thread
 *   - Multiple capture/playback handles can be opened concurrently
 *
 * Backend Behavioral Differences:
 *   ALSA:
 *     - True hardware access with accurate buffer level reporting
 *     - avail() returns exact frames available in hardware buffer
 *     - read()/write() support partial transfers (may return fewer frames)
 *     - close() respects prior drop() call (won't drain if dropped)
 *
 *   PulseAudio (blocking stream, pulse_stream.c):
 *     - avail() returns TIME-BASED ESTIMATE, not actual buffer level
 *     - read()/write() ALWAYS complete fully (blocks until done)
 *     - Suitable for DAWN's voice pipeline where blocking is acceptable
 *
 *   Both backends:
 *     - Normalize error codes to AUDIO_ERR_* values
 *     - Use static handle pools (no malloc per stream)
 */

#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* For ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Error Codes (positive values per DAWN coding standards)
 * ============================================================================= */

/**
 * @brief Audio backend error codes
 *
 * All error returns use positive values. Functions returning ssize_t use
 * negative values only to indicate errors (the actual error code is the
 * absolute value). Functions returning int use 0 for success and positive
 * values for specific errors.
 */
typedef enum {
   AUDIO_SUCCESS = 0,       /**< Operation successful */
   AUDIO_ERR_INVALID = 1,   /**< Invalid parameter or NULL handle */
   AUDIO_ERR_NOT_INIT = 2,  /**< Backend not initialized */
   AUDIO_ERR_NO_DEVICE = 3, /**< Device not found or cannot open */
   AUDIO_ERR_OVERRUN = 4,   /**< Capture buffer overrun (data lost) */
   AUDIO_ERR_UNDERRUN = 5,  /**< Playback buffer underrun (gap in audio) */
   AUDIO_ERR_SUSPENDED = 6, /**< Device suspended (power management) */
   AUDIO_ERR_IO = 7,        /**< I/O error during read/write */
   AUDIO_ERR_BUSY = 8,      /**< Device busy or no handles available */
   AUDIO_ERR_TIMEOUT = 9,   /**< Operation timed out */
   AUDIO_ERR_UNKNOWN = 10   /**< Unknown or unrecoverable error */
} audio_error_t;

/* =============================================================================
 * Backend Types
 * ============================================================================= */

/**
 * @brief Audio backend type
 */
typedef enum {
   AUDIO_BACKEND_AUTO = 0, /**< Auto-detect best available backend */
   AUDIO_BACKEND_ALSA,     /**< ALSA (Advanced Linux Sound Architecture) */
   AUDIO_BACKEND_PULSE,    /**< PulseAudio (async API) */
   AUDIO_BACKEND_NONE      /**< No backend (testing/headless) */
} audio_backend_type_t;

/**
 * @brief Audio sample format
 */
typedef enum {
   AUDIO_FORMAT_S16_LE = 0, /**< Signed 16-bit little-endian (default) */
   AUDIO_FORMAT_S24_3LE,    /**< Signed 24-bit 3-byte little-endian */
   AUDIO_FORMAT_S32_LE,     /**< Signed 32-bit little-endian */
   AUDIO_FORMAT_FLOAT32     /**< 32-bit float */
} audio_sample_format_t;

/**
 * @brief Audio stream parameters
 */
typedef struct {
   unsigned int sample_rate;     /**< Sample rate in Hz (e.g., 16000, 48000) */
   unsigned int channels;        /**< Number of channels (1=mono, 2=stereo) */
   audio_sample_format_t format; /**< Sample format */
   size_t period_frames;         /**< Frames per period (latency control) */
   size_t buffer_frames;         /**< Total buffer size in frames */
} audio_stream_params_t;

/**
 * @brief Actual hardware parameters (may differ from requested)
 */
typedef struct {
   unsigned int sample_rate;     /**< Actual sample rate */
   unsigned int channels;        /**< Actual channel count */
   audio_sample_format_t format; /**< Actual format */
   size_t period_frames;         /**< Actual period size */
   size_t buffer_frames;         /**< Actual buffer size */
} audio_hw_params_t;

/* =============================================================================
 * Opaque Handle Types
 * ============================================================================= */

/**
 * @brief Opaque handle for audio capture stream
 *
 * Each handle contains a backend identifier for runtime validation.
 * Handles should not be shared between threads without external synchronization.
 */
typedef struct audio_stream_capture_handle audio_stream_capture_handle_t;

/**
 * @brief Opaque handle for audio playback stream
 *
 * Each handle contains a backend identifier for runtime validation.
 * Handles should not be shared between threads without external synchronization.
 */
typedef struct audio_stream_playback_handle audio_stream_playback_handle_t;

/* =============================================================================
 * Backend Initialization
 * ============================================================================= */

/**
 * @brief Initialize the audio backend subsystem
 *
 * Must be called before any capture or playback operations.
 * Thread-safe: uses internal mutex to prevent race conditions.
 * Idempotent: multiple calls return success if already initialized.
 *
 * @param type Backend type (AUDIO_BACKEND_AUTO for auto-detection)
 * @return AUDIO_SUCCESS on success, or AUDIO_ERR_* on error
 */
int audio_backend_init(audio_backend_type_t type);

/**
 * @brief Clean up the audio backend subsystem
 *
 * Releases global resources. Does NOT close open handles.
 * Thread-safe and idempotent.
 */
void audio_backend_cleanup(void);

/**
 * @brief Get the currently active backend type
 *
 * @return Current backend type, or AUDIO_BACKEND_NONE if not initialized
 */
audio_backend_type_t audio_backend_get_type(void);

/**
 * @brief Get backend type name as string
 *
 * @param type Backend type
 * @return Human-readable name (e.g., "alsa", "pulse")
 */
const char *audio_backend_type_name(audio_backend_type_t type);

/**
 * @brief Parse backend type from string
 *
 * @param name Backend name ("auto", "alsa", "pulse")
 * @return Backend type, or AUDIO_BACKEND_AUTO on unknown
 */
audio_backend_type_t audio_backend_parse_type(const char *name);

/**
 * @brief Check if a specific backend is available
 *
 * Tests whether the backend libraries are loaded and functional.
 *
 * @param type Backend type to check
 * @return true if available, false otherwise
 */
bool audio_backend_is_available(audio_backend_type_t type);

/**
 * @brief Get error message for error code
 *
 * @param err Error code from audio operation
 * @return Human-readable error string
 */
const char *audio_error_string(audio_error_t err);

/* =============================================================================
 * Audio Stream Capture API
 * ============================================================================= */

/**
 * @brief Open an audio capture stream
 *
 * Opens the specified device for audio capture with the given parameters.
 * Uses static handle allocation (no malloc) for embedded efficiency.
 *
 * @param device Device name (e.g., "default", "plughw:CARD=S3,DEV=0")
 * @param params Desired stream parameters
 * @param hw_params Output: Actual hardware parameters (may differ from requested)
 * @return Capture handle on success, NULL on error
 */
audio_stream_capture_handle_t *audio_stream_capture_open(const char *device,
                                                         const audio_stream_params_t *params,
                                                         audio_hw_params_t *hw_params);

/**
 * @brief Read audio samples from capture stream
 *
 * Blocking read that waits for samples to be available.
 *
 * @note Backend differences:
 *   - ALSA: May return fewer frames than requested (partial read)
 *   - PulseAudio: ALWAYS returns exactly the requested frames (blocks until complete)
 *
 * For portable code, be prepared for both behaviors.
 *
 * @param handle Capture handle
 * @param buffer Output buffer for samples
 * @param frames Number of frames to read
 * @return Number of frames actually read, or negative error code (-AUDIO_ERR_*)
 */
ssize_t audio_stream_capture_read(audio_stream_capture_handle_t *handle,
                                  void *buffer,
                                  size_t frames);

/**
 * @brief Get number of frames available for reading
 *
 * Non-blocking check for available data.
 *
 * @note Backend differences:
 *   - ALSA: Returns exact frames available in hardware buffer
 *   - PulseAudio: Returns TIME-BASED ESTIMATE (frames since last read)
 *
 * For portable code, use this only for flow control hints, not precise timing.
 *
 * @param handle Capture handle
 * @return Number of frames available (or estimate), or negative error code (-AUDIO_ERR_*)
 */
ssize_t audio_stream_capture_avail(audio_stream_capture_handle_t *handle);

/**
 * @brief Recover from underrun/overrun conditions
 *
 * Should be called after read errors to reset the stream.
 *
 * @param handle Capture handle
 * @param err Error code from previous operation (positive AUDIO_ERR_*)
 * @return AUDIO_SUCCESS on successful recovery, or AUDIO_ERR_* on failure
 */
int audio_stream_capture_recover(audio_stream_capture_handle_t *handle, int err);

/**
 * @brief Close capture stream and release resources
 *
 * Returns handle to static pool for reuse. Safe to call with NULL.
 *
 * @param handle Capture handle (can be NULL)
 */
void audio_stream_capture_close(audio_stream_capture_handle_t *handle);

/* =============================================================================
 * Audio Stream Playback API
 * ============================================================================= */

/**
 * @brief Open an audio playback stream
 *
 * Opens the specified device for audio playback with the given parameters.
 * Uses static handle allocation (no malloc) for embedded efficiency.
 *
 * @param device Device name (e.g., "default", "plughw:CARD=S3,DEV=0")
 * @param params Desired stream parameters
 * @param hw_params Output: Actual hardware parameters (may differ from requested)
 * @return Playback handle on success, NULL on error
 */
audio_stream_playback_handle_t *audio_stream_playback_open(const char *device,
                                                           const audio_stream_params_t *params,
                                                           audio_hw_params_t *hw_params);

/**
 * @brief Write audio samples to playback stream
 *
 * Blocking write that waits for buffer space.
 *
 * @note Backend differences:
 *   - ALSA: May write fewer frames than requested (partial write)
 *   - PulseAudio: ALWAYS writes exactly the requested frames (blocks until complete)
 *
 * For portable code, be prepared for both behaviors.
 *
 * @param handle Playback handle
 * @param buffer Input buffer of samples
 * @param frames Number of frames to write
 * @return Number of frames actually written, or negative error code (-AUDIO_ERR_*)
 */
ssize_t audio_stream_playback_write(audio_stream_playback_handle_t *handle,
                                    const void *buffer,
                                    size_t frames);

/**
 * @brief Get number of frames available for writing
 *
 * Non-blocking check for buffer space.
 *
 * @note Backend differences:
 *   - ALSA: Returns exact frames of free buffer space
 *   - PulseAudio: Returns buffer_frames (constant, not actual free space)
 *
 * For portable code, use this only for flow control hints, not precise buffer management.
 *
 * @param handle Playback handle
 * @return Number of frames available for writing (or estimate), or negative error code
 * (-AUDIO_ERR_*)
 */
ssize_t audio_stream_playback_avail(audio_stream_playback_handle_t *handle);

/**
 * @brief Drain all pending samples to hardware
 *
 * Blocks until all queued samples have been played.
 *
 * @param handle Playback handle
 * @return AUDIO_SUCCESS on success, or AUDIO_ERR_* on error
 */
int audio_stream_playback_drain(audio_stream_playback_handle_t *handle);

/**
 * @brief Drop all pending samples (stop immediately)
 *
 * Discards any queued samples and stops playback.
 *
 * @param handle Playback handle
 * @return AUDIO_SUCCESS on success, or AUDIO_ERR_* on error
 */
int audio_stream_playback_drop(audio_stream_playback_handle_t *handle);

/**
 * @brief Recover from underrun/overrun conditions
 *
 * Should be called after write errors to reset the stream.
 *
 * @param handle Playback handle
 * @param err Error code from previous operation (positive AUDIO_ERR_*)
 * @return AUDIO_SUCCESS on successful recovery, or AUDIO_ERR_* on failure
 */
int audio_stream_playback_recover(audio_stream_playback_handle_t *handle, int err);

/**
 * @brief Close playback stream and release resources
 *
 * By default, drains remaining audio before closing. For immediate
 * close without drain, call audio_stream_playback_drop() first.
 * Returns handle to static pool for reuse. Safe to call with NULL.
 *
 * @param handle Playback handle (can be NULL)
 */
void audio_stream_playback_close(audio_stream_playback_handle_t *handle);

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

/**
 * @brief Get bytes per frame for given format and channel count
 *
 * @param format Sample format
 * @param channels Number of channels
 * @return Bytes per frame
 */
size_t audio_bytes_per_frame(audio_sample_format_t format, unsigned int channels);

/**
 * @brief Get default capture parameters
 *
 * Returns sensible defaults for voice capture (48kHz mono S16).
 *
 * @param params Output parameter struct
 */
void audio_stream_capture_default_params(audio_stream_params_t *params);

/**
 * @brief Get default playback parameters
 *
 * Returns sensible defaults for voice playback (22050Hz mono S16).
 *
 * @param params Output parameter struct
 */
void audio_stream_playback_default_params(audio_stream_params_t *params);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_BACKEND_H */
