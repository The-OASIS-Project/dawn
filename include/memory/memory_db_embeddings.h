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
 * Memory Database — fact-embedding sub-API.
 *
 * Storage and bulk-load primitives for per-fact embedding vectors.  Entity
 * embeddings live in memory_db_entities.h alongside the rest of the entity
 * surface (per-record-type cohesion: callers operate on facts-with-embeddings
 * or entities-with-embeddings, never both).
 *
 * Included transitively via memory_db.h — callers that already
 * `#include "memory/memory_db.h"` see these declarations unchanged.
 */

#ifndef MEMORY_DB_EMBEDDINGS_H
#define MEMORY_DB_EMBEDDINGS_H

#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Embedding Operations (Semantic Search)
 * ============================================================================= */

/**
 * @brief Store an embedding vector for a fact
 *
 * @param user_id User ID (ownership filter — fact must belong to this user)
 * @param fact_id Fact ID
 * @param embedding Float array of embedding values
 * @param dims Number of dimensions
 * @param norm Pre-computed L2 norm of the embedding
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_embedding(int user_id,
                                    int64_t fact_id,
                                    const float *embedding,
                                    int dims,
                                    float norm);

/* Which facts the in-memory cache keeps when a user has more than it holds:
 * first any used in the last MEMORY_CACHE_RECENT_USE_SEC (recalled or cited)
 * and memory→note links, then by confidence / (1 + age / scale), where age runs
 * from the fact's last use or creation, so a year-old untouched fact counts at
 * half its confidence. */
#define MEMORY_CACHE_RECENT_USE_SEC (90 * 24 * 60 * 60)
#define MEMORY_CACHE_RECENCY_SCALE_SEC (365 * 24 * 60 * 60)

/** One fact memory_db_fact_foreach_embedding() loads. */
typedef struct {
   int64_t id;
   const void *embedding; /* dims floats; valid only during the callback (memcpy it) */
   float norm;
   int64_t created_at;
   int64_t note_doc_id; /* >0: a memory→note bridge gloss */
} memory_fact_embedding_row_t;

/**
 * @brief Called per fact; @p total is how many facts matched before the limit.
 * @return SUCCESS to continue, anything else to stop (the load then fails)
 */
typedef int (*memory_fact_embedding_fn)(const memory_fact_embedding_row_t *row,
                                        int total,
                                        void *ctx);

/**
 * @brief Stream a user's current facts' embeddings for the cache
 *
 * Current (not superseded, not expired) facts whose embedding has
 * @p expected_dims dimensions.  All of them when there are at most @p limit;
 * otherwise the best @p limit as described at MEMORY_CACHE_RECENT_USE_SEC.
 * Rows come in no particular order.  @p fn runs under the auth_db lock, so it
 * must not call back into the database.
 *
 * @return MEMORY_DB_SUCCESS, or MEMORY_DB_FAILURE on a database error or when
 *         @p fn stopped the load
 */
int memory_db_fact_foreach_embedding(int user_id,
                                     int expected_dims,
                                     int limit,
                                     memory_fact_embedding_fn fn,
                                     void *ctx);

/**
 * @brief List facts that need embedding (backfill)
 *
 * Returns non-empty facts with NULL embedding or mismatched dimensions, in id order,
 * starting after @p after_id (pass 0 for the first page, then the last id seen),
 * so a caller can page past facts it could not embed instead of re-listing them.
 *
 * @param user_id User ID
 * @param after_id Only return facts with id > after_id
 * @param expected_dims Expected embedding dimensions
 * @param out_ids Output: array of fact IDs
 * @param out_texts Output: array of fact text strings (caller allocates char[][512])
 * @param max_count Maximum entries to return
 * @param count_out Output: number of facts found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_list_without_embedding(int user_id,
                                          int64_t after_id,
                                          int expected_dims,
                                          int64_t *out_ids,
                                          char out_texts[][512],
                                          int max_count,
                                          int *count_out);

/**
 * @brief List users that need an embedding-backfill pass
 *
 * A user qualifies if they have a live, non-empty fact with a NULL or
 * wrong-dimension embedding, or if their one-shot category pass has not run
 * (users.categories_backfilled_at = 0) and they have any live fact.  Paged by
 * user id so a sweep larger than the backfill queue can resume.
 *
 * @param expected_dims Expected embedding dimensions
 * @param after_user_id Only return users with id > after_user_id (0 = from start)
 * @param out_user_ids Output: user IDs (ascending)
 * @param max_count Capacity of out_user_ids
 * @param count_out Output: number of users found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_users_needing_backfill(int expected_dims,
                                          int after_user_id,
                                          int *out_user_ids,
                                          int max_count,
                                          int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_EMBEDDINGS_H */
