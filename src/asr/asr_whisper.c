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

#include "asr/asr_whisper.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "dawn.h"
#include "logging.h"
#include "ui/metrics.h"
#include "whisper.h"

/**
 * @brief Maximum audio buffer size (60 seconds at 16kHz = 960,000 samples)
 *
 * Increased from 30s to 60s to handle background conversation and long utterances.
 * WAKEWORD_LISTEN enforces 50s timeout to prevent overflow (see dawn.c).
 * TODO(Week 3): Chunking will eliminate need for large buffer.
 */
#define MAX_AUDIO_BUFFER_SIZE (60 * 16000)

/**
 * @brief Whisper-specific context structure
 */
typedef struct {
   struct whisper_context *ctx;
   int sample_rate;

   // Audio buffer for accumulation (Whisper needs float PCM)
   float *audio_buffer;
   size_t buffer_size;      // Current number of samples in buffer
   size_t buffer_capacity;  // Maximum capacity

   // Whisper processing parameters
   struct whisper_full_params wparams;
} whisper_context_t;

/**
 * @brief Convert int16_t PCM to float PCM
 *
 * @param input int16_t audio data
 * @param output float audio data (normalized to -1.0 to 1.0)
 * @param n Number of samples
 */
static void convert_int16_to_float(const int16_t *input, float *output, size_t n) {
   for (size_t i = 0; i < n; i++) {
      output[i] = (float)input[i] / 32768.0f;
   }
}

void *asr_whisper_init(const char *model_path, int sample_rate) {
   if (!model_path) {
      LOG_ERROR("Whisper: model_path cannot be NULL");
      return NULL;
   }

   if (sample_rate != WHISPER_SAMPLE_RATE) {
      LOG_WARNING("Whisper: Sample rate %d differs from expected %d. Audio may need resampling.",
                  sample_rate, WHISPER_SAMPLE_RATE);
   }

   whisper_context_t *wctx = (whisper_context_t *)calloc(1, sizeof(whisper_context_t));
   if (!wctx) {
      LOG_ERROR("Whisper: Failed to allocate context");
      return NULL;
   }

   wctx->sample_rate = sample_rate;

   // Initialize whisper context with platform-appropriate configuration
   struct whisper_context_params cparams = whisper_context_default_params();

#ifdef HAVE_CUDA_GPU
   // Jetson: GPU-accelerated with flash attention disabled
   // Flash attention causes KV cache alignment crash on Jetson (FATTN_KQ_STRIDE issue)
   cparams.use_gpu = true;
   cparams.flash_attn = false;  // Disable flash attention, keep other GPU acceleration
#else
   // Raspberry Pi and other platforms: CPU only
   cparams.use_gpu = false;
   cparams.flash_attn = false;
#endif

   wctx->ctx = whisper_init_from_file_with_params(model_path, cparams);
   if (!wctx->ctx) {
      LOG_ERROR("Whisper: Failed to load model from: %s", model_path);
      free(wctx);
      return NULL;
   }

   // Allocate audio buffer
   wctx->buffer_capacity = MAX_AUDIO_BUFFER_SIZE;
   wctx->audio_buffer = (float *)calloc(wctx->buffer_capacity, sizeof(float));
   if (!wctx->audio_buffer) {
      LOG_ERROR("Whisper: Failed to allocate audio buffer");
      whisper_free(wctx->ctx);
      free(wctx);
      return NULL;
   }
   wctx->buffer_size = 0;

   // Configure Whisper parameters
   wctx->wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
   wctx->wparams.print_realtime = false;
   wctx->wparams.print_progress = false;
   wctx->wparams.print_timestamps = false;
   wctx->wparams.print_special = false;
   wctx->wparams.translate = false;
   wctx->wparams.language = "en";
   wctx->wparams.n_threads = 4;
   wctx->wparams.offset_ms = 0;
   wctx->wparams.no_context = true;
   wctx->wparams.single_segment = false;

   // Note: initial_prompt was tried to bias towards "Friday" but it causes Whisper
   // to filter out the wake word from output (treats it as already-transcribed text).
   // Left unset - Whisper will transcribe everything including wake word.

   LOG_INFO("Whisper: Initialized successfully (model: %s, sample_rate: %d)", model_path,
            sample_rate);
   return wctx;
}

asr_result_t *asr_whisper_process_partial(void *ctx, const int16_t *audio, size_t samples) {
   if (!ctx || !audio) {
      LOG_ERROR("Whisper: Invalid parameters to process_partial");
      return NULL;
   }

   whisper_context_t *wctx = (whisper_context_t *)ctx;

   // Check if buffer has enough space
   if (wctx->buffer_size + samples > wctx->buffer_capacity) {
      LOG_WARNING("Whisper: Audio buffer full (%zu/%zu samples). Dropping %zu samples.",
                  wctx->buffer_size, wctx->buffer_capacity, samples);
      samples = wctx->buffer_capacity - wctx->buffer_size;
      if (samples == 0) {
         // Buffer completely full, cannot accept more audio
         asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
         if (result) {
            result->text = strdup("");
            result->confidence = -1.0f;
            result->is_partial = 1;
            result->processing_time = 0.0;
         }
         return result;
      }
   }

   // Convert int16 to float and append to buffer
   convert_int16_to_float(audio, wctx->audio_buffer + wctx->buffer_size, samples);
   wctx->buffer_size += samples;

   // Create empty partial result (Whisper doesn't support streaming partials)
   asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
   if (!result) {
      LOG_ERROR("Whisper: Failed to allocate result structure");
      return NULL;
   }

   result->text = strdup("");
   result->confidence = -1.0f;
   result->is_partial = 1;
   result->processing_time = 0.0;

   return result;
}

asr_result_t *asr_whisper_finalize(void *ctx) {
   if (!ctx) {
      LOG_ERROR("Whisper: Invalid context in finalize");
      return NULL;
   }

   whisper_context_t *wctx = (whisper_context_t *)ctx;

   if (wctx->buffer_size == 0) {
      LOG_WARNING("Whisper: No audio data to process");
      asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
      if (result) {
         result->text = strdup("");
         result->confidence = 0.0f;
         result->is_partial = 0;
         result->processing_time = 0.0;
      }
      return result;
   }

   /* Whisper requires minimum 100ms of audio (1600 samples at 16kHz).
    * Pad with silence if needed to avoid "input is too short" warning. */
   const size_t min_samples = (WHISPER_SAMPLE_RATE / 10); /* 100ms */
   if (wctx->buffer_size < min_samples && wctx->buffer_size < wctx->buffer_capacity) {
      size_t pad_samples = min_samples - wctx->buffer_size;
      if (wctx->buffer_size + pad_samples > wctx->buffer_capacity) {
         pad_samples = wctx->buffer_capacity - wctx->buffer_size;
      }
      memset(wctx->audio_buffer + wctx->buffer_size, 0, pad_samples * sizeof(float));
      wctx->buffer_size += pad_samples;
   }

   struct timeval start, end;
   gettimeofday(&start, NULL);

   // Run Whisper inference
   int ret = whisper_full(wctx->ctx, wctx->wparams, wctx->audio_buffer, wctx->buffer_size);
   if (ret != 0) {
      LOG_ERROR("Whisper: Failed to process audio (error code: %d)", ret);
      return NULL;
   }

   gettimeofday(&end, NULL);
   double processing_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;

   // Extract transcription from segments
   const int n_segments = whisper_full_n_segments(wctx->ctx);
   if (n_segments == 0) {
      LOG_WARNING("Whisper: No segments found in transcription");
      asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
      if (result) {
         result->text = strdup("");
         result->confidence = 0.0f;
         result->is_partial = 0;
         result->processing_time = processing_time;
      }
      return result;
   }

   // Concatenate all segment texts
   size_t total_length = 0;
   for (int i = 0; i < n_segments; i++) {
      const char *segment_text = whisper_full_get_segment_text(wctx->ctx, i);
      if (segment_text) {
         total_length += strlen(segment_text);
      }
   }

   char *full_text = (char *)calloc(total_length + 1, sizeof(char));
   if (!full_text) {
      LOG_ERROR("Whisper: Failed to allocate memory for transcription text");
      return NULL;
   }

   for (int i = 0; i < n_segments; i++) {
      const char *segment_text = whisper_full_get_segment_text(wctx->ctx, i);
      if (segment_text) {
         strcat(full_text, segment_text);
      }
   }

   // Create result structure
   asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
   if (!result) {
      LOG_ERROR("Whisper: Failed to allocate result structure");
      free(full_text);
      return NULL;
   }

   result->text = full_text;    // Ownership transferred
   result->confidence = -1.0f;  // Whisper doesn't provide simple confidence scores
   result->is_partial = 0;
   result->processing_time = processing_time;

   // Calculate audio duration for RTF
   double audio_duration = (double)wctx->buffer_size / wctx->sample_rate * 1000.0;
   double rtf = (audio_duration > 0) ? processing_time / audio_duration : 0.0;

   LOG_INFO("Whisper: Final result: \"%s\" (%.1fms, RTF: %.3f)", result->text ? result->text : "",
            processing_time, rtf);

   // Record ASR timing metrics
   metrics_record_asr_timing(processing_time, rtf);

   return result;
}

int asr_whisper_reset(void *ctx) {
   if (!ctx) {
      LOG_ERROR("Whisper: Invalid context in reset");
      return ASR_FAILURE;
   }

   whisper_context_t *wctx = (whisper_context_t *)ctx;

   // Clear audio buffer
   wctx->buffer_size = 0;
   memset(wctx->audio_buffer, 0, wctx->buffer_capacity * sizeof(float));

   LOG_INFO("Whisper: Reset for new utterance");
   return ASR_SUCCESS;
}

void asr_whisper_cleanup(void *ctx) {
   if (!ctx) {
      return;
   }

   whisper_context_t *wctx = (whisper_context_t *)ctx;

   if (wctx->audio_buffer) {
      free(wctx->audio_buffer);
   }

   if (wctx->ctx) {
      whisper_free(wctx->ctx);
   }

   free(wctx);

   LOG_INFO("Whisper: Cleanup complete");
}
