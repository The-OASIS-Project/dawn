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
 * Stubs for test_memory_provenance: provides s_db, g_config, and
 * conv_db_is_private so memory_db.c can link without pulling in auth_db_core
 * or the full conversation module.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

dawn_config_t g_config;

/* Stub: memory_db_delete_user_memories calls this after dropping rows.  Tests
 * never exercise that path but the symbol must resolve at link time. */
void memory_embeddings_invalidate_cache_for_user(int user_id) {
   (void)user_id;
}

void memory_embeddings_invalidate_all(void) {
}

/* Stubs for the dirty-flag invalidators called from memory_db.c.  Tests
 * exercise the SQL path directly and never inspect cache state, so these
 * are link-time only. */
void memory_embeddings_invalidate_cache(void) {
}
void memory_embeddings_invalidate_entity_cache(void) {
}

/* memory_db.c calls these when reading/writing entity embeddings; the
 * provenance tests touch fact rows, not embeddings, so a noop stub is
 * link-time-sufficient. */
float memory_embeddings_l2_norm(const float *v, int dims) {
   (void)v;
   (void)dims;
   return 0.0f;
}

/* Stub: check is_private column directly via the existing s_db handle. */
int conv_db_is_private(int64_t conv_id, int user_id, bool *is_private_out) {
   (void)user_id;
   if (!is_private_out || !s_db.db)
      return AUTH_DB_FAILURE;
   *is_private_out = false;

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT is_private FROM conversations WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK)
      return AUTH_DB_FAILURE;
   sqlite3_bind_int64(stmt, 1, conv_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *is_private_out = (sqlite3_column_int(stmt, 0) != 0);
   }
   sqlite3_finalize(stmt);
   return AUTH_DB_SUCCESS;
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
