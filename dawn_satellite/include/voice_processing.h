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
 * Voice-Activated Processing Loop for DAP2 Tier 1 Satellites
 *
 * Implements the voice processing pipeline:
 * 1. Audio capture -> VAD -> Wake word detection -> ASR -> Query
 * 2. Receive response -> TTS -> Audio playback (with barge-in support)
 */

#ifndef VOICE_PROCESSING_H
#define VOICE_PROCESSING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectrum_defs.h" /* SPECTRUM_BINS shared between audio and UI */

/* Forward declarations - matching actual struct names */
struct satellite_config;
struct satellite_ctx;
struct ws_client;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Voice processing states
 */
typedef enum {
   VOICE_STATE_SILENCE,           /* Waiting for speech */
   VOICE_STATE_WAKEWORD_LISTEN,   /* Collecting audio for wake word check */
   VOICE_STATE_COMMAND_RECORDING, /* Recording user command after wake word */
   VOICE_STATE_PROCESSING,        /* ASR transcription in progress */
   VOICE_STATE_WAITING,           /* Waiting for server response */
   VOICE_STATE_SPEAKING,          /* Playing TTS audio */
} voice_state_t;

/**
 * @brief Voice processing context
 */
typedef struct voice_ctx voice_ctx_t;

/**
 * @brief Initialize voice processing context
 *
 * Loads all models (VAD, ASR, TTS) at startup for predictable latency.
 * Fails fast if any model cannot be loaded.
 *
 * @param config Satellite configuration with model paths
 * @return Allocated context, or NULL on failure
 */
voice_ctx_t *voice_processing_init(const struct satellite_config *config);

/**
 * @brief Cleanup voice processing context
 *
 * Frees all resources including loaded models.
 *
 * @param ctx Voice processing context
 */
void voice_processing_cleanup(voice_ctx_t *ctx);

/**
 * @brief Main voice processing loop
 *
 * Single-threaded polling loop (appropriate for Pi Zero 2 W):
 * - 32ms polling interval (matches VAD frame size)
 * - lws_service() with 10ms timeout for WebSocket events
 * - Non-blocking audio capture and playback
 *
 * @param ctx Voice processing context
 * @param sat_ctx Satellite context (for audio devices)
 * @param ws WebSocket client (for server communication)
 * @param config Configuration (for tuning parameters)
 * @return 0 on clean exit, non-zero on error
 */
int voice_processing_loop(voice_ctx_t *ctx,
                          struct satellite_ctx *sat_ctx,
                          struct ws_client *ws,
                          const struct satellite_config *config);

/**
 * @brief Get current voice processing state
 *
 * @param ctx Voice processing context
 * @return Current state
 */
voice_state_t voice_processing_get_state(const voice_ctx_t *ctx);

/**
 * @brief Get state name for logging/display
 *
 * @param state Voice state
 * @return Human-readable state name
 */
const char *voice_state_name(voice_state_t state);

/**
 * @brief Stop voice processing (signal loop to exit)
 *
 * @param ctx Voice processing context
 */
void voice_processing_stop(voice_ctx_t *ctx);

/**
 * @brief Trigger manual wake (bypass wake word)
 *
 * Thread-safe: sets atomic flag checked by voice loop next iteration (~32ms).
 * Only effective when in SILENCE state.
 *
 * @param ctx Voice processing context
 */
void voice_processing_trigger_wake(voice_ctx_t *ctx);

/**
 * @brief Cancel current operation and return to SILENCE
 *
 * Thread-safe: sets atomic flag and immediately sets tts_stop_flag.
 * State reset happens on next loop iteration.
 *
 * @param ctx Voice processing context
 */
void voice_processing_cancel(voice_ctx_t *ctx);

/**
 * @brief Get current VAD speech probability
 *
 * Returns the most recent speech probability from VAD processing.
 * Safe to call from any thread (volatile float, atomic on ARM).
 *
 * @param ctx Voice processing context
 * @return Speech probability 0.0-1.0, or 0.0 if ctx is NULL
 */
float voice_processing_get_vad_probability(const voice_ctx_t *ctx);

/**
 * @brief Copy current response text to caller buffer
 *
 * Thread-safe: acquires response_mutex internally.
 *
 * @param ctx Voice processing context
 * @param buf Destination buffer
 * @param buf_size Size of destination buffer
 * @return Number of bytes copied (excluding null terminator)
 */
size_t voice_processing_get_response_text(voice_ctx_t *ctx, char *buf, size_t buf_size);

/**
 * @brief Get current audio playback amplitude
 *
 * Returns the RMS amplitude of the most recent playback chunk.
 * Safe to call from any thread (volatile float read).
 *
 * @param ctx Voice processing context
 * @return Amplitude 0.0-1.0, or 0.0 if not playing
 */
float voice_processing_get_playback_amplitude(const voice_ctx_t *ctx);

/**
 * @brief Check if the response is complete
 *
 * Thread-safe atomic read. Returns true when the voice processing loop
 * has finished receiving a complete response from the server.
 *
 * @param ctx Voice processing context
 * @return true if response is complete
 */
bool voice_processing_is_response_complete(voice_ctx_t *ctx);

/**
 * @brief Speak a time-of-day greeting via TTS
 *
 * Plays "Good morning/day/evening" based on current time.
 * Should be called after init and before starting the main loop.
 *
 * @param ctx Voice processing context (with TTS loaded)
 * @param sat_ctx Satellite context (for audio playback)
 */
void voice_processing_speak_greeting(voice_ctx_t *ctx, struct satellite_ctx *sat_ctx);

/**
 * @brief Speak offline fallback message via TTS
 *
 * Plays "I'm sorry, I can't reach the server right now."
 * Should be called when connection to daemon fails or is lost.
 *
 * @param ctx Voice processing context (with TTS loaded)
 * @param sat_ctx Satellite context (for audio playback)
 */
void voice_processing_speak_offline(voice_ctx_t *ctx, struct satellite_ctx *sat_ctx);

/**
 * @brief Copy current status detail from WebSocket client
 *
 * Thread-safe: delegates to ws_client_get_status_detail().
 *
 * @param ctx Voice processing context
 * @param buf Destination buffer
 * @param buf_size Size of destination buffer
 * @return Number of bytes copied (excluding null terminator)
 */
size_t voice_processing_get_status_detail(voice_ctx_t *ctx, char *buf, size_t buf_size);

/**
 * @brief Copy current playback spectrum data to caller buffer
 *
 * Copies the most recent FFT magnitude bins from playback audio.
 * Volatile array — torn reads accepted for visualization purposes.
 *
 * @param ctx Voice processing context
 * @param out Destination buffer (at least count floats)
 * @param count Number of bins to copy (max SPECTRUM_BINS)
 */
void voice_processing_get_playback_spectrum(const voice_ctx_t *ctx, float *out, int count);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_PROCESSING_H */
