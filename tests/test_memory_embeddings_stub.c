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
 * Stub symbols for test_memory_embeddings. Provides globals and DB/provider
 * symbols so the test can link memory_embeddings.c without the full daemon.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "memory/memory_db_embeddings.h"
#include "memory/memory_embeddings.h"

/* Global config stubs */
dawn_config_t g_config;
secrets_config_t g_secrets;

/* Stub auth_db state.  Category backfill paths in memory_embeddings.c reference
 * s_db; we never call the backfill from these tests, but the symbol must link. */
auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

/* Provider stubs.  "onnx" answers with a 2-dim vector so a test can bring the
 * embedding engine up (for the fact cache); the others are never used. */
static int stub_embed_init(const char *endpoint, const char *model, const char *api_key) {
   (void)endpoint;
   (void)model;
   (void)api_key;
   return 0;
}
static void stub_embed_cleanup(void) {
}
static int stub_embed(const char *text, float *out, int max_dims, int *out_dims) {
   (void)text;
   (void)max_dims;
   out[0] = 1.0f;
   out[1] = 0.0f;
   *out_dims = 2;
   return 0;
}
const embedding_provider_t embedding_provider_onnx = { .name = "onnx",
                                                       .init = stub_embed_init,
                                                       .cleanup = stub_embed_cleanup,
                                                       .embed = stub_embed };
const embedding_provider_t embedding_provider_ollama = { .name = "ollama" };
const embedding_provider_t embedding_provider_openai = { .name = "openai" };

/* DB stubs — match signatures from memory_db.h exactly */
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
   return 0;
}

/* The stored facts the cache loads, best first: ids 1..g_stub_fact_total, each a
 * 2-dim unit vector at stub_fact_angle(id).  Ids up to 600 get distinct angles;
 * later ones share angle 0, except the last, which gets its own (so a test can
 * find the first and the last of a large store).  g_stub_load_limit records the
 * limit the last load asked for. */
#include <math.h>
int g_stub_fact_total = 0;
int g_stub_load_limit = 0;

float stub_fact_angle(int64_t id) {
   if (id <= 600) {
      return (float)id * 0.01f;
   }
   return id == g_stub_fact_total ? 6.1f : 0.0f;
}

int memory_db_fact_foreach_embedding(int user_id,
                                     int expected_dims,
                                     int limit,
                                     memory_fact_embedding_fn fn,
                                     void *ctx) {
   (void)user_id;
   g_stub_load_limit = limit;
   for (int i = 1; i <= g_stub_fact_total && i <= limit && expected_dims == 2; i++) {
      const float v[2] = { cosf(stub_fact_angle(i)), sinf(stub_fact_angle(i)) };
      const memory_fact_embedding_row_t row = { .id = i, .embedding = v, .norm = 1.0f };
      if (fn(&row, g_stub_fact_total, ctx) != 0) {
         return 1;
      }
   }
   return 0;
}

int memory_db_fact_list_without_embedding(int user_id,
                                          int64_t after_id,
                                          int expected_dims,
                                          int64_t *out_ids,
                                          char out_texts[][512],
                                          int max_count,
                                          int *count_out) {
   (void)user_id;
   (void)after_id;
   (void)expected_dims;
   (void)out_ids;
   (void)out_texts;
   (void)max_count;
   if (count_out)
      *count_out = 0;
   return 0;
}

int memory_db_fact_users_needing_backfill(int expected_dims,
                                          int after_user_id,
                                          int *out_user_ids,
                                          int max_count,
                                          int *count_out) {
   (void)expected_dims;
   (void)after_user_id;
   (void)out_user_ids;
   (void)max_count;
   if (count_out)
      *count_out = 0;
   return 0;
}

int memory_db_fact_list_general_embedded(int user_id,
                                         int64_t after_id,
                                         int dims,
                                         int max,
                                         int64_t *ids_out,
                                         float *embs_out,
                                         int *count_out,
                                         int64_t *last_id_out) {
   (void)user_id;
   (void)dims;
   (void)max;
   (void)ids_out;
   (void)embs_out;
   if (count_out)
      *count_out = 0;
   if (last_id_out)
      *last_id_out = after_id;
   return 0;
}

int memory_db_fact_set_categories(int user_id,
                                  const int64_t *ids,
                                  const char *const *categories,
                                  int n,
                                  int *written_out) {
   (void)user_id;
   (void)ids;
   (void)categories;
   (void)n;
   if (written_out)
      *written_out = 0;
   return 0;
}

int auth_db_user_get_categories_backfilled_at(int user_id, int64_t *ts_out) {
   (void)user_id;
   if (ts_out)
      *ts_out = 1; /* treat as already classified */
   return 0;
}

int auth_db_user_set_categories_backfilled_at(int user_id, int64_t ts) {
   (void)user_id;
   (void)ts;
   return 0;
}

#include "memory/memory_types.h"

int memory_db_entity_get_embeddings(int user_id,
                                    bool include_aliases,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max_count,
                                    int *count_out) {
   (void)user_id;
   (void)include_aliases;
   (void)expected_dims;
   (void)out_ids;
   (void)out_names;
   (void)out_types;
   (void)out_embeddings;
   (void)out_norms;
   (void)max_count;
   if (count_out)
      *count_out = 0;
   return 0;
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
   return 0;
}

int memory_db_summary_update_embedding(int user_id,
                                       int64_t summary_id,
                                       const float *embedding,
                                       int dims) {
   (void)user_id;
   (void)summary_id;
   (void)embedding;
   (void)dims;
   return 0;
}
