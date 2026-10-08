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

/* Adaptive-dwell speculative decode: cap the speculative runs per utterance so a
 * long stutter can't spawn unbounded decodes (paired with the <=1-in-flight cap in
 * spec_slot). Speculation starts a decode at a pause, before the dwell ends, so its
 * transcript can be compared with the final one. */
#define SPEC_MAX_FIRES 3

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

/* Adaptive-dwell shadow instrumentation (read-only; does NOT affect endpointing).
 * Call on a speech frame BEFORE updating last_speech_ms: if speech resumed after a
 * pause long enough to be a tentative endpoint (>= the chunker pause) but shorter
 * than the commit dwell, log it. These are exactly the mid-utterance pauses a
 * future adaptive/fast-commit endpoint on this path would have to NOT cut off, so
 * the distribution of their lengths is the dataset that decides whether it's safe.
 * Mirrors the local WAKEWORD_LISTEN "EOS shadow" log. */
static void always_on_shadow_note_resume(const always_on_ctx_t *ctx,
                                         int64_t now,
                                         const char *where) {
   if (ctx->last_speech_ms <= 0) {
      return;
   }
   const int64_t gap = now - ctx->last_speech_ms;
   const int64_t hush_ms = (int64_t)(g_config.vad.chunking.pause_duration * 1000.0f);
   const int64_t eos_ms = (int64_t)(g_config.vad.end_of_speech_duration * 1000.0f);
   if (hush_ms > 0 && gap >= hush_ms && gap < eos_ms) {
      OLOG_INFO("AO shadow: %s speech resumed after %lldms pause (dwell=%lldms)", where,
                (long long)gap, (long long)eos_ms);
   }
}

static void set_state(always_on_ctx_t *ctx, always_on_state_t new_state) {
   always_on_state_t old = atomic_load(&ctx->state);
   atomic_store(&ctx->state, new_state);
   ctx->state_entry_ms = now_ms();
   if (new_state == ALWAYS_ON_PROCESSING && old != ALWAYS_ON_PROCESSING)
      ctx->processing_since_ms = ctx->state_entry_ms;
   /* Every state transition invalidates any speculative decode: a stale in-flight
    * decode's store is dropped (generation bump) and any stored result is freed.
    * Entering an armable state (WAKE_CHECK = a fresh utterance, or RECORDING = the
    * command segment of a two-part one) also resets the per-segment fire cap and
    * the shadow miss reason. Single-writer on the LWS thread; see the spec_slot
    * locking contract. */
   bool armable_entry = (new_state == ALWAYS_ON_WAKE_CHECK || new_state == ALWAYS_ON_RECORDING);
   spec_slot_invalidate(&ctx->spec, armable_entry);
   if (armable_entry) {
      ctx->spec_pool_missed = false;
   }
   OLOG_INFO("Always-on state: %s -> %s", always_on_state_name(old),
             always_on_state_name(new_state));
}

/* Back to listening after PROCESSING, with ctx->mutex held: the buffer and
 * VAD reset for the next utterance, a cooldown to drain in-flight echo, and
 * last_audio_ms pushed past the no-audio auto-disable (the server drops audio
 * in PROCESSING without counting it, and the client unmutes only after a long
 * reply's TTS, which can take minutes). */
static void reset_for_listening_locked(always_on_ctx_t *ctx, int64_t now) {
   ctx->valid_len = 0;
   ctx->read_pos = 0;
   ctx->write_pos = 0;
   vad_silero_reset(ctx->vad_ctx);
   ctx->cooldown_until_ms = now + ALWAYS_ON_COOLDOWN_MS;
   ctx->last_audio_ms = now + ALWAYS_ON_LISTEN_GRACE_MS;
   set_state(ctx, ALWAYS_ON_LISTENING);
}

/* PROCESSING ends with no answer to wait for (no turn started, a duplicate,
 * nothing heard): back to listening, and the client told so it stops showing
 * "thinking".  LWS thread, ctx->mutex not held. */
static void processing_done_listening(always_on_ctx_t *ctx) {
   always_on_processing_complete(ctx);
   send_always_on_state(ctx->wsi, "listening");
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

/* Forward declarations */
static void always_on_release(always_on_ctx_t *ctx);
static inline bool always_on_adaptive_enabled(void); /* defined in the spec section */

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
    * clock so a long, TTS-paced reply doesn't trip the watchdog. Only
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
   /* Adaptive-dwell shadow: speculative transcript ready at commit (owned, may be
    * NULL) for spec-vs-committed agreement logging. Shadow-only; never dispatched. */
   char *spec_text;
   const char *spec_where;
} wake_check_work_t;

typedef struct {
   int16_t *pcm_data;    /**< 48kHz mono PCM (owned, must free) */
   size_t pcm_samples;   /**< Number of samples */
   always_on_ctx_t *ctx; /**< Back-pointer to always-on context (retained) */
   session_t *session;   /**< Session for LLM dispatch (retained) */
   /* Adaptive-dwell shadow: the speculative transcript that was ready at commit
    * (owned, may be NULL), so the worker can log spec-vs-committed agreement off
    * the LWS thread. Shadow-only; never dispatched. */
   char *spec_text;
   const char *spec_where; /**< which state armed it ("recording"); static string */
} cmd_transcribe_work_t;

/**
 * Copy audio from wake_start_pos to write_pos as a contiguous raw 48kHz PCM buffer
 * WITHOUT disturbing the ring. This captures all audio from when speech was first
 * detected (WAKE_CHECK entry) through to the current write position, regardless of
 * VAD consumption. Returns allocated buffer (caller must free), sets *out_samples.
 *
 * Non-destructive: read_pos/valid_len are untouched, so capture keeps running. Used
 * by the speculative-decode snapshot (which must not stop capture) and by
 * extract_buffered_audio (which adds the ring reset). Caller holds ctx->mutex.
 */
static int16_t *peek_buffered_audio(always_on_ctx_t *ctx, size_t *out_samples) {
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

   *out_samples = available;
   return pcm;
}

/**
 * Like peek_buffered_audio(), but also resets the ring afterward (the committed
 * command window is consumed, not re-decoded). Caller holds ctx->mutex.
 */
static int16_t *extract_buffered_audio(always_on_ctx_t *ctx, size_t *out_samples) {
   int16_t *pcm = peek_buffered_audio(ctx, out_samples);

   /* Reset the ring only when audio was actually taken (matches the pre-split
    * behavior, which returned before resetting on an empty/failed peek). */
   if (pcm) {
      ctx->read_pos = ctx->write_pos;
      ctx->valid_len = 0;
   }

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
      free(work->spec_text);
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

   /* Adaptive-dwell shadow: compare the speculative transcript (if one was ready at
    * commit) against this authoritative wake-check transcript. Measurement only. */
   if (work->spec_text) {
      bool agree = strcmp(work->spec_text, transcript) == 0;
      OLOG_INFO("AO spec agreement: where=%s agree=%d spec=\"%s\" committed=\"%s\"",
                work->spec_where ? work->spec_where : "?", agree, work->spec_text, transcript);
      free(work->spec_text);
      work->spec_text = NULL;
   }

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
   /* Adaptive-dwell shadow: snapshot the speculative slot BEFORE set_state wipes
    * it. Shadow mode never uses it ("on" isn't available; config_clamp_vad runs it as
    * shadow): it rides to the worker only for agreement logging; the synchronous
    * wake-check decode below is always authoritative. */
   char *spec_text = spec_slot_take_if_current(&ctx->spec);
   if (always_on_adaptive_enabled()) {
      const int64_t commit_now = now_ms();
      const char *miss = spec_text               ? "hit"
                         : ctx->spec.inflight    ? "inflight"
                         : ctx->spec_pool_missed ? "pool"
                         : ctx->spec.fires > 0   ? "cancelled"
                                                 : "none";
      OLOG_INFO("AO spec commit: where=wake_check hit=%d miss=%s fires=%d wait_at_commit_ms=%lld",
                spec_text != NULL, miss, ctx->spec.fires,
                spec_text ? (long long)(commit_now - ctx->spec.ready_ms) : 0LL);
   }

   size_t pcm_samples = 0;
   int16_t *pcm_data = extract_buffered_audio(ctx, &pcm_samples);

   if (!pcm_data || pcm_samples == 0) {
      OLOG_WARNING("Always-on: no audio to check for wake word");
      free(spec_text);
      vad_silero_reset(ctx->vad_ctx);
      set_state(ctx, ALWAYS_ON_LISTENING);
      return;
   }

   wake_check_work_t *work = malloc(sizeof(wake_check_work_t));
   if (!work) {
      OLOG_ERROR("Always-on: failed to allocate wake check work");
      free(spec_text);
      free(pcm_data);
      vad_silero_reset(ctx->vad_ctx);
      set_state(ctx, ALWAYS_ON_LISTENING);
      return;
   }

   work->pcm_data = pcm_data;
   work->pcm_samples = pcm_samples;
   work->ctx = ctx;
   work->spec_text = spec_text;
   work->spec_where = "wake_check";
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
      free(work->spec_text);
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
      free(work->spec_text);
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

   /* Adaptive-dwell shadow: compare the speculative transcript (if one was ready at
    * commit) against this authoritative one. Pure measurement — the committed
    * transcript below is always the one that gets used. */
   if (work->spec_text) {
      const char *committed = (asr_ret == 0 && transcript) ? transcript : "";
      bool agree = strcmp(work->spec_text, committed) == 0;
      OLOG_INFO("AO spec agreement: where=%s agree=%d spec=\"%s\" committed=\"%s\"",
                work->spec_where ? work->spec_where : "?", agree, work->spec_text, committed);
      free(work->spec_text);
      work->spec_text = NULL;
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

/* =============================================================================
 * Adaptive-dwell speculative decode (shadow mode: arms + decodes, never consumes)
 * ============================================================================= */

/* True when speculative decode should run: [vad] adaptive_endpoint is not "off"
 * AND the ASR engine is Whisper (speculation is meaningless for streaming Vosk). */
static inline bool always_on_adaptive_enabled(void) {
   if (strcmp(g_config.vad.adaptive_endpoint, "off") == 0) {
      return false;
   }
   return worker_pool_engine_type() == ASR_ENGINE_WHISPER;
}

typedef struct {
   int16_t *pcm_data;      /**< 48kHz snapshot (owned) */
   size_t pcm_samples;     /**< Number of samples */
   always_on_ctx_t *ctx;   /**< Retained (refcount) */
   asr_context_t *asr_ctx; /**< Borrowed; worker returns it */
   uint64_t gen;           /**< Generation captured at launch (staleness tag) */
   const char *where;      /**< armed-in state ("wake_check"/"recording"); static */
} spec_transcribe_work_t;

/* A speculative decode is worth keeping only while the connection is still in a
 * state that will consume it — i.e. still accumulating this utterance. */
static inline bool always_on_state_speculatable(always_on_state_t s) {
   return s == ALWAYS_ON_WAKE_CHECK || s == ALWAYS_ON_RECORDING;
}

/**
 * Speculative decode worker. Decodes a hush-time audio snapshot on a try-borrowed
 * ASR context and stores the result in the spec slot IFF still current. Never
 * dispatches, never touches the session or wsi, never sets state. Detached.
 */
static void *spec_transcribe_worker(void *arg) {
   spec_transcribe_work_t *work = (spec_transcribe_work_t *)arg;
   always_on_ctx_t *ctx = work->ctx;

   char *transcript = NULL;
   int rc = webui_audio_pcm48k_to_text_on_ctx(work->asr_ctx, work->pcm_data, work->pcm_samples,
                                              &transcript);
   worker_pool_return_asr(work->asr_ctx); /* return the borrowed context ASAP */

   /* Blank/failed decode → no usable result (spec_slot_store(NULL) just clears
    * inflight). Otherwise hand ownership to the slot, which keeps it only if the
    * generation still matches and we're still recording. */
   if (rc != 0 || asr_transcript_is_blank(transcript)) {
      free(transcript);
      transcript = NULL;
   }

   const int64_t now = now_ms();
   pthread_mutex_lock(&ctx->mutex);
   bool active = always_on_state_speculatable(atomic_load(&ctx->state));
   bool kept = spec_slot_store(&ctx->spec, work->gen, transcript, now, active);
   pthread_mutex_unlock(&ctx->mutex);
   if (kept) {
      OLOG_INFO("AO spec: decode ready where=%s (gen=%llu)", work->where,
                (unsigned long long)work->gen);
   }

   free(work->pcm_data);
   free(work);
   always_on_release(ctx);
   return NULL;
}

/**
 * Arm a speculative decode if we're in a hush and conditions allow. Call under
 * ctx->mutex, on a silence frame, AFTER the end-of-speech (commit) check. @p where
 * is a static string for shadow logging. Never blocks; if no ASR context can be
 * spared it records a pool miss and returns.
 */
static void always_on_spec_maybe_arm(always_on_ctx_t *ctx,
                                     int64_t now,
                                     always_on_state_t expected,
                                     const char *where) {
   if (!always_on_adaptive_enabled() || atomic_load(&ctx->state) != expected) {
      return;
   }
   /* With the default network.workers=2 pool and keep_idle=1 this arms only when a
    * spare context remains; a 1-worker pool can never spare one, so log once and
    * stay inert rather than silently never arming. */
   if (worker_pool_size() < 2) {
      static _Atomic bool logged = false;
      if (!atomic_exchange(&logged, true)) {
         OLOG_WARNING("Adaptive endpoint inert: worker pool too small (%d < 2)",
                      worker_pool_size());
      }
      return;
   }

   const int64_t hush_ms = (int64_t)(g_config.vad.chunking.pause_duration * 1000.0f);
   const int64_t eos_ms = (int64_t)(g_config.vad.end_of_speech_duration * 1000.0f);
   if (!spec_in_hush_window(ctx->last_speech_ms, now, hush_ms, eos_ms) ||
       !spec_slot_can_arm(&ctx->spec, SPEC_MAX_FIRES)) {
      return;
   }

   asr_context_t *asr_ctx = worker_pool_try_borrow_asr(1);
   if (!asr_ctx) {
      ctx->spec_pool_missed = true;
      return;
   }

   size_t n = 0;
   int16_t *pcm = peek_buffered_audio(ctx, &n); /* non-destructive: capture continues */
   if (!pcm || n == 0) {
      worker_pool_return_asr(asr_ctx);
      free(pcm);
      return;
   }

   spec_transcribe_work_t *work = malloc(sizeof(spec_transcribe_work_t));
   if (!work) {
      worker_pool_return_asr(asr_ctx);
      free(pcm);
      return;
   }
   work->pcm_data = pcm;
   work->pcm_samples = n;
   work->ctx = ctx;
   work->asr_ctx = asr_ctx;
   work->gen = spec_slot_gen(&ctx->spec);
   work->where = where;

   atomic_fetch_add(&ctx->refcount, 1); /* worker holds a reference */

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int ret = pthread_create(&thread, &attr, spec_transcribe_worker, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      OLOG_ERROR("Always-on: failed to create speculative decode thread: %d", ret);
      atomic_fetch_sub(&ctx->refcount, 1);
      worker_pool_return_asr(asr_ctx);
      free(pcm);
      free(work);
      return; /* slot NOT marked launched → a later frame may retry */
   }

   spec_slot_mark_launched(&ctx->spec, now);
}

/**
 * Dispatch command transcription to a worker thread.
 * Must be called with ctx->mutex held.
 */
static void dispatch_cmd_transcribe(always_on_ctx_t *ctx, ws_connection_t *conn) {
   /* Adaptive-dwell shadow: snapshot the speculative slot BEFORE set_state wipes
    * it. take_if_current detaches a ready + current speculative transcript (else
    * NULL). Shadow mode NEVER uses it ("on" isn't available; config_clamp_vad runs it
    * as shadow): it rides along to the worker only so agreement can be logged; the
    * real synchronous decode below is always authoritative. */
   char *spec_text = spec_slot_take_if_current(&ctx->spec);
   if (always_on_adaptive_enabled()) {
      const int64_t commit_now = now_ms();
      const char *miss = spec_text               ? "hit"
                         : ctx->spec.inflight    ? "inflight"
                         : ctx->spec_pool_missed ? "pool"
                         : ctx->spec.fires > 0   ? "cancelled"
                                                 : "none";
      OLOG_INFO("AO spec commit: hit=%d miss=%s fires=%d wait_at_commit_ms=%lld", spec_text != NULL,
                miss, ctx->spec.fires,
                spec_text ? (long long)(commit_now - ctx->spec.ready_ms) : 0LL);
   }

   size_t pcm_samples = 0;
   int16_t *pcm_data = extract_buffered_audio(ctx, &pcm_samples);

   if (!pcm_data || pcm_samples == 0 || !conn->session) {
      OLOG_WARNING("Always-on: no audio or session for command transcribe");
      free(spec_text);
      vad_silero_reset(ctx->vad_ctx);
      processing_done_listening(ctx);
      return;
   }

   cmd_transcribe_work_t *work = malloc(sizeof(cmd_transcribe_work_t));
   if (!work) {
      OLOG_ERROR("Always-on: failed to allocate cmd transcribe work");
      free(spec_text);
      free(pcm_data);
      vad_silero_reset(ctx->vad_ctx);
      processing_done_listening(ctx);
      return;
   }

   work->pcm_data = pcm_data;
   work->pcm_samples = pcm_samples;
   work->ctx = ctx;
   work->session = conn->session;
   work->spec_text = spec_text;
   work->spec_where = "recording";
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
      free(work->spec_text);
      free(pcm_data);
      free(work);
      vad_silero_reset(ctx->vad_ctx);
      processing_done_listening(ctx);
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
   spec_slot_init(&ctx->spec); /* redundant on calloc, explicit for clarity */

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
   spec_slot_free(&ctx->spec);
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
 * (per-login uniqueness by auth session token, push-to-talk conflict,
 * sample-rate validation) stays adjacent to the state machine it gates.
 * ============================================================================= */

/**
 * Check if another connection sharing this browser login already has always-on
 * active.  Keyed on the auth session token (the login cookie), NOT the user id:
 * one account logged in on several machines has a distinct token per login, so
 * matching on user id wrongly blocked a second machine from listening.
 * Same-browser tabs share the cookie (and are already reduced to one live
 * connection by session eviction); an independent machine has its own token and
 * gets its own always-on session.  An empty token matches nothing (fail open).
 *
 * No self-exclusion is needed: handle_always_on_enable() early-returns when
 * conn->always_on is already set, so the calling connection cannot self-match.
 * The caller is auth-gated upstream (conn_require_auth at dispatch), so the
 * token is already DB-validated and non-empty here; the empty-token guard is
 * only defensive.
 */
static bool session_has_always_on(const char *auth_session_token) {
   if (!auth_session_token || auth_session_token[0] == '\0') {
      return false;
   }
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *c = s_active_connections[i];
      if (c && c->always_on && strcmp(c->auth_session_token, auth_session_token) == 0) {
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

   /* Enforce one always-on per browser login (keyed on the auth session token),
    * so each machine gets its own; a second tab of the same browser shares the
    * token — and is already reduced to one live connection by session eviction. */
   if (session_has_always_on(conn->auth_session_token)) {
      send_error_impl(conn->wsi, "ALREADY_ACTIVE", "Always-on is already active in this browser");
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
   /* Read conn->session ONCE (atomic): the maintenance thread can NULL it
    * concurrently, so re-reading the field between the guard and the deref could
    * tear to a NULL. The lws-thread caller (the always-on sweep) has retained
    * this session, so a non-NULL value stays alive for the dispatch below. */
   session_t *session = conn_get_session(conn);
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

         if (session && cmd[0] != '\0') {
            /* Cross-device dedup: another device already handled this spoken
             * command within the window.  Suppress and return to listening —
             * always_on_processing_complete resets the state machine (a plain
             * "idle" send would leave it wedged in PROCESSING). */
            if (utterance_dedup_check(session->session_id)) {
               OLOG_INFO("Always-on: Dedup suppressed duplicate utterance: \"%s\"", cmd);
               processing_done_listening(ctx);
            } else {
               /* Always-on voice: this turn's input is ASR-transcribed.  Passed
                * as input_was_voice=true; the worker stamps it before dispatch. */
               if (webui_process_text_input(session, cmd, /*input_was_voice=*/true) != 0) {
                  /* No turn started (queue full, out of memory): nothing will
                   * end PROCESSING, so end it here. */
                  processing_done_listening(ctx);
               }
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
   /* Read conn->session ONCE (atomic) — see always_on_consume_wake_result. */
   session_t *session = conn_get_session(conn);
   atomic_store(&ctx->cmd_result_ready, 0);

   pthread_mutex_lock(&ctx->mutex);
   char *transcript = ctx->cmd_transcript;
   ctx->cmd_transcript = NULL;
   pthread_mutex_unlock(&ctx->mutex);

   /* A transcript that came back after the wait was given up belongs to no
    * one: always-on is already listening, perhaps to a new utterance. */
   if (atomic_load(&ctx->state) != ALWAYS_ON_PROCESSING) {
      OLOG_WARNING("Always-on: a command transcript came back too late; dropped");
      free(transcript);
      return;
   }

   if (transcript && session) {
      /* Cross-device dedup: drop a duplicate of a command another device
       * already handled; reset the state machine like the ASR-fail path. */
      if (utterance_dedup_check(session->session_id)) {
         OLOG_INFO("Always-on: Dedup suppressed duplicate utterance: \"%s\"", transcript);
         free(transcript);
         processing_done_listening(ctx);
      } else {
         /* Always-on voice: this turn's input is ASR-transcribed.  Passed as
          * input_was_voice=true; the worker stamps it before dispatch. */
         const int rc = webui_process_text_input(session, transcript, /*input_was_voice=*/true);
         free(transcript);
         if (rc != 0) {
            /* No turn started: nothing will end PROCESSING, so end it here. */
            processing_done_listening(ctx);
         }
      }
   } else {
      free(transcript);
      /* ASR failed or no session — return to listening */
      processing_done_listening(ctx);
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
               always_on_shadow_note_resume(ctx, now, "wake_check");
               /* Resumed speech cancels any armed/in-flight speculative decode. */
               spec_slot_invalidate(&ctx->spec, false);
               ctx->last_speech_ms = now;
            } else if (always_on_eos_reached(ctx, now)) {
               /* Speech ended — dispatch ASR to worker thread */
               dispatch_wake_check(ctx, (ws_connection_t *)conn_ptr);
            } else {
               /* Silence, not yet end-of-speech: at the hush, speculate (shadow). */
               always_on_spec_maybe_arm(ctx, now, ALWAYS_ON_WAKE_CHECK, "wake_check");
            }
            break;
         }

         case ALWAYS_ON_RECORDING:
            if (speech_prob >= speech_threshold) {
               always_on_shadow_note_resume(ctx, now, "recording");
               /* Resumed speech cancels any armed/in-flight speculative decode
                * (its later store is dropped by the generation bump). */
               spec_slot_invalidate(&ctx->spec, false);
               ctx->last_speech_ms = now;
            } else if (always_on_eos_reached(ctx, now)) {
               /* End-of-speech: dispatch ASR to worker thread (non-blocking) */
               send_always_on_state(ctx->wsi, "processing");
               dispatch_cmd_transcribe(ctx, (ws_connection_t *)conn_ptr);
            } else {
               /* Silence, not yet end-of-speech: at the hush, speculate (shadow). */
               always_on_spec_maybe_arm(ctx, now, ALWAYS_ON_RECORDING, "recording");
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

bool always_on_check_timeouts(always_on_ctx_t *ctx, void *conn, session_t *session) {
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
         } else {
            /* DTX client mid-pause: at the hush, speculate (shadow). Self-gates. */
            always_on_spec_maybe_arm(ctx, now, ALWAYS_ON_WAKE_CHECK, "wake_check");
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
         } else {
            /* DTX client mid-pause: at the hush, speculate (shadow). Self-gates on
             * the hush window, so it's a no-op until the pause is long enough. */
            always_on_spec_maybe_arm(ctx, now, ALWAYS_ON_RECORDING, "recording");
         }
         break;
      }

      case ALWAYS_ON_PROCESSING: {
         /* A running turn is progress: a turn busy in a tool speaks nothing.
          * Any turn on the session counts; its end ("idle") ends PROCESSING. */
         const bool running = session && atomic_load(&session->turn_in_flight) > 0;
         if (running)
            ctx->state_entry_ms = now;
         if (always_on_processing_expired(now, ctx->state_entry_ms, ctx->processing_since_ms,
                                          running)) {
            /* A spoken turn still running is the command we're waiting on (or
             * one before it): stopped, as Stop would, so its answer doesn't
             * arrive minutes later and the next command doesn't queue behind
             * it.  A typed turn is the user's own; ours runs after it. */
            const bool spoken = running && atomic_load(&session->input_was_voice);
            const char *notice;
            if (spoken) {
               session_cancel_turn(session);
               notice = "That took too long, so I'm stopping it.";
            } else if (running) {
               notice = "Still busy with an earlier request; your command will run after it.";
            } else {
               notice = "Lost track of that request. Please ask again.";
            }
            OLOG_ERROR("Always-on: stopped waiting for the answer after %lld ms (%s), listening",
                       (long long)(now - ctx->processing_since_ms),
                       spoken    ? "spoken turn cancelled"
                       : running ? "typed turn running"
                                 : "no turn running");
            reset_for_listening_locked(ctx, now);
            send_always_on_state(ctx->wsi, "listening");
            webui_send_error_ex(session, "ALWAYS_ON_GAVE_UP", notice, WS_SEVERITY_WARNING);
         }
         break;
      }

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
   reset_for_listening_locked(ctx, now_ms());
   pthread_mutex_unlock(&ctx->mutex);
}
