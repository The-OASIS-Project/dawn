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
 * the project author(s).
 *
 * Stub globals + embedding-engine stubs for test_memory_db_alias.
 *
 * The alias surface (memory_db_alias.c) calls into memory_embeddings.c for
 * Stage 4 cosine; the embedding module is not linked into this standalone
 * test (it would pull in ONNX runtime).  The stubs report "no engine
 * available" so the cascade gracefully zeroes out the embedding-cosine
 * signal — Stage 4 contributes nothing, the other signals still drive the
 * composite.  Tests that need a non-zero cosine signal would need a
 * heavier fixture; Phase 1 does not require it.
 *
 * The dirty-bit observer pattern: the stub `_invalidate_entity_cache`
 * function increments a counter so tests can verify write paths actually
 * trigger invalidation (alias_link, alias_unlink — design §12).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_embeddings.h"

/* g_config and g_secrets are now defined by config_defaults.c (linked
 * into this test binary so config_set_defaults() works in setUp).  The
 * stub previously declared them; that became a multiple-definition error
 * once the real source was linked. */

/* The auth_db_state_t storage is owned by auth_db_core.c; we don't redeclare
 * it here.  But test code reads s_db directly, which is fine via the
 * AUTH_DB_INTERNAL_ALLOWED include. */

/* =============================================================================
 * Embedding-engine stubs
 * ============================================================================= */

/* Counter incremented every time the alias surface invalidates the entity
 * cache.  Tests use this to verify the invalidate-after-COMMIT contract. */
int g_alias_test_entity_cache_invalidations = 0;

bool memory_embeddings_available(void) {
   return false;
}

int memory_embeddings_dims(void) {
   return 0;
}

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   (void)text;
   (void)out;
   if (out_dims)
      *out_dims = 0;
   return FAILURE;
}

float memory_embeddings_l2_norm(const float *vec, int dims) {
   (void)vec;
   (void)dims;
   return 0.0f;
}

int memory_embeddings_entity_cosine(int user_id,
                                    int64_t entity_id,
                                    const float *query_embedding,
                                    int query_dims,
                                    float query_norm,
                                    float *out_cosine) {
   (void)user_id;
   (void)entity_id;
   (void)query_embedding;
   (void)query_dims;
   (void)query_norm;
   (void)out_cosine;
   return FAILURE;
}

void memory_embeddings_invalidate_entity_cache(void) {
   g_alias_test_entity_cache_invalidations++;
}
void memory_embeddings_invalidate_entity_cache_for_user(int user_id) {
   (void)user_id;
   g_alias_test_entity_cache_invalidations++;
}
void memory_embeddings_invalidate_cache_for_user(int user_id) {
   (void)user_id;
}

void memory_embeddings_invalidate_cache(void) {
   /* No-op — alias surface only invalidates the entity cache. */
}

void memory_embeddings_invalidate_all(void) {
   memory_embeddings_invalidate_entity_cache();
}

/* Summary-search semantic compare; reachable from memory_db_summaries.c via
 * memory_db_summary_search_semantic.  This test does not exercise that path,
 * so a no-op returning 0.0f matches the surrounding stubbing style and
 * causes any accidental summary-search call to produce zero-similarity
 * results (graceful degradation, not a crash). */
float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b) {
   (void)a;
   (void)b;
   (void)dims;
   (void)norm_a;
   (void)norm_b;
   return 0.0f;
}
