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
 * Semantic ranking over every document chunk a user can access, from an
 * in-memory copy of the chunk embeddings.
 */

#ifndef DOCUMENT_EMBED_CACHE_H
#define DOCUMENT_EMBED_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most entries document_embed_rank() returns. */
#define DOCUMENT_RANK_MAX 256

/** One ranked chunk. */
typedef struct {
   int64_t chunk_id;
   float cosine;
} document_chunk_score_t;

/** Over the whole accessible corpus, for corpus-relative relevance. */
typedef struct {
   int pool;          /**< chunks scored */
   double cosine_sum; /**< sum of their cosines */
} document_rank_stats_t;

/**
 * @brief Rank every chunk @p user_id can access (own + shared) by cosine
 *
 * Scores against an in-memory copy of the chunk embeddings, rebuilt when the
 * user's chunk generation moves (document_db_chunk_generation: their own
 * documents' and the shared documents'), so the database lock is held only for
 * the one-row generation check, not for the scan.  A corpus too large to copy is
 * scored straight from the database instead.
 *
 * @param user_id  User
 * @param query    Query embedding
 * @param dims     Its dimension; chunks of another size are skipped
 * @param keep     Top entries to return (<= DOCUMENT_RANK_MAX)
 * @param top      [out] At least @p keep entries, best first
 * @param n_out    [out] Entries written
 * @param stats    [out] Pool statistics (may be NULL)
 * @return 0 (SUCCESS) or 1 (FAILURE)
 */
int document_embed_rank(int user_id,
                        const float *query,
                        int dims,
                        int keep,
                        document_chunk_score_t *top,
                        int *n_out,
                        document_rank_stats_t *stats);

/** Free the cache (daemon shutdown). */
void document_embed_cache_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_EMBED_CACHE_H */
