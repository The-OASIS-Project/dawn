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
 * Withdrawing what a user forgot or deleted from their conversations' stored
 * request context.
 *
 * A turn's context (retrieved items, the USER MEMORY block) is saved with the
 * conversation, so a reload replays the request the model was sent.  When the
 * user forgets or deletes something, its copies there are withdrawn: an item's
 * line says it was withdrawn (found by its handle,
 * conversation_focus_handles), a USER MEMORY block keeps only its framing.  A
 * conversation whose stored context changed no longer replays what its turns
 * were sent, so its model's earlier reasoning is left behind: its reasoning
 * floor rises to its newest row (a declared boundary).  What the user removed
 * is known from the withdrawn_items rows the delete triggers leave while a
 * removal is marked (conv_db_withdraw_intent_begin), and a turn saved after a
 * forgetting it was built before is withdrawn as it is saved
 * (conv_db_save_turn).
 */

#ifndef AUTH_DB_WITHDRAW_H
#define AUTH_DB_WITHDRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One item line withdrawn: its conversation and handle. */
typedef struct {
   int64_t conv_id;
   int handle;
} conv_withdrawn_item_t;

/** What a withdrawal changed. */
typedef struct {
   conv_withdrawn_item_t *items; /**< the item lines withdrawn, by conversation */
   int n_items;
   int64_t *convs; /**< every conversation whose stored context changed */
   int n_convs;
   /** Every withdrawn item id a live session of the user checks its own
    *  handles against (its history may hold one no conversation stored yet),
    *  sorted (strcmp) */
   char **item_ids;
   int n_item_ids;
} conv_withdrawn_t;

/** Free a conv_withdrawn_t's arrays and zero it.  NULL-safe. */
void conv_withdrawn_free(conv_withdrawn_t *w);

/**
 * @brief Mark this thread's deletes as @p user_id removing what they delete
 *        (forgetting a memory, deleting a document): the delete triggers
 *        record each item for withdrawal only while marked.  Wrap the
 *        delete calls, then call session_withdraw_forgotten[_async].  Every
 *        other delete (decay, merges, re-indexing) records nothing.
 */
void conv_db_withdraw_intent_begin(int user_id);

/** End conv_db_withdraw_intent_begin(). */
void conv_db_withdraw_intent_end(void);

/**
 * @brief Withdraw every item @p user_id removed since the last withdrawal
 *        from every conversation it was injected into (the user's own, and
 *        other users' for a shared document), and with @p memory_bodies the
 *        user's stored USER MEMORY blocks
 *
 * One transaction.  A handle is withdrawn once (it is marked).  A calendar
 * occurrence is never withdrawn: the calendar's sync removes them, and what
 * the user saw stays true of when it was sent.  A conversation that changed
 * leaves its reasoning behind: every row now, and every row saved before a
 * turn built after this is (conversations.reasoning_floor_pending, the
 * withdrawal's place in the sequence, conv_db_withdraw_seq).
 *
 * @param out What changed (free with conv_withdrawn_free); may be NULL
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_withdraw(int user_id, bool memory_bodies, conv_withdrawn_t *out);

/**
 * @brief Withdraw, from conversation @p conv_id's stored context, the items
 *        removed since @p since (a minute's grace; 0: within the week), and
 *        its USER MEMORY blocks when the user's memory was withdrawn after
 *        @p since_seq (conv_db_withdraw_seq): for a history saved whole (a
 *        voice session), once its rows and handles are stored, both taken
 *        when the save began
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (not the user's) or AUTH_DB_FAILURE
 */
int conv_db_withdraw_conversation(int64_t conv_id, int user_id, int64_t since, int64_t since_seq);

/**
 * @brief Where withdrawals stand now: a number that rises with each one (and
 *        each removal recorded).  A turn records it when its prompt is built;
 *        a withdrawal later in the sequence is one it was built before.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_withdraw_seq(int64_t *seq_out);

/**
 * @brief Delete withdrawn-item rows older than they are needed (a week).
 *        Called from maintenance; takes the lock itself.
 * @param deleted_out Rows deleted (may be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_withdrawn_items_purge(int *deleted_out);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_WITHDRAW_H */
