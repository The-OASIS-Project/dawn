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
   int context_of_row;       /**< conv_db_add_rows only: that question as a row of the
                                  same batch (1-based); 0 = context_of */
   bool is_error;            /**< tool: 1 = confirmed failure */
   const char *images;       /**< tool: the result's images, a JSON array of image ids
                                  (at most CONV_MESSAGE_IMAGES_MAX); NULL = none */
   bool images_bind_later;   /**< leave those images unbound: the caller binds them
                                  with conv_db_bind_images() as its last step */
} conv_message_row_t;

/** Most image ids one row names (a tool result's images). */
#define CONV_MESSAGE_IMAGES_MAX 64

/**
 * @brief Insert a message row, checking the conversation belongs to @p user_id.
 *
 * A row naming `images` binds them (unbound captures of the user's, made
 * permanent) in the same transaction as its insert, unless
 * `images_bind_later`.  Ids a row can't hold (not a tool row, not a JSON
 * array of image ids) are dropped with a warning; the row still saves.
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
 * @brief Insert @p n rows in one transaction: all of them, or none
 *
 * Each row as conv_db_add_row(), except that a row's `context_of_row` may name
 * an earlier row of the same batch as its question.  One commit, one list
 * signal.
 *
 * @param ids_out Receives each row's id (n entries); may be NULL.
 * @return As conv_db_add_row(); on failure nothing is saved.
 */
int conv_db_add_rows(int64_t conv_id,
                     int user_id,
                     const conv_message_row_t *rows,
                     int n,
                     int64_t *ids_out);

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
   const char *images; /**< a tool row's image ids (JSON array), or NULL */
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
 * @brief Bind the images @p conv_id's rows name: the user's unbound captures
 *        become permanent (they go with the conversation).  A voice save's
 *        last step: its rows are written with images_bind_later.
 *
 * A capture a row names that is no longer stored (the unbound grace reclaimed
 * it) can never be bound: the bind is short.  That is a warning, not a
 * failure (a save that failed on it would fail on every retry), counted in
 * @p missing_out (may be NULL).
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID or AUTH_DB_FAILURE.
 */
int conv_db_bind_images(int64_t conv_id, int user_id, int *missing_out);

/* -----------------------------------------------------------------------------
 * The images a conversation owns
 *
 * A conversation names an image in conversation_images (v98) when a row that
 * holds it is stored: a tool row's captures (messages.images) and a question's
 * uploads or MMS ([IMAGE:<id>] markers on an ordinary user row), the owner's
 * only.  A reply's marker names nothing: a reply can quote any id.  An image
 * the conversation owns is one only it names, bound (never one a running turn
 * or a save to be retried still holds); a conversation delete takes those with
 * it, in the same transaction.
 * -------------------------------------------------------------------------- */

/** What a conversation delete does with the images it owns. */
typedef enum {
   CONV_IMAGES_KEEP,   /**< they stay (images outlive the conversation) */
   CONV_IMAGES_DELETE, /**< they go: rows in the delete's transaction, files after */
   CONV_IMAGES_UNBIND, /**< a save rolled back: the captures it bound go back to
                            unbound, for its retry (or the grace sweep) */
} conv_images_mode_t;

/** Longest image file name a delete hands back. */
#define CONV_IMAGE_FILENAME_MAX 40

/** The files of the images a delete removed, for the caller to unlink once
 *  the database lock is released.  Free with conv_image_files_free(). */
typedef struct {
   char (*names)[CONV_IMAGE_FILENAME_MAX];
   int count;
} conv_image_files_t;

void conv_image_files_free(conv_image_files_t *files);

/**
 * @brief Delete a conversation, and with @p mode the images it owns, in one
 *        transaction under the database lock
 *
 * conv_db_delete() (or, with @p admin, any owner's conversation)
 * with its images: their rows are removed (or unbound) in the same
 * transaction as the conversation, so no row naming one can be stored between
 * the choice and the delete.  The removed images' files come back in
 * @p files_out (CONV_IMAGES_DELETE; may be NULL only for the other modes).
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_INVALID or
 *         AUTH_DB_FAILURE (nothing deleted, *@p files_out empty).
 */
int conv_db_delete_ex(int64_t conv_id,
                      int user_id,
                      bool admin,
                      conv_images_mode_t mode,
                      conv_image_files_t *files_out);

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
