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
 * Vosk ASR (Automatic Speech Recognition) wrapper for common library.
 *
 * Vosk processes audio incrementally (streaming). Each call to process()
 * feeds audio and returns partial results. finalize() returns near-instantly
 * since audio has already been decoded.
 *
 * Usage Pattern:
 *   1. Call asr_vosk_init() with model path
 *   2. Feed audio via asr_vosk_process() (streaming, returns partials)
 *   3. Call asr_vosk_finalize() to get final result (~instant)
 *   4. Call asr_vosk_reset() to start new utterance
 *   5. Call asr_vosk_cleanup() when done
 */

#ifndef DAWN_COMMON_ASR_VOSK_H
#define DAWN_COMMON_ASR_VOSK_H

#include <stddef.h>
#include <stdint.h>

#include "asr/asr_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque Vosk ASR context
 */
typedef struct vosk_asr_context vosk_asr_context_t;

/**
 * @brief Vosk initialization options
 */
typedef struct {
   const char *model_path; /**< Path to Vosk model directory */
   int sample_rate;        /**< Audio sample rate (typically 16000) */
} asr_vosk_config_t;

/**
 * @brief Initialize Vosk ASR engine
 *
 * @param config Configuration options
 * @return Opaque context pointer, or NULL on error
 */
vosk_asr_context_t *asr_vosk_init(const asr_vosk_config_t *config);

/**
 * @brief Set optional callback for timing metrics
 *
 * @param ctx Vosk context
 * @param callback Callback function, or NULL to disable
 * @param user_data User-provided context passed to callback
 */
void asr_vosk_set_timing_callback(vosk_asr_context_t *ctx,
                                  asr_timing_callback_t callback,
                                  void *user_data);

/**
 * @brief Process audio chunk (streaming)
 *
 * Feeds audio to Vosk and returns partial transcription.
 * Unlike Whisper, Vosk decodes incrementally so partial text
 * is available immediately.
 *
 * @param ctx Vosk context
 * @param audio PCM audio data (16-bit signed, mono)
 * @param samples Number of samples
 * @return Partial result with live transcription, or NULL on error
 */
asr_result_t *asr_vosk_process(vosk_asr_context_t *ctx, const int16_t *audio, size_t samples);

/**
 * @brief Finalize processing and get final result
 *
 * Returns near-instantly since Vosk has already decoded the audio
 * incrementally during process() calls.
 *
 * @param ctx Vosk context
 * @return Final transcription result, or NULL on error
 */
asr_result_t *asr_vosk_finalize(vosk_asr_context_t *ctx);

/**
 * @brief Reset for new utterance
 *
 * @param ctx Vosk context
 * @return ASR_SUCCESS on success, error code on failure
 */
int asr_vosk_reset(vosk_asr_context_t *ctx);

/**
 * @brief Free result structure
 *
 * @param result Result to free (can be NULL)
 */
void asr_vosk_result_free(asr_result_t *result);

/**
 * @brief Clean up Vosk context
 *
 * @param ctx Vosk context to free (can be NULL)
 */
void asr_vosk_cleanup(vosk_asr_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_ASR_VOSK_H */
