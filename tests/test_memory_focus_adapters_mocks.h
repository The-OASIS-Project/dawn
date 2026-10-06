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
 * Mock state shared between test_memory_focus_adapters.c,
 * test_memory_fact_search.c, and test_memory_focus_adapters_stub.c.
 * Each test function programs the relevant fields, then calls the
 * code-under-test, then asserts on outputs.  `mock_reset()` between
 * tests zeroes everything.
 */
#ifndef TEST_MEMORY_FOCUS_ADAPTERS_MOCKS_H
#define TEST_MEMORY_FOCUS_ADAPTERS_MOCKS_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_db_provenance.h" /* for memory_provenance_t */
#include "memory/memory_types.h"

#define MOCK_MAX_FACTS 32
#define MOCK_MAX_ENTITIES 600 /* past the entity cache's first read (512) */
#define MOCK_MAX_RELATIONS 32
#define MOCK_MAX_SUMMARIES 16
#define MOCK_MAX_HYBRID 16
#define MOCK_DIMS 4 /* keep tiny for fast cosine; production uses 384 */

typedef struct {
   int64_t fact_id;
   float score;
   int user_id_filter; /* 0 = always emit; non-zero gates the result */
} mock_hybrid_result_t;

typedef struct {
   /* Facts */
   memory_fact_t facts[MOCK_MAX_FACTS];
   memory_provenance_t fact_provenance[MOCK_MAX_FACTS];
   int fact_count;

   /* Entities */
   memory_entity_t entities[MOCK_MAX_ENTITIES];
   bool entity_keyword_match[MOCK_MAX_ENTITIES];
   const float *entity_embeddings[MOCK_MAX_ENTITIES]; /* NULL = no embedding */
   float entity_norms[MOCK_MAX_ENTITIES];
   const char *entity_photo_ids[MOCK_MAX_ENTITIES]; /* NULL = no photo */
   int entity_count;
   int call_count_entity_embeddings; /* loads of the entity cache */
   int entity_dim;

   /* Relations */
   memory_relation_t relations[MOCK_MAX_RELATIONS];
   int relation_user_id[MOCK_MAX_RELATIONS]; /* parallel — relation struct
                                                doesn't carry user_id */
   memory_provenance_t relation_provenance[MOCK_MAX_RELATIONS];
   int relation_count;

   /* Summaries */
   memory_summary_t summaries[MOCK_MAX_SUMMARIES];
   memory_provenance_t summary_provenance[MOCK_MAX_SUMMARIES];
   int summary_count;
   /* Semantic path: a summary with a score > 0 is returned by
    * memory_db_summary_search_semantic with that cosine; the pool the search
    * reports is summary_pool_scored rows summing to summary_pool_sum. */
   float summary_sem_score[MOCK_MAX_SUMMARIES];
   int summary_pool_scored;
   double summary_pool_sum;
   bool summary_keyword_off; /* keyword search finds nothing */

   /* Embedding engine */
   bool embeddings_available;

   /* Hybrid search behavior */
   bool hybrid_enabled;
   mock_hybrid_result_t hybrid_results[MOCK_MAX_HYBRID];
   int hybrid_result_count;
} mock_state_t;

extern mock_state_t s_mock;
void mock_reset(void);

#endif /* TEST_MEMORY_FOCUS_ADAPTERS_MOCKS_H */
