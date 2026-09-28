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
 * Stubs for the temporal_relations test: provides s_db and a minimal
 * g_config so memory_db.c can link without pulling in auth_db_core or
 * the config parser.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

dawn_config_t g_config;

/* Embedding-cache invalidators and math helpers called from memory_db.c
 * mutation paths and semantic search; the embeddings module isn't linked
 * into this standalone test, so the stubs are no-ops (l2_norm returns 0
 * since the test never exercises semantic summary search). */
void memory_embeddings_invalidate_all(void) {
}

void memory_embeddings_invalidate_cache(void) {
}

void memory_embeddings_invalidate_entity_cache(void) {
}
void memory_embeddings_invalidate_entity_cache_for_user(int user_id) {
   (void)user_id;
}

float memory_embeddings_l2_norm(const float *vec, int dims) {
   (void)vec;
   (void)dims;
   return 0.0f;
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
