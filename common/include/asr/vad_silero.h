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
 * @file vad_silero.h
 * @brief Silero VAD (Voice Activity Detection) ONNX Runtime wrapper
 *
 * This module provides a C interface to the Silero VAD model for detecting
 * speech activity in audio streams. It uses ONNX Runtime for inference.
 *
 * The Silero VAD model outputs a probability (0.0-1.0) indicating the
 * likelihood that the audio contains speech. Higher values indicate speech,
 * lower values indicate silence or non-speech audio.
 *
 * **Four Use Cases:**
 * 1. Wake word detection (SILENCE -> WAKEWORD_LISTEN when speech_prob > 0.5)
 * 2. Speech end detection (silence > 1.5s indicates command complete)
 * 3. Pause detection (silence > 0.5s for chunking boundaries)
 * 4. Interruption detection (speech_prob > 0.6 while TTS playing)
 *
 * **Performance:**
 * - Model size: ~1.8MB (half-precision ONNX)
 * - Inference latency: <1ms on Jetson platforms
 * - Input: 512 samples (32ms at 16kHz)
 * - Output: Single float (speech probability)
 *
 * **Thread Safety:**
 * - Each context is independent and thread-safe
 * - Do not share a context between threads without external synchronization
 */

#ifndef DAWN_COMMON_VAD_SILERO_H
#define DAWN_COMMON_VAD_SILERO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque context for Silero VAD
 *
 * Holds ONNX Runtime session, model state, and internal buffers.
 * Created by vad_silero_init(), destroyed by vad_silero_cleanup().
 */
typedef struct silero_vad_context silero_vad_context_t;

/**
 * @brief Callback type for VAD probability updates
 *
 * Optional callback for UI/metrics updates after each inference.
 * Set via vad_silero_set_probability_callback().
 *
 * @param probability Speech probability (0.0-1.0)
 * @param user_data User-provided context
 */
typedef void (*vad_probability_callback_t)(float probability, void *user_data);

/**
 * @brief Set optional callback for VAD probability updates
 *
 * The callback is invoked after each successful inference with the
 * speech probability value. Useful for UI/metrics updates.
 *
 * @param ctx VAD context from vad_silero_init()
 * @param callback Callback function, or NULL to disable
 * @param user_data User-provided context passed to callback
 */
void vad_silero_set_probability_callback(silero_vad_context_t *ctx,
                                         vad_probability_callback_t callback,
                                         void *user_data);

/**
 * @brief Initialize Silero VAD system
 *
 * Loads the ONNX model and initializes the ONNX Runtime session.
 * This function attempts to share the ONNX Runtime environment with
 * Piper (Option A) for optimal resource usage. If shared environment
 * initialization fails, it falls back to creating a separate environment.
 *
 * @param model_path Absolute path to silero_vad.onnx model file
 * @param shared_env Optional shared ONNX Runtime environment (pass NULL for separate env)
 *
 * @return Pointer to initialized VAD context, or NULL on failure
 *
 * @note Call this after initializing Piper TTS to attempt shared environment.
 * @note The context must be freed with vad_silero_cleanup() when done.
 */
silero_vad_context_t *vad_silero_init(const char *model_path, void *shared_env);

/**
 * @brief Process audio chunk and get speech probability
 *
 * Runs Silero VAD inference on 512 samples of audio (32ms at 16kHz).
 * The model maintains internal LSTM state, so consecutive calls are
 * context-aware (past audio influences current predictions).
 *
 * **Performance:** <1ms inference time on Jetson platforms
 *
 * @param ctx VAD context initialized by vad_silero_init()
 * @param audio_samples Pointer to 512 int16_t audio samples at 16kHz
 * @param num_samples Number of samples (must be 512 for Silero VAD)
 *
 * @return Speech probability (0.0-1.0), or -1.0 on error
 *
 * @note Input audio must be 16kHz mono PCM
 * @note Call vad_silero_reset() at interaction boundaries to clear state
 */
float vad_silero_process(silero_vad_context_t *ctx,
                         const int16_t *audio_samples,
                         size_t num_samples);

/**
 * @brief Reset VAD internal state
 *
 * Clears the LSTM hidden and cell state to prevent past audio from
 * influencing current inference. Call this at "epoch boundaries"
 * between distinct user interactions.
 *
 * **When to reset:**
 * - State transitions to SILENCE or WAKEWORD_LISTEN from any other state
 * - After interruption detection (before processing new command)
 * - On command timeout (before returning to idle)
 * - When starting a new wake word detection cycle
 *
 * **Why this matters:**
 * The Silero VAD model maintains internal state (LSTM memory) that
 * accumulates over consecutive audio frames. Without resets, speech
 * from a previous interaction can bias detection in the current
 * interaction, leading to increased false positives or negatives.
 *
 * @param ctx VAD context initialized by vad_silero_init()
 *
 * @note This is a lightweight operation (zeros internal state buffers)
 * @note Not calling reset at interaction boundaries is a common bug
 */
void vad_silero_reset(silero_vad_context_t *ctx);

/**
 * @brief Clean up VAD resources
 *
 * Frees ONNX Runtime session, model state, and all internal buffers.
 * After calling this, the context pointer is invalid and must not be used.
 *
 * @param ctx VAD context to clean up (can be NULL, in which case this is a no-op)
 *
 * @note If using shared ONNX Runtime environment, this does NOT free the shared env
 * @note Always call this before program exit to prevent memory leaks
 */
void vad_silero_cleanup(silero_vad_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_VAD_SILERO_H */
