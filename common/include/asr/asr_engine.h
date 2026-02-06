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
 * Unified ASR engine abstraction for the common library.
 *
 * Provides a polymorphic dispatch layer over Whisper (batch) and Vosk
 * (streaming) ASR backends. Both engines can be compiled in simultaneously;
 * the engine is selected at runtime via config.
 *
 * Whisper: accumulates audio in process(), runs inference in finalize()
 * Vosk: decodes incrementally in process(), finalize() is near-instant
 */

#ifndef DAWN_COMMON_ASR_ENGINE_H
#define DAWN_COMMON_ASR_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "asr/asr_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supported ASR engine types
 */
typedef enum {
   ASR_ENGINE_WHISPER = 0,
   ASR_ENGINE_VOSK = 1,
} asr_engine_type_t;

/**
 * @brief Opaque unified ASR engine context
 */
typedef struct asr_engine_context asr_engine_context_t;

/**
 * @brief Unified ASR engine configuration
 */
typedef struct {
   asr_engine_type_t engine; /**< ASR_ENGINE_WHISPER or ASR_ENGINE_VOSK */
   const char *model_path;   /**< Path to model file/directory */
   int sample_rate;          /**< Audio sample rate (typically 16000) */
   int use_gpu;              /**< Enable GPU acceleration (Whisper only) */
   int n_threads;            /**< CPU threads for inference (Whisper only) */
   const char *language;     /**< Language code, e.g. "en" (Whisper only) */
   size_t max_audio_seconds; /**< Max audio buffer in seconds (Whisper only) */
} asr_engine_config_t;

/**
 * @brief Initialize ASR engine
 *
 * Creates the appropriate backend (Whisper or Vosk) based on config.
 *
 * @param config Engine configuration
 * @return Opaque context pointer, or NULL on error
 */
asr_engine_context_t *asr_engine_init(const asr_engine_config_t *config);

/**
 * @brief Process audio chunk
 *
 * Behavior depends on engine type:
 * - Whisper: accumulates audio internally, returns empty partial
 * - Vosk: decodes incrementally, returns live partial transcription
 *
 * @param ctx Engine context
 * @param audio PCM audio data (16-bit signed, mono)
 * @param samples Number of samples
 * @return Partial result, or NULL on error. Caller must free via asr_engine_result_free().
 */
asr_result_t *asr_engine_process(asr_engine_context_t *ctx, const int16_t *audio, size_t samples);

/**
 * @brief Finalize processing and get transcription
 *
 * Behavior depends on engine type:
 * - Whisper: runs batch inference on accumulated audio (~seconds)
 * - Vosk: returns final result near-instantly
 *
 * @param ctx Engine context
 * @return Final transcription result, or NULL on error
 */
asr_result_t *asr_engine_finalize(asr_engine_context_t *ctx);

/**
 * @brief Reset for new utterance
 *
 * Clears internal state (audio buffer for Whisper, recognizer for Vosk).
 *
 * @param ctx Engine context
 * @return ASR_SUCCESS on success, error code on failure
 */
int asr_engine_reset(asr_engine_context_t *ctx);

/**
 * @brief Free result structure
 *
 * @param result Result to free (can be NULL)
 */
void asr_engine_result_free(asr_result_t *result);

/**
 * @brief Clean up engine context
 *
 * @param ctx Engine context to free (can be NULL)
 */
void asr_engine_cleanup(asr_engine_context_t *ctx);

/**
 * @brief Get engine type
 *
 * @param ctx Engine context
 * @return Engine type, or -1 if ctx is NULL
 */
asr_engine_type_t asr_engine_get_type(asr_engine_context_t *ctx);

/**
 * @brief Get engine name string
 *
 * @param engine_type Engine type
 * @return Human-readable engine name ("Whisper", "Vosk", or "Unknown")
 */
const char *asr_engine_name(asr_engine_type_t engine_type);

/**
 * @brief Set optional timing callback
 *
 * @param ctx Engine context
 * @param callback Callback function, or NULL to disable
 * @param user_data User-provided context passed to callback
 */
void asr_engine_set_timing_callback(asr_engine_context_t *ctx,
                                    asr_timing_callback_t callback,
                                    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_ASR_ENGINE_H */
