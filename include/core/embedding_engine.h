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
 * Provider-agnostic embedding generation and vector math. Shared between
 * the memory system and document search (RAG). Supports ONNX local
 * inference (default), Ollama, and OpenAI-compatible HTTP endpoints.
 */

#ifndef EMBEDDING_ENGINE_H
#define EMBEDDING_ENGINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum embedding dimensions (all-MiniLM = 384, OpenAI ada = 1536) */
#define EMBEDDING_MAX_DIMS 2048

/* Provider interface */
typedef struct {
   const char *name;
   int (*init)(const char *endpoint, const char *model, const char *api_key);
   void (*cleanup)(void);
   int (*embed)(const char *text, float *out, int max_dims, int *out_dims);
} embedding_provider_t;

/* Provider registration (called by provider implementations) */
extern const embedding_provider_t embedding_provider_onnx;
extern const embedding_provider_t embedding_provider_ollama;
extern const embedding_provider_t embedding_provider_openai;

/**
 * @brief Initialize the embedding engine
 *
 * Selects and initializes the configured provider based on dawn.toml
 * [memory] embedding_provider setting. Idempotent — safe to call multiple
 * times (subsequent calls are no-ops).
 *
 * @return 0 on success, non-zero on failure (non-fatal — callers fall back)
 */
int embedding_engine_init(void);

/**
 * @brief Shut down the embedding engine
 *
 * Releases provider resources. Only cleans up the provider — callers
 * (memory system, document search) manage their own caches/threads.
 */
void embedding_engine_cleanup(void);

/**
 * @brief Check if the embedding engine is available
 *
 * @return true if a provider is initialized and ready
 */
bool embedding_engine_available(void);

/**
 * @brief Get the embedding dimension for the current provider
 *
 * @return Number of dimensions, or 0 if not initialized
 */
int embedding_engine_dims(void);

/**
 * @brief Generate an embedding for text
 *
 * Thread-safe for HTTP providers. ONNX provider thread safety depends
 * on the ONNX runtime session configuration.
 *
 * @param text Input text to embed
 * @param out Output float array (must hold at least EMBEDDING_MAX_DIMS)
 * @param max_dims Maximum dimensions to write
 * @param out_dims Output: actual dimensions written
 * @return 0 on success, non-zero on failure
 */
int embedding_engine_embed(const char *text, float *out, int max_dims, int *out_dims);

/**
 * @brief Compute L2 norm of a float vector
 */
float embedding_engine_l2_norm(const float *vec, int dims);

/**
 * @brief Compute cosine similarity using pre-computed norms
 *
 * @return Cosine similarity clamped to [0, 1]
 */
float embedding_engine_cosine_with_norms(const float *a,
                                         const float *b,
                                         int dims,
                                         float norm_a,
                                         float norm_b);

/**
 * @brief Compute cosine similarity (computes norms internally)
 *
 * @return Cosine similarity clamped to [0, 1]
 */
float embedding_engine_cosine(const float *a, const float *b, int dims);

/** Fewest scored items for a corpus mean to serve as a relevance baseline. */
#define EMBEDDING_RELEVANCE_MIN_POOL 32

/**
 * @brief How far a cosine stands above its corpus: (cos - mean) / (1 - mean)
 *
 * Embedding models put unrelated text at a model-specific baseline similarity
 * (bge-small about 0.43, MiniLM about 0.1), so a raw cosine threshold doesn't
 * transfer between models.  Measured from the corpus mean it does: 0 is
 * corpus-typical, 1 is identical.  Only meaningful for a pool of at least
 * EMBEDDING_RELEVANCE_MIN_POOL.
 *
 * @param cosine     Item's cosine to the query
 * @param cosine_sum Sum of the pool's cosines to the query
 * @param pool       Pool size
 * @return Relevance (can be negative), or 0 for an empty/degenerate pool
 */
static inline float embedding_corpus_relevance(float cosine, double cosine_sum, int pool) {
   if (pool <= 0) {
      return 0.0f;
   }
   const double mean = cosine_sum / (double)pool;
   if (mean >= 1.0) {
      return 0.0f;
   }
   return (float)(((double)cosine - mean) / (1.0 - mean));
}

#ifdef __cplusplus
}
#endif

#endif /* EMBEDDING_ENGINE_H */
