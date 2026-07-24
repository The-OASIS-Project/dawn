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
 * Serialized per-session turn queue (Layer 1).
 *
 * PROBLEM: two LLM turns can run `core_text_input_dispatch()` on ONE session_t
 * concurrently (a user turn + a background reinvoke turn, or two rapid user
 * messages) and stomp the shared per-turn streaming state — garbling the output
 * stream (TSan-confirmed data races in session_text_chunk_callback / llm_streaming).
 * `request_generation` is a post-hoc "who persists" arbiter, not a lock.
 *
 * FIX: this module is the mutual-exclusion point.  A turn worker for a session
 * is spawned ONLY when no turn is already in flight for that session; the rest
 * queue FIFO and are chained one at a time as each finishes.  The module owns
 * ONLY ordering + the per-session in-flight gate + one leaf mutex; it never
 * executes a turn (execution is a producer-supplied closure).  Sibling of
 * input_queue.c / pending_system_msg.c.
 *
 * SAFETY INVARIANT: a turn is never spawned onto a session that already has a
 * turn in flight — this is what guarantees any two truly-concurrent turns run on
 * DISTINCT session objects (distinct streaming state).  See
 * docs/... / ~/.claude/plans/per-conversation-turn-queue.md.
 *
 * LOCKING: s_turn_queue_mutex is a per-module LEAF.  Never held across the
 * spawn/free closures, session locks, or any dispatch.
 */

#ifndef TURN_QUEUE_H
#define TURN_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Turn origin — governs the queue-bound / drop policy. */
typedef enum {
   TURN_SOURCE_USER = 0,      /**< interactive user input — NEVER silently dropped */
   TURN_SOURCE_BACKGROUND = 1 /**< reinvoke / child agent — may be bound/coalesced */
} turn_source_t;

/* Return codes.  Follows the project convention: 0 = success, 1 = generic
 * failure, specific conditions > 1 — so TURN_QUEUE_FULL (a specific, and
 * caller-actionable, capacity condition) must not sit on the generic-failure
 * value.  All callers compare symbolically. */
#define TURN_QUEUE_OK 0
#define TURN_QUEUE_FAIL 1
#define TURN_QUEUE_FULL 2 /**< per-source bound hit; caller still owns @p work */

/**
 * @brief Enqueue a turn for @p session_id, spawning it now iff no turn is in
 *        flight for that session, else queuing it FIFO.
 *
 * @param session_id  The session the turn runs on (its serialization key).
 * @param source      User vs background (bound policy).
 * @param work        Opaque work item owned by the queue on success; passed to
 *                    @p spawn (to run) or @p free_work (on purge/drop).
 * @param spawn       Producer closure that starts the turn worker for @p work
 *                    (e.g. pthread_create of the turn thread).  Called with NO
 *                    queue lock held.  The worker MUST call turn_queue_turn_done()
 *                    when it finishes so the next queued turn is chained.
 * @param free_work   Producer closure that frees @p work WITHOUT running it (used
 *                    on bound-reject and on purge).  Called with no lock held.
 * @return TURN_QUEUE_OK (queue took ownership of @p work), TURN_QUEUE_FULL
 *         (bound hit — caller still owns @p work), or TURN_QUEUE_FAIL.
 */
int turn_queue_enqueue(uint32_t session_id,
                       turn_source_t source,
                       void *work,
                       void (*spawn)(void *work),
                       void (*free_work)(void *work));

/**
 * @brief Called by a turn worker when it finishes: chains the next queued turn
 *        for @p session_id (spawns it) or clears the in-flight gate if empty.
 *
 * @param session_id Session whose in-flight turn just completed.
 */
void turn_queue_turn_done(uint32_t session_id);

/**
 * @brief Mark @p session_id as closing and drop every QUEUED (not in-flight)
 *        turn, freeing each via its free_work closure.  Call EARLY in session
 *        teardown (before the ref-count wait) so no queued turn dereferences the
 *        session later and no NEW turn can be enqueued onto it.  Once closing:
 *        `turn_queue_enqueue` refuses (returns TURN_QUEUE_FAIL) and the in-flight
 *        turn's `turn_queue_turn_done` does NOT chain a successor — it retires
 *        the slot.  The in-flight turn (if any) is left to finish/abort on its
 *        own (teardown's cancel flag aborts it promptly).
 *
 * @param session_id Session being torn down.
 */
void turn_queue_purge_session(uint32_t session_id);

#ifdef __cplusplus
}
#endif

#endif /* TURN_QUEUE_H */
