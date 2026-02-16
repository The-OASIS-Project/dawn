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
 * @file asr_whisper.c
 * @brief Whisper ASR implementation for common library
 *
 * Wrapper around whisper.cpp for speech-to-text transcription.
 * Accumulates audio and performs batch processing on finalize.
 */

#include "asr/asr_whisper.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "logging_common.h"
#include "whisper.h"

/**
 * @brief Default maximum audio buffer (60 seconds at 16kHz)
 */
#define DEFAULT_MAX_AUDIO_SECONDS 60

/**
 * @brief Whisper ASR context structure
 */
struct whisper_asr_context {
   struct whisper_context *ctx;
   int sample_rate;

   /* Audio buffer for accumulation (Whisper needs float PCM) */
   float *audio_buffer;
   size_t buffer_size;     /* Current number of samples in buffer */
   size_t buffer_capacity; /* Maximum capacity */

   /* Whisper processing parameters */
   struct whisper_full_params wparams;

   /* Optional timing callback */
   asr_timing_callback_t timing_callback;
   void *timing_callback_user_data;
};

/**
 * @brief Convert int16_t PCM to float PCM
 */
static void convert_int16_to_float(const int16_t *input, float *output, size_t n) {
   for (size_t i = 0; i < n; i++) {
      output[i] = (float)input[i] / 32768.0f;
   }
}

asr_whisper_config_t asr_whisper_default_config(void) {
   asr_whisper_config_t config = {
      .model_path = NULL,
      .sample_rate = WHISPER_SAMPLE_RATE,
      .use_gpu = 0,
      .n_threads = 4,
      .language = "en",
      .max_audio_seconds = DEFAULT_MAX_AUDIO_SECONDS,
   };
   return config;
}

whisper_asr_context_t *asr_whisper_init(const asr_whisper_config_t *config) {
   if (!config || !config->model_path) {
      DAWN_LOG_ERROR("asr_whisper_init: config or model_path is NULL");
      return NULL;
   }

   if (config->sample_rate != WHISPER_SAMPLE_RATE) {
      DAWN_LOG_WARNING("asr_whisper_init: Sample rate %d differs from expected %d",
                       config->sample_rate, WHISPER_SAMPLE_RATE);
   }

   whisper_asr_context_t *wctx = (whisper_asr_context_t *)calloc(1, sizeof(whisper_asr_context_t));
   if (!wctx) {
      DAWN_LOG_ERROR("asr_whisper_init: Failed to allocate context");
      return NULL;
   }

   wctx->sample_rate = config->sample_rate;

   /* Initialize whisper context with platform-appropriate configuration */
   struct whisper_context_params cparams = whisper_context_default_params();
   cparams.use_gpu = config->use_gpu ? true : false;
   cparams.flash_attn = false; /* Disable flash attention for compatibility */

   wctx->ctx = whisper_init_from_file_with_params(config->model_path, cparams);
   if (!wctx->ctx) {
      DAWN_LOG_ERROR("asr_whisper_init: Failed to load model from: %s", config->model_path);
      free(wctx);
      return NULL;
   }

   /* Allocate audio buffer */
   size_t max_seconds = config->max_audio_seconds > 0 ? config->max_audio_seconds
                                                      : DEFAULT_MAX_AUDIO_SECONDS;
   wctx->buffer_capacity = max_seconds * WHISPER_SAMPLE_RATE;
   wctx->audio_buffer = (float *)calloc(wctx->buffer_capacity, sizeof(float));
   if (!wctx->audio_buffer) {
      DAWN_LOG_ERROR("asr_whisper_init: Failed to allocate audio buffer");
      whisper_free(wctx->ctx);
      free(wctx);
      return NULL;
   }
   wctx->buffer_size = 0;

   /* Configure Whisper parameters */
   wctx->wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
   wctx->wparams.print_realtime = false;
   wctx->wparams.print_progress = false;
   wctx->wparams.print_timestamps = false;
   wctx->wparams.print_special = false;
   wctx->wparams.translate = false;
   wctx->wparams.language = config->language ? config->language : "en";
   wctx->wparams.n_threads = config->n_threads > 0 ? config->n_threads : 4;
   wctx->wparams.offset_ms = 0;
   wctx->wparams.no_context = true;
   wctx->wparams.single_segment = false;

   /* Initialize callbacks to NULL */
   wctx->timing_callback = NULL;
   wctx->timing_callback_user_data = NULL;

   DAWN_LOG_INFO("asr_whisper_init: Initialized (model: %s, gpu: %s, threads: %d)",
                 config->model_path, config->use_gpu ? "yes" : "no", wctx->wparams.n_threads);

   return wctx;
}

void asr_whisper_set_timing_callback(whisper_asr_context_t *ctx,
                                     asr_timing_callback_t callback,
                                     void *user_data) {
   if (!ctx)
      return;
   ctx->timing_callback = callback;
   ctx->timing_callback_user_data = user_data;
}

/**
 * @brief Static sentinel result for process() — avoids heap allocation per audio chunk.
 *
 * Whisper is batch-only: process() just accumulates audio, always returning an empty partial.
 * This sentinel eliminates ~62 mallocs/sec at 16kHz/512-sample chunks.
 * The text field points to a string literal (caller must NOT free sentinel results).
 * Callers using asr_engine_result_free() or asr_whisper_result_free() handle NULL-text safely.
 */
/**
 * @brief Allocate an empty partial result (Whisper doesn't support streaming)
 *
 * Returns a heap-allocated result so callers can always free() uniformly.
 * Previously used a static sentinel, but asr_engine_result_free() in
 * asr_engine.c calls free() unconditionally, causing double-free crashes.
 */
static asr_whisper_result_t *alloc_empty_partial(void) {
   asr_whisper_result_t *r = (asr_whisper_result_t *)calloc(1, sizeof(asr_whisper_result_t));
   if (r) {
      r->confidence = -1.0f;
      r->is_partial = 1;
   }
   return r;
}

asr_whisper_result_t *asr_whisper_process(whisper_asr_context_t *ctx,
                                          const int16_t *audio,
                                          size_t samples) {
   if (!ctx || !audio) {
      DAWN_LOG_ERROR("asr_whisper_process: Invalid parameters");
      return NULL;
   }

   /* Check if buffer has enough space */
   if (ctx->buffer_size + samples > ctx->buffer_capacity) {
      DAWN_LOG_WARNING("asr_whisper_process: Buffer full (%zu/%zu). Dropping %zu samples.",
                       ctx->buffer_size, ctx->buffer_capacity, samples);
      samples = ctx->buffer_capacity - ctx->buffer_size;
      if (samples == 0) {
         return alloc_empty_partial();
      }
   }

   /* Convert int16 to float and append to buffer */
   convert_int16_to_float(audio, ctx->audio_buffer + ctx->buffer_size, samples);
   ctx->buffer_size += samples;

   /* Return static empty partial (Whisper doesn't support streaming) */
   return alloc_empty_partial();
}

asr_whisper_result_t *asr_whisper_finalize(whisper_asr_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("asr_whisper_finalize: Invalid context");
      return NULL;
   }

   if (ctx->buffer_size == 0) {
      DAWN_LOG_WARNING("asr_whisper_finalize: No audio data to process");
      asr_whisper_result_t *result = (asr_whisper_result_t *)calloc(1,
                                                                    sizeof(asr_whisper_result_t));
      if (result) {
         result->text = strdup("");
         result->confidence = 0.0f;
         result->is_partial = 0;
         result->processing_time = 0.0;
      }
      return result;
   }

   /* Whisper requires minimum 100ms of audio (1600 samples at 16kHz).
    * Pad with silence if needed. */
   const size_t min_samples = (WHISPER_SAMPLE_RATE / 10); /* 100ms */
   if (ctx->buffer_size < min_samples && ctx->buffer_size < ctx->buffer_capacity) {
      size_t pad_samples = min_samples - ctx->buffer_size;
      if (ctx->buffer_size + pad_samples > ctx->buffer_capacity) {
         pad_samples = ctx->buffer_capacity - ctx->buffer_size;
      }
      memset(ctx->audio_buffer + ctx->buffer_size, 0, pad_samples * sizeof(float));
      ctx->buffer_size += pad_samples;
   }

   struct timeval start, end;
   gettimeofday(&start, NULL);

   /* Run Whisper inference */
   int ret = whisper_full(ctx->ctx, ctx->wparams, ctx->audio_buffer, ctx->buffer_size);
   if (ret != 0) {
      DAWN_LOG_ERROR("asr_whisper_finalize: Inference failed (error: %d)", ret);
      return NULL;
   }

   gettimeofday(&end, NULL);
   double processing_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;

   /* Extract transcription from segments */
   const int n_segments = whisper_full_n_segments(ctx->ctx);
   if (n_segments == 0) {
      DAWN_LOG_WARNING("asr_whisper_finalize: No segments found");
      asr_whisper_result_t *result = (asr_whisper_result_t *)calloc(1,
                                                                    sizeof(asr_whisper_result_t));
      if (result) {
         result->text = strdup("");
         result->confidence = 0.0f;
         result->is_partial = 0;
         result->processing_time = processing_time;
      }
      return result;
   }

   /* Concatenate all segment texts */
   size_t total_length = 0;
   for (int i = 0; i < n_segments; i++) {
      const char *segment_text = whisper_full_get_segment_text(ctx->ctx, i);
      if (segment_text) {
         total_length += strlen(segment_text);
      }
   }

   char *full_text = (char *)malloc(total_length + 1);
   if (!full_text) {
      DAWN_LOG_ERROR("asr_whisper_finalize: Failed to allocate text buffer");
      return NULL;
   }

   size_t offset = 0;
   for (int i = 0; i < n_segments; i++) {
      const char *segment_text = whisper_full_get_segment_text(ctx->ctx, i);
      if (segment_text) {
         size_t seg_len = strlen(segment_text);
         if (offset + seg_len > total_length)
            break; /* Safety guard against segment text changing between passes */
         memcpy(full_text + offset, segment_text, seg_len);
         offset += seg_len;
      }
   }
   full_text[offset] = '\0';

   /* Create result structure */
   asr_whisper_result_t *result = (asr_whisper_result_t *)calloc(1, sizeof(asr_whisper_result_t));
   if (!result) {
      DAWN_LOG_ERROR("asr_whisper_finalize: Failed to allocate result");
      free(full_text);
      return NULL;
   }

   result->text = full_text;
   result->confidence = -1.0f; /* Whisper doesn't provide simple confidence scores */
   result->is_partial = 0;
   result->processing_time = processing_time;

   /* Calculate RTF and invoke optional timing callback */
   double audio_duration = (double)ctx->buffer_size / ctx->sample_rate * 1000.0;
   double rtf = (audio_duration > 0) ? processing_time / audio_duration : 0.0;

   DAWN_LOG_INFO("asr_whisper_finalize: \"%s\" (%.1fms, RTF: %.3f)",
                 result->text ? result->text : "", processing_time, rtf);

   if (ctx->timing_callback) {
      ctx->timing_callback(processing_time, rtf, ctx->timing_callback_user_data);
   }

   return result;
}

int asr_whisper_reset(whisper_asr_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("asr_whisper_reset: Invalid context");
      return ASR_FAILURE;
   }

   ctx->buffer_size = 0;
   /* No memset needed - buffer_size gates all reads, old data is overwritten by new audio */

   DAWN_LOG_INFO("asr_whisper_reset: Reset for new utterance");
   return ASR_SUCCESS;
}

void asr_whisper_result_free(asr_whisper_result_t *result) {
   if (!result)
      return;

   free(result->text);
   free(result);
}

void asr_whisper_cleanup(whisper_asr_context_t *ctx) {
   if (!ctx)
      return;

   if (ctx->audio_buffer) {
      free(ctx->audio_buffer);
   }

   if (ctx->ctx) {
      whisper_free(ctx->ctx);
   }

   free(ctx);

   DAWN_LOG_INFO("asr_whisper_cleanup: Cleanup complete");
}

size_t asr_whisper_get_buffer_size(whisper_asr_context_t *ctx) {
   return ctx ? ctx->buffer_size : 0;
}

double asr_whisper_get_buffer_duration_ms(whisper_asr_context_t *ctx) {
   if (!ctx || ctx->sample_rate == 0)
      return 0.0;
   return (double)ctx->buffer_size / ctx->sample_rate * 1000.0;
}
