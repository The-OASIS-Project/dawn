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
 * @file tts_piper.cpp
 * @brief Piper TTS wrapper implementation for common library
 *
 * Provides a simplified C interface to the Piper TTS engine.
 */

#include "tts/tts_piper.h"

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include "logging_common.h"

/* Include Piper headers */
#include "tts/piper.hpp"

/**
 * @brief Internal TTS context structure
 */
struct tts_piper_context {
   piper::PiperConfig config;
   piper::Voice voice;
   int sample_rate;
   float length_scale;

   /* Optional timing callback */
   tts_timing_callback_t timing_callback;
   void *timing_callback_user_data;
};

extern "C" {

tts_piper_config_t tts_piper_default_config(void) {
   tts_piper_config_t config;
   memset(&config, 0, sizeof(config));
   config.model_path = NULL;
   config.model_config_path = NULL;
   config.espeak_data_path = "/usr/share/espeak-ng-data";
   config.length_scale = 1.0f;
   config.use_cuda = 0;
   return config;
}

tts_piper_context_t *tts_piper_init(const tts_piper_config_t *config) {
   if (!config || !config->model_path || !config->model_config_path) {
      DAWN_LOG_ERROR("tts_piper_init: invalid config (missing model paths)");
      return NULL;
   }

   tts_piper_context_t *ctx = new (std::nothrow) tts_piper_context_t();
   if (!ctx) {
      DAWN_LOG_ERROR("tts_piper_init: failed to allocate context");
      return NULL;
   }

   /* Initialize callbacks */
   ctx->timing_callback = NULL;
   ctx->timing_callback_user_data = NULL;

   /* Configure Piper */
   ctx->config.eSpeakDataPath = config->espeak_data_path ? config->espeak_data_path
                                                         : "/usr/share/espeak-ng-data";
   ctx->config.useESpeak = true;
   ctx->config.useTashkeel = false;

   ctx->length_scale = config->length_scale > 0 ? config->length_scale : 1.0f;

   /* Load voice model */
   std::optional<piper::SpeakerId> speakerId = 0;
   try {
      piper::loadVoice(ctx->config, std::string(config->model_path),
                       std::string(config->model_config_path), ctx->voice, speakerId,
                       config->use_cuda ? true : false);

      DAWN_LOG_INFO("tts_piper_init: loaded model %s", config->model_path);
   } catch (const std::exception &e) {
      DAWN_LOG_ERROR("tts_piper_init: failed to load voice: %s", e.what());
      delete ctx;
      return NULL;
   }

   /* Initialize Piper engine (espeak-ng) */
   try {
      piper::initialize(ctx->config);
   } catch (const std::exception &e) {
      DAWN_LOG_ERROR("tts_piper_init: failed to initialize engine: %s", e.what());
      delete ctx;
      return NULL;
   }

   /* Apply length scale */
   ctx->voice.synthesisConfig.lengthScale = ctx->length_scale;

   /* Store sample rate */
   ctx->sample_rate = ctx->voice.synthesisConfig.sampleRate;

   DAWN_LOG_INFO("tts_piper_init: initialized (rate=%d, scale=%.2f, cuda=%s)", ctx->sample_rate,
                 ctx->length_scale, config->use_cuda ? "yes" : "no");

   return ctx;
}

void tts_piper_set_timing_callback(tts_piper_context_t *ctx,
                                   tts_timing_callback_t callback,
                                   void *user_data) {
   if (!ctx)
      return;
   ctx->timing_callback = callback;
   ctx->timing_callback_user_data = user_data;
}

int tts_piper_synthesize(tts_piper_context_t *ctx,
                         const char *text,
                         int16_t **pcm_out,
                         size_t *samples_out,
                         tts_piper_result_t *result) {
   if (!ctx || !text || !pcm_out || !samples_out) {
      DAWN_LOG_ERROR("tts_piper_synthesize: invalid parameters");
      return TTS_ERR_INVALID_PARAM;
   }

   *pcm_out = NULL;
   *samples_out = 0;

   try {
      std::vector<int16_t> audioBuffer;
      piper::SynthesisResult synthResult = {};
      std::atomic<bool> stopFlag(false);

      /* Synthesize audio (nullptr callback to keep all samples in buffer) */
      piper::textToAudio(ctx->config, ctx->voice, std::string(text), audioBuffer, synthResult,
                         stopFlag, nullptr);

      if (audioBuffer.empty()) {
         DAWN_LOG_WARNING("tts_piper_synthesize: no audio generated for text");
         return TTS_SUCCESS; /* Not an error - just empty output */
      }

      /* Allocate output buffer */
      *samples_out = audioBuffer.size();
      *pcm_out = (int16_t *)malloc(*samples_out * sizeof(int16_t));
      if (!*pcm_out) {
         DAWN_LOG_ERROR("tts_piper_synthesize: failed to allocate %zu samples", *samples_out);
         *samples_out = 0;
         return TTS_ERR_OUT_OF_MEMORY;
      }

      /* Copy samples */
      memcpy(*pcm_out, audioBuffer.data(), *samples_out * sizeof(int16_t));

      /* Fill result if provided */
      if (result) {
         result->infer_seconds = synthResult.inferSeconds;
         result->audio_seconds = synthResult.audioSeconds;
         result->real_time_factor = synthResult.realTimeFactor;
      }

      /* Invoke timing callback if registered */
      if (ctx->timing_callback) {
         double infer_ms = synthResult.inferSeconds * 1000.0;
         ctx->timing_callback(infer_ms, synthResult.realTimeFactor, ctx->timing_callback_user_data);
      }

      DAWN_LOG_INFO("tts_piper_synthesize: %zu samples (%.1f ms, RTF=%.3f)", *samples_out,
                    synthResult.inferSeconds * 1000.0, synthResult.realTimeFactor);

      return TTS_SUCCESS;

   } catch (const std::exception &e) {
      DAWN_LOG_ERROR("tts_piper_synthesize: exception: %s", e.what());
      return TTS_ERR_SYNTHESIS;
   }
}

int tts_piper_get_sample_rate(tts_piper_context_t *ctx) {
   return ctx ? ctx->sample_rate : TTS_PIPER_SAMPLE_RATE;
}

void tts_piper_cleanup(tts_piper_context_t *ctx) {
   if (!ctx)
      return;

   try {
      piper::terminate(ctx->config);
   } catch (...) {
      /* Ignore exceptions during cleanup */
   }

   delete ctx;
   DAWN_LOG_INFO("tts_piper_cleanup: cleanup complete");
}

} /* extern "C" */
