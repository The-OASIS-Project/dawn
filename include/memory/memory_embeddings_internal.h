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
 * Fact-embedding internals shared between memory_embeddings.c and the
 * embedding backfill (memory_embed_backfill.c).  Not for use outside src/memory.
 */

#ifndef MEMORY_EMBEDDINGS_INTERNAL_H
#define MEMORY_EMBEDDINGS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Embed @p text and store it on the fact
 *
 * Rejects an empty vector or one whose dimension differs from the engine's.
 *
 * @param user_id       Owner of the fact
 * @param fact_id       Fact to update
 * @param text          Fact text to embed
 * @param from_backfill The backfill worker's call: it refreshes the cached fact
 *                      embeddings periodically and decides itself whether a
 *                      failure warrants a retry sweep.  Otherwise the cache is
 *                      marked stale on success and a failure arms a retry.
 * @return 0 on success, non-zero on failure
 */
int memory_embeddings_embed_and_store_ex(int user_id,
                                         int64_t fact_id,
                                         const char *text,
                                         bool from_backfill);

/**
 * @brief Mark the fact-embedding cache stale only if it holds @p user_id
 *
 * Takes the cache mutex, which is held across a full reload, so a caller may
 * briefly wait behind one.
 */
void memory_embeddings_invalidate_cache_for_user(int user_id);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EMBEDDINGS_INTERNAL_H */
