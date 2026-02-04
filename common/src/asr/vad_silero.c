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
 * @file vad_silero.c
 * @brief Silero VAD ONNX Runtime implementation
 *
 * Implements voice activity detection using the Silero VAD model
 * (silero_vad_16k_op15.onnx). Provides C API for speech probability
 * inference on 16kHz audio streams.
 */

#include "asr/vad_silero.h"

#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging_common.h"

#define VAD_SAMPLE_SIZE 512          /* 32ms at 16kHz */
#define VAD_CONTEXT_SIZE 64          /* Context window for 16kHz (required by model) */
#define VAD_STATE_SIZE (2 * 1 * 128) /* LSTM state dimensions: [2, 1, 128] */
#define SAMPLE_RATE 16000
#define INT16_MAX_FLOAT 32768.0f /* Maximum int16_t value for PCM normalization */

/**
 * @brief Silero VAD context structure
 *
 * Holds ONNX Runtime session, model state, and configuration.
 * Created by vad_silero_init(), destroyed by vad_silero_cleanup().
 */
struct silero_vad_context {
   const OrtApi *ort;          /* ONNX Runtime API */
   OrtEnv *env;                /* ONNX Runtime environment (shared or owned) */
   OrtSession *session;        /* ONNX Runtime session */
   OrtMemoryInfo *memory_info; /* CPU memory allocator */

   int owns_env; /* 1 if we created env, 0 if shared */

   float state[VAD_STATE_SIZE];     /* LSTM state: [2, 1, 128] */
   float context[VAD_CONTEXT_SIZE]; /* Context buffer: last 64 samples */
   int64_t sample_rate;             /* Sample rate (always 16000) */

   /* Optional callback for probability updates */
   vad_probability_callback_t prob_callback;
   void *prob_callback_user_data;
};

/**
 * @brief Set optional callback for VAD probability updates
 */
void vad_silero_set_probability_callback(silero_vad_context_t *ctx,
                                         vad_probability_callback_t callback,
                                         void *user_data) {
   if (!ctx)
      return;
   ctx->prob_callback = callback;
   ctx->prob_callback_user_data = user_data;
}

/**
 * @brief Initialize Silero VAD system
 *
 * Loads the ONNX model and creates the inference session.
 * Attempts to use shared ONNX Runtime environment (Option A) if provided.
 *
 * @param model_path Absolute path to silero_vad_16k_op15.onnx
 * @param shared_env Optional shared OrtEnv pointer (pass NULL for separate env)
 * @return Initialized VAD context, or NULL on failure
 */
silero_vad_context_t *vad_silero_init(const char *model_path, void *shared_env) {
   if (!model_path) {
      DAWN_LOG_ERROR("vad_silero_init: model_path is NULL");
      return NULL;
   }

   silero_vad_context_t *ctx = calloc(1, sizeof(silero_vad_context_t));
   if (!ctx) {
      DAWN_LOG_ERROR("vad_silero_init: failed to allocate context");
      return NULL;
   }

   /* Get ONNX Runtime API */
   ctx->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
   if (!ctx->ort) {
      DAWN_LOG_ERROR("vad_silero_init: failed to get ONNX Runtime API");
      free(ctx);
      return NULL;
   }

   /* Option A: Try to use shared environment with Piper */
   if (shared_env) {
      DAWN_LOG_INFO("vad_silero_init: using shared ONNX Runtime environment");
      ctx->env = (OrtEnv *)shared_env;
      ctx->owns_env = 0;
   } else {
      /* Fallback: Create our own environment */
      DAWN_LOG_INFO("vad_silero_init: creating separate ONNX Runtime environment");
      OrtStatus *status = ctx->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "silero_vad", &ctx->env);
      if (status != NULL) {
         DAWN_LOG_ERROR("vad_silero_init: failed to create ONNX Runtime environment: %s",
                        ctx->ort->GetErrorMessage(status));
         ctx->ort->ReleaseStatus(status);
         free(ctx);
         return NULL;
      }
      ctx->owns_env = 1;
   }

   /* Create session options */
   OrtSessionOptions *session_options;
   OrtStatus *status = ctx->ort->CreateSessionOptions(&session_options);
   if (status != NULL) {
      DAWN_LOG_ERROR("vad_silero_init: failed to create session options: %s",
                     ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
      if (ctx->owns_env)
         ctx->ort->ReleaseEnv(ctx->env);
      free(ctx);
      return NULL;
   }

   /* Configure session for low-latency inference (non-critical optimization hints) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   ctx->ort->SetIntraOpNumThreads(session_options, 1);
   ctx->ort->SetSessionGraphOptimizationLevel(session_options, ORT_ENABLE_ALL);
#pragma GCC diagnostic pop

   /* Load model */
   DAWN_LOG_INFO("vad_silero_init: loading model from %s", model_path);
   status = ctx->ort->CreateSession(ctx->env, model_path, session_options, &ctx->session);
   ctx->ort->ReleaseSessionOptions(session_options);

   if (status != NULL) {
      DAWN_LOG_ERROR("vad_silero_init: failed to load model: %s",
                     ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
      if (ctx->owns_env)
         ctx->ort->ReleaseEnv(ctx->env);
      free(ctx);
      return NULL;
   }

   /* Create CPU memory info */
   status = ctx->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &ctx->memory_info);
   if (status != NULL) {
      DAWN_LOG_ERROR("vad_silero_init: failed to create memory info: %s",
                     ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
      ctx->ort->ReleaseSession(ctx->session);
      if (ctx->owns_env)
         ctx->ort->ReleaseEnv(ctx->env);
      free(ctx);
      return NULL;
   }

   /* Debug: Print model input/output info (non-critical debug logging) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   size_t num_inputs, num_outputs;
   ctx->ort->SessionGetInputCount(ctx->session, &num_inputs);
   ctx->ort->SessionGetOutputCount(ctx->session, &num_outputs);
   DAWN_LOG_INFO("vad_silero_init: model has %zu inputs, %zu outputs", num_inputs, num_outputs);

   OrtAllocator *allocator;
   ctx->ort->GetAllocatorWithDefaultOptions(&allocator);

   for (size_t i = 0; i < num_inputs; i++) {
      char *name;
      ctx->ort->SessionGetInputName(ctx->session, i, allocator, &name);
      OrtTypeInfo *type_info;
      ctx->ort->SessionGetInputTypeInfo(ctx->session, i, &type_info);
      const OrtTensorTypeAndShapeInfo *tensor_info;
      ctx->ort->CastTypeInfoToTensorInfo(type_info, &tensor_info);
      size_t num_dims;
      ctx->ort->GetDimensionsCount(tensor_info, &num_dims);
      DAWN_LOG_INFO("  Input %zu: name='%s', dims=%zu", i, name, num_dims);
      allocator->Free(allocator, name);
      ctx->ort->ReleaseTypeInfo(type_info);
   }

   for (size_t i = 0; i < num_outputs; i++) {
      char *name;
      ctx->ort->SessionGetOutputName(ctx->session, i, allocator, &name);
      DAWN_LOG_INFO("  Output %zu: name='%s'", i, name);
      allocator->Free(allocator, name);
   }
#pragma GCC diagnostic pop

   /* Initialize state and context to zero (required for first inference) */
   memset(ctx->state, 0, sizeof(ctx->state));
   memset(ctx->context, 0, sizeof(ctx->context));
   ctx->sample_rate = SAMPLE_RATE;

   /* Initialize callback to NULL */
   ctx->prob_callback = NULL;
   ctx->prob_callback_user_data = NULL;

   DAWN_LOG_INFO("vad_silero_init: initialized successfully");
   return ctx;
}

/**
 * @brief Process audio chunk and get speech probability
 *
 * Runs Silero VAD inference on 512 samples (32ms at 16kHz).
 * Maintains LSTM state across calls for context-aware detection.
 *
 * @param ctx VAD context from vad_silero_init()
 * @param audio_samples 512 int16_t samples at 16kHz
 * @param num_samples Number of samples (must be 512)
 * @return Speech probability (0.0-1.0), or -1.0 on error
 */
float vad_silero_process(silero_vad_context_t *ctx,
                         const int16_t *audio_samples,
                         size_t num_samples) {
   if (!ctx || !audio_samples) {
      DAWN_LOG_ERROR("vad_silero_process: NULL context or audio_samples");
      return -1.0f;
   }

   if (num_samples != VAD_SAMPLE_SIZE) {
      DAWN_LOG_ERROR("vad_silero_process: expected %d samples, got %zu", VAD_SAMPLE_SIZE,
                     num_samples);
      return -1.0f;
   }

   /* Convert int16 to normalized float [-1.0, 1.0] and prepend context */
   /* Model expects [context + audio] = [64 + 512] = 576 samples total */
   float audio_with_context[VAD_CONTEXT_SIZE + VAD_SAMPLE_SIZE];

   /* Copy context (last 64 samples from previous call) */
   memcpy(audio_with_context, ctx->context, VAD_CONTEXT_SIZE * sizeof(float));

   /* Normalize and append current audio samples */
   for (size_t i = 0; i < VAD_SAMPLE_SIZE; i++) {
      audio_with_context[VAD_CONTEXT_SIZE + i] = (float)audio_samples[i] / INT16_MAX_FLOAT;
   }

   /* Create input tensor: [1, 576] (context + audio) */
   int64_t input_shape[] = { 1, VAD_CONTEXT_SIZE + VAD_SAMPLE_SIZE };
   OrtValue *input_tensor = NULL;
   OrtStatus *status = ctx->ort->CreateTensorWithDataAsOrtValue(
       ctx->memory_info, audio_with_context, (VAD_CONTEXT_SIZE + VAD_SAMPLE_SIZE) * sizeof(float),
       input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor);
   if (status != NULL) {
      DAWN_LOG_ERROR("vad_silero_process: failed to create input tensor: %s",
                     ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
      return -1.0f;
   }

   /* Create state tensor: [2, 1, 128] */
   int64_t state_shape[] = { 2, 1, 128 };
   OrtValue *state_tensor = NULL;
   status = ctx->ort->CreateTensorWithDataAsOrtValue(ctx->memory_info, ctx->state,
                                                     VAD_STATE_SIZE * sizeof(float), state_shape, 3,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                     &state_tensor);
   if (status != NULL) {
      DAWN_LOG_ERROR("vad_silero_process: failed to create state tensor: %s",
                     ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
      ctx->ort->ReleaseValue(input_tensor);
      return -1.0f;
   }

   /* Create sample rate tensor: scalar (0 dimensions) */
   OrtValue *sr_tensor = NULL;
   status = ctx->ort->CreateTensorWithDataAsOrtValue(ctx->memory_info, &ctx->sample_rate,
                                                     sizeof(int64_t), NULL, 0,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                     &sr_tensor);
   if (status != NULL) {
      DAWN_LOG_ERROR("vad_silero_process: failed to create sample rate tensor: %s",
                     ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
      ctx->ort->ReleaseValue(input_tensor);
      ctx->ort->ReleaseValue(state_tensor);
      return -1.0f;
   }

   /* Run inference */
   const char *input_names[] = { "input", "state", "sr" };
   const OrtValue *inputs[] = { input_tensor, state_tensor, sr_tensor };

   const char *output_names[] = { "output", "stateN" };
   OrtValue *outputs[2] = { NULL, NULL };

   status = ctx->ort->Run(ctx->session, NULL, input_names, inputs, 3, output_names, 2, outputs);

   float speech_prob = -1.0f;

   if (status == NULL) {
      /* Extract speech probability */
      float *output_data;
      status = ctx->ort->GetTensorMutableData(outputs[0], (void **)&output_data);
      if (status == NULL) {
         speech_prob = output_data[0];

         /* Call optional probability callback */
         if (ctx->prob_callback) {
            ctx->prob_callback(speech_prob, ctx->prob_callback_user_data);
         }

         /* Update internal state for next inference */
         float *new_state;
         status = ctx->ort->GetTensorMutableData(outputs[1], (void **)&new_state);
         if (status == NULL) {
            memcpy(ctx->state, new_state, VAD_STATE_SIZE * sizeof(float));
         } else {
            DAWN_LOG_WARNING("vad_silero_process: failed to get new state: %s",
                             ctx->ort->GetErrorMessage(status));
            ctx->ort->ReleaseStatus(status);
         }

         /* Save last 64 samples as context for next inference */
         memcpy(ctx->context,
                &audio_with_context[(VAD_CONTEXT_SIZE + VAD_SAMPLE_SIZE) - VAD_CONTEXT_SIZE],
                VAD_CONTEXT_SIZE * sizeof(float));
      } else {
         DAWN_LOG_ERROR("vad_silero_process: failed to get output data: %s",
                        ctx->ort->GetErrorMessage(status));
         ctx->ort->ReleaseStatus(status);
      }

      /* Release outputs */
      if (outputs[0])
         ctx->ort->ReleaseValue(outputs[0]);
      if (outputs[1])
         ctx->ort->ReleaseValue(outputs[1]);
   } else {
      DAWN_LOG_ERROR("vad_silero_process: inference failed: %s", ctx->ort->GetErrorMessage(status));
      ctx->ort->ReleaseStatus(status);
   }

   /* Cleanup input tensors */
   ctx->ort->ReleaseValue(input_tensor);
   ctx->ort->ReleaseValue(state_tensor);
   ctx->ort->ReleaseValue(sr_tensor);

   return speech_prob;
}

/**
 * @brief Reset VAD internal state
 *
 * Zeros the LSTM state to prevent past audio from influencing
 * current inference. Call at interaction boundaries.
 *
 * @param ctx VAD context from vad_silero_init()
 */
void vad_silero_reset(silero_vad_context_t *ctx) {
   if (!ctx) {
      DAWN_LOG_WARNING("vad_silero_reset: NULL context");
      return;
   }

   memset(ctx->state, 0, sizeof(ctx->state));
   memset(ctx->context, 0, sizeof(ctx->context));
   /* State reset (lightweight operation, no logging needed) */
}

/**
 * @brief Clean up VAD resources
 *
 * Frees ONNX Runtime session and all internal resources.
 * Does NOT free shared environment if using Option A.
 *
 * @param ctx VAD context to clean up (can be NULL)
 */
void vad_silero_cleanup(silero_vad_context_t *ctx) {
   if (!ctx)
      return;

   if (ctx->memory_info)
      ctx->ort->ReleaseMemoryInfo(ctx->memory_info);

   if (ctx->session)
      ctx->ort->ReleaseSession(ctx->session);

   /* Only release environment if we own it (not shared) */
   if (ctx->owns_env && ctx->env)
      ctx->ort->ReleaseEnv(ctx->env);

   free(ctx);
   DAWN_LOG_INFO("vad_silero_cleanup: cleanup complete");
}
