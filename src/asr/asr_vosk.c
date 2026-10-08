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

#include "asr/asr_vosk.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "logging.h"
#include "ui/metrics.h"
#include "vosk_api.h"

/**
 * @brief Vosk-specific context structure
 */
typedef struct {
   VoskModel *model;
   VoskRecognizer *recognizer;
   int sample_rate;
} vosk_context_t;

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
      OLOG_ERROR("Vosk: Failed to parse JSON response");
      return ASR_FAILURE;
   }

   struct json_object *text_obj = NULL;
   const char *field_name = is_partial ? "partial" : "text";

   // Extract text field
   if (json_object_object_get_ex(parsed_json, field_name, &text_obj)) {
      const char *text = json_object_get_string(text_obj);
      if (text) {
         *out_text = strdup(text);
         if (!*out_text) {
            OLOG_ERROR("Vosk: Failed to allocate memory for text");
            json_object_put(parsed_json);
            return ASR_ERR_OUT_OF_MEMORY;
         }
      }
   }

   // Extract confidence (only available in final results, not partials)
   if (!is_partial) {
      struct json_object *result_obj = NULL;
      if (json_object_object_get_ex(parsed_json, "result", &result_obj)) {
         if (json_object_is_type(result_obj, json_type_array)) {
            int array_len = json_object_array_length(result_obj);
            if (array_len > 0) {
               // Average confidence across all words
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

void *asr_vosk_init(const char *model_path, int sample_rate) {
   if (!model_path) {
      OLOG_ERROR("Vosk: model_path cannot be NULL");
      return NULL;
   }

   vosk_context_t *ctx = (vosk_context_t *)calloc(1, sizeof(vosk_context_t));
   if (!ctx) {
      OLOG_ERROR("Vosk: Failed to allocate context");
      return NULL;
   }

   ctx->sample_rate = sample_rate;

   // Initialize GPU support (if available)
   vosk_gpu_init();
   vosk_gpu_thread_init();

   // Load model
   ctx->model = vosk_model_new(model_path);
   if (!ctx->model) {
      OLOG_ERROR("Vosk: Failed to load model from: %s", model_path);
      free(ctx);
      return NULL;
   }

   // Create recognizer
   ctx->recognizer = vosk_recognizer_new(ctx->model, (float)sample_rate);
   if (!ctx->recognizer) {
      OLOG_ERROR("Vosk: Failed to create recognizer");
      vosk_model_free(ctx->model);
      free(ctx);
      return NULL;
   }

   OLOG_INFO("Vosk: Initialized successfully (model: %s, sample_rate: %d)", model_path,
             sample_rate);
   return ctx;
}

asr_result_t *asr_vosk_process_partial(void *ctx, const int16_t *audio, size_t samples) {
   if (!ctx || !audio) {
      OLOG_ERROR("Vosk: Invalid parameters to process_partial");
      return NULL;
   }

   vosk_context_t *vosk_ctx = (vosk_context_t *)ctx;

   struct timeval start, end;
   gettimeofday(&start, NULL);

   // Feed audio to Vosk (audio is int16_t, cast to char* for Vosk API)
   vosk_recognizer_accept_waveform_s(vosk_ctx->recognizer, (const short *)audio, samples);

   // Get partial result
   const char *json_result = vosk_recognizer_partial_result(vosk_ctx->recognizer);
   if (!json_result) {
      OLOG_ERROR("Vosk: vosk_recognizer_partial_result() returned NULL");
      return NULL;
   }

   gettimeofday(&end, NULL);
   double processing_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;

   // Parse JSON to extract text
   char *text = NULL;
   float confidence = -1.0f;
   if (parse_vosk_json(json_result, 1, &text, &confidence) != 0) {
      OLOG_ERROR("Vosk: Failed to parse partial result JSON");
      return NULL;
   }

   // Create result structure
   asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
   if (!result) {
      OLOG_ERROR("Vosk: Failed to allocate result structure");
      free(text);
      return NULL;
   }

   result->text = text;  // Ownership transferred
   result->confidence = confidence;
   result->is_partial = 1;
   result->processing_time = processing_time;

   return result;
}

asr_result_t *asr_vosk_finalize(void *ctx) {
   if (!ctx) {
      OLOG_ERROR("Vosk: Invalid context in finalize");
      return NULL;
   }

   vosk_context_t *vosk_ctx = (vosk_context_t *)ctx;

   struct timeval start, end;
   gettimeofday(&start, NULL);

   // Get final result
   const char *json_result = vosk_recognizer_final_result(vosk_ctx->recognizer);
   if (!json_result) {
      OLOG_ERROR("Vosk: vosk_recognizer_final_result() returned NULL");
      return NULL;
   }

   gettimeofday(&end, NULL);
   double processing_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;

   // Parse JSON to extract text and confidence
   char *text = NULL;
   float confidence = -1.0f;
   if (parse_vosk_json(json_result, 0, &text, &confidence) != 0) {
      OLOG_ERROR("Vosk: Failed to parse final result JSON");
      return NULL;
   }

   // Create result structure
   asr_result_t *result = (asr_result_t *)calloc(1, sizeof(asr_result_t));
   if (!result) {
      OLOG_ERROR("Vosk: Failed to allocate result structure");
      free(text);
      return NULL;
   }

   result->text = text;  // Ownership transferred
   result->confidence = confidence;
   result->is_partial = 0;
   result->processing_time = processing_time;

   OLOG_INFO("Vosk: Final result: \"%s\" (confidence: %.2f)",
             result->text ? result->text : "(null)", result->confidence);

   // Record ASR timing metrics (Vosk doesn't provide RTF, use 0)
   metrics_record_asr_timing(processing_time, 0.0);

   return result;
}

int asr_vosk_reset(void *ctx) {
   if (!ctx) {
      OLOG_ERROR("Vosk: Invalid context in reset");
      return ASR_FAILURE;
   }

   vosk_context_t *vosk_ctx = (vosk_context_t *)ctx;
   vosk_recognizer_reset(vosk_ctx->recognizer);

   OLOG_INFO("Vosk: Reset recognizer for new utterance");
   return ASR_SUCCESS;
}

void asr_vosk_cleanup(void *ctx) {
   if (!ctx) {
      return;
   }

   vosk_context_t *vosk_ctx = (vosk_context_t *)ctx;

   if (vosk_ctx->recognizer) {
      vosk_recognizer_free(vosk_ctx->recognizer);
   }

   if (vosk_ctx->model) {
      vosk_model_free(vosk_ctx->model);
   }

   free(vosk_ctx);

   OLOG_INFO("Vosk: Cleanup complete");
}
