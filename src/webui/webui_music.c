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
 * WebUI Music Streaming - Core
 *
 * Streaming engine, lifecycle, state communication, and shared utilities.
 * Message handlers are in webui_music_handlers.c.
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
#include <time.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/plex_client.h"
#include "audio/resampler.h"
#include "config/dawn_config.h"
#include "core/path_utils.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_music_queue_db.h"
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

/* Types (music_queue_entry_t, session_music_state_t, user_music_queue_t)
 * are in webui_music_internal.h */

/**
 * @brief Pick a random queue index different from the current one.
 *
 * Uses rand_r() with the provided seed for thread-safety.
 */
int webui_music_pick_random_index(int current_index, int queue_length, unsigned int *shuffle_seed) {
   if (queue_length <= 1)
      return current_index;
   int idx;
   do {
      idx = rand_r(shuffle_seed) % queue_length;
   } while (idx == current_index);
   return idx;
}

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
 * Shared User Queue Registry
 *
 * Authenticated users share a single queue across all their browser tabs.
 * Unauthenticated/satellite sessions get a per-connection queue (not shared).
 * ============================================================================= */

#define MAX_USER_QUEUES 32
static user_music_queue_t *s_user_queues[MAX_USER_QUEUES];
static int s_user_queue_count = 0;
static pthread_mutex_t s_user_queues_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Serializes the music-server thread's access to conn->music_state (report_buffer,
 * write_pending, set_stream_wsi) against webui_music_session_cleanup(), which frees
 * the state on the main WS thread. Without it, a dedicated-socket callback can
 * dereference music_state in the window between free() and the conn->music_state=NULL
 * store (use-after-free on ordinary tab close; hot on the ~32ms buffer-report path).
 * Lock order: this is acquired BEFORE state->write_mutex, never the reverse. */
static pthread_mutex_t s_music_teardown_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Find or create a shared queue for an authenticated user
 *
 * @param user_id Must be > 0 (authenticated). Returns NULL for <= 0.
 * @return Shared queue with ref_count incremented, or NULL on failure.
 */
static user_music_queue_t *find_or_create_user_queue(int user_id) {
   if (user_id <= 0) {
      return NULL;
   }

   pthread_mutex_lock(&s_user_queues_mutex);

   /* Look for existing queue */
   for (int i = 0; i < s_user_queue_count; i++) {
      if (s_user_queues[i] && s_user_queues[i]->user_id == user_id) {
         s_user_queues[i]->ref_count++;
         pthread_mutex_unlock(&s_user_queues_mutex);
         return s_user_queues[i];
      }
   }

   /* Not found — try to create new, evicting zero-refcount entries if full */
   if (s_user_queue_count >= MAX_USER_QUEUES) {
      /* Evict first zero-refcount entry to make room */
      bool evicted = false;
      for (int i = 0; i < s_user_queue_count; i++) {
         if (s_user_queues[i] && s_user_queues[i]->ref_count == 0) {
            OLOG_INFO("WebUI music: Evicting idle queue for user %d", s_user_queues[i]->user_id);
            pthread_mutex_destroy(&s_user_queues[i]->queue_mutex);
            free(s_user_queues[i]);
            /* Compact array */
            for (int j = i; j < s_user_queue_count - 1; j++) {
               s_user_queues[j] = s_user_queues[j + 1];
            }
            s_user_queues[--s_user_queue_count] = NULL;
            evicted = true;
            break;
         }
      }
      if (!evicted) {
         OLOG_ERROR("WebUI music: User queue registry full (%d), all in use", MAX_USER_QUEUES);
         pthread_mutex_unlock(&s_user_queues_mutex);
         return NULL;
      }
   }

   user_music_queue_t *uq = calloc(1, sizeof(user_music_queue_t));
   if (!uq) {
      pthread_mutex_unlock(&s_user_queues_mutex);
      return NULL;
   }

   uq->user_id = user_id;
   pthread_mutex_init(&uq->queue_mutex, NULL);
   uq->ref_count = 1;
   uq->generation = 0;

   /* Restore from DB */
   music_queue_db_load(user_id, uq);

   s_user_queues[s_user_queue_count++] = uq;
   pthread_mutex_unlock(&s_user_queues_mutex);

   OLOG_INFO("WebUI music: Created shared queue for user %d (%d tracks restored)", user_id,
             uq->queue_length);
   return uq;
}

/**
 * @brief Release a reference to a shared user queue
 */
static void release_user_queue(user_music_queue_t *uq) {
   if (!uq) {
      return;
   }
   pthread_mutex_lock(&s_user_queues_mutex);
   uq->ref_count--;
   pthread_mutex_unlock(&s_user_queues_mutex);
   /* Queue stays alive in registry for next connection */
}

/**
 * @brief Create a per-connection (non-shared) queue for unauthenticated sessions
 */
static user_music_queue_t *create_private_queue(void) {
   user_music_queue_t *uq = calloc(1, sizeof(user_music_queue_t));
   if (!uq) {
      return NULL;
   }
   uq->user_id = 0;
   pthread_mutex_init(&uq->queue_mutex, NULL);
   uq->ref_count = 1;
   uq->generation = 0;
   return uq;
}

/**
 * @brief Free all shared user queues (called from webui_music_cleanup)
 */
static void cleanup_all_user_queues(void) {
   pthread_mutex_lock(&s_user_queues_mutex);
   for (int i = 0; i < s_user_queue_count; i++) {
      if (s_user_queues[i]) {
         pthread_mutex_destroy(&s_user_queues[i]->queue_mutex);
         free(s_user_queues[i]);
         s_user_queues[i] = NULL;
      }
   }
   s_user_queue_count = 0;
   pthread_mutex_unlock(&s_user_queues_mutex);
}

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

/* queue_music_direct() return value when a frame is dropped because the dedicated
 * send ring was full — distinct from SUCCESS(0)/FAILURE(1) so the closed-loop pacer
 * does not credit its buffer estimate for a frame the client never received. */
#define WEBUI_MUSIC_QUEUE_DROPPED 2

/* Forward declarations */
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
bool wait_decoder_idle(session_music_state_t *state, int timeout_ms) {
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
      OLOG_WARNING("WebUI music: Timeout waiting for decoder to become idle");
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

   /* Plex paths: validate prefix, source config, and Part.key format */
   if (strncmp(path, "plex:", 5) == 0) {
      if (!plex_client_is_configured()) {
         OLOG_WARNING("WebUI music: Plex path rejected (Plex not configured): %s", path);
         return false;
      }
      const char *part_key = path + 5;
      /* Validate Part.key starts with expected Plex API path */
      if (strncmp(part_key, "/library/parts/", 15) != 0 &&
          strncmp(part_key, "/library/metadata/", 18) != 0) {
         OLOG_WARNING("WebUI music: Invalid Plex part key: %s", part_key);
         return false;
      }
      /* No authority injection */
      if (strchr(part_key, '@') != NULL) {
         OLOG_WARNING("WebUI music: Authority injection in Plex path: %s", path);
         return false;
      }
      /* No query string or fragment injection */
      if (strchr(part_key, '?') != NULL || strchr(part_key, '#') != NULL) {
         OLOG_WARNING("WebUI music: Query/fragment injection in Plex path: %s", path);
         return false;
      }
      /* No path traversal in the Part.key */
      if (contains_path_traversal(part_key)) {
         OLOG_WARNING("WebUI music: Path traversal in Plex path: %s", path);
         return false;
      }
      return true;
   }

   /* Local paths: existing realpath() validation */

   /* Quick check for obvious traversal patterns */
   if (contains_path_traversal(path)) {
      OLOG_WARNING("WebUI music: Path traversal detected in: %s", path);
      return false;
   }

   /* Check if music directory is configured */
   if (g_config.paths.music_dir[0] == '\0') {
      OLOG_WARNING("WebUI music: No music directory configured");
      return false;
   }

   char resolved_path[PATH_MAX];
   char resolved_music_dir[PATH_MAX];

   /* Resolve the music library base path */
   if (realpath(g_config.paths.music_dir, resolved_music_dir) == NULL) {
      OLOG_ERROR("WebUI music: Cannot resolve music directory: %s", g_config.paths.music_dir);
      return false;
   }

   /* Resolve the requested file path */
   if (realpath(path, resolved_path) == NULL) {
      /* File doesn't exist - not valid for playback */
      OLOG_WARNING("WebUI music: Cannot resolve path: %s", path);
      return false;
   }

   /* Ensure resolved path starts with resolved music directory */
   size_t music_dir_len = strlen(resolved_music_dir);
   if (strncmp(resolved_path, resolved_music_dir, music_dir_len) != 0) {
      OLOG_WARNING("WebUI music: Path outside music library: %s", path);
      return false;
   }

   /* Ensure it's either exact match or followed by '/' */
   if (resolved_path[music_dir_len] != '\0' && resolved_path[music_dir_len] != '/') {
      OLOG_WARNING("WebUI music: Path prefix mismatch: %s", path);
      return false;
   }

   return true;
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
      OLOG_ERROR("WebUI music: Failed to create Opus encoder: %s", opus_strerror(err));
      return 1;
   }

   /* Configure for quality tier */
   opus_encoder_ctl(state->encoder, OPUS_SET_BITRATE(QUALITY_BITRATES[quality]));
   opus_encoder_ctl(state->encoder, OPUS_SET_COMPLEXITY(QUALITY_COMPLEXITY[quality]));
   opus_encoder_ctl(state->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

   /* VBR or CBR */
   opus_encoder_ctl(state->encoder, OPUS_SET_VBR(state->bitrate_mode == MUSIC_BITRATE_VBR ? 1 : 0));

   state->quality = quality;
   OLOG_INFO("WebUI music: Encoder configured for %s quality (%d kbps)", QUALITY_NAMES[quality],
             QUALITY_BITRATES[quality] / 1000);

   return 0;
}

/**
 * @brief Send music state update to client
 *
 * Manages its own locking internally. Locks queue_mutex briefly to snapshot
 * shared fields, then locks state_mutex for per-session fields.
 * Callers do NOT need to hold any lock.
 * Lock hierarchy: queue_mutex → state_mutex (never reversed).
 */
static void webui_music_send_state_impl(ws_connection_t *conn,
                                        session_music_state_t *state,
                                        bool advance_auto) {
   user_music_queue_t *uq = state->shared_queue;
   if (!uq) {
      return;
   }

   /* Snapshot shared queue fields under queue_mutex */
   int queue_length;
   int queue_index;
   bool shuffle;
   music_repeat_mode_t repeat_mode;
   music_queue_entry_t current_track;
   bool has_track = false;

   pthread_mutex_lock(&uq->queue_mutex);
   queue_length = uq->queue_length;
   shuffle = uq->shuffle;
   repeat_mode = uq->repeat_mode;
   pthread_mutex_unlock(&uq->queue_mutex);

   /* Snapshot per-session fields under state_mutex */
   pthread_mutex_lock(&state->state_mutex);
   queue_index = state->queue_index;
   bool playing = state->playing;
   bool paused = state->paused;
   double position_sec = 0.0;
   if (state->source_rate > 0) {
      position_sec = (double)state->position_frames / state->source_rate;
   }
   audio_format_type_t source_format = state->source_format;
   uint32_t source_rate = state->source_rate;
   music_quality_t quality = state->quality;
   music_bitrate_mode_t bitrate_mode = state->bitrate_mode;
   pthread_mutex_unlock(&state->state_mutex);

   /* Read track info under queue_mutex (needs index from state) */
   pthread_mutex_lock(&uq->queue_mutex);
   if (queue_length > 0 && queue_index < uq->queue_length) {
      current_track = uq->queue[queue_index];
      has_track = true;
   }
   pthread_mutex_unlock(&uq->queue_mutex);

   /* Build JSON (no locks held) */
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_state"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "playing", json_object_new_boolean(playing));
   json_object_object_add(payload, "paused", json_object_new_boolean(paused));

   /* Tag a natural end-of-track advance so the client keeps its buffer (gapless):
    * with the server running a lead, EOF fires ~2s before the user hears the
    * boundary, and the client's default track-change/seek flush would truncate
    * that lead. User-initiated skips are NOT tagged, so they still flush. */
   if (advance_auto) {
      json_object_object_add(payload, "advance", json_object_new_string("auto"));
   }

   if (has_track) {
      struct json_object *track_obj = json_object_new_object();
      json_object_object_add(track_obj, "path", json_object_new_string(current_track.path));
      json_object_object_add(track_obj, "title", json_object_new_string(current_track.title));
      json_object_object_add(track_obj, "artist", json_object_new_string(current_track.artist));
      json_object_object_add(track_obj, "album", json_object_new_string(current_track.album));
      json_object_object_add(track_obj, "duration_sec",
                             json_object_new_int(current_track.duration_sec));
      json_object_object_add(payload, "track", track_obj);
   } else {
      json_object_object_add(payload, "track", NULL);
   }

   json_object_object_add(payload, "position_sec", json_object_new_double(position_sec));
   json_object_object_add(payload, "queue_length", json_object_new_int(queue_length));
   json_object_object_add(payload, "queue_index", json_object_new_int(queue_index));

   json_object_object_add(payload, "source_format",
                          json_object_new_string(audio_decoder_format_name(source_format)));
   json_object_object_add(payload, "source_rate", json_object_new_int(source_rate));

   json_object_object_add(payload, "quality", json_object_new_string(QUALITY_NAMES[quality]));
   json_object_object_add(payload, "bitrate", json_object_new_int(QUALITY_BITRATES[quality]));
   json_object_object_add(payload, "bitrate_mode",
                          json_object_new_string(bitrate_mode == MUSIC_BITRATE_VBR ? "vbr"
                                                                                   : "cbr"));

   json_object_object_add(payload, "shuffle", json_object_new_boolean(shuffle));
   json_object_object_add(payload, "repeat_mode", json_object_new_int(repeat_mode));

   json_object_object_add(payload, "volume", json_object_new_double(conn->volume));

   json_object_object_add(response, "payload", payload);

   const char *json_str = json_object_to_json_string(response);
   char *json_copy = strdup(json_str);
   if (!json_copy) {
      OLOG_ERROR("WebUI music: strdup failed for state update");
      json_object_put(response);
      return;
   }
   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_MUSIC_STATE,
                          .music_json = { .json = json_copy } };
   queue_response(&resp);

   json_object_put(response);
}

void webui_music_send_state(ws_connection_t *conn, session_music_state_t *state) {
   webui_music_send_state_impl(conn, state, false);
}

/**
 * @brief Queue position update for client (thread-safe)
 *
 * Uses the response queue instead of direct lws_write() to be safe
 * for calling from the streaming thread.
 */
static void send_position_update(ws_connection_t *conn, session_music_state_t *state) {
   double position_sec = 0.0;
   uint32_t duration_sec = state->cached_duration_sec;

   if (state->source_rate > 0) {
      position_sec = (double)state->position_frames / state->source_rate;
   }

   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_MUSIC_POSITION,
                          .music_position = { .position_sec = position_sec,
                                              .duration_sec = duration_sec } };

   queue_response(&resp);
}

/**
 * @brief Send music error to client (thread-safe)
 *
 * Uses the response queue — safe to call from any thread.
 */
void webui_music_send_error(ws_connection_t *conn, const char *code, const char *message) {
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_error"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "code", json_object_new_string(code));
   json_object_object_add(payload, "message", json_object_new_string(message));

   json_object_object_add(response, "payload", payload);

   const char *json_str = json_object_to_json_string(response);
   char *json_copy = strdup(json_str);
   if (!json_copy) {
      OLOG_ERROR("WebUI music: strdup failed for error message");
      json_object_put(response);
      return;
   }
   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_MUSIC_ERROR,
                          .music_json = { .json = json_copy } };
   queue_response(&resp);

   json_object_put(response);
}

/* =============================================================================
 * Broadcast
 * ============================================================================= */

/**
 * @brief Broadcast queue state to all tabs for the same user
 *
 * Collects matching connections under conn_registry_mutex, releases it,
 * then sends state to each. This avoids holding conn_registry_mutex
 * while send_state acquires queue_mutex (would invert lock hierarchy).
 */
void webui_music_broadcast_queue_state(user_music_queue_t *uq, ws_connection_t *exclude) {
   if (!uq || uq->user_id <= 0) {
      return; /* Private queues don't broadcast */
   }

   /* Collect connections outside the send loop */
   ws_connection_t *conns[MAX_USER_QUEUES * 4]; /* Generous upper bound */
   int count = webui_collect_conns_by_user(uq->user_id, conns,
                                           (int)(sizeof(conns) / sizeof(conns[0])));

   for (int i = 0; i < count && i < (int)(sizeof(conns) / sizeof(conns[0])); i++) {
      if (conns[i] != exclude) {
         session_music_state_t *s = (session_music_state_t *)conns[i]->music_state;
         if (s) {
            webui_music_send_state(conns[i], s);
         }
      }
   }
}

/* =============================================================================
 * Streaming Thread
 * ============================================================================= */

/**
 * @brief Music streaming thread
 *
 * Reads audio from decoder, resamples to 48kHz, encodes to Opus,
 * and sends to client via WebSocket.
 *
 * NOTE: This runs on its own thread, NOT the LWS service thread.
 * All WebSocket sends must use thread-safe paths: queue_response(),
 * webui_music_send_state(), webui_music_send_error(), or
 * queue_music_direct(). Never call send_json_response() or
 * lws_write() directly from here.
 */
static void *music_stream_thread(void *arg) {
   session_music_state_t *state = (session_music_state_t *)arg;
   ws_connection_t *conn = state->conn;

   OLOG_INFO("WebUI music: Streaming thread started");
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
   bool was_sending = false; /* Diagnostic: track state transitions for logging */

   /* Closed-loop flow control: hold the client worklet buffer near
    * WEBUI_MUSIC_TARGET_BUFFER_MS using periodic client reports, so a TTS-induced
    * CPU preemption can't drain a near-empty buffer into silence. Estimate is
    * bookkept (+frame on send, -sleep as the client drains) and re-anchored to
    * each fresh report; falls back to the real-time pacer above when reports go
    * stale (old clients, or a client whose report sender stalled). */
   uint64_t est_buffered_us = 0;     /* server's estimate of client buffer depth */
   uint64_t last_seen_report_ms = 0; /* last report timestamp already consumed */
   bool was_closed_loop = false;     /* were we closed-loop last frame? (fallback re-baseline) */
   unsigned pace_log_counter = 0;    /* throttle for the periodic pacing log */

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
            OLOG_INFO("WebUI music: Reconfigured encoder to %s %s", QUALITY_NAMES[new_quality],
                      new_bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");
         }
      }

      /* Brief lock to check state and mark decoder busy */
      pthread_mutex_lock(&state->state_mutex);
      if (!state->decoder || !state->playing || state->paused) {
         if (was_sending) {
            OLOG_INFO("WebUI music: Streaming paused (decoder=%p playing=%d paused=%d)",
                      (void *)state->decoder, state->playing, state->paused);
            was_sending = false;
         }
         pthread_mutex_unlock(&state->state_mutex);
         usleep(50000); /* 50ms idle sleep */
         continue;
      }
      if (!was_sending) {
         OLOG_INFO("WebUI music: Streaming resumed");
         was_sending = true;
         /* Reset pacing so we don't fast-forward after a pause */
         stream_start_time = 0;
         frames_sent = 0;
         state->resample_accum_count = 0;
         /* Resume is a client-flush boundary (the client clears its worklet on
          * resume), so the client buffer is empty — reset the estimate to refill
          * from scratch. (Natural auto-advance, by contrast, is gapless and must
          * NOT reset the estimate; it stays continuous across the boundary.) */
         est_buffered_us = 0;
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
         OLOG_INFO("WebUI music: Track finished (read returned %zd)", frames_read);

         audio_decoder_close(state->decoder);
         state->decoder = NULL;
         pthread_mutex_unlock(&state->state_mutex);

         /* Read shared queue fields under queue_mutex */
         user_music_queue_t *uq = state->shared_queue;
         char next_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
         uint32_t next_duration = 0;
         int next_index = state->queue_index;

         pthread_mutex_lock(&uq->queue_mutex);
         uint32_t saved_gen = uq->generation;
         bool q_shuffle = uq->shuffle;
         music_repeat_mode_t q_repeat = uq->repeat_mode;
         int q_len = uq->queue_length;

         /* Advance to next track based on shuffle/repeat mode */
         if (q_repeat == MUSIC_REPEAT_ONE) {
            /* Repeat one — replay same track (index stays) */
         } else if (q_shuffle) {
            if (q_len > 1)
               next_index = webui_music_pick_random_index(next_index, q_len, &state->shuffle_seed);
         } else {
            next_index++;
            if (next_index >= q_len) {
               if (q_repeat == MUSIC_REPEAT_ALL) {
                  next_index = 0;
               } else {
                  /* No repeat — stop */
                  pthread_mutex_unlock(&uq->queue_mutex);
                  pthread_mutex_lock(&state->state_mutex);
                  state->playing = false;
                  state->queue_index = 0;
                  pthread_mutex_unlock(&state->state_mutex);
                  webui_music_send_state(conn, state);
                  continue;
               }
            }
         }

         /* Copy next track path and duration */
         if (next_index >= 0 && next_index < q_len) {
            snprintf(next_path, sizeof(next_path), "%s", uq->queue[next_index].path);
            next_duration = uq->queue[next_index].duration_sec;
         }
         pthread_mutex_unlock(&uq->queue_mutex);

         /* Re-lock state_mutex and validate generation */
         pthread_mutex_lock(&state->state_mutex);
         if (atomic_load(&state->stop_requested)) {
            pthread_mutex_unlock(&state->state_mutex);
            continue;
         }

         /* Check if queue was mutated while we were unlocked */
         pthread_mutex_lock(&uq->queue_mutex);
         if (uq->generation != saved_gen) {
            /* Queue changed — re-read */
            q_len = uq->queue_length;
            if (q_len == 0) {
               pthread_mutex_unlock(&uq->queue_mutex);
               state->playing = false;
               state->queue_index = 0;
               pthread_mutex_unlock(&state->state_mutex);
               webui_music_send_state(conn, state);
               continue;
            }
            if (next_index >= q_len) {
               next_index = 0;
            }
            snprintf(next_path, sizeof(next_path), "%s", uq->queue[next_index].path);
            next_duration = uq->queue[next_index].duration_sec;
         }
         pthread_mutex_unlock(&uq->queue_mutex);

         state->queue_index = next_index;
         state->cached_duration_sec = next_duration;

         if (next_path[0] == '\0') {
            state->playing = false;
            pthread_mutex_unlock(&state->state_mutex);
            webui_music_send_state(conn, state);
            continue;
         }

         /* Open next track */
         state->decoder = audio_decoder_open(next_path);
         if (!state->decoder) {
            OLOG_ERROR("WebUI music: Failed to open next track: %s", next_path);
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

         /* Send state update for new track — tag as a natural auto-advance so the
          * client keeps its buffered lead (gapless) instead of flushing. */
         webui_music_send_state_impl(conn, state, true);
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
         OLOG_WARNING("WebUI music: Resample accumulator overflow, dropping samples");
      }
      pthread_mutex_unlock(&state->state_mutex);

      /* Encode and send while we have enough samples (960 stereo frames = 1920 samples) */
      while (state->resample_accum_count >= WEBUI_MUSIC_FRAME_SAMPLES * 2) {
         /* Check pause/stop in inner loop too — the outer loop only checks at the
          * top of each iteration, and audio_decoder_read + resampling may accumulate
          * enough samples for many frames between checks. */
         if (state->paused || !state->playing || atomic_load(&state->stop_requested))
            break;

         /* Initialize stream timing on first frame. NOTE: est_buffered_us is
          * deliberately NOT reset here — a brand-new thread (fresh play / seek /
          * user skip) starts with est=0 as a thread local, while a natural
          * auto-advance reaches this point on the SAME thread and must keep its
          * estimate (the buffered lead is continuous across the gapless boundary).
          * The only in-thread flush boundary, resume-from-pause, resets est above. */
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
            OLOG_WARNING("WebUI music: Opus encode error: %s", opus_strerror(opus_bytes));
            break;
         }

         /* Prepend length prefix (little-endian) */
         opus_buffer[0] = opus_bytes & 0xFF;
         opus_buffer[1] = (opus_bytes >> 8) & 0xFF;

         /* Send to client — dedicated music socket if available, else main socket.
          * A WEBUI_MUSIC_QUEUE_DROPPED return means the send ring was full, so we
          * must NOT credit the estimate for a frame the client never received. */
         int qrc = queue_music_direct(state, opus_buffer, opus_bytes + 2);
         frames_sent++;

         /* ---- Closed-loop pacing (see the est_buffered_us notes above) ----------
          * Hold the client buffer near the target depth. Bookkeep the estimate
          * (+frame queued, -sleep as the client plays it back in real time) and
          * re-anchor to each fresh client report; fall back to the real-time pacer
          * when reports are stale. */
         if (qrc == SUCCESS) {
            est_buffered_us += frame_duration_us;
         }

         uint64_t now_ms = get_time_ms();
         uint64_t rts = atomic_load(&state->last_buffer_report_ms);
         bool report_fresh = (rts != 0) && ((now_ms - rts) <= WEBUI_MUSIC_REPORT_STALE_MS);
         if (report_fresh && rts != last_seen_report_ms) {
            est_buffered_us = (uint64_t)atomic_load(&state->client_buffered_ms) * 1000ULL;
            last_seen_report_ms = rts;
         }

         if (report_fresh) {
            was_closed_loop = true;
            uint64_t target_us = (uint64_t)WEBUI_MUSIC_TARGET_BUFFER_MS * 1000ULL;
            if (est_buffered_us > target_us) {
               /* At/above target: pace so the client drains back toward it. */
               uint64_t sleep_us = est_buffered_us - target_us;
               if (sleep_us > WEBUI_MUSIC_MAX_PACE_SLEEP_US) {
                  sleep_us = WEBUI_MUSIC_MAX_PACE_SLEEP_US;
               }
               if (sleep_us >= 1000) {
                  usleep((useconds_t)sleep_us);
                  est_buffered_us -= sleep_us; /* wall time the client drained */
               }
            } else {
               /* Below target: refill faster than real time, but gate on the send
                * ring so the burst can't overflow the 8-slot ring (dropped frames
                * would defeat the refill). */
               uint64_t sleep_us = (state->write_ring_count >= WEBUI_MUSIC_REFILL_RING_HIGH)
                                       ? frame_duration_us      /* ring backing up: ~1x */
                                       : frame_duration_us / 4; /* headroom: ~4x fill */
               usleep((useconds_t)sleep_us);
               est_buffered_us = (est_buffered_us > sleep_us) ? (est_buffered_us - sleep_us) : 0;
            }

            if ((++pace_log_counter % 250) == 0) {
               OLOG_INFO("WebUI music: buffer est=%llums target=%dms ring=%d/%d",
                         (unsigned long long)(est_buffered_us / 1000), WEBUI_MUSIC_TARGET_BUFFER_MS,
                         state->write_ring_count, WEBUI_MUSIC_WRITE_RING);
            }
         } else {
            /* No fresh report: fall back to the real-time pacer. On the transition
             * out of closed-loop, re-baseline its clock to NOW — otherwise an
             * accumulated lead in frames_sent makes expected_time race ahead of
             * wall-clock and the sanity clamp skips the sleep entirely, blasting
             * the ring unpaced (the exact stutter this feature removes). */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t current_time = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
            if (was_closed_loop) {
               stream_start_time = current_time - (frames_sent * frame_duration_us);
               was_closed_loop = false;
            }
            uint64_t expected_time = stream_start_time + (frames_sent * frame_duration_us);
            if (current_time < expected_time) {
               uint64_t sleep_us = expected_time - current_time;
               if (sleep_us > 1000 && sleep_us < 100000) { /* Sanity check: 1ms - 100ms */
                  usleep((useconds_t)sleep_us);
               }
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

   OLOG_INFO("WebUI music: Streaming thread stopped");
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
int webui_music_start_streaming(session_music_state_t *state) {
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
      OLOG_ERROR("WebUI music: Failed to create streaming thread");
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

   /* Clean up any previous temp file */
   if (state->temp_file[0]) {
      unlink(state->temp_file);
      state->temp_file[0] = '\0';
   }

   /* Determine the local file path to open */
   char local_path[PATH_MAX];

   if (strncmp(path, "plex:", 5) == 0) {
      /* Plex track: download to temp file first.
       * Note: this blocks the caller (WebSocket handler thread) during download.
       * Acceptable for home LAN latency; move to worker thread if needed. */
      const char *part_key = path + 5;
      if (plex_client_download_track(part_key, local_path, sizeof(local_path)) != 0) {
         pthread_mutex_unlock(&state->state_mutex);
         OLOG_ERROR("WebUI music: Plex download failed for: %s", path);
         return 1;
      }
      /* Remember temp file for cleanup */
      strncpy(state->temp_file, local_path, sizeof(state->temp_file) - 1);
      state->temp_file[sizeof(state->temp_file) - 1] = '\0';
   } else {
      strncpy(local_path, path, sizeof(local_path) - 1);
      local_path[sizeof(local_path) - 1] = '\0';
   }

   /* Open the file (always a local path at this point) */
   state->decoder = audio_decoder_open(local_path);
   if (!state->decoder) {
      pthread_mutex_unlock(&state->state_mutex);
      OLOG_ERROR("WebUI music: Failed to open: %s", path);
      /* Clean up temp file on failure */
      if (state->temp_file[0]) {
         unlink(state->temp_file);
         state->temp_file[0] = '\0';
      }
      return 1;
   }

   /* Unlink temp file now that decoder has it open (Unix fd trick —
    * file stays accessible via fd until decoder closes, auto-cleaned on crash) */
   if (state->temp_file[0]) {
      unlink(state->temp_file);
      /* Keep the path in temp_file as a flag that this is a Plex track,
       * but the file is already unlinked */
   }

   /* Get audio info */
   audio_decoder_info_t info;
   audio_decoder_get_info(state->decoder, &info);
   state->source_rate = info.sample_rate;
   state->source_channels = info.channels;
   state->source_format = info.format;
   state->position_frames = 0;

   OLOG_INFO("WebUI music: Playing %s (%s %d Hz, %d ch)", path,
             audio_decoder_format_name(info.format), info.sample_rate, info.channels);

   /* Create resampler if needed */
   if (state->resampler) {
      resampler_destroy(state->resampler);
   }
   if (info.sample_rate != OPUS_SAMPLE_RATE) {
      state->resampler = resampler_create(info.sample_rate, OPUS_SAMPLE_RATE, info.channels);
      if (!state->resampler) {
         OLOG_ERROR("WebUI music: Failed to create resampler");
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
   int qidx = state->queue_index;
   user_music_queue_t *uq = state->shared_queue;
   pthread_mutex_unlock(&state->state_mutex);

   /* Cache the track duration for the 1 Hz position updates. Do NOT hold
    * state_mutex while taking queue_mutex — the documented lock order is
    * queue_mutex → state_mutex, so holding state here and then taking queue
    * (state → queue) inverts it and can deadlock against paths that correctly
    * nest queue → state (e.g. next/previous). Snapshot the index under state
    * above, read the duration under queue alone, then store it under state. */
   if (uq) {
      bool have_dur = false;
      uint32_t dur = 0;
      pthread_mutex_lock(&uq->queue_mutex);
      if (qidx >= 0 && qidx < uq->queue_length) {
         dur = uq->queue[qidx].duration_sec;
         have_dur = true;
      }
      pthread_mutex_unlock(&uq->queue_mutex);
      if (have_dur) {
         pthread_mutex_lock(&state->state_mutex);
         state->cached_duration_sec = dur;
         pthread_mutex_unlock(&state->state_mutex);
      }
   }

   /* Start streaming thread if not already running */
   return webui_music_start_streaming(state);
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
      OLOG_WARNING("WebUI music: Music database not initialized - library features unavailable");
   }

   /* Initialize queue persistence DB */
   {
      char queue_db_path[512];
      snprintf(queue_db_path, sizeof(queue_db_path), "%s/music.db", g_config.paths.data_dir);
      if (music_queue_db_init(queue_db_path) != 0) {
         OLOG_WARNING("WebUI music: Queue DB init failed — queue persistence unavailable");
      }
   }

   /* Initialize Plex client if configured (for download/scrobble support) */
   if (plex_client_is_configured()) {
      if (plex_client_init() != 0) {
         OLOG_WARNING("WebUI music: Plex client init failed — Plex features unavailable");
      }
   }

   s_initialized = true;
   OLOG_INFO("WebUI music streaming initialized (default quality: %s, bitrate: %s)",
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

   /* Clean up shared user queues */
   cleanup_all_user_queues();

   /* Clean up queue persistence DB */
   music_queue_db_cleanup();

   /* Clean up Plex client */
   plex_client_cleanup();

   s_initialized = false;
   OLOG_INFO("WebUI music streaming cleaned up");

   pthread_mutex_unlock(&s_music_mutex);
}

bool webui_music_is_available(void) {
   return s_initialized && s_config.enabled;
}

int webui_music_session_init(ws_connection_t *conn) {
   session_music_state_t *state = calloc(1, sizeof(session_music_state_t));
   if (!state) {
      OLOG_ERROR("WebUI music: Failed to allocate session state");
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
   state->shuffle_seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)state;
   state->music_wsi = NULL;
   state->write_ring_head = 0;
   state->write_ring_tail = 0;
   state->write_ring_count = 0;
   state->write_drop_count = 0;

   /* Get or create shared queue */
   user_music_queue_t *uq = find_or_create_user_queue(conn->auth_user_id);
   if (!uq) {
      /* Unauthenticated or satellite — create private queue */
      uq = create_private_queue();
      if (!uq) {
         OLOG_ERROR("WebUI music: Failed to allocate private queue");
         pthread_mutex_destroy(&state->state_mutex);
         pthread_mutex_destroy(&state->write_mutex);
         free(state);
         return 1;
      }
   }
   state->shared_queue = uq;
   state->queue_index = 0;

   /* Allocate resampling accumulation buffer
    * Size: enough for ~100ms of stereo audio at 48kHz = 4800 * 2 = 9600 samples */
   state->resample_accum_size = 48000 / 10 * 2; /* 100ms stereo */
   state->resample_accum = malloc(state->resample_accum_size * sizeof(int16_t));
   if (!state->resample_accum) {
      OLOG_ERROR("WebUI music: Failed to allocate resample buffer");
      release_user_queue(state->shared_queue);
      pthread_mutex_destroy(&state->state_mutex);
      pthread_mutex_destroy(&state->write_mutex);
      free(state);
      return 1;
   }
   state->resample_accum_count = 0;

   /* Create encoder */
   if (webui_music_configure_encoder(state, state->quality) != 0) {
      free(state->resample_accum);
      release_user_queue(state->shared_queue);
      pthread_mutex_destroy(&state->state_mutex);
      pthread_mutex_destroy(&state->write_mutex);
      free(state);
      return 1;
   }

   conn->music_state = state;
   OLOG_INFO("WebUI music: Session initialized (user_id=%d, shared=%s)", conn->auth_user_id,
             conn->auth_user_id > 0 ? "yes" : "no");

   return 0;
}

void webui_music_session_cleanup(ws_connection_t *conn) {
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return;
   }

   /* Stop the producer thread first (it holds no teardown lock; it only touches the
    * write ring under write_mutex, which we destroy below). After this returns, the
    * only other thread that can still reach `state` is the music-server callback. */
   webui_music_stop_streaming(state);

   /* Serialize the destroy+free against the music-server accessors. Once we hold this,
    * no report_buffer/write_pending/set_stream_wsi can be mid-flight on this state, and
    * any that arrive after we NULL conn->music_state below will read NULL and bail. */
   pthread_mutex_lock(&s_music_teardown_mutex);

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

   /* Release shared queue reference */
   if (state->shared_queue) {
      if (state->shared_queue->user_id <= 0) {
         /* Private (non-shared) queue — free it */
         pthread_mutex_destroy(&state->shared_queue->queue_mutex);
         free(state->shared_queue);
      } else {
         release_user_queue(state->shared_queue);
      }
      state->shared_queue = NULL;
   }

   pthread_cond_destroy(&state->decoder_idle_cond);
   pthread_mutex_destroy(&state->state_mutex);
   pthread_mutex_destroy(&state->write_mutex);

   free(state);
   conn->music_state = NULL;

   pthread_mutex_unlock(&s_music_teardown_mutex);

   OLOG_INFO("WebUI music: Session cleaned up");
}

/* Message handlers are in webui_music_handlers.c */

/* =============================================================================
 * State Query Functions
 * ============================================================================= */

struct json_object *webui_music_get_state(ws_connection_t *conn) {
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state || !state->shared_queue) {
      return NULL;
   }

   struct json_object *result = json_object_new_object();
   user_music_queue_t *uq = state->shared_queue;

   pthread_mutex_lock(&state->state_mutex);
   json_object_object_add(result, "playing", json_object_new_boolean(state->playing));
   json_object_object_add(result, "paused", json_object_new_boolean(state->paused));
   double position_sec = 0.0;
   if (state->source_rate > 0) {
      position_sec = (double)state->position_frames / state->source_rate;
   }
   int queue_index = state->queue_index;
   music_quality_t quality = state->quality;
   pthread_mutex_unlock(&state->state_mutex);

   pthread_mutex_lock(&uq->queue_mutex);
   if (uq->queue_length > 0 && queue_index < uq->queue_length) {
      music_queue_entry_t *track = &uq->queue[queue_index];
      struct json_object *track_obj = json_object_new_object();
      json_object_object_add(track_obj, "title", json_object_new_string(track->title));
      json_object_object_add(track_obj, "artist", json_object_new_string(track->artist));
      json_object_object_add(track_obj, "album", json_object_new_string(track->album));
      json_object_object_add(track_obj, "duration_sec", json_object_new_int(track->duration_sec));
      json_object_object_add(result, "track", track_obj);
   }
   json_object_object_add(result, "queue_length", json_object_new_int(uq->queue_length));
   pthread_mutex_unlock(&uq->queue_mutex);

   json_object_object_add(result, "position_sec", json_object_new_double(position_sec));
   json_object_object_add(result, "quality", json_object_new_string(QUALITY_NAMES[quality]));

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
 * Dedicated Music Server Integration
 * ============================================================================= */

void webui_music_set_stream_wsi(session_t *session, struct lws *wsi) {
   if (!session)
      return;

   /* Guard conn->music_state against a concurrent cleanup free (see mutex comment). */
   pthread_mutex_lock(&s_music_teardown_mutex);

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   if (!conn) {
      pthread_mutex_unlock(&s_music_teardown_mutex);
      return;
   }

   /* Lazily initialize music state if needed (music stream may connect
    * before any music_subscribe/control message arrives from the client) */
   if (!conn->music_state && wsi) {
      if (webui_music_session_init(conn) != 0) {
         OLOG_ERROR("WebUI music: Failed to init session for stream wsi");
         pthread_mutex_unlock(&s_music_teardown_mutex);
         return;
      }
   }

   if (!conn->music_state) {
      pthread_mutex_unlock(&s_music_teardown_mutex);
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;

   pthread_mutex_lock(&state->write_mutex);
   state->music_wsi = wsi;
   /* Drop any stale queued frames — they belong to a previous socket. */
   state->write_ring_head = 0;
   state->write_ring_tail = 0;
   state->write_ring_count = 0;
   pthread_mutex_unlock(&state->write_mutex);

   pthread_mutex_unlock(&s_music_teardown_mutex);

   OLOG_INFO("WebUI music: %s music stream wsi for session %u", wsi ? "Set" : "Cleared",
             session->session_id);
}

void webui_music_report_buffer(session_t *session, uint32_t buffered_ms) {
   if (!session)
      return;

   /* Guard conn->music_state against a concurrent cleanup free (see mutex comment). */
   pthread_mutex_lock(&s_music_teardown_mutex);

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   session_music_state_t *state = conn ? (session_music_state_t *)conn->music_state : NULL;
   if (state) {
      /* Defensive clamp: the caller pre-clamps, but the pacer must never see a value
       * beyond the client ring even if a future/rogue caller doesn't (contract was
       * advisory-only before). */
      if (buffered_ms > WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS)
         buffered_ms = WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS;
      /* Publish value before timestamp: the streaming thread gates on a fresh
       * timestamp, so a fresh timestamp must imply an at-least-as-fresh value.
       * A one-report skew across the two atomics is harmless for a continuously
       * re-anchoring pacer. */
      atomic_store(&state->client_buffered_ms, buffered_ms);
      atomic_store(&state->last_buffer_report_ms, get_time_ms());
   }

   pthread_mutex_unlock(&s_music_teardown_mutex);
}

int webui_music_write_pending(session_t *session, struct lws *wsi) {
   if (!session)
      return FAILURE;

   /* Guard conn->music_state against a concurrent cleanup free (see mutex comment). */
   pthread_mutex_lock(&s_music_teardown_mutex);

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   session_music_state_t *state = conn ? (session_music_state_t *)conn->music_state : NULL;
   if (!state) {
      pthread_mutex_unlock(&s_music_teardown_mutex);
      return FAILURE;
   }

   pthread_mutex_lock(&state->write_mutex);

   if (state->write_ring_count == 0) {
      pthread_mutex_unlock(&state->write_mutex);
      pthread_mutex_unlock(&s_music_teardown_mutex);
      return 1; /* No data pending */
   }

   /* Write the oldest queued frame (data is already positioned after LWS_PRE).
    * lws permits exactly one lws_write per writable callback, so we drain one
    * slot here and re-arm below if more remain. */
   uint8_t *slot = state->write_ring[state->write_ring_head];
   size_t frame_len = state->write_ring_len[state->write_ring_head];
   int written = lws_write(wsi, slot + LWS_PRE, frame_len, LWS_WRITE_BINARY);

   /* Treat a short write the same as an error: lws cannot resume a partially-sent
    * binary frame, so advancing past it would put a truncated Opus frame (broken
    * length framing) on the wire — worse than a gap. Discard the whole ring rather
    * than spin on it; the producer refills and the client resyncs on the next frame. */
   if (written < 0 || (size_t)written < frame_len) {
      OLOG_WARNING("WebUI music: lws_write %s (%d/%zu bytes) — dropping send ring",
                   written < 0 ? "failed" : "short", written, frame_len);
      state->write_ring_head = 0;
      state->write_ring_tail = 0;
      state->write_ring_count = 0;
      pthread_mutex_unlock(&state->write_mutex);
      pthread_mutex_unlock(&s_music_teardown_mutex);
      /* Signal the writable callback to close: a truncated WS frame would desync
       * the client's framing, not just this Opus frame. */
      return WEBUI_MUSIC_WRITE_CLOSE;
   }

   state->write_ring_head = (state->write_ring_head + 1) % WEBUI_MUSIC_WRITE_RING;
   state->write_ring_count--;
   bool more_pending = (state->write_ring_count > 0);

   pthread_mutex_unlock(&state->write_mutex);

   /* More frames buffered — schedule another writable so the ring keeps draining. */
   if (more_pending) {
      lws_callback_on_writable(wsi);
   }

   pthread_mutex_unlock(&s_music_teardown_mutex);
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
      /* No dedicated music socket connected — drop the frame. The dedicated
       * "dawn-music" socket is the sole music transport for every client (browser
       * worker + Tier-1 satellite); a client whose dedicated socket isn't up yet
       * (startup race) or is reconnecting briefly gets no audio and resyncs once it
       * registers. Count drops so the gap is visible, not silent. Returning DROPPED
       * also keeps the pacer from crediting a frame the client never received. */
      uint64_t drops = ++state->write_drop_count;
      pthread_mutex_unlock(&state->write_mutex);
      if (drops % 50 == 1)
         OLOG_WARNING("WebUI music: no dedicated music socket connected — dropped %llu "
                      "frame(s) (client dedicated 'dawn-music' socket not registered)",
                      (unsigned long long)drops);
      return WEBUI_MUSIC_QUEUE_DROPPED;
   }

   /* Buffer the frame for writeable callback.
    * Format: [type_byte][data...]
    * The data already contains length prefix + opus frame. */
   size_t total_len = 1 + len;
   if (total_len > sizeof(state->write_ring[0]) - LWS_PRE) {
      pthread_mutex_unlock(&state->write_mutex);
      return FAILURE;
   }

   /* Ring full — the writable callback is lagging behind real-time. Drop this
    * frame (backpressure) and count it so the drops are visible (previously this
    * was a single slot that dropped silently, presenting as unexplained stutter). */
   if (state->write_ring_count >= WEBUI_MUSIC_WRITE_RING) {
      uint64_t drops = ++state->write_drop_count;
      pthread_mutex_unlock(&state->write_mutex);
      if (drops % 50 == 1) {
         OLOG_WARNING("WebUI music: dedicated-socket send ring full, dropped %llu frame(s) "
                      "(writable callback lagging — playback may stutter)",
                      (unsigned long long)drops);
      }
      return WEBUI_MUSIC_QUEUE_DROPPED;
   }

   uint8_t *slot = state->write_ring[state->write_ring_tail];
   slot[LWS_PRE] = WS_BIN_MUSIC_DATA;
   memcpy(slot + LWS_PRE + 1, data, len);
   state->write_ring_len[state->write_ring_tail] = total_len;
   state->write_ring_tail = (state->write_ring_tail + 1) % WEBUI_MUSIC_WRITE_RING;
   state->write_ring_count++;

   /* Request writeable callback */
   lws_callback_on_writable(state->music_wsi);

   /* Wake up the music server's event loop to process the writeable request */
   webui_music_server_wake();

   pthread_mutex_unlock(&state->write_mutex);
   return 0;
}
