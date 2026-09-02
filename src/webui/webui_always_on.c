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
 * WebUI Always-On Voice Mode — server-side state machine
 *
 * Handles continuous audio streaming from WebUI clients. Each connection
 * gets its own VAD context, Opus decoder, resampler, and circular buffer.
 * VAD runs inline (<1ms); ASR is dispatched async to worker pool.
 */

#include "webui/webui_always_on.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asr/asr_interface.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "core/utterance_dedup.h"
#include "core/wake_word.h"
#include "core/worker_pool.h"
#include "logging.h"
#include "utils/asr_transcript.h"
#include "webui/webui_audio.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* Valid sample rates (definition for extern in header) */
const uint32_t ALWAYS_ON_VALID_SAMPLE_RATES[] = { 8000, 16000, 22050, 44100, 48000 };

/* Opus constants */
#define OPUS_SAMPLE_RATE 48000
#define OPUS_MAX_FRAME_SIZE 5760 /* 120ms at 48kHz */
#define ASR_SAMPLE_RATE 16000

/* Raw PCM frame limit: 48kHz * 250ms = 12000 samples.
 * Must be larger than OPUS_MAX_FRAME_SIZE since browser sends
 * full 200ms chunks at native rate (e.g., 9600 samples at 48kHz). */
#define RAW_PCM_MAX_FRAME 12000

/* VAD frame size (must match vad_silero.c). The speech gate and end-of-speech
 * dwell are read from the shared [vad] config at use (see the state machine
 * below) so remote always-on tracks the same knobs as the local mic path. */
#define VAD_SAMPLE_SIZE 512 /* 32ms at 16kHz */

/* =============================================================================
 * Helpers
 * ============================================================================= */

static int64_t now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* End-of-speech reached on the wall clock: the dwell (from [vad]
 * end_of_speech_duration) has elapsed since the last speech frame. Shared by the
 * per-frame VAD path (always_on_process_audio) and the timer path
 * (always_on_check_timeouts) so the dwell logic cannot drift between them.
 * last_speech_ms == 0 means "no speech seen yet this window" (e.g. a freshly-
 * entered RECORDING, before the first command word), which correctly reads as
 * not-yet-reached — do not remove that guard. */
static inline bool always_on_eos_reached(const always_on_ctx_t *ctx, int64_t now) {
   const int64_t eos_ms = (int64_t)(g_config.vad.end_of_speech_duration * 1000.0f);
   return ctx->last_speech_ms > 0 && (now - ctx->last_speech_ms) >= eos_ms;
}

static void set_state(always_on_ctx_t *ctx, always_on_state_t new_state) {
   always_on_state_t old = atomic_load(&ctx->state);
   atomic_store(&ctx->state, new_state);
   ctx->state_entry_ms = now_ms();
   OLOG_INFO("Always-on state: %s -> %s", always_on_state_name(old),
             always_on_state_name(new_state));
}

/**
 * Write PCM data to the circular buffer with overflow detection.
 * Advances read_pos if buffer would overflow (drops oldest data).
 */
static void buffer_write(always_on_ctx_t *ctx, const uint8_t *data, size_t len) {
   if (len == 0 || !data) {
      return;
   }

   /* Overflow detection: if we'd exceed buffer, advance read_pos */
   if (ctx->valid_len + len > ALWAYS_ON_BUFFER_SIZE) {
      size_t overflow = (ctx->valid_len + len) - ALWAYS_ON_BUFFER_SIZE;
      ctx->read_pos = (ctx->read_pos + overflow) % ALWAYS_ON_BUFFER_SIZE;
      ctx->valid_len -= overflow;
      OLOG_WARNING("Always-on: buffer overflow, dropped %zu bytes of oldest audio", overflow);
   }

   /* Write with wrap-around */
   size_t first_chunk = ALWAYS_ON_BUFFER_SIZE - ctx->write_pos;
   if (len <= first_chunk) {
      memcpy(ctx->audio_buffer + ctx->write_pos, data, len);
   } else {
      memcpy(ctx->audio_buffer + ctx->write_pos, data, first_chunk);
      memcpy(ctx->audio_buffer, data + first_chunk, len - first_chunk);
   }
   ctx->write_pos = (ctx->write_pos + len) % ALWAYS_ON_BUFFER_SIZE;
   ctx->valid_len += len;
}

/**
 * Read PCM samples from the circular buffer for VAD processing.
 * Returns pointer to contiguous samples (may use provided scratch buffer for wrap-around).
 */
static const int16_t *buffer_read_samples(always_on_ctx_t *ctx,
                                          size_t num_samples,
                                          int16_t *scratch,
                                          size_t *available_out) {
   size_t available_samples = ctx->valid_len / sizeof(int16_t);
   if (available_samples < num_samples) {
      *available_out = available_samples;
      return NULL;
   }
   *available_out = num_samples;

   size_t byte_count = num_samples * sizeof(int16_t);
   size_t first_chunk = ALWAYS_ON_BUFFER_SIZE - ctx->read_pos;

   if (byte_count <= first_chunk) {
      /* No wrap — return pointer directly into buffer */
      return (const int16_t *)(ctx->audio_buffer + ctx->read_pos);
   }

   /* Wrap-around — copy into scratch buffer */
   memcpy(scratch, ctx->audio_buffer + ctx->read_pos, first_chunk);
   memcpy((uint8_t *)scratch + first_chunk, ctx->audio_buffer, byte_count - first_chunk);
   return scratch;
}

/**
 * Advance read position after consuming samples
 */
static void buffer_consume(always_on_ctx_t *ctx, size_t num_samples) {
   size_t byte_count = num_samples * sizeof(int16_t);
   ctx->read_pos = (ctx->read_pos + byte_count) % ALWAYS_ON_BUFFER_SIZE;
   ctx->valid_len -= byte_count;
}

/* Forward declarations */
static void always_on_release(always_on_ctx_t *ctx);

void send_always_on_state(struct lws *wsi, const char *state_name) {
   if (!wsi)
      return;

   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "state", json_object_new_string(state_name));
   json_object_object_add(obj, "type", json_object_new_string("always_on_state"));
   json_object_object_add(obj, "payload", payload);
   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void always_on_note_tts_activity(always_on_ctx_t *ctx) {
   if (!ctx)
      return;
   /* Progress signal: each streamed TTS sentence resets the PROCESSING watchdog
    * clock so a long, TTS-paced reply doesn't false-trip "LLM stalled". Only
    * bumps while actually answering (state == PROCESSING); a genuinely hung turn
    * emits no TTS, so the watchdog still recovers it. Reuses state_entry_ms, the
    * same field the timeout reads — matching the existing lockless access. */
   if (atomic_load(&ctx->state) == ALWAYS_ON_PROCESSING) {
      ctx->state_entry_ms = now_ms();
   }
}

/**
 * Rate limiting check. Returns true if the frame should be dropped.
 */
static bool rate_limit_check(always_on_ctx_t *ctx, size_t frame_bytes) {
   int64_t now = now_ms();

   /* Reset window every second */
   if (now - ctx->rate_window_start_ms >= 1000) {
      ctx->rate_window_start_ms = now;
      ctx->rate_bytes_in_window = 0;
   }

   ctx->rate_bytes_in_window += frame_bytes;
   if (ctx->rate_bytes_in_window > ALWAYS_ON_MAX_BYTES_PER_SEC) {
      return true; /* Drop frame */
   }
   return false;
}

/* =============================================================================
 * Async Wake Word Check (runs on worker thread)
 * ============================================================================= */

typedef struct {
   int16_t *pcm_data;    /**< 48kHz mono PCM (owned, must free) */
   size_t pcm_samples;   /**< Number of samples */
   always_on_ctx_t *ctx; /**< Back-pointer to always-on context (retained) */
} wake_check_work_t;

typedef struct {
   int16_t *pcm_data;    /**< 48kHz mono PCM (owned, must free) */
   size_t pcm_samples;   /**< Number of samples */
   always_on_ctx_t *ctx; /**< Back-pointer to always-on context (retained) */
   session_t *session;   /**< Session for LLM dispatch (retained) */
} cmd_transcribe_work_t;

/**
 * Extract audio from wake_start_pos to write_pos as a contiguous 16kHz PCM buffer.
 * This captures all audio from when speech was first detected (WAKE_CHECK entry)
 * through to the current write position, regardless of VAD consumption.
 * Returns allocated buffer (caller must free), sets *out_samples.
 * Resets the circular buffer afterward.
 */
static int16_t *extract_buffered_audio(always_on_ctx_t *ctx, size_t *out_samples) {
   /* Calculate bytes from wake_start_pos to write_pos */
   size_t byte_count;
   if (ctx->write_pos >= ctx->wake_start_pos) {
      byte_count = ctx->write_pos - ctx->wake_start_pos;
   } else {
      byte_count = ALWAYS_ON_BUFFER_SIZE - ctx->wake_start_pos + ctx->write_pos;
   }

   /* Defensive cap: byte_count should never exceed buffer size */
   if (byte_count > ALWAYS_ON_BUFFER_SIZE) {
      byte_count = ALWAYS_ON_BUFFER_SIZE;
   }

   size_t available = byte_count / sizeof(int16_t);
   if (available == 0) {
      *out_samples = 0;
      return NULL;
   }

   int16_t *pcm = malloc(byte_count);
   if (!pcm) {
      *out_samples = 0;
      return NULL;
   }

   size_t first_chunk = ALWAYS_ON_BUFFER_SIZE - ctx->wake_start_pos;
   if (byte_count <= first_chunk) {
      memcpy(pcm, ctx->audio_buffer + ctx->wake_start_pos, byte_count);
   } else {
      memcpy(pcm, ctx->audio_buffer + ctx->wake_start_pos, first_chunk);
      memcpy((uint8_t *)pcm + first_chunk, ctx->audio_buffer, byte_count - first_chunk);
   }

   /* Reset buffer */
   ctx->read_pos = ctx->write_pos;
   ctx->valid_len = 0;

   *out_samples = available;
   return pcm;
}

/**
 * Worker thread: transcribe buffered audio and check for wake word.
 * Stores result in ctx fields; the LWS thread picks it up via
 * always_on_consume_wake_result().
 */
static void *wake_check_worker(void *arg) {
   wake_check_work_t *work = (wake_check_work_t *)arg;
   always_on_ctx_t *ctx = work->ctx;

   /* Check if context was destroyed (disconnect during WAKE_PENDING) */
   if (always_on_get_state(ctx) == ALWAYS_ON_DISABLED) {
      OLOG_INFO("Always-on: wake check worker aborted (context disabled)");
      free(work->pcm_data);
      free(work);
      always_on_release(ctx);
      return NULL;
   }

   /* Use the proven 48kHz→16kHz resample + transcribe pipeline */
   OLOG_INFO("Always-on: ASR input: %zu samples (%.1f sec at 48kHz)", work->pcm_samples,
             (float)work->pcm_samples / 48000.0f);

   char *transcript_str = NULL;
   int asr_ret = webui_audio_pcm48k_to_text(work->pcm_data, work->pcm_samples, &transcript_str);

   const char *transcript = (asr_ret == 0 && transcript_str) ? transcript_str : "";
   OLOG_INFO("Always-on: wake check transcript: \"%s\"", transcript);

   /* Check for wake word */
   wake_word_result_t ww = wake_word_check(transcript);

   /* Store result for LWS thread to consume (under mutex for thread safety) */
   pthread_mutex_lock(&ctx->mutex);
   ctx->wake_detected = ww.detected ? 1 : 0;
   ctx->wake_has_command = ww.has_command ? 1 : 0;
   if (ww.has_command && ww.command) {
      ctx->wake_command = strdup(ww.command);
   } else {
      ctx->wake_command = NULL;
   }
   atomic_store(&ctx->wake_result_ready, 1);
   pthread_mutex_unlock(&ctx->mutex);

   free(transcript_str);

   free(work->pcm_data);
   free(work);
   always_on_release(ctx); /* Release worker's reference */
   return NULL;
}

/**
 * Dispatch wake word check to a worker thread.
 * Extracts buffered audio and spawns a detached thread.
 * Must be called with ctx->mutex held; releases before thread dispatch.
 */
static void dispatch_wake_check(always_on_ctx_t *ctx, ws_connection_t *conn) {
   size_t pcm_samples = 0;
   int16_t *pcm_data = extract_buffered_audio(ctx, &pcm_samples);

   if (!pcm_data || pcm_samples == 0) {
      OLOG_WARNING("Always-on: no audio to check for wake word");
      vad_silero_reset(ctx->vad_ctx);
      set_state(ctx, ALWAYS_ON_LISTENING);
      return;
   }

   wake_check_work_t *work = malloc(sizeof(wake_check_work_t));
   if (!work) {
      OLOG_ERROR("Always-on: failed to allocate wake check work");
      free(pcm_data);
      vad_silero_reset(ctx->vad_ctx);
      set_state(ctx, ALWAYS_ON_LISTENING);
      return;
   }

   work->pcm_data = pcm_data;
   work->pcm_samples = pcm_samples;
   work->ctx = ctx;
   (void)conn; /* Result consumed by LWS thread via always_on_consume_wake_result */

   set_state(ctx, ALWAYS_ON_WAKE_PENDING);
   send_always_on_state(ctx->wsi, "wake_pending");

   /* Retain reference for worker thread (released in wake_check_worker) */
   atomic_fetch_add(&ctx->refcount, 1);

   /* Spawn detached thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int ret = pthread_create(&thread, &attr, wake_check_worker, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      OLOG_ERROR("Always-on: failed to create wake check thread: %d", ret);
      atomic_fetch_sub(&ctx->refcount, 1); /* Undo retain */
      free(pcm_data);
      free(work);
      vad_silero_reset(ctx->vad_ctx);
      set_state(ctx, ALWAYS_ON_LISTENING);
   }
}

/**
 * Worker thread: transcribe recorded command audio and store result.
 * Runs ASR off the LWS thread to avoid blocking all WebSocket clients.
 */
static void *cmd_transcribe_worker(void *arg) {
   cmd_transcribe_work_t *work = (cmd_transcribe_work_t *)arg;
   always_on_ctx_t *ctx = work->ctx;

   if (always_on_get_state(ctx) == ALWAYS_ON_DISABLED) {
      OLOG_INFO("Always-on: cmd transcribe worker aborted (context disabled)");
      free(work->pcm_data);
      session_release(work->session);
      free(work);
      always_on_release(ctx);
      return NULL;
   }

   char *transcript = NULL;
   int asr_ret = webui_audio_pcm48k_to_text(work->pcm_data, work->pcm_samples, &transcript);

   if (asr_ret == 0 && transcript) {
      OLOG_INFO("Always-on: command transcript: \"%s\"", transcript);
   }

   /* Store result under mutex for thread safety.  Drop blank/silence transcripts
    * (empty, whitespace, or "[BLANK_AUDIO]") so nothing-heard never reaches the LLM. */
   pthread_mutex_lock(&ctx->mutex);
   if (asr_ret == 0 && !asr_transcript_is_blank(transcript)) {
      ctx->cmd_transcript = transcript;
   } else {
      ctx->cmd_transcript = NULL;
      free(transcript);
   }
   atomic_store(&ctx->cmd_result_ready, 1);
   pthread_mutex_unlock(&ctx->mutex);

   free(work->pcm_data);
   session_release(work->session);
   free(work);
   always_on_release(ctx);
   return NULL;
}

/**
 * Dispatch command transcription to a worker thread.
 * Must be called with ctx->mutex held.
 */
static void dispatch_cmd_transcribe(always_on_ctx_t *ctx, ws_connection_t *conn) {
   size_t pcm_samples = 0;
   int16_t *pcm_data = extract_buffered_audio(ctx, &pcm_samples);

   if (!pcm_data || pcm_samples == 0 || !conn->session) {
      OLOG_WARNING("Always-on: no audio or session for command transcribe");
      vad_silero_reset(ctx->vad_ctx);
      always_on_processing_complete(ctx);
      return;
   }

   cmd_transcribe_work_t *work = malloc(sizeof(cmd_transcribe_work_t));
   if (!work) {
      OLOG_ERROR("Always-on: failed to allocate cmd transcribe work");
      free(pcm_data);
      vad_silero_reset(ctx->vad_ctx);
      always_on_processing_complete(ctx);
      return;
   }

   work->pcm_data = pcm_data;
   work->pcm_samples = pcm_samples;
   work->ctx = ctx;
   work->session = conn->session;
   session_retain(work->session);

   set_state(ctx, ALWAYS_ON_PROCESSING);

   /* Retain reference for worker thread */
   atomic_fetch_add(&ctx->refcount, 1);

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int ret = pthread_create(&thread, &attr, cmd_transcribe_worker, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      OLOG_ERROR("Always-on: failed to create cmd transcribe thread: %d", ret);
      atomic_fetch_sub(&ctx->refcount, 1);
      session_release(work->session);
      free(pcm_data);
      free(work);
      vad_silero_reset(ctx->vad_ctx);
      always_on_processing_complete(ctx);
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

const char *always_on_state_name(always_on_state_t state) {
   switch (state) {
      case ALWAYS_ON_DISABLED:
         return "disabled";
      case ALWAYS_ON_LISTENING:
         return "listening";
      case ALWAYS_ON_WAKE_CHECK:
         return "wake_check";
      case ALWAYS_ON_WAKE_PENDING:
         return "wake_pending";
      case ALWAYS_ON_RECORDING:
         return "recording";
      case ALWAYS_ON_PROCESSING:
         return "processing";
      default:
         return "unknown";
   }
}

bool always_on_valid_sample_rate(uint32_t sample_rate) {
   for (int i = 0; i < ALWAYS_ON_NUM_VALID_RATES; i++) {
      if (ALWAYS_ON_VALID_SAMPLE_RATES[i] == sample_rate) {
         return true;
      }
   }
   return false;
}

always_on_ctx_t *always_on_create(uint32_t client_sample_rate, struct lws *wsi) {
   always_on_ctx_t *ctx = calloc(1, sizeof(always_on_ctx_t));
   if (!ctx) {
      OLOG_ERROR("Always-on: failed to allocate context");
      return NULL;
   }

   pthread_mutex_init(&ctx->mutex, NULL);
   atomic_store(&ctx->state, ALWAYS_ON_DISABLED);
   atomic_store(&ctx->refcount, 1); /* LWS thread holds initial reference */
   ctx->wsi = wsi;
   ctx->client_sample_rate = client_sample_rate;

   /* Allocate circular buffer */
   ctx->audio_buffer = calloc(1, ALWAYS_ON_BUFFER_SIZE);
   if (!ctx->audio_buffer) {
      OLOG_ERROR("Always-on: failed to allocate %d byte audio buffer", ALWAYS_ON_BUFFER_SIZE);
      goto fail;
   }

   /* Create per-connection VAD context */
   ctx->vad_ctx = vad_silero_init("models/silero_vad_16k_op15.onnx", NULL);
   if (!ctx->vad_ctx) {
      OLOG_ERROR("Always-on: failed to create VAD context");
      goto fail;
   }

   /* Create per-connection Opus decoder */
   int opus_err;
   ctx->opus_decoder = opus_decoder_create(OPUS_SAMPLE_RATE, 1, &opus_err);
   if (opus_err != OPUS_OK) {
      OLOG_ERROR("Always-on: failed to create Opus decoder: %s", opus_strerror(opus_err));
      goto fail;
   }

   /* No per-connection resampler needed — VAD uses simple decimation,
    * and ASR extraction uses the proven webui_audio_pcm48k_to_text pipeline. */

   int64_t now = now_ms();
   ctx->last_audio_ms = now;
   ctx->state_entry_ms = now;
   ctx->rate_window_start_ms = now;

   set_state(ctx, ALWAYS_ON_LISTENING);

   OLOG_INFO("Always-on: context created (sample_rate=%u)", client_sample_rate);

   return ctx;

fail:
   always_on_destroy(ctx);
   return NULL;
}

/**
 * Internal: free all resources. Called when refcount reaches 0.
 */
static void always_on_free(always_on_ctx_t *ctx) {
   OLOG_INFO("Always-on: freeing context (refcount reached 0)");

   if (ctx->resampler) {
      resampler_destroy(ctx->resampler);
   }
   if (ctx->opus_decoder) {
      opus_decoder_destroy(ctx->opus_decoder);
   }
   if (ctx->vad_ctx) {
      vad_silero_cleanup(ctx->vad_ctx);
   }
   free(ctx->wake_command);
   free(ctx->cmd_transcript);
   free(ctx->audio_buffer);
   pthread_mutex_destroy(&ctx->mutex);
   free(ctx);
}

/**
 * Release a reference. Frees the context when the last reference is released.
 */
static void always_on_release(always_on_ctx_t *ctx) {
   if (!ctx) {
      return;
   }
   int old = atomic_fetch_sub(&ctx->refcount, 1);
   if (old <= 1) {
      always_on_free(ctx);
   }
}

void always_on_destroy(always_on_ctx_t *ctx) {
   if (!ctx) {
      return;
   }

   OLOG_INFO("Always-on: destroying context");

   /* Mark as disabled so in-flight workers abort early */
   atomic_store(&ctx->state, ALWAYS_ON_DISABLED);
   ctx->wsi = NULL; /* Prevent stale pointer use after disconnect */

   /* Release the LWS thread's reference. If a worker is still running,
    * the context stays alive until the worker calls always_on_release. */
   always_on_release(ctx);
}

/* =============================================================================
 * WebSocket Message Handlers
 *
 * Lifecycle for always-on voice mode dispatched from
 * handle_json_message() in webui_message_dispatch.c when the client sends
 * `always_on_enable` / `always_on_disable`.  Kept here next to the
 * context lifecycle (always_on_create / always_on_destroy) so policy
 * (per-user uniqueness, push-to-talk conflict, sample-rate validation)
 * stays adjacent to the state machine it gates.
 * ============================================================================= */

/**
 * Check if another connection for the same user already has always-on active.
 * Must NOT hold s_conn_registry_mutex when calling conn_require_auth.
 */
static bool user_has_always_on(int auth_user_id) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *c = s_active_connections[i];
      if (c && c->auth_user_id == auth_user_id && c->always_on) {
         pthread_mutex_unlock(&s_conn_registry_mutex);
         return true;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return false;
}

void handle_always_on_enable(void *conn_ptr, struct json_object *payload) {
   ws_connection_t *conn = (ws_connection_t *)conn_ptr;
   /* Reject if already enabled on this connection */
   if (conn->always_on) {
      send_always_on_state(conn->wsi, "listening"); /* Already active, re-confirm */
      return;
   }

   /* Reject if push-to-talk audio is in progress */
   if (conn->audio_buffer && conn->audio_buffer_len > 0) {
      send_error_impl(conn->wsi, "PTT_ACTIVE",
                      "Cannot enable always-on while push-to-talk recording is active");
      return;
   }

   /* Enforce per-user limit (max 1 always-on session) */
   if (user_has_always_on(conn->auth_user_id)) {
      send_error_impl(conn->wsi, "ALREADY_ACTIVE",
                      "Always-on is active in another tab for this user");
      return;
   }

   /* Validate sample_rate from payload */
   uint32_t sample_rate = 48000; /* Default if not specified */
   if (payload) {
      struct json_object *sr_obj;
      if (json_object_object_get_ex(payload, "sample_rate", &sr_obj)) {
         sample_rate = (uint32_t)json_object_get_int(sr_obj);
      }
   }

   if (!always_on_valid_sample_rate(sample_rate)) {
      OLOG_WARNING("WebUI: Invalid always-on sample rate %u", sample_rate);
      send_error_impl(conn->wsi, "INVALID_SAMPLE_RATE", "Unsupported sample rate");
      return;
   }

   /* Create always-on context */
   conn->always_on = always_on_create(sample_rate, conn->wsi);
   if (!conn->always_on) {
      send_error_impl(conn->wsi, "INIT_FAILED", "Failed to initialize always-on mode");
      return;
   }

   OLOG_INFO("WebUI: Always-on enabled for user %d (sample_rate=%u)", conn->auth_user_id,
             sample_rate);
   send_always_on_state(conn->wsi, "listening");
}

void handle_always_on_disable(void *conn_ptr) {
   ws_connection_t *conn = (ws_connection_t *)conn_ptr;
   if (!conn->always_on) {
      send_always_on_state(conn->wsi, "disabled");
      return;
   }

   OLOG_INFO("WebUI: Always-on disabled for user %d", conn->auth_user_id);
   always_on_destroy(conn->always_on);
   conn->always_on = NULL;
   send_always_on_state(conn->wsi, "disabled");
}

void always_on_consume_wake_result(always_on_ctx_t *ctx, void *conn_ptr) {
   if (!ctx || !atomic_load(&ctx->wake_result_ready)) {
      return;
   }

   ws_connection_t *conn = (ws_connection_t *)conn_ptr;
   atomic_store(&ctx->wake_result_ready, 0);

   pthread_mutex_lock(&ctx->mutex);
   vad_silero_reset(ctx->vad_ctx);

   if (ctx->wake_detected) {
      OLOG_INFO("Always-on: wake word confirmed (has_command=%d)", ctx->wake_has_command);

      if (ctx->wake_has_command && ctx->wake_command) {
         /* Wake word + command — process through LLM */
         set_state(ctx, ALWAYS_ON_PROCESSING);
         char *cmd = ctx->wake_command;
         ctx->wake_command = NULL;
         pthread_mutex_unlock(&ctx->mutex);

         /* Capitalize first letter of the extracted command */
         if (cmd && cmd[0] >= 'a' && cmd[0] <= 'z') {
            cmd[0] -= 32;
         }

         send_always_on_state(ctx->wsi, "processing");

         if (conn->session && cmd[0] != '\0') {
            /* Cross-device dedup: another device already handled this spoken
             * command within the window.  Suppress and return to listening —
             * always_on_processing_complete resets the state machine (a plain
             * "idle" send would leave it wedged in PROCESSING). */
            if (utterance_dedup_check(conn->session->session_id)) {
               OLOG_INFO("Always-on: Dedup suppressed duplicate utterance: \"%s\"", cmd);
               always_on_processing_complete(ctx);
            } else {
               /* Always-on voice: this turn's input is ASR-transcribed.  Passed
                * as input_was_voice=true; the worker stamps it before dispatch. */
               webui_process_text_input(conn->session, cmd, /*input_was_voice=*/true);
            }
         }
         free(cmd);
      } else {
         /* Wake word only — greet and start recording the command */
         free(ctx->wake_command);
         ctx->wake_command = NULL;
         /* Arm end-of-speech from the first command word, not the stale wake-word
          * speech. last_speech_ms still holds a timestamp from wake detection
          * (already older than the dwell), so without this reset the wall-clock
          * end-of-speech check in always_on_check_timeouts would fire on the very
          * next tick and transcribe an empty buffer, dropping the command. 0 means
          * "no speech yet this window"; the >0 guards defer the dwell until the
          * user actually starts speaking (also removes any first-word grace race). */
         ctx->last_speech_ms = 0;
         /* Start the command recording from a clean ring. extract_buffered_audio
          * reads wake_start_pos -> write_pos; wake_start_pos still points at the
          * wake-word onset, so without this the command transcript is prefixed
          * with the wake word ("Okay Friday. <command>"). Reset all three so the
          * command captures only what is spoken after the greeting. */
         ctx->wake_start_pos = ctx->write_pos;
         ctx->read_pos = ctx->write_pos;
         ctx->valid_len = 0;
         set_state(ctx, ALWAYS_ON_RECORDING);
         pthread_mutex_unlock(&ctx->mutex);

         /* The "recording" state frame IS the ready cue — the client lights its mic
          * button on it. Deliberately NO spoken greeting here: a "Hello." TTS plays
          * through the client speaker WHILE the recording mic is live and (without
          * perfect client-side AEC) echoes back in, which the VAD scores as speech
          * and which stalls or hangs end-of-speech (observed: 15s+ tails, and full
          * hangs on clients whose TTS output isn't AEC-referenceable). An optional
          * acknowledgement belongs on the client as a short non-speech CHIME, which
          * the VAD won't arm on even if it bleeds into the mic. */
         send_always_on_state(ctx->wsi, "recording");
      }
   } else {
      /* No wake word — return to listening */
      OLOG_INFO("Always-on: no wake word found, returning to LISTENING");
      free(ctx->wake_command);
      ctx->wake_command = NULL;
      set_state(ctx, ALWAYS_ON_LISTENING);
      pthread_mutex_unlock(&ctx->mutex);

      send_always_on_state(ctx->wsi, "listening");
   }
}

/**
 * Consume command transcribe result from worker thread.
 * If ASR succeeded, dispatch to LLM. Otherwise return to listening.
 */
static void always_on_consume_cmd_result(always_on_ctx_t *ctx, void *conn_ptr) {
   if (!ctx || !atomic_load(&ctx->cmd_result_ready)) {
      return;
   }

   ws_connection_t *conn = (ws_connection_t *)conn_ptr;
   atomic_store(&ctx->cmd_result_ready, 0);

   pthread_mutex_lock(&ctx->mutex);
   char *transcript = ctx->cmd_transcript;
   ctx->cmd_transcript = NULL;
   pthread_mutex_unlock(&ctx->mutex);

   if (transcript && conn->session) {
      /* Cross-device dedup: drop a duplicate of a command another device
       * already handled; reset the state machine like the ASR-fail path. */
      if (utterance_dedup_check(conn->session->session_id)) {
         OLOG_INFO("Always-on: Dedup suppressed duplicate utterance: \"%s\"", transcript);
         free(transcript);
         always_on_processing_complete(ctx);
      } else {
         /* Always-on voice: this turn's input is ASR-transcribed.  Passed as
          * input_was_voice=true; the worker stamps it before dispatch. */
         webui_process_text_input(conn->session, transcript, /*input_was_voice=*/true);
         free(transcript);
      }
   } else {
      free(transcript);
      /* ASR failed or no session — return to listening */
      always_on_processing_complete(ctx);
   }
}

int always_on_process_audio(always_on_ctx_t *ctx,
                            const uint8_t *data,
                            size_t len,
                            bool is_opus,
                            void *conn_ptr) {
   /* Check for completed async results from worker threads */
   always_on_consume_wake_result(ctx, conn_ptr);
   always_on_consume_cmd_result(ctx, conn_ptr);
   if (!ctx || !data || len == 0) {
      return FAILURE;
   }

   always_on_state_t state = always_on_get_state(ctx);

   /* Server-side discard during PROCESSING (primary echo prevention) */
   if (state == ALWAYS_ON_PROCESSING || state == ALWAYS_ON_DISABLED) {
      return 0;
   }

   /* Post-TTS cooldown: discard audio to drain echo frames */
   int64_t now = now_ms();
   if (ctx->cooldown_until_ms > 0 && now < ctx->cooldown_until_ms) {
      return 0;
   }
   ctx->cooldown_until_ms = 0;

   pthread_mutex_lock(&ctx->mutex);

   ctx->last_audio_ms = now;

   /* Rate limiting */
   if (rate_limit_check(ctx, len)) {
      pthread_mutex_unlock(&ctx->mutex);
      OLOG_WARNING("Always-on: rate limit exceeded, dropping frame (%zu bytes)", len);
      return 0;
   }

   /* Decode Opus or copy raw PCM into aligned buffer */
   int16_t pcm_buf[RAW_PCM_MAX_FRAME]; /* Sized for raw 48kHz/250ms frames */
   const int16_t *pcm_data;
   size_t pcm_samples;

   if (is_opus) {
      /* Browser sends length-prefixed Opus frames: [2-byte LE len][frame]...
       * Decode all frames in the packet into pcm_buf. */
      pcm_samples = 0;
      size_t offset = 0;
      while (offset + 2 <= len) {
         uint16_t frame_len = data[offset] | (data[offset + 1] << 8);
         offset += 2;
         if (frame_len == 0 || offset + frame_len > len) {
            break;
         }
         int space = RAW_PCM_MAX_FRAME - (int)pcm_samples;
         if (space <= 0) {
            break;
         }
         int decoded = opus_decode(ctx->opus_decoder, data + offset, (opus_int32)frame_len,
                                   pcm_buf + pcm_samples, space, 0);
         if (decoded > 0) {
            pcm_samples += (size_t)decoded;
         }
         offset += frame_len;
      }
      if (pcm_samples == 0) {
         pthread_mutex_unlock(&ctx->mutex);
         return FAILURE;
      }
      pcm_data = pcm_buf;
   } else {
      /* Raw PCM: copy to aligned buffer (data may be at odd offset from WS frame). */
      pcm_samples = len / sizeof(int16_t);
      if (pcm_samples > RAW_PCM_MAX_FRAME) {
         pcm_samples = RAW_PCM_MAX_FRAME;
      }
      memcpy(pcm_buf, data, pcm_samples * sizeof(int16_t));
      pcm_data = pcm_buf;
   }

   /* Write raw 48kHz PCM to circular buffer (for ASR extraction later).
    * The proven resample_48k_to_16k pipeline handles resampling at ASR time. */
   buffer_write(ctx, (const uint8_t *)pcm_data, pcm_samples * sizeof(int16_t));

   /* Decimate to ~16kHz for VAD only (VAD expects 16kHz, doesn't need HQ audio).
    * Simple integer decimation (take every Nth sample) is sufficient for speech
    * detection and avoids libsamplerate's sinc filter holdback which loses ~60%
    * of samples on small per-frame chunks. The proven resample_48k_to_16k pipeline
    * handles high-quality resampling at ASR extraction time. */
   int16_t vad_pcm[RAW_PCM_MAX_FRAME]; /* Worst case: no decimation needed */
   size_t vad_pcm_samples;
   const int16_t *vad_input;

   if (ctx->client_sample_rate > ASR_SAMPLE_RATE) {
      /* Decimate: 48kHz→16kHz = take every 3rd sample, 44.1kHz→16kHz ≈ every 3rd */
      unsigned int step = ctx->client_sample_rate / ASR_SAMPLE_RATE;
      if (step < 1)
         step = 1;
      size_t out_idx = 0;
      for (size_t i = 0; i < pcm_samples && out_idx < RAW_PCM_MAX_FRAME; i += step) {
         vad_pcm[out_idx++] = pcm_data[i];
      }
      vad_input = vad_pcm;
      vad_pcm_samples = out_idx;
   } else {
      vad_input = pcm_data;
      vad_pcm_samples = pcm_samples;
   }

   /* Run VAD on the 16kHz resampled frame.
    * Process in VAD_SAMPLE_SIZE (512 sample / 32ms) chunks.
    * All chunks in a single frame share the same wall-clock time. */
   now = now_ms();
   size_t vad_offset = 0;

   /* Speech gate from the shared [vad] config (was a hardcoded 0.5) so remote
    * always-on matches the local mic. The end-of-speech dwell is applied through
    * always_on_eos_reached(), shared with the timer path. */
   const float speech_threshold = g_config.vad.speech_threshold;

   while (vad_offset + VAD_SAMPLE_SIZE <= vad_pcm_samples) {
      float speech_prob = vad_silero_process(ctx->vad_ctx, vad_input + vad_offset, VAD_SAMPLE_SIZE);
      vad_offset += VAD_SAMPLE_SIZE;

      /* State-specific VAD handling */
      state = atomic_load(&ctx->state);

      switch (state) {
         case ALWAYS_ON_LISTENING:
            if (speech_prob >= speech_threshold) {
               ctx->last_speech_ms = now;
               ctx->wake_start_pos = ctx->read_pos; /* Save for ASR extraction */
               set_state(ctx, ALWAYS_ON_WAKE_CHECK);
               send_always_on_state(ctx->wsi, "wake_check");
            }
            break;

         case ALWAYS_ON_WAKE_CHECK: {
            if (speech_prob >= speech_threshold) {
               ctx->last_speech_ms = now;
            }

            /* Check if speech ended (silence exceeds pause threshold) */
            if (speech_prob < speech_threshold && always_on_eos_reached(ctx, now)) {
               /* Speech ended — dispatch ASR to worker thread */
               dispatch_wake_check(ctx, (ws_connection_t *)conn_ptr);
            }
            break;
         }

         case ALWAYS_ON_RECORDING:
            if (speech_prob >= speech_threshold) {
               ctx->last_speech_ms = now;
            }
            /* End-of-speech: dispatch ASR to worker thread (non-blocking) */
            if (speech_prob < speech_threshold && always_on_eos_reached(ctx, now)) {
               send_always_on_state(ctx->wsi, "processing");
               dispatch_cmd_transcribe(ctx, (ws_connection_t *)conn_ptr);
            }
            break;

         default:
            /* WAKE_PENDING, PROCESSING, DISABLED: just buffer, don't process */
            break;
      }
   }

   /* In LISTENING state, maintain a sliding window to prevent buffer overflow.
    * Keep ~3 seconds of pre-roll for wake word ASR context. */
   if (atomic_load(&ctx->state) == ALWAYS_ON_LISTENING) {
      size_t max_preroll = ctx->client_sample_rate * sizeof(int16_t) * 3; /* 3 sec */
      if (ctx->valid_len > max_preroll) {
         size_t excess = ctx->valid_len - max_preroll;
         ctx->read_pos = (ctx->read_pos + excess) % ALWAYS_ON_BUFFER_SIZE;
         ctx->valid_len -= excess;
      }
   }

   pthread_mutex_unlock(&ctx->mutex);
   return 0;
}

bool always_on_check_timeouts(always_on_ctx_t *ctx, void *conn) {
   if (!ctx) {
      return false;
   }

   /* Consume async results here too — during PROCESSING the client mutes audio,
    * so process_audio never runs and results would sit unconsumed indefinitely. */
   always_on_consume_wake_result(ctx, conn);
   always_on_consume_cmd_result(ctx, conn);

   always_on_state_t state = always_on_get_state(ctx);
   if (state == ALWAYS_ON_DISABLED) {
      return false;
   }

   int64_t now = now_ms();
   int64_t elapsed = now - ctx->state_entry_ms;

   pthread_mutex_lock(&ctx->mutex);

   switch (state) {
      case ALWAYS_ON_WAKE_CHECK:
         if (always_on_eos_reached(ctx, now)) {
            /* Wall-clock end-of-speech. The per-frame VAD check (process_audio)
             * cannot fire this when the client uses Opus DTX and stops sending
             * frames during silence — no frame arrives, so the per-frame check
             * never runs, and end-of-speech is deferred until the next stray
             * frame (erratic: 1-20s observed). This timer-driven path ends a DTX
             * client's utterance on the dwell regardless of frame arrival. */
            dispatch_wake_check(ctx, (ws_connection_t *)conn);
         } else if (elapsed >= ALWAYS_ON_WAKE_CHECK_TIMEOUT_MS) {
            /* Backstop — dispatch ASR with whatever audio we have instead of
             * discarding. The buffer may contain a valid wake word phrase. */
            OLOG_INFO("Always-on: WAKE_CHECK timeout (%lld ms), dispatching ASR",
                      (long long)elapsed);
            dispatch_wake_check(ctx, (ws_connection_t *)conn);
         }
         break;

      case ALWAYS_ON_WAKE_PENDING:
         if (elapsed >= ALWAYS_ON_WAKE_PENDING_TIMEOUT_MS) {
            OLOG_ERROR(
                "Always-on: WAKE_PENDING timeout (ASR worker stalled), returning to LISTENING");
            vad_silero_reset(ctx->vad_ctx);
            ctx->valid_len = 0;
            ctx->read_pos = 0;
            ctx->write_pos = 0;
            set_state(ctx, ALWAYS_ON_LISTENING);
         }
         break;

      case ALWAYS_ON_RECORDING: {
         if (always_on_eos_reached(ctx, now)) {
            /* Wall-clock end-of-speech (see WAKE_CHECK above) — fires for DTX
             * clients whose silence starves the per-frame VAD check. */
            send_always_on_state(ctx->wsi, "processing");
            dispatch_cmd_transcribe(ctx, (ws_connection_t *)conn);
         } else if (ctx->last_speech_ms == 0 && elapsed >= ALWAYS_ON_NO_COMMAND_TIMEOUT_MS) {
            /* Wake word acknowledged but no command spoken (last_speech_ms stays 0
             * until the first command word). Return to LISTENING instead of holding
             * the mic open to RECORDING_TIMEOUT — there is nothing to transcribe. */
            OLOG_INFO("Always-on: no command after wake word (%lld ms), returning to LISTENING",
                      (long long)elapsed);
            vad_silero_reset(ctx->vad_ctx);
            ctx->valid_len = 0;
            ctx->read_pos = 0;
            ctx->write_pos = 0;
            set_state(ctx, ALWAYS_ON_LISTENING);
         } else if (elapsed >= ALWAYS_ON_RECORDING_TIMEOUT_MS) {
            OLOG_WARNING("Always-on: RECORDING timeout (%lld ms), dispatching ASR",
                         (long long)elapsed);
            send_always_on_state(ctx->wsi, "processing");
            dispatch_cmd_transcribe(ctx, (ws_connection_t *)conn);
         }
         break;
      }

      case ALWAYS_ON_PROCESSING:
         if (elapsed >= ALWAYS_ON_PROCESSING_TIMEOUT_MS) {
            OLOG_ERROR("Always-on: PROCESSING timeout (LLM stalled), returning to LISTENING");
            vad_silero_reset(ctx->vad_ctx);
            set_state(ctx, ALWAYS_ON_LISTENING);
         }
         break;

      default:
         break;
   }

   /* Auto-disable if no audio received for too long.
    * Skip during PROCESSING — the client intentionally mutes audio to prevent echo,
    * so silence is expected. The PROCESSING timeout above handles stalled LLM. */
   if (state != ALWAYS_ON_DISABLED && state != ALWAYS_ON_PROCESSING &&
       (now - ctx->last_audio_ms) >= ALWAYS_ON_NO_AUDIO_TIMEOUT_MS) {
      OLOG_WARNING("Always-on: no audio for %lld ms, auto-disabling",
                   (long long)(now - ctx->last_audio_ms));
      set_state(ctx, ALWAYS_ON_DISABLED);
      pthread_mutex_unlock(&ctx->mutex);
      return true; /* Caller should free ctx and notify client */
   }

   pthread_mutex_unlock(&ctx->mutex);
   return false;
}

void always_on_processing_complete(always_on_ctx_t *ctx) {
   if (!ctx) {
      return;
   }

   pthread_mutex_lock(&ctx->mutex);

   /* Reset buffer and VAD state for next utterance */
   ctx->valid_len = 0;
   ctx->read_pos = 0;
   ctx->write_pos = 0;
   vad_silero_reset(ctx->vad_ctx);

   /* Set cooldown to drain any in-flight echo frames */
   int64_t now = now_ms();
   ctx->cooldown_until_ms = now + ALWAYS_ON_COOLDOWN_MS;

   /* Push last_audio_ms into the future to prevent no-audio auto-disable.
    * The client defers unmute until TTS playback finishes (can be 2+ minutes
    * for long responses). Grace = 3 minutes covers even very long TTS. */
   ctx->last_audio_ms = now + 180000;

   set_state(ctx, ALWAYS_ON_LISTENING);

   pthread_mutex_unlock(&ctx->mutex);
}
