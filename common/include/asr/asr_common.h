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
 * Common ASR types and return codes shared by all ASR engine backends.
 */

#ifndef DAWN_COMMON_ASR_COMMON_H
#define DAWN_COMMON_ASR_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return codes for ASR operations */
#define ASR_SUCCESS 0
#define ASR_FAILURE 1
#define ASR_ERR_INVALID_PARAM 2
#define ASR_ERR_MODEL_LOAD 3
#define ASR_ERR_OUT_OF_MEMORY 4
#define ASR_ERR_PROCESSING 5

/**
 * @brief ASR result structure
 *
 * Common result type returned by all ASR engine backends.
 * Contains transcription text and metadata from ASR processing.
 * Caller owns this structure and must free it via the engine's result_free().
 */
typedef struct {
   char *text;             /**< Transcribed text (caller must free) */
   float confidence;       /**< Confidence score (0.0-1.0, or -1.0 if unavailable) */
   int is_partial;         /**< 1 if partial result (Vosk streaming), 0 if final */
   double processing_time; /**< Processing time in milliseconds */
} asr_result_t;

/**
 * @brief Callback type for ASR timing metrics
 *
 * Optional callback invoked after finalize() with processing statistics.
 *
 * @param processing_time_ms Time spent in ASR inference (milliseconds)
 * @param rtf Real-time factor (processing_time / audio_duration)
 * @param user_data User-provided context
 */
typedef void (*asr_timing_callback_t)(double processing_time_ms, double rtf, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_ASR_COMMON_H */
