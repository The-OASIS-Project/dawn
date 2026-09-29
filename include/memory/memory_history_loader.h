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
 * Conversation history loader — reconstructs the LLM-bound JSON message
 * array from the persisted messages table.  Shared by memory_recovery
 * (re-extract stuck conversations) and memory_summarize_missing
 * (backfill summaries for conversations whose extraction never produced
 * one).  Inline image markers (`[IMAGE:data:image/...]`) are stripped to
 * `[image]` placeholders so vision-heavy histories stay extractable
 * without sending hundreds of KB of base64 to the LLM.
 */

#ifndef MEMORY_HISTORY_LOADER_H
#define MEMORY_HISTORY_LOADER_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A row as a loader callback sees it
 *
 * Append the row's message to the loader's array (at most one per row).  The
 * row's stored blocks are not the callback's to read: the loader attaches them
 * to that message itself (see memory_history_load_rows).
 */
typedef int (*memory_history_row_cb)(const conversation_llm_row_t *row, void *ctx);

/**
 * @brief Load a conversation's context rows past its compaction point
 *
 * The one way a conversation's rows are read back into an LLM context: every
 * row when @p watermark is 0, else the rows after it.  Each row goes through
 * @p cb (which appends it to @p rows).  Past a compaction point, tool results
 * at the start of what was loaded answer a call inside the summary; they are
 * dropped (llm_history_drop_leading_results), on every such load.
 *
 * With @p with_blocks, an assistant row's stored blocks are attached to the
 * message @p cb appended for it (under LLM_TURN_BLOCKS_KEY), once the read is
 * done: only when they parse (llm_turn_blocks_from_stored) and record the
 * message's tool calls.  Only a load that becomes an LLM context replayed to
 * a vendor asks for them.
 *
 * @param rows        The array @p cb appends to (the new rows start at its
 *                    current length)
 * @param chars_out   Receives the length of dropped rows' text (may be NULL)
 * @return AUTH_DB_SUCCESS, or the read's error
 */
int memory_history_load_rows(int64_t conv_id,
                             int user_id,
                             int64_t watermark,
                             bool with_blocks,
                             memory_history_row_cb cb,
                             void *ctx,
                             struct json_object *rows,
                             size_t *chars_out);

/**
 * @brief Reconstruct conversation history as a json_object array.
 *
 * Reads rows from the messages table for (conv_id, user_id), strips inline
 * image markers, and assembles {"role","content","id"} entries in original
 * order.  Carries no stored blocks: for extraction and summaries, which never
 * replay a turn to its vendor.  The returned array is owned by the caller and
 * must be released with json_object_put().
 *
 * @param conv_id        conversation ID
 * @param user_id        owning user ID (defense-in-depth ownership check)
 * @param text_len_out   optional — receives the total post-strip plain-text
 *                       length, useful for "does this conversation have
 *                       enough content to extract from" gates
 * @return owned json_object array, or NULL on failure
 */
struct json_object *memory_history_load_from_db(int64_t conv_id, int user_id, size_t *text_len_out);

/**
 * @brief A conversation's request, rebuilt for the model that continues it
 *
 * The one builder every load that continues a conversation goes through
 * (a WebUI restore, a resumed job, a reinvoke, a messaging channel), so each
 * rebuilds the bytes its turns were sent:
 *  - the rows after @p watermark, with the compaction summary's marker after
 *    any system message the conversation was saved with;
 *  - an image turn's question with its images again (owner-checked,
 *    image_rehydrate_message), request context as the text it was sent, tool
 *    fields and each assistant turn's stored blocks as they were; legacy
 *    display markers at the start of a reply left out;
 *  - each turn's context folded back into its question
 *    (llm_history_fold_context), and the conversation's frozen prefix first
 *    when one is stored.
 *
 * Reads image files: never call it holding a lock.
 *
 * @param compaction_summary The conversation's summary, or NULL
 * @param text_len_out Receives the loaded rows' text length (may be NULL)
 * @param rows_out     Receives the number of rows loaded (may be NULL)
 * @return owned array, or NULL on a read or allocation failure
 */
struct json_object *memory_history_request_context(int64_t conv_id,
                                                   int user_id,
                                                   int64_t watermark,
                                                   const char *compaction_summary,
                                                   size_t *text_len_out,
                                                   int *rows_out);

/**
 * @brief memory_history_request_context() for a conversation, by id
 *
 * NULL when it is missing or not @p user_id's.
 */
struct json_object *memory_history_load_for_llm(int64_t conv_id, int user_id, size_t *text_len_out);

/**
 * @brief Replace `[IMAGE:...]` markers in `src` with `[image]`.
 *
 * Caller owns the returned heap string.  Returns "" (empty owned string)
 * when src is NULL, or NULL on OOM.  Linear scan; safe on arbitrary input
 * (the base64 alphabet contains no `]`, so the closing bracket terminates
 * the marker unambiguously).
 */
char *memory_history_strip_image_markers(const char *src);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_HISTORY_LOADER_H */
