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
 * Unified ASR engine implementation with polymorphic dispatch.
 * Inspired by daemon src/asr/asr_interface.c but simplified for
 * the common library (no normalization, no recording debug).
 */

#include "asr/asr_engine.h"

#include <stdlib.h>
#include <string.h>

#include "logging_common.h"

#ifdef DAWN_HAS_ASR
#include "asr/asr_whisper.h"
#endif

#ifdef DAWN_HAS_ASR_VOSK
#include "asr/asr_vosk.h"
#endif

/**
 * @brief Unified ASR engine context with function pointers for dispatch
 */
struct asr_engine_context {
   asr_engine_type_t engine_type;
   void *engine_context; /**< whisper_asr_context_t* or vosk_asr_context_t* */

   /* Function pointers for polymorphic dispatch */
   asr_result_t *(*process)(void *ctx, const int16_t *audio, size_t samples);
   asr_result_t *(*finalize)(void *ctx);
   int (*reset)(void *ctx);
   void (*cleanup)(void *ctx);
};

/* ============================================================================
 * Whisper adapter functions (typed → void* wrappers)
 * ============================================================================ */

#ifdef DAWN_HAS_ASR
static asr_result_t *whisper_process_adapter(void *ctx, const int16_t *audio, size_t samples) {
   return (asr_result_t *)asr_whisper_process((whisper_asr_context_t *)ctx, audio, samples);
}

static asr_result_t *whisper_finalize_adapter(void *ctx) {
   return (asr_result_t *)asr_whisper_finalize((whisper_asr_context_t *)ctx);
}

static int whisper_reset_adapter(void *ctx) {
   return asr_whisper_reset((whisper_asr_context_t *)ctx);
}

static void whisper_cleanup_adapter(void *ctx) {
   asr_whisper_cleanup((whisper_asr_context_t *)ctx);
}
#endif

/* ============================================================================
 * Vosk adapter functions (typed → void* wrappers)
 * ============================================================================ */

#ifdef DAWN_HAS_ASR_VOSK
static asr_result_t *vosk_process_adapter(void *ctx, const int16_t *audio, size_t samples) {
   return asr_vosk_process((vosk_asr_context_t *)ctx, audio, samples);
}

static asr_result_t *vosk_finalize_adapter(void *ctx) {
   return asr_vosk_finalize((vosk_asr_context_t *)ctx);
}

static int vosk_reset_adapter(void *ctx) {
   return asr_vosk_reset((vosk_asr_context_t *)ctx);
}

static void vosk_cleanup_adapter(void *ctx) {
   asr_vosk_cleanup((vosk_asr_context_t *)ctx);
}
#endif

/* ============================================================================
 * Public API
 * ============================================================================ */

asr_engine_context_t *asr_engine_init(const asr_engine_config_t *config) {
   if (!config || !config->model_path) {
      DAWN_LOG_ERROR("ASR engine: config or model_path cannot be NULL");
      return NULL;
   }

   asr_engine_context_t *ctx = (asr_engine_context_t *)calloc(1, sizeof(asr_engine_context_t));
   if (!ctx) {
      DAWN_LOG_ERROR("ASR engine: Failed to allocate context");
      return NULL;
   }

   ctx->engine_type = config->engine;

   switch (config->engine) {
#ifdef DAWN_HAS_ASR
      case ASR_ENGINE_WHISPER: {
         DAWN_LOG_INFO("ASR engine: Initializing Whisper (model: %s)", config->model_path);

         asr_whisper_config_t whisper_config = {
            .model_path = config->model_path,
            .sample_rate = config->sample_rate > 0 ? config->sample_rate : 16000,
            .use_gpu = config->use_gpu,
            .n_threads = config->n_threads > 0 ? config->n_threads : 4,
            .language = config->language ? config->language : "en",
            .max_audio_seconds = config->max_audio_seconds > 0 ? config->max_audio_seconds : 60,
         };

         ctx->engine_context = asr_whisper_init(&whisper_config);
         if (!ctx->engine_context) {
            DAWN_LOG_ERROR("ASR engine: Whisper initialization failed");
            free(ctx);
            return NULL;
         }

         ctx->process = whisper_process_adapter;
         ctx->finalize = whisper_finalize_adapter;
         ctx->reset = whisper_reset_adapter;
         ctx->cleanup = whisper_cleanup_adapter;
         break;
      }
#endif

#ifdef DAWN_HAS_ASR_VOSK
      case ASR_ENGINE_VOSK: {
         DAWN_LOG_INFO("ASR engine: Initializing Vosk (model: %s)", config->model_path);

         asr_vosk_config_t vosk_config = {
            .model_path = config->model_path,
            .sample_rate = config->sample_rate > 0 ? config->sample_rate : 16000,
         };

         ctx->engine_context = asr_vosk_init(&vosk_config);
         if (!ctx->engine_context) {
            DAWN_LOG_ERROR("ASR engine: Vosk initialization failed");
            free(ctx);
            return NULL;
         }

         ctx->process = vosk_process_adapter;
         ctx->finalize = vosk_finalize_adapter;
         ctx->reset = vosk_reset_adapter;
         ctx->cleanup = vosk_cleanup_adapter;
         break;
      }
#endif

      default:
         DAWN_LOG_ERROR("ASR engine: Unsupported engine type %d (check build options)",
                        config->engine);
         free(ctx);
         return NULL;
   }

   DAWN_LOG_INFO("ASR engine: %s initialized successfully", asr_engine_name(config->engine));
   return ctx;
}

asr_result_t *asr_engine_process(asr_engine_context_t *ctx, const int16_t *audio, size_t samples) {
   if (!ctx || !audio) {
      DAWN_LOG_ERROR("ASR engine: Invalid parameters to process");
      return NULL;
   }
   return ctx->process(ctx->engine_context, audio, samples);
}

asr_result_t *asr_engine_finalize(asr_engine_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("ASR engine: Invalid context in finalize");
      return NULL;
   }
   return ctx->finalize(ctx->engine_context);
}

int asr_engine_reset(asr_engine_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("ASR engine: Invalid context in reset");
      return ASR_FAILURE;
   }
   return ctx->reset(ctx->engine_context);
}

void asr_engine_result_free(asr_result_t *result) {
   if (!result)
      return;
   /* All backends allocate result via calloc + text via strdup/malloc,
    * so a single free(text) + free(result) is sufficient for all engines. */
   free(result->text);
   free(result);
}

void asr_engine_cleanup(asr_engine_context_t *ctx) {
   if (!ctx)
      return;

   DAWN_LOG_INFO("ASR engine: Cleaning up %s", asr_engine_name(ctx->engine_type));

   if (ctx->cleanup && ctx->engine_context) {
      ctx->cleanup(ctx->engine_context);
   }

   free(ctx);
}

asr_engine_type_t asr_engine_get_type(asr_engine_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_ERROR("ASR engine: get_type called with NULL context");
      return (asr_engine_type_t)-1;
   }
   return ctx->engine_type;
}

const char *asr_engine_name(asr_engine_type_t engine_type) {
   switch (engine_type) {
      case ASR_ENGINE_WHISPER:
         return "Whisper";
      case ASR_ENGINE_VOSK:
         return "Vosk";
      default:
         return "Unknown";
   }
}

void asr_engine_set_timing_callback(asr_engine_context_t *ctx,
                                    asr_timing_callback_t callback,
                                    void *user_data) {
   if (!ctx)
      return;

   switch (ctx->engine_type) {
#ifdef DAWN_HAS_ASR
      case ASR_ENGINE_WHISPER:
         asr_whisper_set_timing_callback((whisper_asr_context_t *)ctx->engine_context, callback,
                                         user_data);
         break;
#endif
#ifdef DAWN_HAS_ASR_VOSK
      case ASR_ENGINE_VOSK:
         asr_vosk_set_timing_callback((vosk_asr_context_t *)ctx->engine_context, callback,
                                      user_data);
         break;
#endif
      default:
         break;
   }
}
