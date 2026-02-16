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
 */

#ifndef ASR_INTERFACE_H
#define ASR_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asr/asr_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file asr_interface.h
 * @brief Unified ASR (Automatic Speech Recognition) abstraction layer
 *
 * This interface provides a polymorphic dispatch system for multiple ASR engines,
 * allowing runtime selection between Vosk and Whisper without code changes.
 *
 * **THREAD SAFETY:**
 * - asr_context_t is NOT thread-safe
 * - Do NOT share a single context across multiple threads
 * - Create separate contexts for concurrent sessions (e.g., per network client)
 * - Thread-local or mutex-protected access required if sharing unavoidable
 *
 * **MEMORY MANAGEMENT:**
 * - Caller owns asr_result_t and must call asr_result_free()
 * - Context owns internal buffers and engine state
 * - Call asr_cleanup() to free all context resources
 *
 * **BEHAVIORAL DIFFERENCES:**
 * - Vosk: Supports streaming partial results (real-time transcription)
 * - Whisper: Batch processing only (returns empty partials, final result at finalize)
 */

/**
 * @brief ASR engine types
 */
typedef enum {
   ASR_ENGINE_VOSK = 0,   /**< Vosk ASR engine (Kaldi-based, supports streaming) */
   ASR_ENGINE_WHISPER = 1 /**< Whisper ASR engine (OpenAI, batch processing) */
} asr_engine_type_t;

/**
 * @brief Opaque ASR context handle
 */
typedef struct asr_context asr_context_t;

/**
 * @brief Initialize ASR engine
 *
 * Creates and initializes an ASR context with the specified engine type.
 * For Vosk: model_path should point to Vosk model directory
 * For Whisper: model_path should point to .bin model file
 *
 * @param engine_type ASR engine to use (Vosk or Whisper)
 * @param model_path Path to model directory or file
 * @param sample_rate Audio sample rate (typically 16000)
 * @return Initialized ASR context, or NULL on error
 */
asr_context_t *asr_init(asr_engine_type_t engine_type, const char *model_path, int sample_rate);

/**
 * @brief Process audio and get partial result
 *
 * Feeds audio data to ASR engine and returns partial transcription.
 * Audio is buffered internally; call asr_reset() to clear buffer.
 *
 * **Engine-Specific Behavior:**
 * - Vosk: Returns real-time partial results as transcription progresses
 * - Whisper: Accumulates audio but returns empty partials (batch-only processing)
 *
 * Partial results are intermediate and may change as more audio is processed.
 *
 * @param ctx ASR context (must not be NULL)
 * @param audio_data PCM audio data (16-bit signed, mono, must not be NULL)
 * @param num_samples Number of samples in audio_data
 * @return Partial ASR result, or NULL on error. Caller must call asr_result_free()
 */
asr_result_t *asr_process_partial(asr_context_t *ctx,
                                  const int16_t *audio_data,
                                  size_t num_samples);

/**
 * @brief Finalize processing and get final result
 *
 * Signals end of utterance and returns final transcription.
 * Processes all accumulated audio since last asr_reset().
 *
 * **Engine-Specific Behavior:**
 * - Vosk: Returns final result combining all partial results
 * - Whisper: Performs batch inference on entire accumulated audio buffer
 *
 * This should be called after all audio for an utterance has been fed via asr_process_partial().
 *
 * @param ctx ASR context (must not be NULL)
 * @return Final ASR result, or NULL on error. Caller must call asr_result_free()
 */
asr_result_t *asr_finalize(asr_context_t *ctx);

/**
 * @brief Reset ASR state for new utterance
 *
 * Clears internal audio buffer and resets recognition state.
 *
 * **Usage:**
 * - Before processing a new user command (after PROCESS_COMMAND)
 * - Between chunks when using chunking_manager (mid-utterance)
 *
 * **Engine-Specific Behavior:**
 * - **Whisper**: Safe to call mid-utterance for chunking (stateless per-chunk inference)
 * - **Vosk**: May affect streaming context (avoid mid-utterance reset)
 *
 * **Thread Safety:** NOT thread-safe. Call from same thread as asr_process_partial().
 *
 * @param ctx ASR context
 * @return ASR_SUCCESS (0) on success, ASR_ERROR_GENERIC on error
 */
int asr_reset(asr_context_t *ctx);

/**
 * @brief Free ASR result structure
 *
 * Releases memory allocated by asr_process_partial() or asr_finalize().
 *
 * @param result ASR result to free
 */
void asr_result_free(asr_result_t *result);

/**
 * @brief Clean up ASR context
 *
 * Releases all resources associated with the ASR context.
 *
 * @param ctx ASR context to free
 */
void asr_cleanup(asr_context_t *ctx);

/**
 * @brief Get engine type name as string
 *
 * @param engine_type Engine type
 * @return String representation ("Vosk" or "Whisper")
 */
const char *asr_engine_name(asr_engine_type_t engine_type);

/**
 * @brief Get the engine type of an ASR context
 *
 * Returns the engine type (Vosk or Whisper) that this context was initialized with.
 * Useful for conditional logic and validation (e.g., chunking manager Whisper-only check).
 *
 * @param ctx ASR context (must not be NULL)
 * @return Engine type (ASR_ENGINE_VOSK or ASR_ENGINE_WHISPER), or -1 if ctx is NULL
 */
asr_engine_type_t asr_get_engine_type(asr_context_t *ctx);

// ============================================================================
// ASR Audio Recording API for Debugging
// ============================================================================

/**
 * @brief Set directory for ASR recording output files
 *
 * @param dir Directory path (default: /tmp)
 */
void asr_set_recording_dir(const char *dir);

/**
 * @brief Enable or disable ASR recording capability
 *
 * Must be called with true before asr_start_recording() will work.
 *
 * @param enable true to enable, false to disable
 */
void asr_enable_recording(bool enable);

/**
 * @brief Check if ASR recording is currently active
 *
 * @return true if actively recording, false otherwise
 */
bool asr_is_recording(void);

/**
 * @brief Check if ASR recording capability is enabled
 *
 * @return true if recording is enabled, false otherwise
 */
bool asr_is_recording_enabled(void);

/**
 * @brief Start recording ASR audio streams
 *
 * Creates two WAV files with timestamped names:
 * - asr_pre_YYYYMMDD_HHMMSS.wav - Pre-normalization audio (raw input)
 * - asr_post_YYYYMMDD_HHMMSS.wav - Post-normalization audio (what ASR sees)
 *
 * Recording must be enabled first with asr_enable_recording(true).
 *
 * @return 0 on success, non-zero on error
 */
int asr_start_recording(void);

/**
 * @brief Stop recording and finalize WAV files
 *
 * Closes all recording files and updates WAV headers with final sizes.
 * Safe to call even if not recording.
 */
void asr_stop_recording(void);

#ifdef __cplusplus
}
#endif

#endif  // ASR_INTERFACE_H
