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
 * One turn per conversation at a time.  Several sessions can show the same
 * conversation; each turn on it takes the conversation's lease first, so at
 * most one turn writes its rows at a time and rows of two turns never
 * interleave.  Waiters queue FIFO and a release hands the lease straight to
 * the head waiter.
 *
 * Writers outside a turn (an unprompted post) never wait: a post into a free
 * conversation is written at once under the lease, and a post into a held one
 * is queued on the conversation and written by the holder's release, in
 * order, before the lease passes on.  The lease writes posts through a writer
 * registered from above (conv_turn_lease_set_writer).
 *
 * Lock order: a turn takes its lease with no other lock held, and waits for at
 * most one conversation.  The internal mutex is a leaf: it is never held
 * across the writer, a should_stop callback or a log line.
 */

#ifndef CONV_TURN_LEASE_H
#define CONV_TURN_LEASE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Results.  GRANTED is 0 (SUCCESS); every other result is positive. */
#define CONV_TURN_LEASE_GRANTED 0
#define CONV_TURN_LEASE_FAILURE 1  /* bad argument, release by a non-holder, no writer */
#define CONV_TURN_LEASE_STOPPED 2  /* should_stop said so, or the lease is shut down */
#define CONV_TURN_LEASE_FULL 3     /* no free table entry: nothing was taken */
#define CONV_TURN_LEASE_BUSY 4     /* held: try_acquire, or the token holds another conversation */
#define CONV_TURN_LEASE_WRITTEN 5  /* post_or_defer: the item was written now */
#define CONV_TURN_LEASE_DEFERRED 6 /* post_or_defer: queued for the holder's release */
#define CONV_TURN_LEASE_WRITE_FAILED 7 /* post_or_defer: written now, and the write failed */

/* How often a waiting turn re-checks its should_stop callback. */
#define CONV_TURN_LEASE_WAIT_SLICE_MS 1000

/*
 * Table entries: one per conversation that is held or waited on.  A session
 * holds or waits for at most one conversation at a time, so the table needs
 * one entry per session that can run a turn: the interactive sessions
 * (MAX_SESSIONS, 8), the background-job pool (JOB_POOL_MAX_SLOTS, 256) and the
 * messaging channels (MESSAGING_MAX_SESSIONS, 64).  Those live in higher
 * layers; the lease's unit test asserts this sum against them.
 */
#define CONV_TURN_LEASE_SLOTS (8 + 256 + 64)

/* The session id a post's internal hold is recorded under (no session runs it). */
#define CONV_TURN_LEASE_POST_SESSION UINT32_MAX

/**
 * A post waiting for its conversation's lease.  Embedded at the start (or
 * anywhere) of the caller's item; the link belongs to the lease while queued.
 */
typedef struct conv_lease_item {
   struct conv_lease_item *next;
} conv_lease_item_t;

/** Whether a waiting turn should give up; called with no lease lock held. */
typedef bool (*conv_lease_stop_fn)(void *ctx);

/** Writes a post's row and consumes the item, whatever happens; true when the
 *  row was written.  Called with no lease lock held, while the lease of the
 *  item's conversation is held for the write. */
typedef bool (*conv_lease_write_fn)(conv_lease_item_t *item);

/** Disposes of an item that will not be written. */
typedef void (*conv_lease_drop_fn)(conv_lease_item_t *item);

/**
 * @brief A new token, unique for the life of the process (never 0)
 *
 * Every holder identifies itself by a token from here, so a turn's token never
 * collides with the one a post holds the lease under.
 */
uint64_t conv_turn_lease_new_token(void);

/**
 * @brief Take @p conv's lease for a turn, waiting FIFO behind earlier waiters
 *
 * Taking it again with the token that holds it counts: release it as many
 * times as it was taken.  A token that holds another conversation never
 * waits: it gets the lease only if it is free (an ERROR is logged either way,
 * since a turn holds one conversation).
 *
 * @param conv        Conversation id (> 0)
 * @param session     The turn's session id, for diagnostics and the reaper
 * @param token       From conv_turn_lease_new_token()
 * @param should_stop Checked every CONV_TURN_LEASE_WAIT_SLICE_MS while waiting,
 *                    and at once after conv_turn_lease_wake_session() (may be
 *                    NULL); it wins only if the lease hasn't come
 * @param ctx         Passed to @p should_stop
 * @return CONV_TURN_LEASE_GRANTED (held: release it), _STOPPED, _FULL, _BUSY
 *         or _FAILURE (bad argument)
 */
int conv_turn_lease_acquire(int64_t conv,
                            uint32_t session,
                            uint64_t token,
                            conv_lease_stop_fn should_stop,
                            void *ctx);

/**
 * @brief Take @p conv's lease if it is free; never waits
 *
 * For a caller that does other work while a conversation is held (and tries
 * again later) rather than wait on its thread.  A token that holds the lease
 * already gets _BUSY: this never re-enters.
 *
 * @return CONV_TURN_LEASE_GRANTED (held: release it), _BUSY (held by another
 *         holder, or being released), _FULL (ERROR logged), _STOPPED (shut
 *         down) or _FAILURE (bad argument)
 */
int conv_turn_lease_try_acquire(int64_t conv, uint32_t session, uint64_t token);

/**
 * @brief Take a just-created conversation's lease; never waits
 *
 * Nothing else can know a new id yet, so this is a try.  Safe on the network
 * thread.
 *
 * @return true when taken; false if it is held or waited on, the table is
 *         full (ERROR logged), the lease is shut down, or an argument is bad
 */
bool conv_turn_lease_claim_new(int64_t conv, uint32_t session, uint64_t token);

/**
 * @brief Release @p conv's lease held under @p token
 *
 * On the last release of a re-entered lease, every post queued on the
 * conversation is written first (each through the writer, with the lease
 * still held by this caller, including posts queued while the drain runs),
 * then the lease passes to the head waiter or the entry is freed.  While
 * that runs, no other release of the conversation is accepted.
 *
 * @return SUCCESS, or CONV_TURN_LEASE_FAILURE when @p token doesn't hold it
 *         (or is releasing it already)
 */
int conv_turn_lease_release(int64_t conv, uint64_t token);

/**
 * @brief Write a post into @p conv now, or queue it for the holder's release
 *
 * Never waits.  The lease takes the item in every result but _FAILURE.  If
 * the conversation is free, the item is written on this thread through the
 * writer while the lease is held for it, then the lease is released (writing
 * anything queued meanwhile).  If it is held, by anyone (the posting turn
 * included), the item is queued.  After conv_turn_lease_shutdown(), or when
 * the table is full, the item is written at once without the lease.
 *
 * @return CONV_TURN_LEASE_WRITTEN, _WRITE_FAILED (written now, the writer
 *         said it failed), _DEFERRED, or _FAILURE (bad argument or no writer
 *         registered; the item stays the caller's)
 */
int conv_turn_lease_post_or_defer(int64_t conv, conv_lease_item_t *item);

/**
 * @brief Register how posts are written and disposed of
 *
 * Set once at startup, before any post.  Passing a NULL @p write unregisters
 * the writer: queued items drained after that go to @p drop (or are lost with
 * an ERROR when it is NULL too), and new posts are refused.
 */
void conv_turn_lease_set_writer(conv_lease_write_fn write, conv_lease_drop_fn drop);

/**
 * @brief Log an ERROR once per wait when a turn has waited this long
 * @param seconds 0 (the default) never logs
 */
void conv_turn_lease_set_stale_after(int seconds);

/** @brief The stale-wait threshold set by conv_turn_lease_set_stale_after() (0 = none) */
int conv_turn_lease_stale_after(void);

/**
 * @brief Have every turn of @p session waiting for a lease check its
 *        should_stop now, rather than at the end of its wait slice
 *
 * For a Stop or a teardown aimed at a waiting turn: set what should_stop
 * reads first, then call this.  Safe under any lock (the lease's own mutex is
 * a leaf).
 */
void conv_turn_lease_wake_session(uint32_t session);

/**
 * @brief Who holds @p conv's lease
 * @param session Out (may be NULL): the holder's session id
 * @param token   Out (may be NULL): the holder's token
 * @param since   Out (may be NULL): when it was taken (wall clock)
 * @return true if it is held
 */
bool conv_turn_lease_holder(int64_t conv, uint32_t *session, uint64_t *token, time_t *since);

/** @brief How many turns wait for @p conv's lease */
int conv_turn_lease_waiters(int64_t conv);

/**
 * @brief Release every lease still held by @p session, as a safety net
 *
 * For a session being freed: by then no thread runs its turn.  Queued posts
 * are written and waiters served as in conv_turn_lease_release().  A lease
 * whose holder is releasing it right now is left to that release.
 *
 * @return How many conversations were released (the caller logs)
 */
int conv_turn_lease_release_session(uint32_t session);

/**
 * @brief Stop the lease for daemon shutdown
 *
 * Every waiting turn returns CONV_TURN_LEASE_STOPPED, then every queued post
 * is written (with a WARNING when its conversation's holder still runs:
 * losing a sent message's record is worse than an out-of-order row).  Later
 * acquires return _STOPPED, claim_new returns false, and later posts are
 * written at once.  Holders still release normally.
 */
void conv_turn_lease_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CONV_TURN_LEASE_H */
