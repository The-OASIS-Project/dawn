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
 * Audio Resampler for DAWN
 *
 * Provides high-quality sample rate conversion using libsamplerate.
 * Used primarily to convert TTS output (22050Hz) to AEC reference (16kHz).
 *
 * Design Constraints:
 * - Pre-allocated buffers (no malloc in processing path)
 * - Fixed maximum chunk size to bound memory usage
 * - Thread-safe per-instance (each thread should have its own resampler)
 */

#ifndef RESAMPLER_H
#define RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum samples that can be processed in one call
 *
 * Pre-allocated buffer size. TTS typically sends 1024-sample chunks,
 * so 8192 provides generous headroom.
 */
#define RESAMPLER_MAX_SAMPLES 8192

/**
 * @brief Opaque resampler handle
 */
typedef struct resampler_t resampler_t;

/**
 * @brief Converter quality tier
 *
 * Maps to a libsamplerate converter internally without leaking the SRC_*
 * constants to callers. BEST is the cleanest (and most CPU-expensive) sinc
 * filter — required for ASR downsampling and hi-fi music. MEDIUM is roughly
 * 4-5x cheaper and inaudible for speech, so it fits the TTS output path where
 * a large synchronous resample would otherwise spike CPU and starve other
 * real-time audio threads.
 */
typedef enum {
   RESAMPLER_QUALITY_BEST = 0, /**< SRC_SINC_BEST_QUALITY — ASR/music (default) */
   RESAMPLER_QUALITY_MEDIUM,   /**< SRC_SINC_MEDIUM_QUALITY — voice/TTS output */
} resampler_quality_t;

/**
 * @brief Create a resampler instance with pre-allocated buffers
 *
 * Allocates all memory upfront. Processing calls will not allocate.
 * Uses the best-quality converter; equivalent to resampler_create_ex() with
 * RESAMPLER_QUALITY_BEST.
 *
 * @param src_rate Source sample rate (e.g., 22050)
 * @param dst_rate Destination sample rate (e.g., 16000)
 * @param channels Number of channels (1 for mono)
 * @return Resampler handle, or NULL on error
 */
resampler_t *resampler_create(int src_rate, int dst_rate, int channels);

/**
 * @brief Create a resampler instance with an explicit quality tier
 *
 * @param src_rate Source sample rate (e.g., 22050)
 * @param dst_rate Destination sample rate (e.g., 48000)
 * @param channels Number of channels (1 for mono)
 * @param quality Converter quality tier (see resampler_quality_t)
 * @return Resampler handle, or NULL on error
 */
resampler_t *resampler_create_ex(int src_rate,
                                 int dst_rate,
                                 int channels,
                                 resampler_quality_t quality);

/**
 * @brief Destroy resampler instance and free all resources
 *
 * @param rs Resampler handle (NULL safe)
 */
void resampler_destroy(resampler_t *rs);

/**
 * @brief Resample audio data (no allocation)
 *
 * Input must not exceed RESAMPLER_MAX_SAMPLES.
 * Output buffer must be large enough for resampled data
 * (use resampler_get_output_size() to calculate).
 *
 * @param rs Resampler handle
 * @param in Input samples (16-bit signed)
 * @param in_samples Number of input samples (must be <= RESAMPLER_MAX_SAMPLES)
 * @param out Output buffer
 * @param out_samples_max Maximum output samples (buffer size)
 * @return Number of output samples produced, 0 on error
 */
size_t resampler_process(resampler_t *rs,
                         const int16_t *in,
                         size_t in_samples,
                         int16_t *out,
                         size_t out_samples_max);

/**
 * @brief Calculate required output buffer size for given input
 *
 * @param rs Resampler handle
 * @param in_samples Number of input samples
 * @return Required output buffer size in samples
 */
size_t resampler_get_output_size(resampler_t *rs, size_t in_samples);

/**
 * @brief Reset resampler state (clear internal buffers)
 *
 * Call this when audio stream is discontinuous.
 *
 * @param rs Resampler handle
 */
void resampler_reset(resampler_t *rs);

#ifdef __cplusplus
}
#endif

#endif  // RESAMPLER_H
