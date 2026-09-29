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
 * A conversation's frozen request prefix: the system prompt and tool set as
 * first sent, stored once per distinct value in prompt_blobs (keyed by their
 * SHA-256), so every later request and every reload replays them byte for byte.
 * The reasoning floor marks where a declared boundary left earlier reasoning
 * behind: reasoning on rows at or below it is never replayed.
 */

#ifndef AUTH_DB_CONV_PREFIX_H
#define AUTH_DB_CONV_PREFIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/hash_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest stored prefix or tool set, in bytes. */
#define CONV_PROMPT_BLOB_MAX 1048576

/** A conversation's frozen prefix, as stored. */
typedef struct {
   char *prefix;   /**< System prompt bytes (heap), NULL when none is bound */
   char *tools;    /**< Advertised tool set (heap), NULL when none is bound */
   char *in_force; /**< Which sections and directions are in force (heap JSON), or NULL */
   char prefix_hash[DAWN_SHA256_HEX_LEN];
   char tools_hash[DAWN_SHA256_HEX_LEN];
   char in_force_hash[DAWN_SHA256_HEX_LEN];
   int64_t reasoning_floor_msg_id;
} conv_prefix_t;

/** Free the strings a conv_prefix_t holds and zero it. NULL-safe. */
void conv_prefix_free(conv_prefix_t *p);

/**
 * Read a conversation's frozen prefix, tool set and reasoning floor. A stored
 * value that is missing or whose bytes no longer hash to its key is refused
 * (logged), never replayed: the caller declares a boundary and its next turn
 * replaces it (conv_db_save_turn).
 *
 * @param out Filled on success; free with conv_prefix_free(). Fields are NULL /
 *            empty when nothing is bound yet.
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (no such conversation for the
 *         user), AUTH_DB_INVALID (the stored prefix or tool set is unusable), or
 *         AUTH_DB_FAILURE
 */
int conv_db_prefix_get(int64_t conv_id, int user_id, conv_prefix_t *out);

/** What a turn's prompt added to its conversation, saved together. */
typedef struct {
   const conv_message_row_t *rows; /**< Request-context rows (each with a kind), in order */
   size_t n_rows;
   const char *prefix;   /**< The prefix the turn ran under, or NULL to leave it */
   const char *tools;    /**< Its tool set (with @p prefix), or NULL for none */
   const char *in_force; /**< What is in force now (JSON), or NULL to leave it */
   int64_t floor_msg_id; /**< Raise the reasoning floor to this row, or 0 */
   /** A boundary with no question to place it at: the floor rises to the
    *  first of these rows (every row the conversation has, with none) */
   bool floor_at_rows;
   int64_t question_id; /**< The question the rows were sent with, or 0 */
   int64_t built_at;    /**< When the turn's prompt was built (unix time), or 0 */
   int64_t built_seq;   /**< Where withdrawals stood then (conv_db_withdraw_seq) */
} conv_turn_save_t;

/**
 * Save a turn's request context in one transaction: its rows, the prefix and
 * tool set it ran under (stored when they differ from those bound), what is in
 * force after it, and a declared boundary's reasoning floor.  All of it, or
 * none.  What the user forgot after the turn was built (@p save->built_at,
 * @p save->built_seq) is withdrawn from the rows as they are saved
 * (auth_db_withdraw.h), and a floor a withdrawal left pending settles at
 * @p save->question_id when the turn was built after it.
 *
 * Only request context is accepted (every row has a kind): it moves no
 * conversation in the list and counts as no message.
 *
 * @param ids_out Receives each row's id, n_rows entries (may be NULL)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN,
 *         AUTH_DB_INVALID or AUTH_DB_FAILURE
 */
int conv_db_save_turn(int64_t conv_id, int user_id, const conv_turn_save_t *save, int64_t *ids_out);

/**
 * Take back a background turn's envelope that got no reply (the turn produced
 * nothing, and is tried again with a new one): the envelope row and the
 * context sent in front of it go, so the conversation doesn't keep an
 * unanswered copy per attempt; the instruction or direction changes its turn
 * announced stay, naming no question (as session_rollback_turn keeps them).
 * Only when nothing else was saved to the conversation after it (such
 * announcements aside); otherwise it stays (AUTH_DB_DUPLICATE: the attempt
 * did work, or another writer's rows follow it).
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_DUPLICATE, AUTH_DB_NOT_FOUND (not the
 *         user's, or not an envelope) or AUTH_DB_FAILURE
 */
int conv_db_retract_envelope(int64_t conv_id, int user_id, int64_t envelope_id);

/**
 * Delete stored prefixes and tool sets no conversation refers to any more.
 * Called from maintenance; takes the lock itself.
 *
 * @param deleted_out Rows deleted (may be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_prompt_blobs_gc(int *deleted_out);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_CONV_PREFIX_H */
