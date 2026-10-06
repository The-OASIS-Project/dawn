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
 * WebUI Audio Processing - Opus codec and ASR integration for browser clients
 *
 * This module handles:
 * - Opus decoding of audio from browser (WebSocket binary frames)
 * - ASR transcription using a shared Whisper context
 * - TTS synthesis for responses
 * - Opus encoding for audio playback in browser
 *
 * Thread Safety:
 * - webui_audio_init/cleanup must be called from main thread
 * - All other functions are thread-safe (use internal mutex for ASR)
 * - Opus codec operations are per-call stateless (stateful encoder/decoder)
 */

#ifndef WEBUI_AUDIO_H
#define WEBUI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward decl: a caller-owned ASR context (full type in asr/asr_interface.h). */
typedef struct asr_context asr_context_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Opus configuration for voice */
/* 48kHz is Opus native rate - browser uses this, server resamples for ASR */
#define WEBUI_OPUS_SAMPLE_RATE 48000 /* 48kHz - Opus native rate */
#define WEBUI_ASR_SAMPLE_RATE 16000  /* 16kHz for ASR/Whisper */
#define WEBUI_OPUS_CHANNELS 1        /* Mono */
#define WEBUI_OPUS_BITRATE 48000     /* 48kbps - full quality for TTS */
#define WEBUI_OPUS_FRAME_MS 20       /* 20ms frames (standard) */
#define WEBUI_OPUS_FRAME_SAMPLES (WEBUI_OPUS_SAMPLE_RATE * WEBUI_OPUS_FRAME_MS / 1000) /* 960 */

/* Max recording duration (shared with webui_server.h) */
#define WEBUI_MAX_RECORDING_SECONDS 30

/* Buffer sizes */
#define WEBUI_OPUS_MAX_FRAME_SIZE 1276 /* Max Opus frame size */
#define WEBUI_PCM_MAX_SAMPLES                                         \
   (WEBUI_OPUS_SAMPLE_RATE * WEBUI_MAX_RECORDING_SECONDS) /* at 48kHz \
                                                           */

/* TTS pacing: how far ahead of real-time to allow audio buffering.
 * 1s is enough for uninterrupted playback without the burst problem. */
#define WEBUI_TTS_PACE_LOOKAHEAD_US 1000000ULL /* 1s lookahead buffer */
#define WEBUI_TTS_PACE_SLEEP_MIN_US 5000ULL    /* 5ms minimum (skip tiny sleeps) */
#define WEBUI_TTS_PACE_SLEEP_MAX_US 5000000ULL /* 5s cap (clamped, not skipped) */
#define WEBUI_TTS_PACE_CHECK_US 500000ULL      /* 500ms disconnect/cancel check interval */

/* Error codes */
#define WEBUI_AUDIO_SUCCESS 0
#define WEBUI_AUDIO_ERROR 1
#define WEBUI_AUDIO_ERROR_NOT_INITIALIZED 2
#define WEBUI_AUDIO_ERROR_DECODE 3
#define WEBUI_AUDIO_ERROR_ENCODE 4
#define WEBUI_AUDIO_ERROR_ASR 5
#define WEBUI_AUDIO_ERROR_ALLOC 6

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the WebUI audio subsystem
 *
 * Creates:
 * - Opus decoder for incoming audio from browser
 * - Opus encoder for outgoing TTS audio to browser
 * - Shared ASR context (Whisper) for transcription
 *
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Must be called after ASR engine is initialized (worker_pool_init)
 * @note Must be called from main thread before WebUI server starts
 */
int webui_audio_init(void);

/**
 * @brief Cleanup the WebUI audio subsystem
 *
 * Destroys all Opus codec contexts and ASR context.
 *
 * @note Must be called from main thread during shutdown
 * @note Safe to call if not initialized (no-op)
 */
void webui_audio_cleanup(void);

/**
 * @brief Check if WebUI audio subsystem is initialized
 *
 * @return true if initialized and ready, false otherwise
 */
bool webui_audio_is_initialized(void);

/* =============================================================================
 * Opus Decoding Functions
 * ============================================================================= */

/**
 * @brief Decode a stream of Opus frames to PCM
 *
 * The input buffer contains concatenated Opus frames with length prefixes:
 * [2-byte length][opus frame][2-byte length][opus frame]...
 *
 * @param opus_data Buffer containing length-prefixed Opus frames
 * @param opus_len Total size of input buffer
 * @param pcm_out Output: allocated PCM buffer (caller must free)
 * @param pcm_samples Output: number of PCM samples decoded
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe (uses internal mutex)
 * @note Caller is responsible for freeing *pcm_out
 */
int webui_opus_decode_stream(const uint8_t *opus_data,
                             size_t opus_len,
                             int16_t **pcm_out,
                             size_t *pcm_samples);

/**
 * @brief Decode a single Opus frame to PCM
 *
 * @param opus_frame Single Opus frame data
 * @param opus_len Size of Opus frame
 * @param pcm_out Output PCM buffer (must be pre-allocated)
 * @param max_samples Maximum samples to decode
 * @return Number of samples decoded, or negative error code
 *
 * @note Thread-safe (uses internal mutex)
 */
int webui_opus_decode_frame(const uint8_t *opus_frame,
                            size_t opus_len,
                            int16_t *pcm_out,
                            int max_samples);

/* =============================================================================
 * Opus Encoding Functions
 * ============================================================================= */

/**
 * @brief Encode PCM audio to Opus stream with length prefixes
 *
 * Output format: [2-byte length][opus frame][2-byte length][opus frame]...
 * This matches the format expected by webui_opus_decode_stream().
 *
 * @param pcm_data Input PCM samples (16-bit signed, mono, 16kHz)
 * @param pcm_samples Number of input samples
 * @param opus_out Output: allocated buffer with length-prefixed Opus frames
 * @param opus_len Output: total size of encoded data
 * @param frame_count_out Output: number of Opus frames encoded (optional, NULL to ignore)
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe (uses internal mutex)
 * @note Caller is responsible for freeing *opus_out
 */
int webui_opus_encode_stream(const int16_t *pcm_data,
                             size_t pcm_samples,
                             uint8_t **opus_out,
                             size_t *opus_len,
                             size_t *frame_count_out);

/* =============================================================================
 * ASR Integration Functions
 * ============================================================================= */

/**
 * @brief Transcribe PCM audio to text using ASR
 *
 * Uses the shared Whisper ASR context to transcribe audio.
 *
 * @param pcm_data PCM samples (16-bit signed, mono, 16kHz)
 * @param pcm_samples Number of samples
 * @param text_out Output: transcribed text (caller must free)
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe (uses internal mutex for ASR context)
 * @note Caller is responsible for freeing *text_out
 */
int webui_audio_transcribe(const int16_t *pcm_data, size_t pcm_samples, char **text_out);

/**
 * @brief Transcribe 16kHz PCM on a CALLER-OWNED ASR context (no borrow/return).
 *
 * Same decode as webui_audio_transcribe() but operates on a context the caller
 * has already obtained (e.g. via worker_pool_try_borrow_asr()) and will return
 * itself. Lets opportunistic/speculative work manage the context lifecycle so it
 * never blocks in the pool's borrow path.
 *
 * @param asr_ctx     A borrowed ASR context; the caller owns borrow AND return.
 * @param pcm_data    PCM samples (16-bit signed, mono, 16kHz).
 * @param pcm_samples Number of samples.
 * @param text_out    Output: transcribed text (caller must free).
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure.
 */
int webui_audio_transcribe_on_ctx(asr_context_t *asr_ctx,
                                  const int16_t *pcm_data,
                                  size_t pcm_samples,
                                  char **text_out);

/**
 * @brief Complete audio processing pipeline: Opus → ASR → text
 *
 * Convenience function that decodes Opus stream and runs ASR.
 *
 * @param opus_data Buffer containing length-prefixed Opus frames
 * @param opus_len Total size of input buffer
 * @param text_out Output: transcribed text (caller must free)
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe
 * @note Caller is responsible for freeing *text_out
 */
int webui_audio_opus_to_text(const uint8_t *opus_data, size_t opus_len, char **text_out);

/**
 * @brief Complete audio processing pipeline: 48kHz PCM → resample → ASR → text
 *
 * For non-Opus clients sending raw 48kHz PCM. Resamples to 16kHz and runs ASR.
 *
 * @param pcm_data PCM samples at 48kHz (16-bit signed, mono)
 * @param pcm_samples Number of input samples
 * @param text_out Output: transcribed text (caller must free)
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe
 * @note Caller is responsible for freeing *text_out
 */
int webui_audio_pcm48k_to_text(const int16_t *pcm_data, size_t pcm_samples, char **text_out);

/**
 * @brief 48kHz PCM → resample → ASR → text on a CALLER-OWNED ASR context.
 *
 * The caller-owned-context sibling of webui_audio_pcm48k_to_text(): the caller has
 * already borrowed @p asr_ctx (and will return it). Used by the always-on
 * speculative decode, which try-borrows a context so it never blocks a real turn.
 *
 * @param asr_ctx     A borrowed ASR context; the caller owns borrow AND return.
 * @param pcm_data    PCM samples at 48kHz (16-bit signed, mono).
 * @param pcm_samples Number of input samples.
 * @param text_out    Output: transcribed text (caller must free).
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure.
 */
int webui_audio_pcm48k_to_text_on_ctx(asr_context_t *asr_ctx,
                                      const int16_t *pcm_data,
                                      size_t pcm_samples,
                                      char **text_out);

/* =============================================================================
 * TTS Integration Functions
 * ============================================================================= */

/**
 * @brief Generate TTS audio and encode to Opus
 *
 * Uses Piper TTS to synthesize text, then encodes result to Opus.
 *
 * @param text Input text to synthesize
 * @param opus_out Output: allocated buffer with length-prefixed Opus frames
 * @param opus_len Output: total size of encoded data
 * @param frame_count_out Output: number of Opus frames encoded (optional, NULL to ignore)
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe
 * @note Caller is responsible for freeing *opus_out
 */
int webui_audio_text_to_opus(const char *text,
                             uint8_t **opus_out,
                             size_t *opus_len,
                             size_t *frame_count_out);

/**
 * @brief Generate TTS audio as raw PCM (for browser playback)
 *
 * Uses Piper TTS to synthesize text, resamples to match Opus rate, returns raw PCM.
 * For non-Opus clients that need raw PCM at the same rate as Opus output.
 *
 * @param text Input text to synthesize
 * @param pcm_out Output: allocated buffer with 16-bit PCM samples
 * @param pcm_samples Output: number of samples
 * @return WEBUI_AUDIO_SUCCESS on success, error code on failure
 *
 * @note Thread-safe
 * @note Caller is responsible for freeing *pcm_out
 * @note Output is 48kHz (WEBUI_OPUS_SAMPLE_RATE), mono, 16-bit signed PCM
 */
int webui_audio_text_to_pcm(const char *text, int16_t **pcm_out, size_t *pcm_samples);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_AUDIO_H */
