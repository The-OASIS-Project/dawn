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
 * @file tts_piper.h
 * @brief Piper TTS (Text-to-Speech) wrapper for common library
 *
 * This module provides a simplified C interface to the Piper TTS engine
 * for speech synthesis. It generates raw PCM audio from text input.
 *
 * **Usage Pattern:**
 * 1. Call tts_piper_init() with model/config paths
 * 2. Call tts_piper_synthesize() to generate PCM from text
 * 3. Call tts_piper_cleanup() when done
 *
 * **Thread Safety:**
 * - Each context is independent
 * - Do not share a context between threads without synchronization
 * - Use mutex protection if calling from multiple threads
 */

#ifndef DAWN_COMMON_TTS_PIPER_H
#define DAWN_COMMON_TTS_PIPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default sample rate for Piper TTS output */
#define TTS_PIPER_SAMPLE_RATE 22050

/** @brief Return codes for TTS operations */
#define TTS_SUCCESS 0
#define TTS_FAILURE 1
#define TTS_ERR_INVALID_PARAM 2
#define TTS_ERR_MODEL_LOAD 3
#define TTS_ERR_OUT_OF_MEMORY 4
#define TTS_ERR_SYNTHESIS 5

/**
 * @brief Opaque Piper TTS context
 *
 * Created by tts_piper_init(), destroyed by tts_piper_cleanup().
 */
typedef struct tts_piper_context tts_piper_context_t;

/**
 * @brief TTS synthesis result
 *
 * Contains timing information from synthesis.
 */
typedef struct {
   double infer_seconds;    /**< Time spent in neural network inference */
   double audio_seconds;    /**< Duration of generated audio */
   double real_time_factor; /**< Ratio of infer_seconds to audio_seconds */
} tts_piper_result_t;

/**
 * @brief Callback type for TTS timing metrics
 *
 * Optional callback invoked after synthesis with timing statistics.
 *
 * @param infer_ms Inference time in milliseconds
 * @param rtf Real-time factor (infer_time / audio_duration)
 * @param user_data User-provided context
 */
typedef void (*tts_timing_callback_t)(double infer_ms, double rtf, void *user_data);

/**
 * @brief Piper TTS configuration
 */
typedef struct {
   const char *model_path;        /**< Path to .onnx model file */
   const char *model_config_path; /**< Path to .onnx.json config file */
   const char *espeak_data_path;  /**< Path to espeak-ng-data directory */
   float length_scale;            /**< Speech rate (1.0 = normal, <1 = faster, >1 = slower) */
   int use_cuda;                  /**< 1 to enable CUDA acceleration, 0 for CPU only */
} tts_piper_config_t;

/**
 * @brief Get default configuration
 *
 * @return Default configuration structure
 */
tts_piper_config_t tts_piper_default_config(void);

/**
 * @brief Initialize Piper TTS engine
 *
 * Loads the voice model and initializes the synthesis engine.
 *
 * @param config Configuration options
 * @return Opaque context pointer, or NULL on error
 */
tts_piper_context_t *tts_piper_init(const tts_piper_config_t *config);

/**
 * @brief Set optional callback for timing metrics
 *
 * @param ctx TTS context
 * @param callback Callback function, or NULL to disable
 * @param user_data User-provided context passed to callback
 */
void tts_piper_set_timing_callback(tts_piper_context_t *ctx,
                                   tts_timing_callback_t callback,
                                   void *user_data);

/**
 * @brief Synthesize text to PCM audio
 *
 * Generates 16-bit signed PCM audio from text input.
 * The output buffer is allocated by this function; caller must free it.
 *
 * @param ctx TTS context
 * @param text Input text to synthesize
 * @param pcm_out Output: pointer to allocated PCM buffer (caller must free)
 * @param samples_out Output: number of samples in buffer
 * @param result Optional: synthesis timing information
 * @return TTS_SUCCESS on success, error code on failure
 */
int tts_piper_synthesize(tts_piper_context_t *ctx,
                         const char *text,
                         int16_t **pcm_out,
                         size_t *samples_out,
                         tts_piper_result_t *result);

/**
 * @brief Get output sample rate
 *
 * @param ctx TTS context
 * @return Sample rate in Hz (typically 22050)
 */
int tts_piper_get_sample_rate(tts_piper_context_t *ctx);

/**
 * @brief Clean up TTS context
 *
 * @param ctx TTS context to free (can be NULL)
 */
void tts_piper_cleanup(tts_piper_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_TTS_PIPER_H */
