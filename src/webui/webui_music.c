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
 * WebUI music: the music player's WebUI output (stereo 48 kHz Opus to the
 * browser's music socket, paced on its buffer reports), each connection's
 * music state and its lifecycle, and the state frames.  Message handlers are
 * in webui_music_handlers.c; the player itself is in src/audio.
 */

#include "webui/webui_music.h"

#include <json-c/json.h>
#include <libwebsockets.h>
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
#include "logging.h"
#include "tools/time_utils.h"
#include "utils/string_utils.h"
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

/* session_music_state_t is in webui_music_internal.h; the queue types are in
 * audio/music_queue.h */

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

/* Serializes the music-server thread's access to conn->music_state (report_buffer,
 * write_pending, set_stream_wsi, clear_stream_wsi) against webui_music_session_cleanup(), which
 * frees the state on the main WS thread. Without it, a dedicated-socket callback can dereference
 * music_state in the window between free() and the conn->music_state=NULL store (use-after-free on
 * ordinary tab close; hot on the ~32ms buffer-report path). Lock order: this is acquired BEFORE
 * state->write_mutex, never the reverse. */
static pthread_mutex_t s_music_teardown_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

/* queue_music_direct() return value when a frame is dropped because the dedicated
 * send ring was full — distinct from SUCCESS(0)/FAILURE(1) so the closed-loop pacer
 * does not credit its buffer estimate for a frame the client never received. */
#define WEBUI_MUSIC_QUEUE_DROPPED 2

/* Forward declarations */
static int queue_music_direct(session_music_state_t *state, const uint8_t *data, size_t len);

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
 * Reads the player through music_player_snapshot() (queue_mutex, then the
 * player's state_mutex), so callers must not hold either.
 */
void webui_music_send_state_impl(ws_connection_t *conn,
                                 session_music_state_t *state,
                                 bool advance_auto) {
   music_player_status_t st;
   music_player_snapshot(state->player, &st);
   bool has_track = st.track.path[0] != '\0';
   music_quality_t quality = state->quality;
   music_bitrate_mode_t bitrate_mode = state->bitrate_mode;

   /* Build JSON (no locks held) */
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_state"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "playing", json_object_new_boolean(st.playing));
   json_object_object_add(payload, "paused", json_object_new_boolean(st.paused));
   if (st.loading) {
      json_object_object_add(payload, "loading", json_object_new_boolean(true));
   }

   /* Tag a natural end-of-track advance so the client keeps its buffer (gapless):
    * with the server running a lead, EOF fires ~2s before the user hears the
    * boundary, and the client's default track-change/seek flush would truncate
    * that lead. User-initiated skips are NOT tagged, so they still flush. */
   if (advance_auto) {
      json_object_object_add(payload, "advance", json_object_new_string("auto"));
   }

   if (has_track) {
      struct json_object *track_obj = json_object_new_object();
      json_object_object_add(track_obj, "path", json_object_new_string(st.track.path));
      json_object_object_add(track_obj, "title", json_object_new_string(st.track.title));
      json_object_object_add(track_obj, "artist", json_object_new_string(st.track.artist));
      json_object_object_add(track_obj, "album", json_object_new_string(st.track.album));
      json_object_object_add(track_obj, "duration_sec", json_object_new_int(st.track.duration_sec));
      json_object_object_add(payload, "track", track_obj);
   } else {
      json_object_object_add(payload, "track", NULL);
   }

   json_object_object_add(payload, "position_sec", json_object_new_double(st.position_sec));
   json_object_object_add(payload, "queue_length", json_object_new_int(st.queue_length));
   json_object_object_add(payload, "queue_index", json_object_new_int(st.queue_index));

   json_object_object_add(payload, "source_format",
                          json_object_new_string(audio_decoder_format_name(st.source_format)));
   json_object_object_add(payload, "source_rate", json_object_new_int(st.source_rate));

   json_object_object_add(payload, "quality", json_object_new_string(QUALITY_NAMES[quality]));
   json_object_object_add(payload, "bitrate", json_object_new_int(QUALITY_BITRATES[quality]));
   json_object_object_add(payload, "bitrate_mode",
                          json_object_new_string(bitrate_mode == MUSIC_BITRATE_VBR ? "vbr"
                                                                                   : "cbr"));

   json_object_object_add(payload, "shuffle", json_object_new_boolean(st.shuffle));
   json_object_object_add(payload, "repeat_mode", json_object_new_int(st.repeat_mode));

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

   /* queue_response() takes ownership of the response strings (freed by free_response) */
   // NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
   json_object_put(response);
}

void webui_music_send_state(ws_connection_t *conn, session_music_state_t *state) {
   webui_music_send_state_impl(conn, state, false);
}

/**
 * @brief Queue position update for client (thread-safe)
 *
 * Uses the response queue instead of direct lws_write() to be safe
 * for calling from the player's thread.
 */
static void send_position_update(ws_connection_t *conn, const music_event_t *ev) {
   ws_response_t resp = { .session = conn->session,
                          .type = WS_RESP_MUSIC_POSITION,
                          .music_position = { .position_sec = ev->position_sec,
                                              .duration_sec = ev->duration_sec } };

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

   /* queue_response() takes ownership of the response strings (freed by free_response) */
   // NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
   json_object_put(response);
}

/* =============================================================================
 * Broadcast
 * ============================================================================= */

/**
 * @brief Broadcast queue state to all tabs for the same user
 *
 * Sends to each matching connection WHILE HOLDING s_conn_registry_mutex, so a
 * conn cannot be freed by the lws thread mid-iteration (the lws CLOSED handler's
 * unregister_connection takes this same lock). This is what makes the broadcast
 * safe when called from a NON-lws thread — the LLM tool-execution worker
 * (webui_music_tool.c) among them; the previous collect-then-send-off-lock raced
 * connection teardown and could deref a freed conn (use-after-free).
 *
 * Deadlock-safe: s_conn_registry_mutex is the OUTERMOST lock here. webui_music_
 * send_state acquires uq->queue_mutex / the player's state_mutex and ultimately
 * queue_response's s_queue_mutex, and NONE of those paths ever re-acquire the
 * registry lock (verified: webui_music.c and webui_send.c never lock it), and
 * every caller releases uq->queue_mutex before broadcasting. So registry →
 * {queue_mutex, state_mutex, s_queue_mutex} never inverts.
 */
void webui_music_broadcast_queue_state(user_music_queue_t *uq, ws_connection_t *exclude) {
   if (!uq || uq->user_id <= 0) {
      return; /* Private queues don't broadcast */
   }

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn != exclude && conn->authenticated && conn->auth_user_id == uq->user_id) {
         session_music_state_t *s = (session_music_state_t *)conn->music_state;
         if (s) {
            webui_music_send_state(conn, s);
         }
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* =============================================================================
 * WebUI output: stereo 48 kHz Opus to the browser, paced on its buffer
 *
 * These run on the player's thread, NOT the lws service thread: they send
 * only through thread-safe paths (queue_response(), queue_music_direct()),
 * never send_json_response() or lws_write().
 * ============================================================================= */

/* A read hands write() at most one Opus frame of source audio */
_Static_assert(WEBUI_MUSIC_FRAME_SAMPLES <= MUSIC_PLAYER_READ_FRAMES_MAX,
               "the player's read buffer must hold an Opus frame");

static int webui_out_track_begin(void *ctx,
                                 const audio_decoder_info_t *info,
                                 bool gapless,
                                 size_t *read_frames) {
   session_music_state_t *state = (session_music_state_t *)ctx;
   /* Below the minimum one Opus frame needs more source frames than a read
    * buffer holds */
   if (info->sample_rate < WEBUI_MUSIC_MIN_SOURCE_RATE ||
       info->sample_rate > WEBUI_MUSIC_MAX_SOURCE_RATE) {
      return FAILURE;
   }
   /* The resampler always runs stereo: mono is expanded before it */
   if (state->resampler) {
      resampler_destroy(state->resampler);
      state->resampler = NULL;
   }
   if (info->sample_rate != OPUS_SAMPLE_RATE) {
      state->resampler = resampler_create(info->sample_rate, OPUS_SAMPLE_RATE, 2);
      if (!state->resampler) {
         OLOG_ERROR("WebUI music: Failed to create resampler");
         return FAILURE;
      }
   }
   /* About one Opus frame's worth of source audio: below 48 kHz a full frame
    * of source would resample to more than the buffers hold */
   *read_frames = WEBUI_MUSIC_FRAME_SAMPLES;
   if (info->sample_rate < OPUS_SAMPLE_RATE) {
      *read_frames = (size_t)WEBUI_MUSIC_FRAME_SAMPLES * info->sample_rate / OPUS_SAMPLE_RATE;
   }
   state->source_rate = info->sample_rate;
   state->source_channels = info->channels;
   state->resample_accum_count = 0;
   state->stream_start_time = 0;
   state->frames_sent = 0;
   if (!gapless) {
      state->est_buffered_us = 0; /* a natural advance keeps the buffered lead */
   }
   return SUCCESS;
}

static void webui_out_halt(void *ctx, music_halt_t kind) {
   (void)kind; /* the browser keeps or flushes its own buffer */
   session_music_state_t *state = (session_music_state_t *)ctx;
   /* Restart pacing and the estimate when audio resumes.  A client resuming
    * from a pause keeps its frozen buffer (it suspends playback rather than
    * flushing), so the first frames over-refill until its next buffer report
    * corrects the estimate; one resuming from a stop or seek has flushed. */
   state->stream_start_time = 0;
   state->frames_sent = 0;
   state->est_buffered_us = 0;
   state->was_closed_loop = false;
   state->resample_accum_count = 0;
}

/* Hold the client buffer near the target depth.  Bookkeep the estimate (+frame
 * queued, -sleep as the client plays it back in real time) and re-anchor it to
 * each fresh client report; fall back to the real-time pacer when reports are
 * stale (old clients, or a client whose report sender stalled), so a
 * TTS-induced CPU preemption can't drain a near-empty buffer into silence. */
static void pace_after_send(session_music_state_t *state, bool queued) {
   const uint64_t frame_duration_us = (WEBUI_MUSIC_FRAME_SAMPLES * 1000000ULL) / OPUS_SAMPLE_RATE;
   if (queued) {
      state->est_buffered_us += frame_duration_us;
   }

   uint64_t now_ms = get_time_ms();
   uint64_t rts = atomic_load(&state->last_buffer_report_ms);
   bool report_fresh = (rts != 0) && ((now_ms - rts) <= WEBUI_MUSIC_REPORT_STALE_MS);
   if (report_fresh && rts != state->last_seen_report_ms) {
      state->est_buffered_us = (uint64_t)atomic_load(&state->client_buffered_ms) * 1000ULL;
      state->last_seen_report_ms = rts;
   }

   if (report_fresh) {
      state->was_closed_loop = true;
      uint64_t target_us = (uint64_t)WEBUI_MUSIC_TARGET_BUFFER_MS * 1000ULL;
      if (state->est_buffered_us > target_us) {
         /* At/above target: pace so the client drains back toward it. */
         uint64_t sleep_us = state->est_buffered_us - target_us;
         if (sleep_us > WEBUI_MUSIC_MAX_PACE_SLEEP_US) {
            sleep_us = WEBUI_MUSIC_MAX_PACE_SLEEP_US;
         }
         if (sleep_us >= 1000) {
            usleep((useconds_t)sleep_us);
            state->est_buffered_us -= sleep_us; /* wall time the client drained */
         }
      } else {
         /* Below target: refill faster than real time, but gate on the send
          * ring so the burst can't overflow the 8-slot ring (dropped frames
          * would defeat the refill). */
         uint64_t sleep_us = (state->write_ring_count >= WEBUI_MUSIC_REFILL_RING_HIGH)
                                 ? frame_duration_us      /* ring backing up: ~1x */
                                 : frame_duration_us / 4; /* headroom: ~4x fill */
         usleep((useconds_t)sleep_us);
         state->est_buffered_us = (state->est_buffered_us > sleep_us)
                                      ? (state->est_buffered_us - sleep_us)
                                      : 0;
      }

      if ((++state->pace_log_counter % 250) == 0) {
         OLOG_INFO("WebUI music: buffer est=%llums target=%dms ring=%d/%d",
                   (unsigned long long)(state->est_buffered_us / 1000),
                   WEBUI_MUSIC_TARGET_BUFFER_MS, state->write_ring_count, WEBUI_MUSIC_WRITE_RING);
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
      if (state->was_closed_loop) {
         state->stream_start_time = current_time - (state->frames_sent * frame_duration_us);
         state->was_closed_loop = false;
      }
      uint64_t expected_time = state->stream_start_time + (state->frames_sent * frame_duration_us);
      if (current_time < expected_time) {
         uint64_t sleep_us = expected_time - current_time;
         if (sleep_us > 1000 && sleep_us < 100000) { /* Sanity check: 1ms - 100ms */
            usleep((useconds_t)sleep_us);
         }
      }
   }
}

static void webui_out_write(void *ctx, int16_t *pcm, size_t frames) {
   session_music_state_t *state = (session_music_state_t *)ctx;
   /* resample_buffer needs extra space for upsampling (e.g., 44.1kHz -> 48kHz
    * = 1.088x); 3x accommodates upsampling + resampler margin requirements */
   int16_t resample_buffer[WEBUI_MUSIC_FRAME_SAMPLES * 3];
   uint8_t opus_buffer[OPUS_MAX_FRAME_SIZE + 2];      /* Opus frame + length prefix */
   float float_buffer[WEBUI_MUSIC_FRAME_SAMPLES * 3]; /* Float conversion for Opus */

   /* A quality change, applied between frames */
   if (atomic_exchange(&state->reconfigure_requested, false)) {
      music_quality_t new_quality = state->pending_quality;
      music_bitrate_mode_t new_bitrate_mode = state->pending_bitrate_mode;

      state->quality = new_quality;
      state->bitrate_mode = new_bitrate_mode;
      if (webui_music_configure_encoder(state, new_quality) == 0) {
         OLOG_INFO("WebUI music: Reconfigured encoder to %s %s", QUALITY_NAMES[new_quality],
                   new_bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");
      }
   }

   /* Expand mono to stereo in place (backwards, so nothing is overwritten
    * before it's read); the buffer holds a full stereo frame. */
   size_t samples = frames * 2;
   if (state->source_channels == 1) {
      for (size_t i = frames; i-- > 0;) {
         pcm[i * 2] = pcm[i];
         pcm[i * 2 + 1] = pcm[i];
      }
   }

   /* Resample to 48kHz if needed */
   int16_t *audio_data = pcm;
   if (state->resampler && state->source_rate != OPUS_SAMPLE_RATE) {
      samples = resampler_process(state->resampler, pcm, samples, resample_buffer,
                                  sizeof(resample_buffer) / sizeof(resample_buffer[0]));
      audio_data = resample_buffer;
   }

   /* Append resampled samples to accumulation buffer */
   if (state->resample_accum_count + samples <= state->resample_accum_size) {
      memcpy(state->resample_accum + state->resample_accum_count, audio_data,
             samples * sizeof(int16_t));
      state->resample_accum_count += samples;
   } else {
      /* Buffer overflow - this shouldn't happen with proper sizing */
      OLOG_WARNING("WebUI music: Resample accumulator overflow, dropping samples");
   }

   /* Encode and send while we have enough samples (960 stereo frames = 1920 samples) */
   while (state->resample_accum_count >= WEBUI_MUSIC_FRAME_SAMPLES * 2) {
      /* A read and resample may hold enough for many frames: stop between
       * them on a pause or stop */
      if (music_player_interrupted(state->player)) {
         break;
      }

      /* Start the real-time pacer's clock on the first frame */
      if (state->stream_start_time == 0) {
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         state->stream_start_time = (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
         state->frames_sent = 0;
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
      state->frames_sent++;
      pace_after_send(state, qrc == SUCCESS);

      /* Shift remaining samples to beginning of buffer */
      size_t remaining = state->resample_accum_count - WEBUI_MUSIC_FRAME_SAMPLES * 2;
      if (remaining > 0) {
         memmove(state->resample_accum, state->resample_accum + WEBUI_MUSIC_FRAME_SAMPLES * 2,
                 remaining * sizeof(int16_t));
      }
      state->resample_accum_count = remaining;
   }
}

static void webui_out_notify(void *ctx, const music_event_t *ev) {
   session_music_state_t *state = (session_music_state_t *)ctx;
   switch (ev->type) {
      case MUSIC_EVENT_STATE:
         webui_music_send_state_impl(state->conn, state, ev->gapless);
         break;
      case MUSIC_EVENT_POSITION:
         send_position_update(state->conn, ev);
         break;
      case MUSIC_EVENT_ERROR: {
         const char *code = ev->error == MUSIC_ERROR_UNREACHABLE ? "PLEX_UNREACHABLE"
                            : ev->error == MUSIC_ERROR_PLAYBACK  ? "PLAYBACK_ERROR"
                                                                 : "DECODE_ERROR";
         webui_music_send_error(state->conn, code, ev->message);
         break;
      }
   }
}

static const music_output_ops_t s_webui_output = {
   .track_begin = webui_out_track_begin,
   .write = webui_out_write,
   .halt = webui_out_halt,
   .notify = webui_out_notify,
};

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
   while (music_player_running() > 0) {
      pthread_mutex_unlock(&s_music_mutex);
      usleep(100000); /* 100ms */
      pthread_mutex_lock(&s_music_mutex);
   }

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

   pthread_mutex_init(&state->write_mutex, NULL);
   atomic_store(&state->reconfigure_requested, false);
   state->conn = conn;
   state->quality = s_config.default_quality;
   state->bitrate_mode = s_config.bitrate_mode;
   state->music_wsi = NULL;
   state->write_ring_head = 0;
   state->write_ring_tail = 0;
   state->write_ring_count = 0;
   state->write_drop_count = 0;

   /* Allocate resampling accumulation buffer
    * Size: enough for ~100ms of stereo audio at 48kHz = 4800 * 2 = 9600 samples */
   state->resample_accum_size = 48000 / 10 * 2; /* 100ms stereo */
   state->resample_accum = malloc(state->resample_accum_size * sizeof(int16_t));
   if (!state->resample_accum) {
      OLOG_ERROR("WebUI music: Failed to allocate resample buffer");
      pthread_mutex_destroy(&state->write_mutex);
      free(state);
      return 1;
   }
   state->resample_accum_count = 0;

   /* Create encoder */
   if (webui_music_configure_encoder(state, state->quality) != 0) {
      free(state->resample_accum);
      pthread_mutex_destroy(&state->write_mutex);
      free(state);
      return 1;
   }

   /* The user's shared queue (a private one when not signed in), played by
    * this connection's player */
   user_music_queue_t *uq = music_queue_acquire(conn->auth_user_id);
   state->player = uq ? music_player_create(&s_webui_output, state, uq) : NULL;
   if (!state->player) {
      OLOG_ERROR("WebUI music: Failed to create the player");
      if (uq) {
         music_queue_release(uq);
      }
      opus_encoder_destroy(state->encoder);
      free(state->resample_accum);
      pthread_mutex_destroy(&state->write_mutex);
      free(state);
      return 1;
   }

   conn->music_state = state;
   OLOG_INFO("WebUI music: Session initialized (user_id=%d, shared=%s)", conn->auth_user_id,
             conn->auth_user_id > 0 ? "yes" : "no");

   return 0;
}

session_music_state_t *webui_music_state_get_or_init(ws_connection_t *conn) {
   /* Under the teardown lock, like the music server's lazy init, so two
    * threads can't each create a state and orphan one */
   pthread_mutex_lock(&s_music_teardown_mutex);
   if (!conn->music_state && webui_music_session_init(conn) != 0) {
      OLOG_ERROR("WebUI music: Failed to initialize session state");
   }
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   pthread_mutex_unlock(&s_music_teardown_mutex);
   return state;
}

void webui_music_session_cleanup(ws_connection_t *conn) {
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return;
   }

   /* Stop the player first, without the teardown lock: it takes its control
    * lock, which is never held with the teardown lock.  This narrows, but
    * doesn't close, the window for a tool call that read conn->music_state
    * just before this and waits for the control lock: that needs the
    * connection's lifetime pinned for the call.  Once it returns no output
    * function runs. */
   music_player_shutdown(state->player);

   /* Serialize the free against the music-server accessors. Once we hold this,
    * no report_buffer/write_pending/set_stream_wsi/clear_stream_wsi can be mid-flight on this
    * state, and any that arrive after we NULL conn->music_state below will read NULL and bail. */
   pthread_mutex_lock(&s_music_teardown_mutex);
   conn->music_state = NULL;
   music_player_destroy(state->player);

   if (state->resampler) {
      resampler_destroy(state->resampler);
   }
   if (state->encoder) {
      opus_encoder_destroy(state->encoder);
   }
   if (state->resample_accum) {
      free(state->resample_accum);
   }
   pthread_mutex_destroy(&state->write_mutex);

   free(state);

   pthread_mutex_unlock(&s_music_teardown_mutex);

   OLOG_INFO("WebUI music: Session cleaned up");
}

/* Message handlers are in webui_music_handlers.c */

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
   if (!session || !wsi)
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
   if (!conn->music_state) {
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

   OLOG_INFO("WebUI music: Set music stream wsi for session %u", session->session_id);
}

void webui_music_clear_stream_wsi(session_t *session, struct lws *wsi) {
   if (!session || !wsi)
      return;

   /* Guard conn->music_state against a concurrent cleanup free (see mutex comment). */
   pthread_mutex_lock(&s_music_teardown_mutex);

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   session_music_state_t *state = conn ? (session_music_state_t *)conn->music_state : NULL;
   bool cleared = false;
   if (state) {
      pthread_mutex_lock(&state->write_mutex);
      /* A client that replaced its socket has registered the new one already;
       * the old one's close must leave it alone. */
      if (state->music_wsi == wsi) {
         state->music_wsi = NULL;
         state->write_ring_head = 0;
         state->write_ring_tail = 0;
         state->write_ring_count = 0;
         cleared = true;
      }
      pthread_mutex_unlock(&state->write_mutex);
   }

   pthread_mutex_unlock(&s_music_teardown_mutex);

   if (cleared) {
      OLOG_INFO("WebUI music: Cleared music stream wsi for session %u", session->session_id);
   } else {
      OLOG_INFO("WebUI music: Closed music socket wasn't the current one (session %u)",
                session->session_id);
   }
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

   /* Frames belong to the current socket; a replaced one still open gets none */
   if (state->write_ring_count == 0 || state->music_wsi != wsi) {
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

   struct lws *music_wsi = state->music_wsi;
   pthread_mutex_unlock(&state->write_mutex);

   /* The music thread arms the writeable callback itself (lws calls belong on
    * the service thread; this runs on the stream thread). */
   webui_music_server_request_write(music_wsi);
   return 0;
}
