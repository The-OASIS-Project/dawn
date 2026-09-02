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
 * WebUI Always-On Voice Mode
 *
 * Server-side state machine for continuous voice listening via WebUI.
 * Each always-on connection gets its own VAD context, Opus decoder,
 * resampler, and circular audio buffer. ASR is dispatched async to
 * the worker pool to avoid blocking the LWS event loop.
 *
 * Lock hierarchy: s_conn_registry_mutex > always_on_ctx_t.mutex
 *
 * Thread safety:
 * - State reads via atomic_load (lock-free from worker threads)
 * - State transitions and buffer writes protected by mutex
 * - One context per connection, never shared between connections
 */

#ifndef WEBUI_ALWAYS_ON_H
#define WEBUI_ALWAYS_ON_H

#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asr/vad_silero.h"
#include "audio/resampler.h"

/* Forward decl — only used by handle_always_on_enable below.  Including
 * <json-c/json.h> here would pull json-c into every TU that uses
 * always_on, which most don't need. */
struct json_object;

/* Forward declarations */
struct lws;
typedef struct session session_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/** Circular buffer: 30 seconds @ 48kHz mono 16-bit = 2,880,000 bytes (~2.8MB).
 * Matches WEBUI_MAX_RECORDING_SECONDS and Whisper's 30-second chunk limit.
 * Stores raw 48kHz PCM (pre-resample) so the proven resample_48k_to_16k
 * pipeline can be used for ASR. VAD gets small per-frame resampled chunks. */
#define ALWAYS_ON_BUFFER_SIZE (48000 * 2 * 30)

/** Timeouts (milliseconds) */
#define ALWAYS_ON_WAKE_CHECK_TIMEOUT_MS 30000
#define ALWAYS_ON_WAKE_PENDING_TIMEOUT_MS 10000
#define ALWAYS_ON_RECORDING_TIMEOUT_MS 30000
/* Bare wake word acknowledged but no command word spoken yet: give up and return
 * to LISTENING rather than holding the recording open to RECORDING_TIMEOUT (30s).
 * Distinct from RECORDING_TIMEOUT, which bounds a command already in progress. */
#define ALWAYS_ON_NO_COMMAND_TIMEOUT_MS 6000
#define ALWAYS_ON_PROCESSING_TIMEOUT_MS 30000
#define ALWAYS_ON_NO_AUDIO_TIMEOUT_MS 60000

/** Post-TTS cooldown: discard audio for this long after returning to LISTENING */
#define ALWAYS_ON_COOLDOWN_MS 300

/** Rate limiting: max bytes per second per connection.
 * Must accommodate 48kHz/16-bit/mono raw PCM (96,000 B/s) + margin for timing
 * jitter. With 200ms frames (19,200 B each), 7 frames in a jittered window =
 * 134,400 B. Opus is ~4KB/s but raw PCM is the fallback before Opus loads. */
#define ALWAYS_ON_MAX_BYTES_PER_SEC 150000

/** Valid sample rates from browser */
extern const uint32_t ALWAYS_ON_VALID_SAMPLE_RATES[];
#define ALWAYS_ON_NUM_VALID_RATES 5

/* =============================================================================
 * State Machine
 * ============================================================================= */

typedef enum {
   ALWAYS_ON_DISABLED,     /**< Feature off, normal push-to-talk */
   ALWAYS_ON_LISTENING,    /**< Streaming, server VAD waiting for speech */
   ALWAYS_ON_WAKE_CHECK,   /**< VAD triggered, accumulating audio for ASR */
   ALWAYS_ON_WAKE_PENDING, /**< ASR dispatched to worker thread, awaiting result */
   ALWAYS_ON_RECORDING,    /**< Wake word confirmed, recording command */
   ALWAYS_ON_PROCESSING,   /**< ASR -> LLM -> TTS (server discards audio, client mutes) */
} always_on_state_t;

/* =============================================================================
 * Context (per-connection, allocated on enable, freed on disable/disconnect)
 * ============================================================================= */

typedef struct always_on_ctx {
   _Atomic always_on_state_t state;
   _Atomic int refcount; /**< Reference count: 1=LWS thread, +1 per in-flight worker */

   pthread_mutex_t mutex; /**< Protects state transitions, buffer, and rate tracking */

   /* Circular audio buffer (16kHz mono 16-bit PCM, post-resample) */
   uint8_t *audio_buffer;
   size_t write_pos;
   size_t read_pos;
   size_t valid_len;

   /* Per-connection audio processing */
   silero_vad_context_t *vad_ctx; /**< Own LSTM state, shared ONNX env */
   OpusDecoder *opus_decoder;     /**< Per-connection (stateful for PLC) */
   resampler_t *resampler;        /**< 48kHz -> 16kHz (if needed) */
   uint32_t client_sample_rate;   /**< From enable message (validated) */

   /* Timing */
   int64_t last_speech_ms;    /**< Last VAD speech detection */
   int64_t last_audio_ms;     /**< Last audio frame received (for auto-disable) */
   int64_t wake_word_ms;      /**< When wake word was detected */
   int64_t state_entry_ms;    /**< When current state was entered */
   int64_t cooldown_until_ms; /**< Post-TTS cooldown: discard audio until this time */
   size_t wake_start_pos;     /**< Buffer read_pos saved when entering WAKE_CHECK */

   /* Rate limiting */
   int64_t rate_window_start_ms; /**< Start of current 1-second rate window */
   size_t rate_bytes_in_window;  /**< Bytes received in current window */

   /* Wake check async result (set by worker thread, consumed by LWS thread) */
   _Atomic int wake_result_ready; /**< 0=none, 1=result pending */
   int wake_detected;             /**< True if wake word was found */
   int wake_has_command;          /**< True if command text follows wake word */
   char *wake_command;            /**< Command text (malloc'd, consumed by LWS thread) */

   /* Command transcribe async result (set by worker thread, consumed by LWS thread) */
   _Atomic int cmd_result_ready; /**< 0=none, 1=result pending */
   char *cmd_transcript;         /**< Transcribed text (malloc'd, consumed by LWS thread) */

   /* Connection back-pointer (non-owning, for sending responses) */
   struct lws *wsi;

} always_on_ctx_t;

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Create and initialize an always-on context
 *
 * Allocates circular buffer, creates per-connection VAD context (using shared
 * ONNX env from the daemon's VAD init), Opus decoder, and resampler.
 *
 * @param client_sample_rate Sample rate reported by the browser (validated)
 * @param wsi WebSocket instance for sending responses
 * @return Allocated context, or NULL on failure
 */
always_on_ctx_t *always_on_create(uint32_t client_sample_rate, struct lws *wsi);

/**
 * @brief Destroy an always-on context and free all resources
 *
 * @param ctx Context to destroy (NULL-safe)
 */
void always_on_destroy(always_on_ctx_t *ctx);

/**
 * @brief Process incoming audio data
 *
 * Decodes Opus (if needed), resamples to 16kHz, runs VAD, and manages
 * state transitions. ASR is dispatched async to the worker pool.
 *
 * Called from the LWS event loop thread on each WS_BIN_AUDIO_IN frame.
 *
 * @param ctx Always-on context
 * @param data Raw audio data (Opus or PCM, with type byte prefix stripped)
 * @param len Length of audio data
 * @param is_opus True if data is Opus-encoded
 * @param conn Connection context (ws_connection_t *, for session access)
 * @return 0 on success, non-zero on error
 */
int always_on_process_audio(always_on_ctx_t *ctx,
                            const uint8_t *data,
                            size_t len,
                            bool is_opus,
                            void *conn);

/**
 * @brief Check for timeouts and auto-disable
 *
 * Called periodically (e.g., from LWS timer callback). Checks all timeout
 * conditions and auto-disables if no audio received for ALWAYS_ON_NO_AUDIO_TIMEOUT_MS.
 *
 * @param ctx Always-on context
 * @param conn Connection context (ws_connection_t *)
 * @return true if always-on was auto-disabled
 */
bool always_on_check_timeouts(always_on_ctx_t *ctx, void *conn);

/**
 * @brief Get current state (lock-free atomic read)
 */
static inline always_on_state_t always_on_get_state(const always_on_ctx_t *ctx) {
   return atomic_load(&ctx->state);
}

/**
 * @brief Get state name string for JSON messages
 */
const char *always_on_state_name(always_on_state_t state);

/**
 * @brief Validate a sample rate against the whitelist
 *
 * @param sample_rate Rate to validate
 * @return true if valid
 */
bool always_on_valid_sample_rate(uint32_t sample_rate);

/**
 * @brief Consume wake word check result from worker thread
 *
 * Called from the LWS thread (via process_audio or check_timeouts).
 * If a worker thread has completed a wake word check, this function
 * processes the result and transitions the state machine.
 *
 * @param ctx Always-on context
 * @param conn Connection context (ws_connection_t *)
 */
void always_on_consume_wake_result(always_on_ctx_t *ctx, void *conn);

/**
 * @brief Notify that processing is complete (response sent)
 *
 * Transitions from PROCESSING back to LISTENING with cooldown.
 * Called after TTS audio has been fully sent to the client.
 *
 * @param ctx Always-on context
 */
void always_on_processing_complete(always_on_ctx_t *ctx);

/**
 * @brief Send always_on_state JSON message to the client
 *
 * Builds and sends {"type":"always_on_state","payload":{"state":"..."}} message.
 * MUST only be called from the LWS service thread.
 *
 * @param wsi WebSocket instance (NULL-safe)
 * @param state_name State name string
 */
void send_always_on_state(struct lws *wsi, const char *state_name);

/**
 * @brief Note TTS-streaming progress so the PROCESSING watchdog doesn't false-fire.
 *
 * Call as TTS audio frames are sent to the answering connection. While the
 * context is in PROCESSING, this resets the watchdog clock — a long, TTS-paced
 * reply then can't trip the "LLM stalled" timeout, while a genuinely hung turn
 * (no TTS) still recovers. NULL-safe (no-op when always-on is inactive).
 *
 * THREAD SAFETY: call ONLY from the LWS service thread (e.g. the response-queue
 * drain in webui_send.c). It touches the always_on context, which is freed on
 * that same thread by always_on_destroy — calling from a worker thread would
 * race that free (use-after-free). It also writes state_entry_ms locklessly,
 * which the watchdog reader (always_on_check_timeouts, also LWS thread) reads;
 * keeping both on one thread avoids the race.
 *
 * @param ctx Always-on context (typically `conn->always_on`; may be NULL).
 */
void always_on_note_tts_activity(always_on_ctx_t *ctx);

/**
 * @brief Handle `always_on_enable` WebSocket message.
 *
 * Validates per-user uniqueness, push-to-talk conflict, sample-rate;
 * allocates the always_on_ctx and emits the "listening" state to the
 * client.  Called from handle_json_message in webui_message_dispatch.c.
 *
 * @param conn    ws_connection_t * (declared in webui_internal.h)
 * @param payload The JSON payload object (may be NULL)
 */
void handle_always_on_enable(void *conn, struct json_object *payload);

/**
 * @brief Handle `always_on_disable` WebSocket message.
 *
 * Tears down the per-connection always_on_ctx and emits the "disabled"
 * state to the client.
 *
 * @param conn ws_connection_t * (declared in webui_internal.h)
 */
void handle_always_on_disable(void *conn);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_ALWAYS_ON_H */
