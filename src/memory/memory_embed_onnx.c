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
 * ONNX Embedding Provider
 *
 * Local embedding generation with a 384-dimensional BERT-family model
 * (default bge-small-en-v1.5, int8 quantized).  Implements WordPiece
 * tokenizer and ONNX Runtime inference with mean pooling.
 *
 * Model files: models/embeddings/bge-small-en-v1.5-int8.onnx (default)
 *              models/embeddings/vocab.txt
 */

#include <math.h>
#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embed_tokenizer.h"
#include "memory/memory_embeddings.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define ONNX_MAX_SEQ_LEN 256 /* Max tokens (MiniLM limit) */
#define ONNX_HIDDEN_DIM 384  /* model output dimension */
#define MODEL_PATH "models/embeddings/bge-small-en-v1.5-int8.onnx"
#define VOCAB_PATH "models/embeddings/vocab.txt"

/* =============================================================================
 * ONNX Runtime State
 * ============================================================================= */

static struct {
   const OrtApi *ort;
   OrtEnv *env;
   OrtSession *session;
   OrtMemoryInfo *memory_info;
   bool initialized;
} s_onnx;

/* =============================================================================
 * Provider Implementation
 * ============================================================================= */

static int onnx_init(const char *endpoint, const char *model, const char *api_key) {
   (void)endpoint;
   (void)model;
   (void)api_key;

   memset(&s_onnx, 0, sizeof(s_onnx));

   /* Allow env vars to override paths for benchmarking/experimentation. */
   const char *vocab_path = getenv("DAWN_ONNX_VOCAB");
   if (!vocab_path || *vocab_path == '\0')
      vocab_path = VOCAB_PATH;

   /* Load vocabulary via shared tokenizer (refcounted). */
   if (memory_embed_tokenizer_acquire(vocab_path) != SUCCESS) {
      return FAILURE;
   }

   /* Get ONNX Runtime API */
   s_onnx.ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
   if (!s_onnx.ort) {
      OLOG_ERROR("memory_embed_onnx: failed to get ONNX Runtime API");
      memory_embed_tokenizer_release();
      return FAILURE;
   }

   /* Create environment */
   OrtStatus *status = s_onnx.ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "memory_embed",
                                             &s_onnx.env);
   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: create env failed: %s", s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      memory_embed_tokenizer_release();
      return FAILURE;
   }

   /* Session options */
   OrtSessionOptions *opts;
   status = s_onnx.ort->CreateSessionOptions(&opts);
   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: create session options failed: %s",
                 s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseEnv(s_onnx.env);
      memory_embed_tokenizer_release();
      return FAILURE;
   }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
   s_onnx.ort->SetIntraOpNumThreads(opts, 1);
   s_onnx.ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
#pragma GCC diagnostic pop

   /* Load model */
   const char *model_path = getenv("DAWN_ONNX_MODEL");
   if (!model_path || *model_path == '\0')
      model_path = MODEL_PATH;
   status = s_onnx.ort->CreateSession(s_onnx.env, model_path, opts, &s_onnx.session);
   s_onnx.ort->ReleaseSessionOptions(opts);

   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: load model failed: %s", s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseEnv(s_onnx.env);
      memory_embed_tokenizer_release();
      return FAILURE;
   }

   /* Memory info */
   status = s_onnx.ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                            &s_onnx.memory_info);
   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: create memory info failed: %s",
                 s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseSession(s_onnx.session);
      s_onnx.ort->ReleaseEnv(s_onnx.env);
      memory_embed_tokenizer_release();
      return FAILURE;
   }

   s_onnx.initialized = true;
   OLOG_INFO("memory_embed_onnx: ONNX provider initialized (model: %s)", model_path);
   return 0;
}

static void onnx_cleanup(void) {
   if (!s_onnx.initialized)
      return;

   if (s_onnx.memory_info)
      s_onnx.ort->ReleaseMemoryInfo(s_onnx.memory_info);
   if (s_onnx.session)
      s_onnx.ort->ReleaseSession(s_onnx.session);
   if (s_onnx.env)
      s_onnx.ort->ReleaseEnv(s_onnx.env);

   memory_embed_tokenizer_release();
   s_onnx.initialized = false;
   OLOG_INFO("memory_embed_onnx: cleanup complete");
}

/**
 * @brief Run ONNX inference and mean-pool to get sentence embedding
 *
 * Input: 3 tensors (input_ids, attention_mask, token_type_ids) [1, seq_len]
 * Output: [1, seq_len, 384] -> mean pool over seq_len -> [384]
 */
static int onnx_embed(const char *text, float *out, int max_dims, int *out_dims) {
   if (!s_onnx.initialized || !text || !out || !out_dims)
      return FAILURE;

   /* Tokenize */
   int64_t input_ids[ONNX_MAX_SEQ_LEN];
   int64_t attention_mask[ONNX_MAX_SEQ_LEN];
   int64_t token_type_ids[ONNX_MAX_SEQ_LEN];

   int seq_len = memory_embed_tokenizer_encode(text, input_ids, attention_mask, token_type_ids,
                                               ONNX_MAX_SEQ_LEN);
   if (seq_len <= 2) {
      /* Only [CLS] and [SEP] — no real content */
      *out_dims = 0;
      return FAILURE;
   }

   /* Create input tensors */
   int64_t shape[2] = { 1, seq_len };

   OrtValue *input_tensors[3] = { NULL, NULL, NULL };
   OrtStatus *status;

   status = s_onnx.ort->CreateTensorWithDataAsOrtValue(s_onnx.memory_info, input_ids,
                                                       seq_len * sizeof(int64_t), shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                       &input_tensors[0]);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      return FAILURE;
   }

   status = s_onnx.ort->CreateTensorWithDataAsOrtValue(s_onnx.memory_info, attention_mask,
                                                       seq_len * sizeof(int64_t), shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                       &input_tensors[1]);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseValue(input_tensors[0]);
      return FAILURE;
   }

   status = s_onnx.ort->CreateTensorWithDataAsOrtValue(s_onnx.memory_info, token_type_ids,
                                                       seq_len * sizeof(int64_t), shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                       &input_tensors[2]);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseValue(input_tensors[0]);
      s_onnx.ort->ReleaseValue(input_tensors[1]);
      return FAILURE;
   }

   /* Run inference */
   const char *input_names[] = { "input_ids", "attention_mask", "token_type_ids" };
   const char *output_names[] = { "last_hidden_state" };
   OrtValue *output_tensor = NULL;

   status = s_onnx.ort->Run(s_onnx.session, NULL, input_names,
                            (const OrtValue *const *)input_tensors, 3, output_names, 1,
                            &output_tensor);

   /* Release inputs */
   s_onnx.ort->ReleaseValue(input_tensors[0]);
   s_onnx.ort->ReleaseValue(input_tensors[1]);
   s_onnx.ort->ReleaseValue(input_tensors[2]);

   if (status != NULL) {
      OLOG_ERROR("memory_embed_onnx: inference failed: %s", s_onnx.ort->GetErrorMessage(status));
      s_onnx.ort->ReleaseStatus(status);
      return FAILURE;
   }

   /* Get output data — shape [1, seq_len, hidden_dim] */
   float *output_data;
   status = s_onnx.ort->GetTensorMutableData(output_tensor, (void **)&output_data);
   if (status != NULL) {
      s_onnx.ort->ReleaseStatus(status);
      s_onnx.ort->ReleaseValue(output_tensor);
      return FAILURE;
   }

   /* Get output shape to determine hidden_dim */
   OrtTensorTypeAndShapeInfo *shape_info;
   OrtStatus *shape_status = s_onnx.ort->GetTensorTypeAndShape(output_tensor, &shape_info);
   if (shape_status) {
      s_onnx.ort->ReleaseStatus(shape_status);
      s_onnx.ort->ReleaseValue(output_tensor);
      return FAILURE;
   }
   size_t num_dims;
   OrtStatus *dim_status = s_onnx.ort->GetDimensionsCount(shape_info, &num_dims);
   if (dim_status)
      s_onnx.ort->ReleaseStatus(dim_status);
   int64_t out_shape[4];
   dim_status = s_onnx.ort->GetDimensions(shape_info, out_shape, num_dims);
   if (dim_status)
      s_onnx.ort->ReleaseStatus(dim_status);
   s_onnx.ort->ReleaseTensorTypeAndShapeInfo(shape_info);

   int hidden_dim = (num_dims >= 3) ? (int)out_shape[2] : ONNX_HIDDEN_DIM;
   if (hidden_dim > max_dims)
      hidden_dim = max_dims;

   /* Mean pooling with attention mask */
   memset(out, 0, (size_t)hidden_dim * sizeof(float));
   float token_sum = 0.0f;

   for (int t = 0; t < seq_len; t++) {
      if (attention_mask[t] == 0)
         continue;
      for (int d = 0; d < hidden_dim; d++) {
         out[d] += output_data[t * hidden_dim + d];
      }
      token_sum += 1.0f;
   }

   if (token_sum > 0.0f) {
      for (int d = 0; d < hidden_dim; d++) {
         out[d] /= token_sum;
      }
   }

   /* L2 normalize */
   float norm = 0.0f;
   for (int d = 0; d < hidden_dim; d++) {
      norm += out[d] * out[d];
   }
   norm = sqrtf(norm);
   if (norm > 0.0f) {
      for (int d = 0; d < hidden_dim; d++) {
         out[d] /= norm;
      }
   }

   *out_dims = hidden_dim;
   s_onnx.ort->ReleaseValue(output_tensor);

   return 0;
}

/* =============================================================================
 * Provider Registration
 * ============================================================================= */

const embedding_provider_t embedding_provider_onnx = {
   .name = "onnx",
   .init = onnx_init,
   .cleanup = onnx_cleanup,
   .embed = onnx_embed,
};
