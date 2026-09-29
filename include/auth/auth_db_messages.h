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
 * Conversation message rows: the one insert every writer goes through, and the
 * replay read that rebuilds an LLM context with each assistant turn's blocks.
 *
 * `messages.llm_blocks` holds an assistant turn as the model produced it
 * (text, tool calls, and reasoning a vendor issued for itself). It is read
 * ONLY by conv_db_get_messages_for_llm(); every display, search, export and
 * admin read leaves it out, so it never reaches a client.
 */

#ifndef AUTH_DB_MESSAGES_H
#define AUTH_DB_MESSAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Largest stored `llm_blocks` value, in bytes. A plain integer literal: the
 * schema's CHECK constraint is built from it. Larger turns are stored without
 * their blocks (they reload as text and tool calls).
 */
#define CONV_LLM_BLOCKS_MAX 4194304

/**
 * Most block bytes one replay load returns. Past it, the OLDEST rows load
 * without blocks (they replay as text and tool calls), which bounds the memory
 * a reload holds and the time it parses on the thread that restores it.
 */
#define CONV_LLM_BLOCKS_LOAD_BUDGET ((size_t)8 * 1024 * 1024)

/* A single row must fit a load, or one oversized turn would push every row of
 * its conversation past the budget. */
#ifndef __cplusplus
_Static_assert(CONV_LLM_BLOCKS_MAX <= CONV_LLM_BLOCKS_LOAD_BUDGET,
               "a stored row must fit the per-load budget");
#endif

/**
 * @brief One message row to insert. Any field may be NULL except role and
 *        content.
 */
typedef struct {
   const char *role;         /**< "system", "user", "assistant" or "tool" */
   const char *content;      /**< Display text (never vendor data) */
   const char *tool_calls;   /**< assistant: OpenAI tool_calls JSON array */
   const char *tool_call_id; /**< tool: the call this result answers */
   const char *reasoning;    /**< assistant: display-only reasoning JSON */
   const char *llm_blocks;   /**< assistant: stored turn blocks (llm_turn_blocks_to_stored) */
   const char *kind;         /**< request-context kind (message_kind.h); NULL = ordinary */
   int64_t context_of;       /**< a kinded row: the question it belongs to; 0 = none */
   bool is_error;            /**< tool: 1 = confirmed failure */
} conv_message_row_t;

/**
 * @brief Insert a message row, checking the conversation belongs to @p user_id.
 *
 * Updates the conversation's updated_at and message count. `llm_blocks` is
 * accepted on assistant rows only, and only up to CONV_LLM_BLOCKS_MAX bytes.
 * A row with a `kind` (request context) must name a kind whose role matches
 * (message_kind.h); it is saved without touching the message count, the
 * conversation's order in the list, or anyone's view of it.  Its `context_of`,
 * when set, must name a user row (ordinary or an envelope) of the same
 * conversation.
 *
 * @param id_out Receives the new row id (0 on failure); may be NULL.
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN
 *         or AUTH_DB_FAILURE.
 */
int conv_db_add_row(int64_t conv_id, int user_id, const conv_message_row_t *row, int64_t *id_out);

/**
 * @brief A message row as the replay read returns it. A separate type from
 *        conversation_message_t so display readers can't reach `llm_blocks`.
 *
 * Every pointer is borrowed and valid only during the callback.
 */
typedef struct {
   int64_t id;
   char role[CONV_ROLE_MAX];
   const char *content;
   const char *tool_calls;
   const char *tool_call_id;
   const char *llm_blocks; /**< NULL when absent, over the cap, or past the load budget */
   size_t llm_blocks_len;
   const char *kind;   /**< request-context kind, or NULL for an ordinary message */
   int64_t context_of; /**< a kinded row's question, or 0 */
   time_t created_at;
   int is_error;
} conversation_llm_row_t;

/** @return 0 to continue, non-zero to stop. */
typedef int (*conversation_llm_row_cb)(const conversation_llm_row_t *row, void *ctx);

/**
 * @brief Read a conversation's rows, with their stored blocks, to rebuild an
 *        LLM context.
 *
 * Only rows with id > @p after_id (0 = all), in id order, and only when the
 * conversation belongs to @p user_id. Blocks past CONV_LLM_BLOCKS_LOAD_BUDGET
 * (counted from the newest row back) are left out.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID or AUTH_DB_FAILURE.
 */
int conv_db_get_messages_for_llm(int64_t conv_id,
                                 int user_id,
                                 int64_t after_id,
                                 conversation_llm_row_cb callback,
                                 void *ctx);

/**
 * @brief Drop stored blocks no reload reads any more
 *
 * Rows at or below their conversation's compaction watermark, a bounded batch
 * per call.  Advancing a watermark clears its conversation's rows right away;
 * this catches anything that left behind (a crash between the two, say).
 *
 * @param cleared_out Receives how many rows were cleared (may be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_sweep_compacted_blocks(int *cleared_out);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_MESSAGES_H */
