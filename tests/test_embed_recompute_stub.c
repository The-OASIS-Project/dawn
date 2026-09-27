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
 * Stub symbols for test_embed_recompute.  Provides the minimal set of globals
 * and provider/DB functions so memory_embed_recompute.c links without the full
 * daemon.
 *
 * embedding_engine_available() returns false so the early-exit config-guard
 * tests never reach DB or thread code.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "memory/memory_db.h"
#include "memory/memory_embed_backfill.h"
#include "memory/memory_embeddings.h"

/* =============================================================================
 * Global config
 * ============================================================================= */

dawn_config_t g_config;
secrets_config_t g_secrets;

/* =============================================================================
 * Auth-DB state stub — uninitialized so any DB path fails gracefully
 * ============================================================================= */

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

/* =============================================================================
 * Embedding engine stubs — engine always reports unavailable
 * ============================================================================= */

bool embedding_engine_available(void) {
   return false;
}

int embedding_engine_dims(void) {
   return 0;
}

int embedding_engine_embed(const char *text, float *out, int max_dims, int *out_dims) {
   (void)text;
   (void)out;
   (void)max_dims;
   if (out_dims)
      *out_dims = 0;
   return 1;
}

float embedding_engine_l2_norm(const float *vec, int dims) {
   (void)vec;
   (void)dims;
   return 0.0f;
}

int embedding_engine_init(void) {
   return 1;
}

void embedding_engine_cleanup(void) {
}

/* =============================================================================
 * memory_embeddings stubs
 * ============================================================================= */

int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   (void)user_id;
   (void)fact_id;
   (void)text;
   return 1;
}

int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text) {
   (void)entity_id;
   (void)user_id;
   (void)text;
   return 1;
}

int memory_embeddings_embed_and_store_summary(int user_id, int64_t summary_id, const char *text) {
   (void)user_id;
   (void)summary_id;
   (void)text;
   return 1;
}

void memory_embeddings_invalidate_cache(void) {
}
void memory_embeddings_invalidate_entity_cache(void) {
}

/* =============================================================================
 * memory_db stubs — not called in the config-guard test paths
 * ============================================================================= */

int memory_db_fact_update_embedding(int user_id,
                                    int64_t fact_id,
                                    const float *embedding,
                                    int dims,
                                    float norm) {
   (void)user_id;
   (void)fact_id;
   (void)embedding;
   (void)dims;
   (void)norm;
   return 1;
}

int memory_db_entity_update_embedding(int64_t entity_id,
                                      int user_id,
                                      const float *embedding,
                                      int dims,
                                      float norm) {
   (void)entity_id;
   (void)user_id;
   (void)embedding;
   (void)dims;
   (void)norm;
   return 1;
}

/* Re-index bracket: the recompute worker parks fact backfill while it runs.
 * Nothing to park in this test, so both are no-ops. */
void memory_embeddings_reindex_begin(void) {
}

void memory_embeddings_reindex_end(bool completed) {
   (void)completed;
}
