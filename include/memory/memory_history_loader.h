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
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load a conversation's context rows past its compaction point
 *
 * The one way a conversation's rows are read back into an LLM context: every
 * row when @p watermark is 0, else the rows after it.  Each row goes through
 * @p cb (which appends it to @p rows).  Past a compaction point, tool results
 * at the start of what was loaded answer a call inside the summary; they are
 * dropped (llm_history_drop_leading_results), on every such load.
 *
 * @param rows        The array @p cb appends to (the new rows start at its
 *                    current length)
 * @param chars_out   Receives the length of dropped rows' text (may be NULL)
 * @return AUTH_DB_SUCCESS, or the read's error
 */
int memory_history_load_rows(int64_t conv_id,
                             int user_id,
                             int64_t watermark,
                             message_callback_t cb,
                             void *ctx,
                             struct json_object *rows,
                             size_t *chars_out);

/**
 * @brief Reconstruct conversation history as a json_object array.
 *
 * Reads rows from the messages table for (conv_id, user_id), strips inline
 * image markers, and assembles {"role","content","id"} entries in original
 * order.  The returned array is owned by the caller and must be released
 * with json_object_put().
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
