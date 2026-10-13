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
 * Stubs for test_memory_db_admin: provides s_db, g_config, and the
 * extraction-template-size helper so the test can link memory_db_admin.c
 * standalone without pulling in memory_extraction.c (which drags
 * llm_interface, json-c, embeddings, etc. into the test binary).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

dawn_config_t g_config;

/* memory_extraction stub — return a representative template size so the
 * cost estimator's real-prompt-size invariant is exercised without
 * pulling memory_extraction.c into the test link. */
size_t memory_extraction_get_template_size_chars(void) {
   return 4744; /* matches actual EXTRACTION_PROMPT_TEMPLATE size as of v42 */
}

/* memory_recovery stub — the test never spawns the worker thread, so
 * memory_recovery_run_pass is reachable only via that path; provide a
 * no-op. */
void memory_recovery_run_pass(int *triggered_out, int *skipped_out) {
   if (triggered_out)
      *triggered_out = 0;
   if (skipped_out)
      *skipped_out = 0;
}

/* memory_embeddings_invalidate_all is called after a successful reset
 * transaction — the test doesn't exercise the embedding cache. */
void memory_embeddings_invalidate_all(void) {
}

/* Stubs for memory_db.c symbols pulled in by linking the full TU.  None
 * should be reached by the admin tests (which only exercise reset-derived
 * and undo-extraction-attempt paths).  Per the project's Unity stub
 * pattern (April 2026 migration), never-called stubs abort with a
 * diagnostic so signature drift becomes a compile error and accidental
 * calls are visible at test runtime instead of silently passing. */

uint32_t memory_normalize_and_hash(const char *text, char *out, size_t out_size) {
   (void)text;
   (void)out;
   (void)out_size;
   fprintf(stderr, "FATAL: memory_normalize_and_hash stub called — admin tests "
                   "shouldn't hit fact paths\n");
   abort();
}

void memory_embeddings_invalidate_cache(int user_id) {
   (void)user_id;
   fprintf(stderr, "FATAL: memory_embeddings_invalidate_cache stub called — admin tests "
                   "shouldn't touch the embedding cache\n");
   abort();
}

void memory_embeddings_invalidate_entity_cache(int user_id) {
   (void)user_id;
   fprintf(stderr, "FATAL: memory_embeddings_invalidate_entity_cache stub called — admin "
                   "tests shouldn't touch the entity embedding cache\n");
   abort();
}

float memory_embeddings_l2_norm(const float *vec, int dim) {
   (void)vec;
   (void)dim;
   fprintf(stderr, "FATAL: memory_embeddings_l2_norm stub called — admin tests shouldn't "
                   "exercise vector ops\n");
   abort();
}

/* memory_db_prune_low_confidence deletes through memory_db_facts.c, which this
 * test doesn't link: no fact is pruned here. */
int memory_db_internal_prune_with_history(int user_id,
                                          const char *seed_sql,
                                          int64_t cutoff,
                                          double threshold,
                                          const char *what,
                                          int *count_out) {
   (void)user_id;
   (void)seed_sql;
   (void)cutoff;
   (void)threshold;
   (void)what;
   if (count_out) {
      *count_out = 0;
   }
   return 0;
}
