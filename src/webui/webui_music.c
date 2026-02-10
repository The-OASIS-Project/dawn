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
 * WebUI Music Streaming - Implementation
 *
 * Streams music from server to WebUI clients using Opus encoding.
 * Each client has independent playback state and queue.
 */

#include "webui/webui_music.h"

#include <json-c/json.h>
#include <libwebsockets.h>
#include <limits.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/resampler.h"
#include "config/dawn_config.h"
#include "core/path_utils.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_music_server.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/** Opus bitrates for each quality tier (bits per second) */
static const int QUALITY_BITRATES[MUSIC_QUALITY_COUNT] = {
   48000,  /* VOICE */
   96000,  /* STANDARD */
   128000, /* HIGH */
   256000, /* HIFI */
};

/** Opus complexity for each quality tier (0-10) */
static const int QUALITY_COMPLEXITY[MUSIC_QUALITY_COUNT] = {
   5, /* VOICE */
   9, /* STANDARD */
   9, /* HIGH */
   9, /* HIFI */
};

/** Quality tier names for logging/UI (exported in webui_music_internal.h) */
const char *QUALITY_NAMES[MUSIC_QUALITY_COUNT] = {
   "voice",
   "standard",
   "high",
   "hifi",
};

/** Opus output sample rate */
#define OPUS_SAMPLE_RATE 48000

/** Position update interval in milliseconds */
#define POSITION_UPDATE_INTERVAL_MS 1000

/** Maximum Opus frame size */
#define OPUS_MAX_FRAME_SIZE 1276

/* Types (music_queue_entry_t, session_music_state_t) are in webui_music_internal.h */

/* =============================================================================
 * Module State
 * ============================================================================= */

static pthread_mutex_t s_music_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_initialized = false;
static webui_music_config_t s_config = {
   .enabled = true,
   .default_quality = MUSIC_QUALITY_STANDARD,
   .bitrate_mode = MUSIC_BITRATE_VBR,
};
static atomic_int s_active_streams = 0;

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

/**
 * @brief Get current time in milliseconds
 */
static uint64_t get_time_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Forward declarations */
static int queue_music_data(ws_connection_t *conn, const uint8_t *data, size_t len);
static int queue_music_direct(session_music_state_t *state, const uint8_t *data, size_t len);

/**
 * @brief Wait for decoder to become idle using condition variable
 *
 * Uses proper synchronization to wait for the streaming thread to finish
 * using the decoder. Falls back to timeout if condition isn't signaled.
 *
 * @param state Music session state
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if decoder is idle, false on timeout
 */
static bool wait_decoder_idle(session_music_state_t *state, int timeout_ms) {
   /* Quick check without lock */
   if (!atomic_load(&state->decoder_busy)) {
      return true;
   }

   pthread_mutex_lock(&state->state_mutex);

   /* Check again with lock held */
   if (!atomic_load(&state->decoder_busy)) {
      pthread_mutex_unlock(&state->state_mutex);
      return true;
   }

   /* Wait with timeout */
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += timeout_ms / 1000;
   ts.tv_nsec += (timeout_ms % 1000) * 1000000;
   if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
   }

   int ret = 0;
   while (atomic_load(&state->decoder_busy) && ret == 0) {
      ret = pthread_cond_timedwait(&state->decoder_idle_cond, &state->state_mutex, &ts);
   }

   bool idle = !atomic_load(&state->decoder_busy);
   pthread_mutex_unlock(&state->state_mutex);

   if (!idle) {
      LOG_WARNING("WebUI music: Timeout waiting for decoder to become idle");
   }

   return idle;
}

/**
 * @brief Validate that a file path is within the music library directory
 *
 * Security check to prevent path traversal attacks. Uses realpath() to resolve
 * symlinks and relative paths, then verifies the canonical path is within
 * the configured music library directory.
 *
 * @param path The file path to validate
 * @return true if path is safe (within music library), false otherwise
 */
bool webui_music_is_path_valid(const char *path) {
   if (!path || path[0] == '\0') {
      return false;
   }

   /* Quick check for obvious traversal patterns */
   if (contains_path_traversal(path)) {
      LOG_WARNING("WebUI music: Path traversal detected in: %s", path);
      return false;
   }

   /* Check if music directory is configured */
   if (g_config.paths.music_dir[0] == '\0') {
      LOG_WARNING("WebUI music: No music directory configured");
      return false;
   }

   char resolved_path[PATH_MAX];
   char resolved_music_dir[PATH_MAX];

   /* Resolve the music library base path */
   if (realpath(g_config.paths.music_dir, resolved_music_dir) == NULL) {
      LOG_ERROR("WebUI music: Cannot resolve music directory: %s", g_config.paths.music_dir);
      return false;
   }

   /* Resolve the requested file path */
   if (realpath(path, resolved_path) == NULL) {
      /* File doesn't exist - not valid for playback */
      LOG_WARNING("WebUI music: Cannot resolve path: %s", path);
      return false;
   }

   /* Ensure resolved path starts with resolved music directory */
   size_t music_dir_len = strlen(resolved_music_dir);
   if (strncmp(resolved_path, resolved_music_dir, music_dir_len) != 0) {
      LOG_WARNING("WebUI music: Path outside music library: %s", path);
      return false;
   }

   /* Ensure it's either exact match or followed by '/' */
   if (resolved_path[music_dir_len] != '\0' && resolved_path[music_dir_len] != '/') {
      LOG_WARNING("WebUI music: Path prefix mismatch: %s", path);
      return false;
   }

   return true;
}

/**
 * @brief Queue music audio data for sending via WebSocket
 *
 * This routes music data through the response queue to ensure thread-safe
 * WebSocket writes. Direct lws_write() from background threads is not safe.
 *
 * @param conn WebSocket connection
 * @param data Opus audio data (will be copied)
 * @param len Length of audio data
 * @return 0 on success, -1 on failure
 */
static int queue_music_data(ws_connection_t *conn, const uint8_t *data, size_t len) {
   if (!conn || !conn->session || !data || len == 0) {
      return -1;
   }

   /* Backpressure: drop music frames when queue is getting full.
    * This prevents music streaming from starving control messages.
    * Better to drop some audio frames than cause UI glitches. */
   int queue_fill = webui_get_queue_fill_pct();
   if (queue_fill > 75) {
      /* Queue is > 75% full - skip this frame to let it drain */
      static int drop_count = 0;
      if (++drop_count % 50 == 1)
         LOG_WARNING("WebUI music: Backpressure dropping frames (queue %d%%, dropped %d)",
                     queue_fill, drop_count);
      return 0; /* Not an error, just backpressure */
   }

   /* Allocate copy of data for queue (will be freed after send) */
   uint8_t *data_copy = malloc(len);
   if (!data_copy) {
      LOG_ERROR("WebUI music: Failed to allocate music data copy (%zu bytes)", len);
      return -1;
   }
   memcpy(data_copy, data, len);

   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_MUSIC_DATA,
                          .audio = { .data = data_copy, .len = len } };

   queue_response(&resp);
   return 0;
}

/**
 * @brief Parse quality string to enum
 */
music_quality_t webui_music_parse_quality(const char *str) {
   if (!str)
      return MUSIC_QUALITY_STANDARD;
   if (strcmp(str, "voice") == 0)
      return MUSIC_QUALITY_VOICE;
   if (strcmp(str, "standard") == 0)
      return MUSIC_QUALITY_STANDARD;
   if (strcmp(str, "high") == 0)
      return MUSIC_QUALITY_HIGH;
   if (strcmp(str, "hifi") == 0)
      return MUSIC_QUALITY_HIFI;
   return MUSIC_QUALITY_STANDARD;
}

/**
 * @brief Create or reconfigure Opus encoder for quality tier
 */
int webui_music_configure_encoder(session_music_state_t *state, music_quality_t quality) {
   int err;

   /* Destroy existing encoder if quality changed */
   if (state->encoder) {
      opus_encoder_destroy(state->encoder);
      state->encoder = NULL;
   }

   /* Create new encoder */
   state->encoder = opus_encoder_create(OPUS_SAMPLE_RATE, 2, /* stereo output */
                                        OPUS_APPLICATION_AUDIO, &err);
   if (err != OPUS_OK || !state->encoder) {
      LOG_ERROR("WebUI music: Failed to create Opus encoder: %s", opus_strerror(err));
      return 1;
   }

   /* Configure for quality tier */
   opus_encoder_ctl(state->encoder, OPUS_SET_BITRATE(QUALITY_BITRATES[quality]));
   opus_encoder_ctl(state->encoder, OPUS_SET_COMPLEXITY(QUALITY_COMPLEXITY[quality]));
   opus_encoder_ctl(state->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

   /* VBR or CBR */
   opus_encoder_ctl(state->encoder, OPUS_SET_VBR(state->bitrate_mode == MUSIC_BITRATE_VBR ? 1 : 0));

   state->quality = quality;
   LOG_INFO("WebUI music: Encoder configured for %s quality (%d kbps)", QUALITY_NAMES[quality],
            QUALITY_BITRATES[quality] / 1000);

   return 0;
}

/**
 * @brief Send music state update to client
 */
void webui_music_send_state(ws_connection_t *conn, session_music_state_t *state) {
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_state"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "playing", json_object_new_boolean(state->playing));
   json_object_object_add(payload, "paused", json_object_new_boolean(state->paused));

   /* Current track info */
   if (state->queue_length > 0 && state->queue_index < state->queue_length) {
      music_queue_entry_t *track = &state->queue[state->queue_index];
      struct json_object *track_obj = json_object_new_object();
      json_object_object_add(track_obj, "path", json_object_new_string(track->path));
      json_object_object_add(track_obj, "title", json_object_new_string(track->title));
      json_object_object_add(track_obj, "artist", json_object_new_string(track->artist));
      json_object_object_add(track_obj, "album", json_object_new_string(track->album));
      json_object_object_add(track_obj, "duration_sec", json_object_new_int(track->duration_sec));
      json_object_object_add(payload, "track", track_obj);
   } else {
      json_object_object_add(payload, "track", NULL);
   }

   /* Position (convert frames to seconds) */
   double position_sec = 0.0;
   if (state->source_rate > 0) {
      position_sec = (double)state->position_frames / state->source_rate;
   }
   json_object_object_add(payload, "position_sec", json_object_new_double(position_sec));

   /* Queue info */
   json_object_object_add(payload, "queue_length", json_object_new_int(state->queue_length));
   json_object_object_add(payload, "queue_index", json_object_new_int(state->queue_index));

   /* Source format info */
   json_object_object_add(payload, "source_format",
                          json_object_new_string(audio_decoder_format_name(state->source_format)));
   json_object_object_add(payload, "source_rate", json_object_new_int(state->source_rate));

   /* Quality info */
   json_object_object_add(payload, "quality",
                          json_object_new_string(QUALITY_NAMES[state->quality]));
   json_object_object_add(payload, "bitrate",
                          json_object_new_int(QUALITY_BITRATES[state->quality]));
   json_object_object_add(payload, "bitrate_mode",
                          json_object_new_string(state->bitrate_mode == MUSIC_BITRATE_VBR ? "vbr"
                                                                                          : "cbr"));

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Queue position update for client (thread-safe)
 *
 * Uses the response queue instead of direct lws_write() to be safe
 * for calling from the streaming thread.
 */
static void send_position_update(ws_connection_t *conn, session_music_state_t *state) {
   double position_sec = 0.0;
   uint32_t duration_sec = 0;

   if (state->source_rate > 0) {
      position_sec = (double)state->position_frames / state->source_rate;
   }
   if (state->queue_index < state->queue_length) {
      duration_sec = state->queue[state->queue_index].duration_sec;
   }

   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_MUSIC_POSITION,
                          .music_position = { .position_sec = position_sec,
                                              .duration_sec = duration_sec } };

   queue_response(&resp);
}

/**
 * @brief Send music error to client
 */
void webui_music_send_error(ws_connection_t *conn, const char *code, const char *message) {
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_error"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "code", json_object_new_string(code));
   json_object_object_add(payload, "message", json_object_new_string(message));

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Streaming Thread
 * ============================================================================= */

/**
 * @brief Music streaming thread
 *
 * Reads audio from decoder, resamples to 48kHz, encodes to Opus,
 * and sends to client via WebSocket.
 */
static void *music_stream_thread(void *arg) {
   session_music_state_t *state = (session_music_state_t *)arg;
   ws_connection_t *conn = state->conn;

   LOG_INFO("WebUI music: Streaming thread started");
   atomic_fetch_add(&s_active_streams, 1);

   /* Buffers for audio processing
    * resample_buffer needs extra space for upsampling (e.g., 44.1kHz -> 48kHz = 1.088x)
    * Using 3x to accommodate upsampling + resampler margin requirements */
   int16_t decode_buffer[WEBUI_MUSIC_FRAME_SAMPLES * 2]; /* Stereo decode buffer */
   int16_t
       resample_buffer[WEBUI_MUSIC_FRAME_SAMPLES * 3]; /* Resampled output (extra for upsampling) */
   uint8_t opus_buffer[OPUS_MAX_FRAME_SIZE + 2];       /* Opus frame + length prefix */
   float float_buffer[WEBUI_MUSIC_FRAME_SAMPLES * 3];  /* Float conversion for Opus */

   /* Time-based pacing to prevent audio speedup */
   uint64_t stream_start_time = 0;
   uint64_t frames_sent = 0;
   const uint64_t frame_duration_us = (WEBUI_MUSIC_FRAME_SAMPLES * 1000000ULL) / OPUS_SAMPLE_RATE;

   while (!atomic_load(&state->stop_requested)) {
      /* Quick check without lock first */
      if (!atomic_load(&state->streaming)) {
         usleep(50000);
         continue;
      }

      /* Check for pending reconfiguration (safe between frames) */
      if (atomic_load(&state->reconfigure_requested)) {
         pthread_mutex_lock(&state->state_mutex);
         music_quality_t new_quality = state->pending_quality;
         music_bitrate_mode_t new_bitrate_mode = state->pending_bitrate_mode;
         atomic_store(&state->reconfigure_requested, false);
         pthread_mutex_unlock(&state->state_mutex);

         /* Reconfigure encoder outside the lock */
         state->quality = new_quality;
         state->bitrate_mode = new_bitrate_mode;
         if (webui_music_configure_encoder(state, new_quality) == 0) {
            LOG_INFO("WebUI music: Reconfigured encoder to %s %s", QUALITY_NAMES[new_quality],
                     new_bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");
         }
      }

      /* Brief lock to check state and mark decoder busy */
      pthread_mutex_lock(&state->state_mutex);
      if (!state->decoder || !state->playing || state->paused) {
         pthread_mutex_unlock(&state->state_mutex);
         usleep(50000); /* 50ms idle sleep */
         continue;
      }

      /* Mark decoder busy and grab what we need */
      atomic_store(&state->decoder_busy, true);
      audio_decoder_t *decoder = state->decoder;
      resampler_t *resampler = state->resampler;
      uint32_t source_rate = state->source_rate;
      uint8_t source_channels = state->source_channels;
      pthread_mutex_unlock(&state->state_mutex);

      /* Read from decoder WITHOUT holding mutex */
      ssize_t frames_read = audio_decoder_read(decoder, decode_buffer, WEBUI_MUSIC_FRAME_SAMPLES);

      if (frames_read <= 0) {
         /* End of track or error - clear busy flag and signal waiters */
         atomic_store(&state->decoder_busy, false);
         pthread_mutex_lock(&state->state_mutex);
         pthread_cond_signal(&state->decoder_idle_cond);
         LOG_INFO("WebUI music: Track finished (read returned %zd)", frames_read);

         audio_decoder_close(state->decoder);
         state->decoder = NULL;

         /* Move to next track in queue */
         state->queue_index++;
         if (state->queue_index >= state->queue_length) {
            /* End of queue */
            state->playing = false;
            state->queue_index = 0;
            pthread_mutex_unlock(&state->state_mutex);

            /* Send final state update */
            webui_music_send_state(conn, state);
            continue;
         }

         /* Open next track */
         music_queue_entry_t *next = &state->queue[state->queue_index];
         state->decoder = audio_decoder_open(next->path);
         if (!state->decoder) {
            LOG_ERROR("WebUI music: Failed to open next track: %s", next->path);
            state->playing = false;
            pthread_mutex_unlock(&state->state_mutex);
            webui_music_send_error(conn, "DECODE_ERROR", "Failed to open next track");
            continue;
         }

         /* Get audio info */
         audio_decoder_info_t info;
         audio_decoder_get_info(state->decoder, &info);
         state->source_rate = info.sample_rate;
         state->source_channels = info.channels;
         state->source_format = info.format;
         state->position_frames = 0;

         /* Reconfigure resampler if needed */
         if (state->resampler) {
            resampler_destroy(state->resampler);
         }
         state->resampler = resampler_create(state->source_rate, OPUS_SAMPLE_RATE,
                                             state->source_channels);

         /* Clear accumulation buffer for fresh start */
         state->resample_accum_count = 0;

         /* Reset stream timing for new track */
         stream_start_time = 0;
         frames_sent = 0;

         pthread_mutex_unlock(&state->state_mutex);

         /* Send state update for new track */
         webui_music_send_state(conn, state);
         continue;
      }

      /* Resample to 48kHz if needed (using local vars, no mutex needed) */
      int16_t *audio_data = decode_buffer;
      size_t samples = frames_read * source_channels;

      if (resampler && source_rate != OPUS_SAMPLE_RATE) {
         size_t resampled = resampler_process(resampler, decode_buffer, samples, resample_buffer,
                                              sizeof(resample_buffer) / 2);
         audio_data = resample_buffer;
         samples = resampled;
      }

      /* Convert mono to stereo if needed */
      if (source_channels == 1) {
         /* Expand mono to stereo in-place (work backwards to avoid overwrite) */
         for (int i = samples - 1; i >= 0; i--) {
            audio_data[i * 2] = audio_data[i];
            audio_data[i * 2 + 1] = audio_data[i];
         }
         samples *= 2;
      }

      /* Done with decoder and resampler, safe for control operations now */
      atomic_store(&state->decoder_busy, false);

      /* Brief lock to update state and signal waiters */
      pthread_mutex_lock(&state->state_mutex);
      pthread_cond_signal(&state->decoder_idle_cond);
      state->position_frames += frames_read;

      /* Append resampled samples to accumulation buffer */
      if (state->resample_accum_count + samples <= state->resample_accum_size) {
         memcpy(state->resample_accum + state->resample_accum_count, audio_data,
                samples * sizeof(int16_t));
         state->resample_accum_count += samples;
      } else {
         /* Buffer overflow - this shouldn't happen with proper sizing */
         LOG_WARNING("WebUI music: Resample accumulator overflow, dropping samples");
      }
      pthread_mutex_unlock(&state->state_mutex);

      /* Encode and send while we have enough samples (960 stereo frames = 1920 samples) */
      while (state->resample_accum_count >= WEBUI_MUSIC_FRAME_SAMPLES * 2) {
         /* Initialize stream timing on first frame */
         if (stream_start_time == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            stream_start_time = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
            frames_sent = 0;
         }

         /* Convert first 960 stereo frames to float for Opus encoder */
         for (size_t i = 0; i < WEBUI_MUSIC_FRAME_SAMPLES * 2; i++) {
            float_buffer[i] = state->resample_accum[i] / 32768.0f;
         }

         /* Encode to Opus */
         int opus_bytes = opus_encode_float(state->encoder, float_buffer, WEBUI_MUSIC_FRAME_SAMPLES,
                                            opus_buffer + 2, sizeof(opus_buffer) - 2);

         if (opus_bytes < 0) {
            LOG_WARNING("WebUI music: Opus encode error: %s", opus_strerror(opus_bytes));
            break;
         }

         /* Prepend length prefix (little-endian) */
         opus_buffer[0] = opus_bytes & 0xFF;
         opus_buffer[1] = (opus_bytes >> 8) & 0xFF;

         /* Send to client - uses dedicated music socket if available, else main socket */
         queue_music_direct(state, opus_buffer, opus_bytes + 2);
         frames_sent++;

         /* Time-based pacing: calculate when this frame should have been sent */
         uint64_t expected_time = stream_start_time + (frames_sent * frame_duration_us);
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         uint64_t current_time = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;

         /* Sleep if we're ahead of schedule */
         if (current_time < expected_time) {
            uint64_t sleep_us = expected_time - current_time;
            if (sleep_us > 1000 && sleep_us < 100000) { /* Sanity check: 1ms - 100ms */
               usleep((useconds_t)sleep_us);
            }
         }

         /* Shift remaining samples to beginning of buffer */
         size_t remaining = state->resample_accum_count - WEBUI_MUSIC_FRAME_SAMPLES * 2;
         if (remaining > 0) {
            memmove(state->resample_accum, state->resample_accum + WEBUI_MUSIC_FRAME_SAMPLES * 2,
                    remaining * sizeof(int16_t));
         }
         state->resample_accum_count = remaining;
      }

      /* Send position update periodically (no mutex needed - queue_response is thread-safe) */
      uint64_t now = get_time_ms();
      if (now - state->last_position_update_ms >= POSITION_UPDATE_INTERVAL_MS) {
         send_position_update(conn, state);
         state->last_position_update_ms = now;
      }
   }

   LOG_INFO("WebUI music: Streaming thread stopped");
   atomic_fetch_sub(&s_active_streams, 1);
   atomic_store(&state->streaming, false);

   return NULL;
}

/**
 * @brief Start streaming for a session
 *
 * Stack requirements: The streaming thread allocates ~25KB on the stack for
 * audio buffers (decode, resample, opus, float conversion). Default pthread
 * stack size (2-8MB on Linux) is sufficient; no custom stack size needed.
 */
static int start_streaming(session_music_state_t *state) {
   if (atomic_load(&state->streaming)) {
      return 0; /* Already streaming */
   }

   atomic_store(&state->stop_requested, false);
   atomic_store(&state->streaming, true);

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   int ret = pthread_create(&state->stream_thread, &attr, music_stream_thread, state);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      LOG_ERROR("WebUI music: Failed to create streaming thread");
      atomic_store(&state->streaming, false);
      return 1;
   }

   return 0;
}

/**
 * @brief Stop streaming for a session
 */
void webui_music_stop_streaming(session_music_state_t *state) {
   if (!atomic_load(&state->streaming)) {
      return;
   }

   atomic_store(&state->stop_requested, true);

   /* Wait for thread to finish */
   pthread_join(state->stream_thread, NULL);
}

/* =============================================================================
 * Playback Control
 * ============================================================================= */

/**
 * @brief Start playing a track
 *
 * IMPORTANT: Stops any existing streaming thread before changing decoder/resampler.
 * This prevents race conditions where the old thread uses freed resources.
 */
int webui_music_start_playback(session_music_state_t *state, const char *path) {
   /* Stop existing streaming thread first - this is critical to prevent crashes
    * where the streaming thread accesses freed decoder/resampler */
   webui_music_stop_streaming(state);

   pthread_mutex_lock(&state->state_mutex);

   /* Close any existing decoder */
   if (state->decoder) {
      audio_decoder_close(state->decoder);
      state->decoder = NULL;
   }

   /* Open the file */
   state->decoder = audio_decoder_open(path);
   if (!state->decoder) {
      pthread_mutex_unlock(&state->state_mutex);
      LOG_ERROR("WebUI music: Failed to open: %s", path);
      return 1;
   }

   /* Get audio info */
   audio_decoder_info_t info;
   audio_decoder_get_info(state->decoder, &info);
   state->source_rate = info.sample_rate;
   state->source_channels = info.channels;
   state->source_format = info.format;
   state->position_frames = 0;

   LOG_INFO("WebUI music: Playing %s (%s %d Hz, %d ch)", path,
            audio_decoder_format_name(info.format), info.sample_rate, info.channels);

   /* Create resampler if needed */
   if (state->resampler) {
      resampler_destroy(state->resampler);
   }
   if (info.sample_rate != OPUS_SAMPLE_RATE) {
      state->resampler = resampler_create(info.sample_rate, OPUS_SAMPLE_RATE, info.channels);
      if (!state->resampler) {
         LOG_ERROR("WebUI music: Failed to create resampler");
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
         pthread_mutex_unlock(&state->state_mutex);
         return 1;
      }
   }

   /* Clear accumulation buffer for fresh start */
   state->resample_accum_count = 0;

   state->playing = true;
   state->paused = false;

   pthread_mutex_unlock(&state->state_mutex);

   /* Start streaming thread if not already running */
   return start_streaming(state);
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int webui_music_init(void) {
   pthread_mutex_lock(&s_music_mutex);

   if (s_initialized) {
      pthread_mutex_unlock(&s_music_mutex);
      return 0;
   }

   /* Load config from g_config (dawn.toml) */
   s_config.enabled = g_config.music.streaming_enabled;
   s_config.default_quality = webui_music_parse_quality(g_config.music.streaming_quality);
   s_config.bitrate_mode = (strcmp(g_config.music.streaming_bitrate_mode, "cbr") == 0)
                               ? MUSIC_BITRATE_CBR
                               : MUSIC_BITRATE_VBR;

   /* Check if music database is available */
   if (!music_db_is_initialized()) {
      LOG_WARNING("WebUI music: Music database not initialized - library features unavailable");
   }

   s_initialized = true;
   LOG_INFO("WebUI music streaming initialized (default quality: %s, bitrate: %s)",
            QUALITY_NAMES[s_config.default_quality],
            s_config.bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");

   pthread_mutex_unlock(&s_music_mutex);
   return 0;
}

void webui_music_cleanup(void) {
   pthread_mutex_lock(&s_music_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_music_mutex);
      return;
   }

   /* Wait for all streams to finish */
   while (atomic_load(&s_active_streams) > 0) {
      pthread_mutex_unlock(&s_music_mutex);
      usleep(100000); /* 100ms */
      pthread_mutex_lock(&s_music_mutex);
   }

   s_initialized = false;
   LOG_INFO("WebUI music streaming cleaned up");

   pthread_mutex_unlock(&s_music_mutex);
}

bool webui_music_is_available(void) {
   return s_initialized && s_config.enabled;
}

int webui_music_session_init(ws_connection_t *conn) {
   session_music_state_t *state = calloc(1, sizeof(session_music_state_t));
   if (!state) {
      LOG_ERROR("WebUI music: Failed to allocate session state");
      return 1;
   }

   pthread_mutex_init(&state->state_mutex, NULL);
   pthread_mutex_init(&state->write_mutex, NULL);
   pthread_cond_init(&state->decoder_idle_cond, NULL);
   atomic_store(&state->shutdown_ack, false);
   atomic_store(&state->reconfigure_requested, false);
   state->conn = conn;
   state->quality = s_config.default_quality;
   state->bitrate_mode = s_config.bitrate_mode;
   state->music_wsi = NULL;
   state->write_pending_len = 0;

   /* Allocate resampling accumulation buffer
    * Size: enough for ~100ms of stereo audio at 48kHz = 4800 * 2 = 9600 samples */
   state->resample_accum_size = 48000 / 10 * 2; /* 100ms stereo */
   state->resample_accum = malloc(state->resample_accum_size * sizeof(int16_t));
   if (!state->resample_accum) {
      LOG_ERROR("WebUI music: Failed to allocate resample buffer");
      pthread_mutex_destroy(&state->state_mutex);
      free(state);
      return 1;
   }
   state->resample_accum_count = 0;

   /* Create encoder */
   if (webui_music_configure_encoder(state, state->quality) != 0) {
      free(state->resample_accum);
      pthread_mutex_destroy(&state->state_mutex);
      free(state);
      return 1;
   }

   conn->music_state = state;
   LOG_INFO("WebUI music: Session initialized");

   return 0;
}

void webui_music_session_cleanup(ws_connection_t *conn) {
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return;
   }

   /* Stop streaming */
   webui_music_stop_streaming(state);

   /* Clean up resources */
   pthread_mutex_lock(&state->state_mutex);

   if (state->decoder) {
      audio_decoder_close(state->decoder);
   }
   if (state->resampler) {
      resampler_destroy(state->resampler);
   }
   if (state->encoder) {
      opus_encoder_destroy(state->encoder);
   }
   if (state->resample_accum) {
      free(state->resample_accum);
   }

   pthread_mutex_unlock(&state->state_mutex);
   pthread_cond_destroy(&state->decoder_idle_cond);
   pthread_mutex_destroy(&state->state_mutex);
   pthread_mutex_destroy(&state->write_mutex);

   free(state);
   conn->music_state = NULL;

   LOG_INFO("WebUI music: Session cleaned up");
}

/* =============================================================================
 * Message Handlers
 * ============================================================================= */

void handle_music_subscribe(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   if (!webui_music_is_available()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music streaming is not available");
      return;
   }

   /* Initialize session state if needed */
   if (!conn->music_state) {
      if (webui_music_session_init(conn) != 0) {
         webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
         return;
      }
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;

   /* Parse quality and bitrate mode settings */
   if (payload) {
      bool reconfigure = false;
      music_quality_t new_quality = state->quality;
      music_bitrate_mode_t new_bitrate_mode = state->bitrate_mode;

      struct json_object *quality_obj;
      if (json_object_object_get_ex(payload, "quality", &quality_obj)) {
         new_quality = webui_music_parse_quality(json_object_get_string(quality_obj));
         if (new_quality != state->quality) {
            reconfigure = true;
         }
      }

      struct json_object *bitrate_mode_obj;
      if (json_object_object_get_ex(payload, "bitrate_mode", &bitrate_mode_obj)) {
         const char *mode_str = json_object_get_string(bitrate_mode_obj);
         if (mode_str && strcmp(mode_str, "cbr") == 0) {
            new_bitrate_mode = MUSIC_BITRATE_CBR;
         } else {
            new_bitrate_mode = MUSIC_BITRATE_VBR;
         }
         if (new_bitrate_mode != state->bitrate_mode) {
            reconfigure = true;
         }
      }

      if (reconfigure) {
         /* Update state values immediately for display purposes */
         pthread_mutex_lock(&state->state_mutex);
         state->quality = new_quality;
         state->bitrate_mode = new_bitrate_mode;
         pthread_mutex_unlock(&state->state_mutex);

         /* Set pending values for streaming thread to reconfigure encoder safely */
         state->pending_quality = new_quality;
         state->pending_bitrate_mode = new_bitrate_mode;
         atomic_store(&state->reconfigure_requested, true);
      }
   }

   /* Send current state */
   pthread_mutex_lock(&state->state_mutex);
   webui_music_send_state(conn, state);
   pthread_mutex_unlock(&state->state_mutex);

   LOG_INFO("WebUI music: Client subscribed (quality: %s, bitrate: %s)",
            QUALITY_NAMES[state->quality],
            state->bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");
}

void handle_music_unsubscribe(ws_connection_t *conn) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return;
   }

   /* Stop playback and streaming */
   pthread_mutex_lock(&state->state_mutex);
   state->playing = false;
   state->paused = false;
   pthread_mutex_unlock(&state->state_mutex);

   webui_music_stop_streaming(state);

   LOG_INFO("WebUI music: Client unsubscribed");
}

void handle_music_control(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      /* Auto-initialize on first control message */
      if (webui_music_session_init(conn) != 0) {
         webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
         return;
      }
      state = (session_music_state_t *)conn->music_state;
   }

   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);
   LOG_INFO("WebUI music: Control action '%s'", action);

   if (strcmp(action, "play") == 0) {
      /* Play specific track by path, or search query */
      struct json_object *path_obj;
      struct json_object *query_obj;

      if (json_object_object_get_ex(payload, "path", &path_obj)) {
         /* Play specific track by path - add to top of queue and play */
         const char *path = json_object_get_string(path_obj);

         /* Security: validate path is within music library */
         if (!webui_music_is_path_valid(path)) {
            webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
            return;
         }

         /* Get track metadata */
         music_search_result_t track_info;
         if (music_db_get_by_path(path, &track_info) != 0) {
            /* Fallback: use path as title */
            safe_strncpy(track_info.path, path, sizeof(track_info.path));
            safe_strncpy(track_info.title, path, sizeof(track_info.title));
            track_info.artist[0] = '\0';
            track_info.album[0] = '\0';
            track_info.duration_sec = 0;
         }

         /* Stop any current playback */
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }

         /* Shift queue down and insert at position 0 */
         if (state->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
            memmove(&state->queue[1], &state->queue[0],
                    state->queue_length * sizeof(music_queue_entry_t));
            state->queue_length++;
         } else {
            /* Queue full - shift and lose last item */
            memmove(&state->queue[1], &state->queue[0],
                    (WEBUI_MUSIC_MAX_QUEUE - 1) * sizeof(music_queue_entry_t));
         }

         /* Insert track at top */
         music_queue_entry_t *entry = &state->queue[0];
         safe_strncpy(entry->path, track_info.path, sizeof(entry->path));
         safe_strncpy(entry->title, track_info.title, sizeof(entry->title));
         safe_strncpy(entry->artist, track_info.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, track_info.album, sizeof(entry->album));
         entry->duration_sec = track_info.duration_sec;
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         /* Start playback */
         if (webui_music_start_playback(state, path) != 0) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
            return;
         }

         /* Send state update so client shows track info immediately */
         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

      } else if (json_object_object_get_ex(payload, "query", &query_obj)) {
         const char *query = json_object_get_string(query_obj);

         /* Search for tracks */
         music_search_result_t *results = malloc(WEBUI_MUSIC_MAX_QUEUE *
                                                 sizeof(music_search_result_t));
         if (!results) {
            webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
            return;
         }

         int count = music_db_search(query, results, WEBUI_MUSIC_MAX_QUEUE);
         if (count <= 0) {
            free(results);
            webui_music_send_error(conn, "NOT_FOUND", "No music found matching query");
            return;
         }

         /* Stop any current playback */
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }

         /* Build queue from results */
         state->queue_length = 0;
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            music_queue_entry_t *entry = &state->queue[state->queue_length];
            safe_strncpy(entry->path, results[i].path, sizeof(entry->path));
            safe_strncpy(entry->title, results[i].title, sizeof(entry->title));
            safe_strncpy(entry->artist, results[i].artist, sizeof(entry->artist));
            safe_strncpy(entry->album, results[i].album, sizeof(entry->album));
            entry->duration_sec = results[i].duration_sec;
            state->queue_length++;
         }
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         free(results);

         /* Start playback */
         if (webui_music_start_playback(state, state->queue[0].path) != 0) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
            return;
         }

         /* Send state update so client shows track info immediately */
         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

      } else if (state->paused) {
         /* Resume from pause */
         pthread_mutex_lock(&state->state_mutex);
         state->paused = false;
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);
      } else {
         /* Already playing - just send state */
         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);
      }

   } else if (strcmp(action, "pause") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = true;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "stop") == 0) {
      /* Stop streaming thread completely before closing decoder.
       * This ensures no use-after-free - thread is fully stopped via pthread_join. */
      webui_music_stop_streaming(state);

      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      state->paused = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->position_frames = 0;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "next") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_index < state->queue_length - 1) {
         state->queue_index++;
         const char *path = state->queue[state->queue_index].path;
         pthread_mutex_unlock(&state->state_mutex);

         /* Start next track (handles decoder close with wait) */
         webui_music_start_playback(state, path);
      } else {
         pthread_mutex_unlock(&state->state_mutex);
      }

      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "previous") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_index > 0) {
         state->queue_index--;
         const char *path = state->queue[state->queue_index].path;
         pthread_mutex_unlock(&state->state_mutex);

         /* Start previous track (handles decoder close with wait) */
         webui_music_start_playback(state, path);
      } else {
         pthread_mutex_unlock(&state->state_mutex);
      }

      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "seek") == 0) {
      struct json_object *position_obj;
      if (json_object_object_get_ex(payload, "position_sec", &position_obj)) {
         double position_sec = json_object_get_double(position_obj);

         /* Stop streaming thread to safely access decoder
          * This prevents race conditions where thread reads while we seek */
         bool was_streaming = atomic_load(&state->streaming);
         if (was_streaming) {
            webui_music_stop_streaming(state);
         }

         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder && state->source_rate > 0) {
            uint64_t sample_pos = (uint64_t)(position_sec * state->source_rate);
            if (audio_decoder_seek(state->decoder, sample_pos) == 0) {
               state->position_frames = sample_pos;
               /* Clear accumulation buffer after seek */
               state->resample_accum_count = 0;
            }
         }
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         /* Restart streaming if it was active */
         if (was_streaming && state->playing) {
            start_streaming(state);
         }
      }

   } else if (strcmp(action, "play_index") == 0) {
      /* Play specific track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      int play_index = json_object_get_int(index_obj);

      pthread_mutex_lock(&state->state_mutex);
      if (play_index < 0 || play_index >= state->queue_length) {
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }

      state->queue_index = play_index;
      const char *path = state->queue[play_index].path;
      pthread_mutex_unlock(&state->state_mutex);

      /* Start playback (handles closing existing decoder with proper wait) */
      if (webui_music_start_playback(state, path) != 0) {
         webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
         return;
      }

      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "add_to_queue") == 0) {
      /* Add track to end of queue without starting playback */
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }
      const char *path = json_object_get_string(path_obj);

      /* Security: validate path is within music library */
      if (!webui_music_is_path_valid(path)) {
         webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
         return;
      }

      /* Get track metadata */
      music_search_result_t track_info;
      if (music_db_get_by_path(path, &track_info) != 0) {
         safe_strncpy(track_info.path, path, sizeof(track_info.path));
         safe_strncpy(track_info.title, path, sizeof(track_info.title));
         track_info.artist[0] = '\0';
         track_info.album[0] = '\0';
         track_info.duration_sec = 0;
      }

      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, track_info.path, sizeof(entry->path));
         safe_strncpy(entry->title, track_info.title, sizeof(entry->title));
         safe_strncpy(entry->artist, track_info.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, track_info.album, sizeof(entry->album));
         entry->duration_sec = track_info.duration_sec;
         state->queue_length++;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "remove_from_queue") == 0) {
      /* Remove track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      int remove_index = json_object_get_int(index_obj);

      pthread_mutex_lock(&state->state_mutex);
      if (remove_index < 0 || remove_index >= state->queue_length) {
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }

      bool removing_current = (remove_index == state->queue_index);
      bool was_playing = state->playing && !state->paused;

      /* Shift queue entries after the removed index */
      if (remove_index < state->queue_length - 1) {
         memmove(&state->queue[remove_index], &state->queue[remove_index + 1],
                 (state->queue_length - remove_index - 1) * sizeof(music_queue_entry_t));
      }
      state->queue_length--;

      /* Adjust current index if needed */
      if (remove_index < state->queue_index) {
         state->queue_index--;
      } else if (removing_current) {
         /* Removed current track - need to handle playback */
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }

         if (state->queue_length > 0) {
            /* Adjust index if we were at end */
            if (state->queue_index >= state->queue_length) {
               state->queue_index = state->queue_length - 1;
            }

            if (was_playing) {
               /* Start playing the next track */
               const char *next_path = state->queue[state->queue_index].path;
               pthread_mutex_unlock(&state->state_mutex);
               webui_music_start_playback(state, next_path);
               pthread_mutex_lock(&state->state_mutex);
            }
         } else {
            /* Queue is now empty */
            state->playing = false;
            state->paused = false;
            state->queue_index = 0;
         }
      }

      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "clear_queue") == 0) {
      /* Clear entire queue and stop playback */
      webui_music_stop_streaming(state);

      pthread_mutex_lock(&state->state_mutex);
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->playing = false;
      state->paused = false;
      state->queue_length = 0;
      state->queue_index = 0;
      state->position_frames = 0;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "add_artist") == 0) {
      /* Add all tracks by an artist to queue */
      struct json_object *artist_obj;
      if (!json_object_object_get_ex(payload, "artist", &artist_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing artist");
         return;
      }
      const char *artist_name = json_object_get_string(artist_obj);

      music_search_result_t *tracks = malloc(100 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         return;
      }
      int count = music_db_get_by_artist(artist_name, tracks, 100);

      pthread_mutex_lock(&state->state_mutex);
      int added = 0;
      for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, tracks[i].path, sizeof(entry->path));
         safe_strncpy(entry->title, tracks[i].title, sizeof(entry->title));
         safe_strncpy(entry->artist, tracks[i].artist, sizeof(entry->artist));
         safe_strncpy(entry->album, tracks[i].album, sizeof(entry->album));
         entry->duration_sec = tracks[i].duration_sec;
         state->queue_length++;
         added++;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      free(tracks);
      LOG_INFO("WebUI music: Added %d tracks by '%s' to queue", added, artist_name);

   } else if (strcmp(action, "add_album") == 0) {
      /* Add all tracks from an album to queue */
      struct json_object *album_obj;
      if (!json_object_object_get_ex(payload, "album", &album_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing album");
         return;
      }
      const char *album_name = json_object_get_string(album_obj);

      music_search_result_t *tracks = malloc(50 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         return;
      }
      int count = music_db_get_by_album(album_name, tracks, 50);

      pthread_mutex_lock(&state->state_mutex);
      int added = 0;
      for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, tracks[i].path, sizeof(entry->path));
         safe_strncpy(entry->title, tracks[i].title, sizeof(entry->title));
         safe_strncpy(entry->artist, tracks[i].artist, sizeof(entry->artist));
         safe_strncpy(entry->album, tracks[i].album, sizeof(entry->album));
         entry->duration_sec = tracks[i].duration_sec;
         state->queue_length++;
         added++;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      free(tracks);
      LOG_INFO("WebUI music: Added %d tracks from album '%s' to queue", added, album_name);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown control action");
   }
}

void handle_music_search(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   if (!music_db_is_initialized()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music database not available");
      return;
   }

   struct json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing query");
      return;
   }

   const char *query = json_object_get_string(query_obj);
   int limit = 50; /* Default limit */

   struct json_object *limit_obj;
   if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
      limit = json_object_get_int(limit_obj);
      if (limit <= 0 || limit > WEBUI_MUSIC_MAX_QUEUE) {
         limit = 50;
      }
   }

   /* Search database */
   music_search_result_t *results = malloc(limit * sizeof(music_search_result_t));
   if (!results) {
      webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
      return;
   }

   int count = music_db_search(query, results, limit);

   /* Build response */
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_search_response"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "query", json_object_new_string(query));
   json_object_object_add(resp_payload, "count", json_object_new_int(count > 0 ? count : 0));

   struct json_object *results_arr = json_object_new_array();
   for (int i = 0; i < count; i++) {
      struct json_object *track = json_object_new_object();
      json_object_object_add(track, "path", json_object_new_string(results[i].path));
      json_object_object_add(track, "title", json_object_new_string(results[i].title));
      json_object_object_add(track, "artist", json_object_new_string(results[i].artist));
      json_object_object_add(track, "album", json_object_new_string(results[i].album));
      json_object_object_add(track, "display_name",
                             json_object_new_string(results[i].display_name));
      json_object_object_add(track, "duration_sec", json_object_new_int(results[i].duration_sec));
      json_object_array_add(results_arr, track);
   }
   json_object_object_add(resp_payload, "results", results_arr);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   free(results);
}

void handle_music_library(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   if (!music_db_is_initialized()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music database not available");
      return;
   }

   const char *browse_type = "stats"; /* Default */

   struct json_object *type_obj;
   if (payload && json_object_object_get_ex(payload, "type", &type_obj)) {
      browse_type = json_object_get_string(type_obj);
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_library_response"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "browse_type", json_object_new_string(browse_type));

   if (strcmp(browse_type, "stats") == 0) {
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      json_object_object_add(resp_payload, "track_count", json_object_new_int(stats.track_count));
      json_object_object_add(resp_payload, "artist_count", json_object_new_int(stats.artist_count));
      json_object_object_add(resp_payload, "album_count", json_object_new_int(stats.album_count));

   } else if (strcmp(browse_type, "tracks") == 0) {
      /* Paginated track listing (default limit=50, max=200) */
      int limit = 50;
      int offset = 0;

      struct json_object *limit_obj, *offset_obj;
      if (payload && json_object_object_get_ex(payload, "limit", &limit_obj))
         limit = json_object_get_int(limit_obj);
      if (payload && json_object_object_get_ex(payload, "offset", &offset_obj))
         offset = json_object_get_int(offset_obj);

      if (limit < 1)
         limit = 1;
      if (limit > 200)
         limit = 200;
      if (offset < 0)
         offset = 0;

      music_search_result_t *tracks = malloc(limit * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = music_db_list_paged(tracks, limit, offset);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.track_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(tracks);

   } else if (strcmp(browse_type, "artists") == 0) {
      /* List artists with stats (album count, track count) */
      music_artist_info_t *artists = malloc(200 * sizeof(music_artist_info_t));
      if (!artists) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate artist list");
         json_object_put(response);
         return;
      }
      int count = music_db_list_artists_with_stats(artists, 200);

      struct json_object *artists_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *artist = json_object_new_object();
         json_object_object_add(artist, "name", json_object_new_string(artists[i].name));
         json_object_object_add(artist, "album_count", json_object_new_int(artists[i].album_count));
         json_object_object_add(artist, "track_count", json_object_new_int(artists[i].track_count));
         json_object_array_add(artists_arr, artist);
      }
      json_object_object_add(resp_payload, "artists", artists_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(artists);

   } else if (strcmp(browse_type, "albums") == 0) {
      /* List albums with stats (track count, artist) */
      music_album_info_t *albums = malloc(200 * sizeof(music_album_info_t));
      if (!albums) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate album list");
         json_object_put(response);
         return;
      }
      int count = music_db_list_albums_with_stats(albums, 200);

      struct json_object *albums_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *album = json_object_new_object();
         json_object_object_add(album, "name", json_object_new_string(albums[i].name));
         json_object_object_add(album, "artist", json_object_new_string(albums[i].artist));
         json_object_object_add(album, "track_count", json_object_new_int(albums[i].track_count));
         json_object_array_add(albums_arr, album);
      }
      json_object_object_add(resp_payload, "albums", albums_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(albums);

   } else if (strcmp(browse_type, "tracks_by_artist") == 0) {
      /* Get tracks for a specific artist */
      struct json_object *artist_obj;
      if (!json_object_object_get_ex(payload, "artist", &artist_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing artist name");
         json_object_put(response);
         return;
      }
      const char *artist_name = json_object_get_string(artist_obj);

      music_search_result_t *tracks = malloc(100 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = music_db_get_by_artist(artist_name, tracks, 100);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "artist", json_object_new_string(artist_name));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(tracks);

   } else if (strcmp(browse_type, "tracks_by_album") == 0) {
      /* Get tracks for a specific album */
      struct json_object *album_obj;
      if (!json_object_object_get_ex(payload, "album", &album_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing album name");
         json_object_put(response);
         return;
      }
      const char *album_name = json_object_get_string(album_obj);

      music_search_result_t *tracks = malloc(50 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = music_db_get_by_album(album_name, tracks, 50);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "album", json_object_new_string(album_name));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(tracks);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

void handle_music_queue(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      webui_music_send_error(conn, "NOT_INITIALIZED", "Music session not initialized");
      return;
   }

   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);

   if (strcmp(action, "list") == 0) {
      /* Return current queue */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("music_queue_response"));

      struct json_object *resp_payload = json_object_new_object();

      pthread_mutex_lock(&state->state_mutex);

      struct json_object *queue_arr = json_object_new_array();
      for (int i = 0; i < state->queue_length; i++) {
         music_queue_entry_t *entry = &state->queue[i];
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(entry->path));
         json_object_object_add(track, "title", json_object_new_string(entry->title));
         json_object_object_add(track, "artist", json_object_new_string(entry->artist));
         json_object_object_add(track, "album", json_object_new_string(entry->album));
         json_object_object_add(track, "duration_sec", json_object_new_int(entry->duration_sec));
         json_object_array_add(queue_arr, track);
      }

      json_object_object_add(resp_payload, "queue", queue_arr);
      json_object_object_add(resp_payload, "current_index",
                             json_object_new_int(state->queue_index));
      json_object_object_add(resp_payload, "length", json_object_new_int(state->queue_length));

      pthread_mutex_unlock(&state->state_mutex);

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);

   } else if (strcmp(action, "clear") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->queue_length = 0;
      state->queue_index = 0;
      state->playing = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "add") == 0) {
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }

      const char *path = json_object_get_string(path_obj);

      /* Security: validate path is within music library */
      if (!webui_music_is_path_valid(path)) {
         webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
         return;
      }

      /* Get metadata from database */
      music_search_result_t result;
      if (music_db_get_by_path(path, &result) != 0) {
         webui_music_send_error(conn, "NOT_FOUND", "Track not found in database");
         return;
      }

      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, result.path, sizeof(entry->path));
         safe_strncpy(entry->title, result.title, sizeof(entry->title));
         safe_strncpy(entry->artist, result.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, result.album, sizeof(entry->album));
         entry->duration_sec = result.duration_sec;
         state->queue_length++;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "remove") == 0) {
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }

      int index = json_object_get_int(index_obj);

      pthread_mutex_lock(&state->state_mutex);
      if (index >= 0 && index < state->queue_length) {
         /* Shift remaining entries */
         for (int i = index; i < state->queue_length - 1; i++) {
            state->queue[i] = state->queue[i + 1];
         }
         state->queue_length--;

         /* Adjust current index if needed */
         if (state->queue_index > index) {
            state->queue_index--;
         } else if (state->queue_index >= state->queue_length) {
            state->queue_index = state->queue_length > 0 ? state->queue_length - 1 : 0;
         }
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown queue action");
   }
}

/* =============================================================================
 * State Query Functions
 * ============================================================================= */

struct json_object *webui_music_get_state(ws_connection_t *conn) {
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return NULL;
   }

   struct json_object *result = json_object_new_object();

   pthread_mutex_lock(&state->state_mutex);

   json_object_object_add(result, "playing", json_object_new_boolean(state->playing));
   json_object_object_add(result, "paused", json_object_new_boolean(state->paused));

   if (state->queue_length > 0 && state->queue_index < state->queue_length) {
      music_queue_entry_t *track = &state->queue[state->queue_index];
      struct json_object *track_obj = json_object_new_object();
      json_object_object_add(track_obj, "title", json_object_new_string(track->title));
      json_object_object_add(track_obj, "artist", json_object_new_string(track->artist));
      json_object_object_add(track_obj, "album", json_object_new_string(track->album));
      json_object_object_add(track_obj, "duration_sec", json_object_new_int(track->duration_sec));
      json_object_object_add(result, "track", track_obj);
   }

   double position_sec = 0.0;
   if (state->source_rate > 0) {
      position_sec = (double)state->position_frames / state->source_rate;
   }
   json_object_object_add(result, "position_sec", json_object_new_double(position_sec));
   json_object_object_add(result, "queue_length", json_object_new_int(state->queue_length));
   json_object_object_add(result, "quality", json_object_new_string(QUALITY_NAMES[state->quality]));

   pthread_mutex_unlock(&state->state_mutex);

   return result;
}

int webui_music_get_stream_count(void) {
   return atomic_load(&s_active_streams);
}

/* =============================================================================
 * Configuration
 * ============================================================================= */

void webui_music_get_config(webui_music_config_t *config) {
   pthread_mutex_lock(&s_music_mutex);
   *config = s_config;
   pthread_mutex_unlock(&s_music_mutex);
}

int webui_music_set_config(const webui_music_config_t *config) {
   pthread_mutex_lock(&s_music_mutex);
   s_config = *config;
   pthread_mutex_unlock(&s_music_mutex);
   return 0;
}

/* =============================================================================
 * LLM Tool Integration
 * ============================================================================= */

int webui_music_execute_tool(ws_connection_t *conn,
                             const char *action,
                             const char *query,
                             char **result_out) {
   if (!conn || !action) {
      return 1;
   }

   /* Ensure music session is initialized */
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      if (webui_music_session_init(conn) != 0) {
         if (result_out) {
            *result_out = strdup("Failed to initialize music session");
         }
         return 1;
      }
      state = (session_music_state_t *)conn->music_state;
   }

   LOG_INFO("WebUI music tool: action='%s' query='%s'", action, query ? query : "(none)");

   if (strcmp(action, "play") == 0) {
      if (!query || query[0] == '\0') {
         /* Resume if paused */
         if (state->paused) {
            pthread_mutex_lock(&state->state_mutex);
            state->paused = false;
            webui_music_send_state(conn, state);
            pthread_mutex_unlock(&state->state_mutex);
            if (result_out) {
               *result_out = strdup("Resumed playback");
            }
            return 0;
         }
         if (result_out) {
            *result_out = strdup("Please specify what to search for");
         }
         return 1;
      }

      /* Search and play */
      if (!music_db_is_initialized()) {
         if (result_out) {
            *result_out = strdup("Music database not available");
         }
         return 1;
      }

      music_search_result_t *results = malloc(WEBUI_MUSIC_MAX_QUEUE *
                                              sizeof(music_search_result_t));
      if (!results) {
         if (result_out) {
            *result_out = strdup("Memory allocation failed");
         }
         return 1;
      }

      int count = music_db_search(query, results, WEBUI_MUSIC_MAX_QUEUE);
      if (count <= 0) {
         free(results);
         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "No music found matching '%s'", query);
            *result_out = strdup(buf);
         }
         return 1;
      }

      /* Stop any current playback */
      webui_music_stop_streaming(state);
      pthread_mutex_lock(&state->state_mutex);
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }

      /* Build queue from results */
      state->queue_length = 0;
      for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, results[i].path, sizeof(entry->path));
         safe_strncpy(entry->title, results[i].title, sizeof(entry->title));
         safe_strncpy(entry->artist, results[i].artist, sizeof(entry->artist));
         safe_strncpy(entry->album, results[i].album, sizeof(entry->album));
         entry->duration_sec = results[i].duration_sec;
         state->queue_length++;
      }
      state->queue_index = 0;
      pthread_mutex_unlock(&state->state_mutex);

      free(results);

      /* Start playback of first track */
      if (webui_music_start_playback(state, state->queue[0].path) != 0) {
         if (result_out) {
            *result_out = strdup("Failed to start playback");
         }
         return 1;
      }

      /* Send state update to client */
      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         char buf[512];
         snprintf(buf, sizeof(buf),
                  "Now playing: %s (track 1 of %d matching '%s') - streaming to WebUI",
                  state->queue[0].title[0] ? state->queue[0].title : "Unknown", count, query);
         *result_out = strdup(buf);
      }
      return 0;

   } else if (strcmp(action, "stop") == 0) {
      webui_music_stop_streaming(state);
      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      state->paused = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->position_frames = 0;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup("Music playback stopped");
      }
      return 0;

   } else if (strcmp(action, "pause") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = true;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup("Music paused");
      }
      return 0;

   } else if (strcmp(action, "resume") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = false;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup("Music resumed");
      }
      return 0;

   } else if (strcmp(action, "next") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_index < state->queue_length - 1) {
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         state->queue_index++;
         const char *next_path = state->queue[state->queue_index].path;
         const char *next_title = state->queue[state->queue_index].title;
         pthread_mutex_unlock(&state->state_mutex);

         webui_music_start_playback(state, next_path);

         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Playing next: %s", next_title[0] ? next_title : "Unknown");
            *result_out = strdup(buf);
         }
      } else {
         pthread_mutex_unlock(&state->state_mutex);
         if (result_out) {
            *result_out = strdup("Already at end of queue");
         }
      }
      return 0;

   } else if (strcmp(action, "previous") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_index > 0) {
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         state->queue_index--;
         const char *prev_path = state->queue[state->queue_index].path;
         const char *prev_title = state->queue[state->queue_index].title;
         pthread_mutex_unlock(&state->state_mutex);

         webui_music_start_playback(state, prev_path);

         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Playing previous: %s",
                     prev_title[0] ? prev_title : "Unknown");
            *result_out = strdup(buf);
         }
      } else {
         pthread_mutex_unlock(&state->state_mutex);
         if (result_out) {
            *result_out = strdup("Already at start of queue");
         }
      }
      return 0;

   } else if (strcmp(action, "list") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_length == 0) {
         pthread_mutex_unlock(&state->state_mutex);
         if (result_out) {
            *result_out = strdup("No music in queue");
         }
         return 0;
      }

      /* Build queue list */
      size_t buf_size = 1024 + (state->queue_length * 128);
      char *buf = malloc(buf_size);
      if (buf) {
         int offset = snprintf(buf, buf_size, "Queue (%d tracks, currently #%d):\n",
                               state->queue_length, state->queue_index + 1);
         for (int i = 0; i < state->queue_length && offset < (int)buf_size - 128; i++) {
            const char *marker = (i == state->queue_index) ? "▶ " : "  ";
            offset += snprintf(buf + offset, buf_size - offset, "%s%d. %s - %s\n", marker, i + 1,
                               state->queue[i].artist[0] ? state->queue[i].artist : "Unknown",
                               state->queue[i].title[0] ? state->queue[i].title : "Unknown");
         }
         if (result_out) {
            *result_out = buf;
         } else {
            free(buf);
         }
      }
      pthread_mutex_unlock(&state->state_mutex);
      return 0;

   } else if (strcmp(action, "search") == 0 || strcmp(action, "library") == 0) {
      /* These don't change playback state, just return info */
      /* For now, let music_tool handle these since they don't affect streaming */
      if (result_out) {
         *result_out = NULL; /* Signal to fall through to regular handler */
      }
      return -1; /* -1 means "not handled, use default" */
   }

   if (result_out) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Unknown action: %s", action);
      *result_out = strdup(buf);
   }
   return 1;
}

/* =============================================================================
 * Dedicated Music Server Integration
 * ============================================================================= */

void webui_music_set_stream_wsi(session_t *session, struct lws *wsi) {
   if (!session)
      return;

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   if (!conn)
      return;

   /* Lazily initialize music state if needed (music stream may connect
    * before any music_subscribe/control message arrives from the client) */
   if (!conn->music_state && wsi) {
      if (webui_music_session_init(conn) != 0) {
         LOG_ERROR("WebUI music: Failed to init session for stream wsi");
         return;
      }
   }

   if (!conn->music_state)
      return;

   session_music_state_t *state = (session_music_state_t *)conn->music_state;

   pthread_mutex_lock(&state->write_mutex);
   state->music_wsi = wsi;
   state->write_pending_len = 0; /* Clear any stale pending data */
   pthread_mutex_unlock(&state->write_mutex);

   LOG_INFO("WebUI music: %s music stream wsi for session %u", wsi ? "Set" : "Cleared",
            session->session_id);
}

int webui_music_write_pending(session_t *session, struct lws *wsi) {
   if (!session)
      return -1;

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   if (!conn || !conn->music_state)
      return -1;

   session_music_state_t *state = (session_music_state_t *)conn->music_state;

   pthread_mutex_lock(&state->write_mutex);

   if (state->write_pending_len == 0) {
      pthread_mutex_unlock(&state->write_mutex);
      return 1; /* No data pending */
   }

   /* Write the pending frame (data is already positioned after LWS_PRE) */
   int written = lws_write(wsi, state->write_buffer + LWS_PRE, state->write_pending_len,
                           LWS_WRITE_BINARY);

   if (written < 0) {
      LOG_WARNING("WebUI music: lws_write failed");
      state->write_pending_len = 0;
      pthread_mutex_unlock(&state->write_mutex);
      return -1;
   }

   state->write_pending_len = 0;
   pthread_mutex_unlock(&state->write_mutex);

   return 0;
}

/**
 * @brief Queue audio data for direct write to music WebSocket
 *
 * Called by streaming thread to send audio. If music_wsi is set,
 * buffers the data and requests a writeable callback. Otherwise
 * falls back to the main WebSocket queue.
 *
 * @param state Session music state
 * @param data Audio data (length prefix + opus frame, WITHOUT type byte)
 * @param len Data length
 * @return 0 on success
 */
static int queue_music_direct(session_music_state_t *state, const uint8_t *data, size_t len) {
   pthread_mutex_lock(&state->write_mutex);

   if (!state->music_wsi) {
      /* No dedicated music socket - fall back to main socket queue */
      pthread_mutex_unlock(&state->write_mutex);
      return queue_music_data(state->conn, data, len);
   }

   /* Check if previous frame is still pending */
   if (state->write_pending_len > 0) {
      /* Drop this frame - backpressure */
      pthread_mutex_unlock(&state->write_mutex);
      return 0;
   }

   /* Buffer the frame for writeable callback.
    * Format: [type_byte][data...]
    * The data already contains length prefix + opus frame. */
   size_t total_len = 1 + len;
   if (total_len > sizeof(state->write_buffer) - LWS_PRE) {
      pthread_mutex_unlock(&state->write_mutex);
      return -1;
   }

   state->write_buffer[LWS_PRE] = WS_BIN_MUSIC_DATA;
   memcpy(state->write_buffer + LWS_PRE + 1, data, len);
   state->write_pending_len = total_len;

   /* Request writeable callback */
   lws_callback_on_writable(state->music_wsi);

   /* Wake up the music server's event loop to process the writeable request */
   webui_music_server_wake();

   pthread_mutex_unlock(&state->write_mutex);
   return 0;
}
