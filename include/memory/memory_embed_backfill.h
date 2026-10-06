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
 * Embedding backfill: a single background worker that embeds facts stored
 * without an embedding, plus its scheduling hooks.
 */

#ifndef MEMORY_EMBED_BACKFILL_H
#define MEMORY_EMBED_BACKFILL_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Queue a background pass that embeds a user's un-embedded facts
 *
 * Non-blocking (no DB work on the caller's thread).  Requests are deduplicated
 * per user and served in order by a single worker thread, so a request made
 * while another user's pass is running is not dropped.  Call it after creating
 * facts in bulk without embeddings (e.g. a memory import) so they reach semantic
 * search promptly.  While a model-change re-index is running the request is
 * parked and served by the sweep that follows it; if the queue is full it is
 * served by a sweep the worker runs once the queue drains.
 *
 * @param user_id User whose facts to backfill (ignored if <= 0)
 */
void memory_embeddings_request_backfill(int user_id);

/**
 * @brief Queue a backfill pass for every user with un-embedded facts
 *
 * Also queues users whose one-shot category pass hasn't run.  Runs one query on
 * the caller's thread (all users' facts), so call it from startup or a
 * background thread, never the lws service thread.  This is the safety net for
 * any fact that missed its embedding (engine unavailable at creation time, or a
 * creation path that doesn't embed).
 */
void memory_embeddings_request_backfill_all(void);

/**
 * @brief Heartbeat: run a delayed backfill sweep after the embedding engine failed
 *
 * Call once per second from the main loop.  Cheap when nothing is scheduled
 * (one atomic load).  The sweep itself runs on the backfill worker.
 *
 * @param now Current time
 */
void memory_embeddings_tick(time_t now);

/**
 * @brief Mark a model-change re-index as running (called by memory_embed_recompute)
 *
 * While active, backfill requests are parked instead of re-embedding the rows the
 * re-index is rewriting.
 */
void memory_embeddings_reindex_begin(void);

/**
 * @brief Mark the re-index finished
 *
 * @param completed true if it ran to completion; a parked backfill request is
 *                  then served by one all-users sweep.  false (interrupted)
 *                  drops it; the next boot's sweep covers the same facts.
 */
void memory_embeddings_reindex_end(bool completed);

/** Stop and join the backfill worker; called from memory_embeddings_cleanup(). */
void memory_embed_backfill_shutdown(void);

/**
 * @brief Arm a delayed all-users backfill sweep after the embedding engine failed
 *
 * Keeps the earliest pending time; memory_embeddings_tick() fires it.  The delay
 * doubles each time it is armed, up to an hour, and resets once a sweep embeds
 * something.  Cheap (atomics only), safe from any thread.
 */
void memory_embed_backfill_schedule_retry(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EMBED_BACKFILL_H */
