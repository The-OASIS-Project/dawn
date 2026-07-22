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
 * @file conv_stream.h
 * @brief Conversation-keyed in-memory replay ring for live turn output.
 *
 * @section rationale Design Rationale
 * Part of the durable-background-jobs / background-conversation work
 * (docs/BACKGROUND_JOBS_DESIGN.md). Streamed LLM tokens are ephemeral — they are
 * never persisted (only *steps* land in `conversation_events`, and the final
 * assistant text lands in `messages`). But a client that attaches to a
 * conversation while a turn is mid-stream needs the **partial text so far**
 * before it can live-tail. This module holds exactly that: per conversation, the
 * accumulating text of the current in-flight turn.
 *
 * Flow: the streaming emit path calls conv_stream_begin() at turn start,
 * conv_stream_append() per token delta, and conv_stream_end() at turn end. A
 * client attaching calls conv_stream_dup_partial() to replay what has streamed
 * so far, then subscribes to live deltas. Finalized partials are kept for a
 * short grace window (late attach) then evicted.
 *
 * Caller contract: pair every begin() with an end() (or a clear()) on EVERY
 * exit path — including interrupt/abort/error — so a slot doesn't stay "active"
 * forever. As a self-healing backstop, evict_stale() also reclaims an active
 * turn left stale beyond CONV_STREAM_ACTIVE_MAX_AGE_SEC. stream_id is assumed
 * monotonic per conversation (it mirrors session->current_stream_id): a newer
 * stream_id supersedes the current partial even if the prior turn is still
 * marked active, and an older stream_id is ignored.
 *
 * This is deliberately keyed by **conversation_id**, not session — the whole
 * point is that live output outlives/spans the websocket viewing it, so it is a
 * passive store that higher layers PULL from; it never calls into the WS send
 * funnel itself.
 *
 * @section bounds Bounds
 * Fixed slot table (CONV_STREAM_MAX_SLOTS); when full, the least-recently-updated
 * slot is reclaimed. Each slot's partial buffer grows on demand up to
 * CONV_STREAM_MAX_PARTIAL_BYTES, past which the partial is marked truncated and
 * stops growing (the full final text still arrives via the durable message).
 *
 * @section threading Thread Safety
 * All functions are thread-safe via a single internal leaf mutex (no other lock
 * is held across these calls; no I/O or send-funnel call is made while held).
 */

#ifndef CONV_STREAM_H
#define CONV_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/** Max distinct conversations tracked at once (LRU-reclaimed when exceeded). */
#define CONV_STREAM_MAX_SLOTS 32

/** Per-turn partial cap; beyond this the partial is marked truncated. */
#define CONV_STREAM_MAX_PARTIAL_BYTES (64 * 1024)

/** Finalized partials are kept this long for a late attach, then evictable. */
#define CONV_STREAM_EVICT_GRACE_SEC 30

/** Backstop: an *active* turn this stale is treated as abandoned (missed end)
 *  and evicted by conv_stream_evict_stale() so a stuck turn can't pin a slot. */
#define CONV_STREAM_ACTIVE_MAX_AGE_SEC 900

/**
 * @brief Begin a new in-flight turn for a conversation (resets its partial).
 *
 * @param conversation_id Conversation key (<= 0 is a no-op).
 * @param stream_id       Monotonic turn id (mirrors session->current_stream_id);
 *                        used to reject stale appends from a superseded turn.
 */
void conv_stream_begin(int64_t conversation_id, uint32_t stream_id);

/**
 * @brief Append streamed token text to a conversation's current partial.
 *
 * A mismatched @p stream_id (older/superseded turn) is ignored. If no turn was
 * begun for this conversation, the append starts one implicitly.
 *
 * @param conversation_id Conversation key (<= 0 is a no-op).
 * @param stream_id       Turn id; must match the active turn.
 * @param text            Token text (NULL/empty is a no-op).
 */
void conv_stream_append(int64_t conversation_id, uint32_t stream_id, const char *text);

/**
 * @brief Mark a conversation's in-flight turn finalized (keeps the partial for
 *        the grace window so a late attach can still replay it).
 *
 * @param conversation_id Conversation key.
 * @param stream_id       Turn id; a mismatch is ignored.
 */
void conv_stream_end(int64_t conversation_id, uint32_t stream_id);

/**
 * @brief Duplicate the current partial text for a conversation (attach replay).
 *
 * @param conversation_id Conversation key.
 * @param active_out      If non-NULL, set to whether the turn is still in flight.
 * @param stream_id_out   If non-NULL, set to the partial's turn id.
 * @param truncated_out   If non-NULL, set to whether the partial hit the cap.
 * @return Newly-allocated NUL-terminated copy (caller frees), or NULL if this
 *         conversation has no current/recent partial.
 */
char *conv_stream_dup_partial(int64_t conversation_id,
                              bool *active_out,
                              uint32_t *stream_id_out,
                              bool *truncated_out);

/** @brief Drop a conversation's partial (e.g. on conversation delete). */
void conv_stream_clear(int64_t conversation_id);

/** @brief Evict finalized partials older than the grace window (heartbeat). */
void conv_stream_evict_stale(time_t now);

/** @brief Free every slot's buffer and reset the table (test/shutdown). */
void conv_stream_reset(void);

#endif /* CONV_STREAM_H */
