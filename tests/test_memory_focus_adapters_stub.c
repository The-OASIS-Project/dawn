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
 * Programmable stubs for test_memory_focus_adapters and
 * test_memory_fact_search.  Each stub serves a per-test fixture that
 * the test programs via the `s_mock_*` globals declared in
 * test_memory_focus_adapters_mocks.h (a header shared by both test
 * binaries).
 *
 * Stubs honor the public memory_db / memory_embeddings signatures so
 * the adapters compile and link against this test TU; the actual SQL
 * paths are tested in test_memory_provenance / test_relation_supersede.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/memory_filter.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"
#include "test_memory_focus_adapters_mocks.h"

/* g_config is touched by focus_source.c (lookup_source_weight, ranker
 * weights) — tests pre-populate before each compose call. */
dawn_config_t g_config;

/* Mock state — extern declarations live in the shared mocks header. */
mock_state_t s_mock;

void mock_reset(void) {
   memset(&s_mock, 0, sizeof(s_mock));
}

/* =============================================================================
 * memory_db_* fact stubs
 * ============================================================================= */

int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts,
                          int *count_out) {
   (void)keywords;
   int n = 0;
   for (int i = 0; i < s_mock.fact_count && n < max_facts; i++) {
      if (s_mock.facts[i].user_id != user_id)
         continue;
      out_facts[n++] = s_mock.facts[i];
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* v48: BM25-ranked search stub.  Mirrors the LIKE search but assigns a
 * uniform 1.0 score so tests gated on the bm25_enabled path get the
 * same fact set without depending on FTS5 + libstemmer in the unit
 * harness.  Tests that need score variation set them post-call. */
int memory_db_fact_search_bm25(int user_id,
                               const char *query,
                               memory_fact_t *out_facts,
                               float *out_scores,
                               int max_facts,
                               int *count_out) {
   (void)query;
   int n = 0;
   for (int i = 0; i < s_mock.fact_count && n < max_facts; i++) {
      if (s_mock.facts[i].user_id != user_id)
         continue;
      out_facts[n] = s_mock.facts[i];
      if (out_scores)
         out_scores[n] = 1.0f;
      n++;
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_search_bm25_since(int user_id,
                                     const char *query,
                                     time_t since_ts,
                                     memory_fact_t *out_facts,
                                     float *out_scores,
                                     int max_facts,
                                     int *count_out) {
   (void)query;
   int n = 0;
   for (int i = 0; i < s_mock.fact_count && n < max_facts; i++) {
      if (s_mock.facts[i].user_id != user_id)
         continue;
      if (since_ts > 0 && s_mock.facts[i].created_at < since_ts)
         continue;
      out_facts[n] = s_mock.facts[i];
      if (out_scores)
         out_scores[n] = 1.0f;
      n++;
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_search_since(int user_id,
                                const char *keywords,
                                time_t since_ts,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   (void)keywords;
   int n = 0;
   for (int i = 0; i < s_mock.fact_count && n < max_facts; i++) {
      if (s_mock.facts[i].user_id != user_id)
         continue;
      if (s_mock.facts[i].created_at < since_ts)
         continue;
      out_facts[n++] = s_mock.facts[i];
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_get(int64_t fact_id, int user_id, memory_fact_t *out_fact) {
   for (int i = 0; i < s_mock.fact_count; i++) {
      if (s_mock.facts[i].id == fact_id && s_mock.facts[i].user_id == user_id) {
         *out_fact = s_mock.facts[i];
         return MEMORY_DB_SUCCESS;
      }
   }
   return MEMORY_DB_NOT_FOUND;
}

/* v58 expiry guard stub: mock facts carry no expiry, so nothing is hidden. */
bool memory_db_fact_expiry_hidden(int64_t expires_at) {
   (void)expires_at;
   return false;
}

int memory_db_facts_get_sources(int user_id,
                                const int64_t *fact_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends) {
   (void)user_id;
   for (int i = 0; i < n; i++) {
      out_conv_ids[i] = 0;
      out_starts[i] = 0;
      out_ends[i] = 0;
      for (int j = 0; j < s_mock.fact_count; j++) {
         if (s_mock.facts[j].id == fact_ids[i]) {
            out_conv_ids[i] = s_mock.fact_provenance[j].conv_id;
            out_starts[i] = s_mock.fact_provenance[j].msg_id_start;
            out_ends[i] = s_mock.fact_provenance[j].msg_id_end;
            break;
         }
      }
   }
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * memory_db_* entity stubs
 * ============================================================================= */

int memory_db_entity_search(int user_id,
                            const char *keywords,
                            memory_entity_t *out,
                            int max,
                            int *count_out) {
   (void)keywords;
   int n = 0;
   for (int i = 0; i < s_mock.entity_count && n < max; i++) {
      if (s_mock.entities[i].user_id != user_id)
         continue;
      if (!s_mock.entity_keyword_match[i])
         continue;
      out[n++] = s_mock.entities[i];
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_get_embeddings(int user_id,
                                    bool include_aliases,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max,
                                    int *count_out) {
   /* Mock data has no canonical_id distinctions, so the include_aliases flag
    * is a no-op here.  Accept the parameter for signature parity with v43
    * production. */
   (void)include_aliases;
   s_mock.call_count_entity_embeddings++;
   if (expected_dims != s_mock.entity_dim) {
      if (count_out)
         *count_out = 0;
      return MEMORY_DB_SUCCESS; /* Empty result; production behavior */
   }
   int n = 0;
   for (int i = 0; i < s_mock.entity_count && n < max; i++) {
      if (s_mock.entities[i].user_id != user_id)
         continue;
      if (s_mock.entity_embeddings[i] == NULL)
         continue;
      out_ids[n] = s_mock.entities[i].id;
      strncpy(out_names[n], s_mock.entities[i].canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
      out_names[n][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      strncpy(out_types[n], s_mock.entities[i].entity_type, MEMORY_ENTITY_TYPE_MAX - 1);
      out_types[n][MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      memcpy(&out_embeddings[n * expected_dims], s_mock.entity_embeddings[i],
             expected_dims * sizeof(float));
      out_norms[n] = s_mock.entity_norms[i];
      n++;
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_embedding_count(int user_id, int expected_dims, int *count_out) {
   if (!count_out)
      return MEMORY_DB_FAILURE;
   int n = 0;
   for (int i = 0; expected_dims == s_mock.entity_dim && i < s_mock.entity_count; i++) {
      if (s_mock.entities[i].user_id == user_id && s_mock.entity_embeddings[i] != NULL)
         n++;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entities_get_by_ids(int user_id,
                                  const int64_t *ids,
                                  int n,
                                  memory_entity_t *out,
                                  int *count_out) {
   if (!ids || !out || !count_out || n <= 0)
      return MEMORY_DB_FAILURE;
   int count = 0;
   for (int i = 0; i < s_mock.entity_count && count < n; i++) {
      if (s_mock.entities[i].user_id != user_id)
         continue;
      for (int k = 0; k < n; k++) {
         if (ids[k] == s_mock.entities[i].id) {
            out[count++] = s_mock.entities[i];
            break;
         }
      }
   }
   *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_get_by_name(int user_id,
                                 const char *canonical_name,
                                 memory_entity_t *out_entity) {
   for (int i = 0; i < s_mock.entity_count; i++) {
      if (s_mock.entities[i].user_id != user_id)
         continue;
      if (strcmp(s_mock.entities[i].canonical_name, canonical_name) == 0) {
         *out_entity = s_mock.entities[i];
         return MEMORY_DB_SUCCESS;
      }
   }
   return MEMORY_DB_NOT_FOUND;
}

int memory_db_entity_get_photo(int user_id,
                               int64_t entity_id,
                               char *out_photo_id,
                               size_t photo_id_size) {
   for (int i = 0; i < s_mock.entity_count; i++) {
      if (s_mock.entities[i].user_id != user_id)
         continue;
      if (s_mock.entities[i].id == entity_id) {
         if (s_mock.entity_photo_ids[i] != NULL) {
            strncpy(out_photo_id, s_mock.entity_photo_ids[i], photo_id_size - 1);
            out_photo_id[photo_id_size - 1] = '\0';
         } else if (photo_id_size > 0) {
            out_photo_id[0] = '\0';
         }
         return MEMORY_DB_SUCCESS;
      }
   }
   return MEMORY_DB_NOT_FOUND;
}

/* =============================================================================
 * memory_db_* relation stubs
 * ============================================================================= */

int memory_db_relation_list_by_subject_at(int user_id,
                                          int64_t subject_entity_id,
                                          int64_t as_of_ts,
                                          memory_relation_t *out,
                                          int max,
                                          int *count_out) {
   int n = 0;
   for (int i = 0; i < s_mock.relation_count && n < max; i++) {
      if (s_mock.relation_user_id[i] != user_id)
         continue;
      if (s_mock.relations[i].subject_entity_id != subject_entity_id)
         continue;
      /* Bitemporal filter mimicking the production semantics:
       * (valid_from == 0 || valid_from <= as_of) AND
       * (valid_to   == 0 || valid_to   >  as_of). */
      if (s_mock.relations[i].valid_from > 0 && s_mock.relations[i].valid_from > as_of_ts)
         continue;
      if (s_mock.relations[i].valid_to > 0 && s_mock.relations[i].valid_to <= as_of_ts)
         continue;
      out[n++] = s_mock.relations[i];
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relations_get_sources(int user_id,
                                    const int64_t *relation_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends) {
   (void)user_id;
   for (int i = 0; i < n; i++) {
      out_conv_ids[i] = 0;
      out_starts[i] = 0;
      out_ends[i] = 0;
      for (int j = 0; j < s_mock.relation_count; j++) {
         if (s_mock.relations[j].id == relation_ids[i]) {
            out_conv_ids[i] = s_mock.relation_provenance[j].conv_id;
            out_starts[i] = s_mock.relation_provenance[j].msg_id_start;
            out_ends[i] = s_mock.relation_provenance[j].msg_id_end;
            break;
         }
      }
   }
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * memory_db_* summary stubs
 * ============================================================================= */

int memory_db_summary_search_since(int user_id,
                                   const char *keywords,
                                   time_t since_ts,
                                   memory_summary_t *out_summaries,
                                   int max_summaries,
                                   int *count_out) {
   (void)keywords;
   int n = 0;
   for (int i = 0; !s_mock.summary_keyword_off && i < s_mock.summary_count && n < max_summaries;
        i++) {
      if (s_mock.summaries[i].user_id != user_id)
         continue;
      if (s_mock.summaries[i].created_at < since_ts)
         continue;
      out_summaries[n++] = s_mock.summaries[i];
   }
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summaries_get_sources(int user_id,
                                    const int64_t *summary_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends) {
   (void)user_id;
   for (int i = 0; i < n; i++) {
      out_conv_ids[i] = 0;
      out_starts[i] = 0;
      out_ends[i] = 0;
      for (int j = 0; j < s_mock.summary_count; j++) {
         if (s_mock.summaries[j].id == summary_ids[i]) {
            out_conv_ids[i] = s_mock.summary_provenance[j].conv_id;
            out_starts[i] = s_mock.summary_provenance[j].msg_id_start;
            out_ends[i] = s_mock.summary_provenance[j].msg_id_end;
            break;
         }
      }
   }
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * memory_embeddings_* stubs
 * ============================================================================= */

bool memory_embeddings_available(void) {
   return s_mock.embeddings_available;
}

int memory_embeddings_dims(void) {
   return s_mock.embeddings_available ? s_mock.entity_dim : 0;
}

float memory_embeddings_l2_norm(const float *vec, int dims) {
   double sum = 0.0;
   for (int i = 0; i < dims; i++)
      sum += (double)vec[i] * (double)vec[i];
   return (float)sqrt(sum);
}

float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b) {
   if (norm_a <= 0.0f || norm_b <= 0.0f)
      return 0.0f;
   double dot = 0.0;
   for (int i = 0; i < dims; i++)
      dot += (double)a[i] * (double)b[i];
   const double cosine = dot / ((double)norm_a * (double)norm_b);
   if (cosine < 0.0)
      return 0.0f;
   if (cosine > 1.0)
      return 1.0f;
   return (float)cosine;
}

int memory_embeddings_hybrid_search(int user_id,
                                    const char *query,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results) {
   (void)query;
   (void)token_count;
   if (!s_mock.hybrid_enabled) {
      /* Pass-through: return keyword set as-is. */
      int n = (keyword_count > max_results) ? max_results : keyword_count;
      for (int i = 0; i < n; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (float)keyword_scores[i];
      }
      return n;
   }
   /* Hybrid path: emit configured re-ranked output, optionally adding
    * a vector-only hit so the user_id post-check path exercises. */
   int n = 0;
   for (int i = 0; i < s_mock.hybrid_result_count && n < max_results; i++) {
      if (s_mock.hybrid_results[i].user_id_filter != 0 &&
          s_mock.hybrid_results[i].user_id_filter != user_id)
         continue;
      out_results[n].fact_id = s_mock.hybrid_results[i].fact_id;
      out_results[n].score = s_mock.hybrid_results[i].score;
      n++;
   }
   return n;
}

/* =============================================================================
 * memory_filter — production blocklist NOT linked.  Tests never inject
 * blocklist payloads through the adapter path; the framework's filter-on-
 * retrieval is exercised in test_focus_source.c (which links the real
 * filter).  Stub returns false so candidates pass through.
 * ============================================================================= */
bool memory_filter_check(const char *text) {
   (void)text;
   return false;
}

/* =============================================================================
 * Phase 1A graph retrieval + RRF + Phase 2 Step 1 stubs (May 2026 ships).
 *
 * Adapter tests don't exercise these paths — they exercise the upstream
 * adapter dispatch + focus composition.  No-op stubs return SUCCESS / 0
 * with `count_out = 0` so the linker is satisfied and any path that
 * accidentally hits these gets a quiet empty result rather than a crash.
 * ============================================================================= */

int memory_embeddings_rrf_search(int user_id,
                                 const char *query,
                                 const int64_t *keyword_facts,
                                 const int *keyword_scores,
                                 int keyword_count,
                                 int token_count,
                                 embedding_search_result_t *out_results,
                                 int max_results) {
   (void)user_id;
   (void)query;
   (void)keyword_facts;
   (void)keyword_scores;
   (void)keyword_count;
   (void)token_count;
   (void)out_results;
   (void)max_results;
   return 0;
}

int memory_graph_extract_seed_entities(int user_id,
                                       const char *query,
                                       int64_t *out_entity_ids,
                                       int max,
                                       int *out_count) {
   (void)user_id;
   (void)query;
   (void)out_entity_ids;
   (void)max;
   if (out_count != NULL)
      *out_count = 0;
   return SUCCESS;
}

int memory_graph_expand_fact_linked(int user_id,
                                    const int64_t *seed_entity_ids,
                                    int seed_count,
                                    memory_fact_t *out_facts,
                                    float *out_scores,
                                    int max,
                                    int *out_count) {
   (void)user_id;
   (void)seed_entity_ids;
   (void)seed_count;
   (void)out_facts;
   (void)out_scores;
   (void)max;
   if (out_count != NULL)
      *out_count = 0;
   return SUCCESS;
}

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   (void)text;
   (void)out;
   if (out_dims != NULL)
      *out_dims = 0;
   return FAILURE;
}

/* The real entity embedding cache (memory_embeddings_entity.c) is linked in
 * and reads the engine's dimension from here: the mock entity pool's. */
int embedding_engine_dims(void) {
   return s_mock.embeddings_available ? s_mock.entity_dim : 0;
}

bool embedding_engine_available(void) {
   return s_mock.embeddings_available;
}

int embedding_engine_embed(const char *text, float *out, int max_dims, int *out_dims) {
   (void)text;
   (void)out;
   (void)max_dims;
   if (out_dims != NULL)
      *out_dims = 0;
   return FAILURE;
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
   return MEMORY_DB_FAILURE;
}

int memory_embeddings_rescore_against_query(int user_id,
                                            const float *query_emb,
                                            float query_norm,
                                            float entity_bonus,
                                            const int64_t *fact_ids,
                                            int fact_count,
                                            float *out_scores) {
   (void)user_id;
   (void)query_emb;
   (void)query_norm;
   (void)entity_bonus;
   (void)fact_ids;
   if (out_scores != NULL) {
      for (int i = 0; i < fact_count; i++)
         out_scores[i] = MEMORY_EMBEDDINGS_RESCORE_SENTINEL;
   }
   return SUCCESS;
}

/* No fact-embedding cache in this harness: the injection relevance gate is
 * skipped (it only applies with a real baseline). */
int memory_embeddings_fact_relevance(int user_id,
                                     const float *query_emb,
                                     const int64_t *ids,
                                     int n,
                                     float *out_rel,
                                     int *pool_out) {
   (void)user_id;
   (void)query_emb;
   (void)ids;
   (void)n;
   (void)out_rel;
   if (pool_out)
      *pool_out = 0;
   return FAILURE;
}

int memory_search_apply_score_floor(memory_fact_t *facts,
                                    float *scores,
                                    int count,
                                    float score_floor) {
   (void)facts;
   (void)scores;
   (void)score_floor;
   return count;
}

int memory_db_summary_search_semantic(int user_id,
                                      const float *query_vec,
                                      int query_dims,
                                      time_t since_ts,
                                      int max_summaries,
                                      int max_scan,
                                      memory_summary_t *out_summaries,
                                      float *out_scores,
                                      int *count_out,
                                      memory_summary_pool_t *pool_out) {
   (void)query_vec;
   (void)query_dims;
   (void)max_scan;
   int n = 0;
   for (int i = 0; i < s_mock.summary_count && n < max_summaries; i++) {
      if (s_mock.summaries[i].user_id != user_id || s_mock.summaries[i].created_at < since_ts ||
          s_mock.summary_sem_score[i] <= 0.0f)
         continue;
      out_summaries[n] = s_mock.summaries[i];
      out_scores[n] = s_mock.summary_sem_score[i];
      n++;
   }
   if (count_out != NULL)
      *count_out = n;
   if (pool_out != NULL) {
      pool_out->scored = s_mock.summary_pool_scored;
      pool_out->cosine_sum = s_mock.summary_pool_sum;
   }
   return MEMORY_DB_SUCCESS;
}

/* Extended variants: same contract as the non-_ex helpers above but with a
 * caller-supplied (query_emb, query_norm) pair the stub ignores.  Delegating
 * to the non-_ex stubs keeps mock semantics consistent. */
int memory_embeddings_hybrid_search_ex(int user_id,
                                       const char *query,
                                       const float *query_emb,
                                       float query_norm,
                                       const int64_t *keyword_facts,
                                       const int *keyword_scores,
                                       int keyword_count,
                                       int token_count,
                                       embedding_search_result_t *out_results,
                                       int max_results) {
   (void)query_emb;
   (void)query_norm;
   return memory_embeddings_hybrid_search(user_id, query, keyword_facts, keyword_scores,
                                          keyword_count, token_count, out_results, max_results);
}

int memory_embeddings_rrf_search_ex(int user_id,
                                    const char *query,
                                    const float *query_emb,
                                    float query_norm,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results) {
   (void)query_emb;
   (void)query_norm;
   return memory_embeddings_rrf_search(user_id, query, keyword_facts, keyword_scores, keyword_count,
                                       token_count, out_results, max_results);
}

/* Stemmer stub: words pass through unstemmed.  memory_terms (entity name
 * matching) stems both sides with this, so names still match consistently;
 * real stemming is covered by the document ranking tests. */
int memory_stem_string(const char *input, char *out, size_t out_sz) {
   if (!out || out_sz == 0)
      return 0;
   out[0] = '\0';
   if (!input)
      return 0;
   snprintf(out, out_sz, "%s", input);
   int n = 0;
   for (const char *p = out; *p;) {
      while (*p == ' ')
         p++;
      if (*p)
         n++;
      while (*p && *p != ' ')
         p++;
   }
   return n;
}

/* Tokenizer stub — split on whitespace, lowercase, drop tokens shorter than
 * min_len.  No stemming.  Enough for test_memory_fact_search's multi-token
 * scoring path, where the test expects "alpha bravo charlie" to decompose
 * into 3 distinct tokens so the dedup-and-score logic can be exercised. */
int memory_stem_tokenize_padded(const char *keywords,
                                char tokens[][64],
                                int max_tokens,
                                int min_len) {
   if (!keywords || !tokens || max_tokens <= 0)
      return 0;
   int out = 0;
   const char *p = keywords;
   while (*p && out < max_tokens) {
      while (*p == ' ' || *p == '\t' || *p == '\n')
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n')
         p++;
      size_t len = (size_t)(p - start);
      if ((int)len < min_len)
         continue;
      if (len > 63)
         len = 63;
      for (size_t i = 0; i < len; i++) {
         char c = start[i];
         if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
         tokens[out][i] = c;
      }
      tokens[out][len] = '\0';
      out++;
   }
   return out;
}
