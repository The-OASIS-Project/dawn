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
 * Shared Embedding Engine
 *
 * Provider selection, initialization, embedding generation, and vector
 * math. Extracted from memory_embeddings.c to share with document search.
 */

#include "core/embedding_engine.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * State
 * ============================================================================= */

static const embedding_provider_t *s_provider = NULL;
static int s_dims = 0;
static bool s_initialized = false;
static pthread_mutex_t s_embed_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Math Utilities
 * ============================================================================= */

float embedding_engine_l2_norm(const float *vec, int dims) {
   if (!vec || dims <= 0)
      return 0.0f;

   float sum = 0.0f;
   for (int i = 0; i < dims; i++) {
      sum += vec[i] * vec[i];
   }
   return sqrtf(sum);
}

float embedding_engine_cosine_with_norms(const float *a,
                                         const float *b,
                                         int dims,
                                         float norm_a,
                                         float norm_b) {
   if (!a || !b || dims <= 0 || norm_a <= 0.0f || norm_b <= 0.0f)
      return 0.0f;

   float dot = 0.0f;
#ifdef __ARM_NEON
   int i = 0;
   float32x4_t vsum = vdupq_n_f32(0.0f);
   for (; i + 3 < dims; i += 4) {
      float32x4_t va = vld1q_f32(a + i);
      float32x4_t vb = vld1q_f32(b + i);
      vsum = vmlaq_f32(vsum, va, vb);
   }
   dot = vaddvq_f32(vsum);
   for (; i < dims; i++) {
      dot += a[i] * b[i];
   }
#else
   for (int i = 0; i < dims; i++) {
      dot += a[i] * b[i];
   }
#endif

   float sim = dot / (norm_a * norm_b);

   /* Clamp to [0, 1] — negative similarity treated as irrelevant */
   if (sim < 0.0f)
      sim = 0.0f;
   if (sim > 1.0f)
      sim = 1.0f;

   return sim;
}

float embedding_engine_cosine(const float *a, const float *b, int dims) {
   float norm_a = embedding_engine_l2_norm(a, dims);
   float norm_b = embedding_engine_l2_norm(b, dims);
   return embedding_engine_cosine_with_norms(a, b, dims, norm_a, norm_b);
}

/* =============================================================================
 * Provider Selection and Init
 * ============================================================================= */

int embedding_engine_init(void) {
   /* Idempotent — safe to call multiple times */
   if (s_initialized)
      return (s_provider != NULL) ? SUCCESS : FAILURE;

   s_initialized = true;

   const char *provider_name = g_config.memory.embedding_provider;

   /* Empty or disabled */
   if (!provider_name || provider_name[0] == '\0') {
      OLOG_INFO("embedding_engine: disabled (no provider configured)");
      return 0;
   }

   /* Select provider */
   if (strcmp(provider_name, "onnx") == 0) {
      s_provider = &embedding_provider_onnx;
   } else if (strcmp(provider_name, "ollama") == 0) {
      s_provider = &embedding_provider_ollama;
   } else if (strcmp(provider_name, "openai") == 0) {
      s_provider = &embedding_provider_openai;
   } else {
      OLOG_WARNING("embedding_engine: unknown provider '%s', disabling", provider_name);
      return 0;
   }

   /* Determine API key — fall back to openai_api_key for openai provider */
   const char *api_key = g_secrets.embedding_api_key;
   if ((!api_key || api_key[0] == '\0') && strcmp(provider_name, "openai") == 0) {
      api_key = g_secrets.openai_api_key;
   }

   /* Init the provider */
   int rc = s_provider->init(g_config.memory.embedding_endpoint, g_config.memory.embedding_model,
                             api_key);
   if (rc != 0) {
      OLOG_WARNING("embedding_engine: provider '%s' init failed", provider_name);
      s_provider = NULL;
      return rc;
   }

   /* Probe dimensions by embedding a test string */
   float test_buf[EMBEDDING_MAX_DIMS];
   int dims = 0;
   rc = s_provider->embed("test", test_buf, EMBEDDING_MAX_DIMS, &dims);
   if (rc != 0 || dims <= 0) {
      OLOG_WARNING("embedding_engine: dimension probe failed, disabling");
      s_provider->cleanup();
      s_provider = NULL;
      return FAILURE;
   }

   s_dims = dims;
   OLOG_INFO("embedding_engine: initialized provider '%s' (%d dimensions)", provider_name, dims);

   return 0;
}

void embedding_engine_cleanup(void) {
   /* Hold s_embed_mutex so we don't race with an in-flight
    * embedding_engine_embed() — provider cleanup releases the shared
    * tokenizer, which would then UAF if a concurrent embed call were
    * still reading the vocab table. */
   pthread_mutex_lock(&s_embed_mutex);
   if (s_provider) {
      s_provider->cleanup();
      s_provider = NULL;
   }
   s_dims = 0;
   s_initialized = false;
   pthread_mutex_unlock(&s_embed_mutex);
}

bool embedding_engine_available(void) {
   return s_provider != NULL && s_dims > 0;
}

int embedding_engine_dims(void) {
   return s_dims;
}

/* =============================================================================
 * Embedding Generation
 * ============================================================================= */

int embedding_engine_embed(const char *text, float *out, int max_dims, int *out_dims) {
   if (!s_provider || !text || !out || !out_dims)
      return FAILURE;

   pthread_mutex_lock(&s_embed_mutex);
   int rc = s_provider->embed(text, out, max_dims, out_dims);
   pthread_mutex_unlock(&s_embed_mutex);
   return rc;
}
