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

/**
 * @file asr_whisper.h
 * @brief Whisper ASR (Automatic Speech Recognition) wrapper for common library
 *
 * This module provides a C interface to the whisper.cpp library for
 * speech-to-text transcription. It accumulates audio and performs
 * batch processing when finalize() is called.
 *
 * **Usage Pattern:**
 * 1. Call asr_whisper_init() with model path
 * 2. Feed audio via asr_whisper_process() (accumulates internally)
 * 3. Call asr_whisper_finalize() to get transcription
 * 4. Call asr_whisper_reset() to start new utterance
 * 5. Call asr_whisper_cleanup() when done
 *
 * **Thread Safety:**
 * - Each context is independent
 * - Do not share a context between threads without synchronization
 * - Create separate contexts for concurrent sessions
 */

#ifndef DAWN_COMMON_ASR_WHISPER_H
#define DAWN_COMMON_ASR_WHISPER_H

#include <stddef.h>
#include <stdint.h>

#include "asr/asr_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Expected sample rate for Whisper (16kHz) */
#define WHISPER_SAMPLE_RATE 16000

/**
 * @brief Opaque Whisper ASR context
 *
 * Created by asr_whisper_init(), destroyed by asr_whisper_cleanup().
 */
typedef struct whisper_asr_context whisper_asr_context_t;

/**
 * @brief Whisper-specific result type (alias for common asr_result_t)
 *
 * Kept as a typedef for backward compatibility with existing code.
 */
typedef asr_result_t asr_whisper_result_t;

/**
 * @brief Whisper initialization options
 */
typedef struct {
   const char *model_path;   /**< Path to Whisper model file (.bin) */
   int sample_rate;          /**< Audio sample rate (should be 16000) */
   int use_gpu;              /**< 1 to enable GPU, 0 for CPU only */
   int n_threads;            /**< Number of CPU threads (default: 4) */
   const char *language;     /**< Language code (default: "en") */
   size_t max_audio_seconds; /**< Max audio buffer size in seconds (default: 60) */
} asr_whisper_config_t;

/**
 * @brief Get default configuration
 *
 * @return Default configuration structure
 */
asr_whisper_config_t asr_whisper_default_config(void);

/**
 * @brief Initialize Whisper ASR engine
 *
 * @param config Configuration options
 * @return Opaque context pointer, or NULL on error
 */
whisper_asr_context_t *asr_whisper_init(const asr_whisper_config_t *config);

/**
 * @brief Set optional callback for timing metrics
 *
 * @param ctx Whisper context
 * @param callback Callback function, or NULL to disable
 * @param user_data User-provided context passed to callback
 */
void asr_whisper_set_timing_callback(whisper_asr_context_t *ctx,
                                     asr_timing_callback_t callback,
                                     void *user_data);

/**
 * @brief Process audio chunk
 *
 * Accumulates audio in internal buffer. Whisper is batch-only,
 * so this returns an empty partial result. Use finalize() for transcription.
 *
 * @param ctx Whisper context
 * @param audio PCM audio data (16-bit signed, mono)
 * @param samples Number of samples
 * @return Partial result (empty text), or NULL on error
 */
asr_whisper_result_t *asr_whisper_process(whisper_asr_context_t *ctx,
                                          const int16_t *audio,
                                          size_t samples);

/**
 * @brief Finalize processing and get transcription
 *
 * Runs Whisper inference on all accumulated audio.
 *
 * @param ctx Whisper context
 * @return Final transcription result, or NULL on error
 */
asr_whisper_result_t *asr_whisper_finalize(whisper_asr_context_t *ctx);

/**
 * @brief Reset for new utterance
 *
 * Clears accumulated audio buffer.
 *
 * @param ctx Whisper context
 * @return ASR_SUCCESS on success, error code on failure
 */
int asr_whisper_reset(whisper_asr_context_t *ctx);

/**
 * @brief Free result structure
 *
 * @param result Result to free (can be NULL)
 */
void asr_whisper_result_free(asr_whisper_result_t *result);

/**
 * @brief Clean up Whisper context
 *
 * @param ctx Whisper context to free (can be NULL)
 */
void asr_whisper_cleanup(whisper_asr_context_t *ctx);

/**
 * @brief Get current buffer size in samples
 *
 * @param ctx Whisper context
 * @return Number of samples in buffer, or 0 if ctx is NULL
 */
size_t asr_whisper_get_buffer_size(whisper_asr_context_t *ctx);

/**
 * @brief Get buffer duration in milliseconds
 *
 * @param ctx Whisper context
 * @return Duration in milliseconds, or 0 if ctx is NULL
 */
double asr_whisper_get_buffer_duration_ms(whisper_asr_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_ASR_WHISPER_H */
