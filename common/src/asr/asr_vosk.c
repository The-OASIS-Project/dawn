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
 * Vosk ASR implementation for common library.
 * Ported from daemon src/asr/asr_vosk.c with adaptations for the
 * common library logging and config patterns.
 */

#include "asr/asr_vosk.h"

#include <json-c/json.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "asr/vosk_api.h"
#include "logging_common.h"

/**
 * @brief Vosk ASR context structure
 */
/**
 * @brief How often to parse partial results (every Nth process() call).
 * At 16kHz/512-sample chunks = ~31 calls/sec, N=10 means ~3 updates/sec
 * which is plenty for UI display. Avoids JSON parsing overhead on every chunk.
 */
#define VOSK_PARTIAL_PARSE_INTERVAL 10

struct vosk_asr_context {
   VoskModel *model;
   VoskRecognizer *recognizer;
   int sample_rate;
   asr_timing_callback_t timing_cb;
   void *timing_cb_data;
   int process_count;       /**< Calls since last partial parse */
   char *cached_text;       /**< Cached partial text (owned, for reuse between parses) */
   float cached_confidence; /**< Cached partial confidence */
};

/**
 * @brief Parse Vosk JSON response and extract text/confidence
 *
 * @param json_str JSON string from Vosk
 * @param is_partial 1 for partial results, 0 for final results
 * @param out_text Output pointer for extracted text (caller must free)
 * @param out_confidence Output pointer for confidence score
 * @return ASR_SUCCESS on success, ASR_FAILURE or ASR_ERR_OUT_OF_MEMORY on error
 */
static int parse_vosk_json(const char *json_str,
                           int is_partial,
                           char **out_text,
                           float *out_confidence) {
   if (!json_str || !out_text || !out_confidence) {
      return ASR_FAILURE;
   }

   *out_text = NULL;
   *out_confidence = -1.0f;

   struct json_object *parsed_json = json_tokener_parse(json_str);
   if (!parsed_json) {
      DAWN_LOG_ERROR("Vosk: Failed to parse JSON response");
      return ASR_FAILURE;
   }

   struct json_object *text_obj = NULL;
   const char *field_name = is_partial ? "partial" : "text";

   if (json_object_object_get_ex(parsed_json, field_name, &text_obj)) {
      const char *text = json_object_get_string(text_obj);
      if (text) {
         *out_text = strdup(text);
         if (!*out_text) {
            DAWN_LOG_ERROR("Vosk: Failed to allocate memory for text");
            json_object_put(parsed_json);
            return ASR_ERR_OUT_OF_MEMORY;
         }
      }
   }

   /* Extract confidence (only available in final results, not partials) */
   if (!is_partial) {
      struct json_object *result_obj = NULL;
      if (json_object_object_get_ex(parsed_json, "result", &result_obj)) {
         if (json_object_is_type(result_obj, json_type_array)) {
            int array_len = json_object_array_length(result_obj);
            if (array_len > 0) {
               double confidence_sum = 0.0;
               int confidence_count = 0;

               for (int i = 0; i < array_len; i++) {
                  struct json_object *word_obj = json_object_array_get_idx(result_obj, i);
                  struct json_object *conf_obj = NULL;

                  if (json_object_object_get_ex(word_obj, "conf", &conf_obj)) {
                     confidence_sum += json_object_get_double(conf_obj);
                     confidence_count++;
                  }
               }

               if (confidence_count > 0) {
                  *out_confidence = (float)(confidence_sum / confidence_count);
               }
            }
         }
      }
   }

   json_object_put(parsed_json);
   return ASR_SUCCESS;
}

vosk_asr_context_t *asr_vosk_init(const asr_vosk_config_t *config) {
   if (!config || !config->model_path) {
      DAWN_LOG_ERROR("Vosk: config or model_path cannot be NULL");
      return NULL;
   }

   vosk_asr_context_t *ctx = (vosk_asr_context_t *)calloc(1, sizeof(vosk_asr_context_t));
   if (!ctx) {
      DAWN_LOG_ERROR("Vosk: Failed to allocate context");
      return NULL;
   }

   ctx->sample_rate = config->sample_rate > 0 ? config->sample_rate : 16000;

   /* Suppress Vosk internal logging (noisy at default level) */
   vosk_set_log_level(-1);

   /* Initialize GPU support (if available, no-op otherwise) */
   vosk_gpu_init();
   vosk_gpu_thread_init();

   /* Load model */
   ctx->model = vosk_model_new(config->model_path);
   if (!ctx->model) {
      DAWN_LOG_ERROR("Vosk: Failed to load model from: %s", config->model_path);
      free(ctx);
      return NULL;
   }

   /* Create recognizer */
   ctx->recognizer = vosk_recognizer_new(ctx->model, (float)ctx->sample_rate);
   if (!ctx->recognizer) {
      DAWN_LOG_ERROR("Vosk: Failed to create recognizer");
      vosk_model_free(ctx->model);
      free(ctx);
      return NULL;
   }

   DAWN_LOG_INFO("Vosk: Initialized (model: %s, sample_rate: %d)", config->model_path,
                 ctx->sample_rate);
   return ctx;
}

void asr_vosk_set_timing_callback(vosk_asr_context_t *ctx,
                                  asr_timing_callback_t callback,
                                  void *user_data) {
   if (!ctx)
      return;
   ctx->timing_cb = callback;
   ctx->timing_cb_data = user_data;
}

asr_result_t *asr_vosk_process(vosk_asr_context_t *ctx, const int16_t *audio, size_t samples) {
   if (!ctx || !audio) {
      DAWN_LOG_ERROR("Vosk: Invalid parameters to process");
      return NULL;
   }

   /* Feed audio to Vosk (int16_t → short for Vosk API).
    * Clamp samples to INT_MAX to prevent truncation on cast to int. */
   size_t safe_samples = samples > (size_t)INT_MAX ? (size_t)INT_MAX : samples;
   vosk_recognizer_accept_waveform_s(ctx->recognizer, (const short *)audio, (int)safe_samples);

   /* Feed audio then return a partial result.
    * Only re-parse Vosk JSON every Nth call to avoid overhead (~31 calls/sec).
    * Always return a fresh heap-allocated copy so callers can free() uniformly. */
   ctx->process_count++;
   if (ctx->process_count >= VOSK_PARTIAL_PARSE_INTERVAL || !ctx->cached_text) {
      ctx->process_count = 0;

      /* Get and parse partial result */
      const char *json_result = vosk_recognizer_partial_result(ctx->recognizer);
      if (!json_result) {
         DAWN_LOG_ERROR("Vosk: vosk_recognizer_partial_result() returned NULL");
         return NULL;
      }

      char *text = NULL;
      float confidence = -1.0f;
      if (parse_vosk_json(json_result, 1, &text, &confidence) != ASR_SUCCESS) {
         DAWN_LOG_ERROR("Vosk: Failed to parse partial result JSON");
         return NULL;
      }

      /* Update cached text for reuse on skipped iterations */
      free(ctx->cached_text);
      ctx->cached_text = text;
      ctx->cached_confidence = confidence;
   }

   /* Return a fresh copy every call (caller owns it and will free) */
   asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
   if (!result) {
      DAWN_LOG_ERROR("Vosk: Failed to allocate result structure");
      return NULL;
   }

   result->text = ctx->cached_text ? strdup(ctx->cached_text) : NULL;
   result->confidence = ctx->cached_confidence;
   result->is_partial = 1;
   result->processing_time = 0.0;

   return result;
}

asr_result_t *asr_vosk_finalize(vosk_asr_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("Vosk: Invalid context in finalize");
      return NULL;
   }

   struct timeval start, end;
   gettimeofday(&start, NULL);

   /* Get final result (near-instant since audio was already decoded) */
   const char *json_result = vosk_recognizer_final_result(ctx->recognizer);
   if (!json_result) {
      DAWN_LOG_ERROR("Vosk: vosk_recognizer_final_result() returned NULL");
      return NULL;
   }

   gettimeofday(&end, NULL);
   double processing_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;

   /* Parse JSON to extract text and confidence */
   char *text = NULL;
   float confidence = -1.0f;
   if (parse_vosk_json(json_result, 0, &text, &confidence) != ASR_SUCCESS) {
      DAWN_LOG_ERROR("Vosk: Failed to parse final result JSON");
      return NULL;
   }

   asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
   if (!result) {
      DAWN_LOG_ERROR("Vosk: Failed to allocate result structure");
      free(text);
      return NULL;
   }

   result->text = text;
   result->confidence = confidence;
   result->is_partial = 0;
   result->processing_time = processing_time;

   DAWN_LOG_INFO("Vosk: Final result: \"%s\" (confidence: %.2f, time: %.1fms)",
                 result->text ? result->text : "(null)", result->confidence,
                 result->processing_time);

   /* Invoke timing callback if registered */
   if (ctx->timing_cb) {
      ctx->timing_cb(processing_time, 0.0, ctx->timing_cb_data);
   }

   return result;
}

int asr_vosk_reset(vosk_asr_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("Vosk: Invalid context in reset");
      return ASR_FAILURE;
   }

   vosk_recognizer_reset(ctx->recognizer);
   ctx->process_count = 0;
   free(ctx->cached_text);
   ctx->cached_text = NULL;
   return ASR_SUCCESS;
}

void asr_vosk_result_free(asr_result_t *result) {
   if (!result)
      return;
   free(result->text);
   free(result);
}

void asr_vosk_cleanup(vosk_asr_context_t *ctx) {
   if (!ctx)
      return;

   free(ctx->cached_text);
   if (ctx->recognizer) {
      vosk_recognizer_free(ctx->recognizer);
   }
   if (ctx->model) {
      vosk_model_free(ctx->model);
   }

   free(ctx);
   DAWN_LOG_INFO("Vosk: Cleanup complete");
}
