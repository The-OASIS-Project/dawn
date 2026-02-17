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
 * WebUI Music Streaming - Internal Declarations
 *
 * This header contains internal types and functions shared between
 * webui_music.c and webui_music_handlers.c. Not part of the public API.
 */

#ifndef WEBUI_MUSIC_INTERNAL_H
#define WEBUI_MUSIC_INTERNAL_H

#include <libwebsockets.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_decoder.h"
#include "audio/resampler.h"
#include "webui/webui_internal.h"
#include "webui/webui_music.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants (shared)
 * ============================================================================= */

/** Maximum Opus frame size */
#define OPUS_MAX_FRAME_SIZE 1276

/** Quality tier names for logging/UI */
extern const char *QUALITY_NAMES[MUSIC_QUALITY_COUNT];

/* =============================================================================
 * Internal Types
 * ============================================================================= */

/**
 * @brief Repeat mode for music playback
 */
typedef enum {
   MUSIC_REPEAT_NONE = 0,
   MUSIC_REPEAT_ALL = 1,
   MUSIC_REPEAT_ONE = 2,
} music_repeat_mode_t;

/**
 * @brief Queue entry for a music track
 */
typedef struct {
   char path[WEBUI_MUSIC_PATH_MAX];
   char title[WEBUI_MUSIC_STRING_MAX];
   char artist[WEBUI_MUSIC_STRING_MAX];
   char album[WEBUI_MUSIC_STRING_MAX];
   uint32_t duration_sec;
} music_queue_entry_t;

/**
 * @brief Per-connection music streaming state
 *
 * Each WebSocket connection has its own music state, allowing
 * independent playback for each browser tab.
 */
typedef struct {
   /* Streaming state */
   atomic_bool streaming;       /**< True if streaming thread is active */
   atomic_bool stop_requested;  /**< Request streaming thread to stop */
   pthread_t stream_thread;     /**< Streaming thread handle */
   pthread_mutex_t state_mutex; /**< Protects non-atomic state */

   /* Thread synchronization for safe shutdown */
   pthread_cond_t decoder_idle_cond; /**< Signaled when decoder is not busy */
   atomic_bool shutdown_ack;         /**< Thread acknowledged shutdown request */

   /* Encoder */
   OpusEncoder *encoder;   /**< Opus encoder (quality-specific) */
   resampler_t *resampler; /**< Source rate -> 48kHz resampler */

   /* Resampling buffer - accumulates resampled samples for exact 960-frame encoding */
   int16_t *resample_accum;     /**< Accumulation buffer for resampled stereo samples */
   size_t resample_accum_size;  /**< Size of accumulation buffer in samples */
   size_t resample_accum_count; /**< Current sample count in buffer */

   /* Decoder */
   audio_decoder_t *decoder; /**< Current file decoder */
   atomic_bool decoder_busy; /**< True while decoder is being read */

   /* Playback state */
   bool playing;                      /**< True if playback in progress */
   bool paused;                       /**< True if paused */
   uint64_t position_frames;          /**< Current position in frames */
   uint32_t source_rate;              /**< Source file sample rate */
   uint8_t source_channels;           /**< Source file channels */
   audio_format_type_t source_format; /**< Source file format (FLAC, MP3, etc.) */

   /* Queue */
   music_queue_entry_t queue[WEBUI_MUSIC_MAX_QUEUE];
   int queue_length;
   int queue_index; /**< Current track in queue */

   /* Playback modes */
   bool shuffle;
   music_repeat_mode_t repeat_mode;
   unsigned int shuffle_seed; /**< Per-session PRNG seed for rand_r() */

   /* Settings */
   music_quality_t quality;
   music_bitrate_mode_t bitrate_mode;

   /* Pending reconfiguration (set by main thread, applied by streaming thread) */
   atomic_bool reconfigure_requested;
   music_quality_t pending_quality;
   music_bitrate_mode_t pending_bitrate_mode;

   /* Connection reference */
   ws_connection_t *conn;

   /* Position update tracking */
   uint64_t last_position_update_ms;

   /* Dedicated music WebSocket (direct streaming) */
   struct lws *music_wsi;                    /**< Music server WebSocket (NULL if not connected) */
   pthread_mutex_t write_mutex;              /**< Protects write buffer */
   uint8_t write_buffer[LWS_PRE + 4 + 1276]; /**< LWS_PRE + length prefix + max Opus frame */
   size_t write_pending_len;                 /**< Bytes pending in write buffer (0 = empty) */
} session_music_state_t;

/* =============================================================================
 * Internal Functions (shared between modules)
 * ============================================================================= */

/**
 * @brief Parse quality string to enum
 */
music_quality_t webui_music_parse_quality(const char *str);

/**
 * @brief Configure encoder for specified quality tier
 */
int webui_music_configure_encoder(session_music_state_t *state, music_quality_t quality);

/**
 * @brief Send current music state to client
 */
void webui_music_send_state(ws_connection_t *conn, session_music_state_t *state);

/**
 * @brief Send error message to client
 */
void webui_music_send_error(ws_connection_t *conn, const char *code, const char *message);

/**
 * @brief Start playback of a file
 */
int webui_music_start_playback(session_music_state_t *state, const char *path);

/**
 * @brief Stop streaming thread safely
 */
void webui_music_stop_streaming(session_music_state_t *state);

/**
 * @brief Check if path is within music library (security validation)
 */
bool webui_music_is_path_valid(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MUSIC_INTERNAL_H */
