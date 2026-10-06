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
 * The compaction boundary: which part of a history a compaction summarizes,
 * which database rows that is, and how a reload past it starts cleanly.
 */

#ifndef LLM_COMPACTION_RANGE_H
#define LLM_COMPACTION_RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/**
 * @brief Whether @p msg is part of a tool exchange, in the OpenAI or Claude
 *        shape: a role "tool" result, an assistant with tool_calls or tool_use
 *        parts, or a user message with tool_result parts
 */
bool llm_compaction_is_tool_exchange(struct json_object *msg);

/** @brief @p msg's database row id ("id"), or 0. */
int64_t llm_compaction_row_id(struct json_object *msg);

/**
 * @brief Where the kept part of a compacted history starts
 *
 * About @p keep_messages from the end, moved back so it starts a turn: past
 * any tool calls and results (OpenAI or Claude shape), then onto the user
 * message that began them.  A call and its results are never split.
 *
 * @param history  The history
 * @param start_idx First summarizable index (after the leading system messages)
 * @param keep_messages How many trailing messages to keep, at least
 * @return The first kept index; <= @p start_idx when nothing can be summarized
 */
int llm_compaction_keep_start(struct json_object *history, int start_idx, int keep_messages);

/**
 * @brief The first and last stamped row ids in the summarized range
 *
 * [@p start_idx, @p end_idx).  Messages carry their database row id once it is
 * known; a live session's tool calls and results carry none (see
 * llm_compaction_tail_call_ids for those).  0 when there is none.
 */
void llm_compaction_summary_ids(struct json_object *history,
                                int start_idx,
                                int end_idx,
                                int64_t *first_out,
                                int64_t *last_out);

/**
 * @brief Call ids of the summarized tool exchanges past the last stamped id
 *
 * Tool calls and results after the last message in [@p start_idx, @p end_idx)
 * that has its row id: their rows are summarized too, but only these ids tell
 * them from other rows of the conversation (a row written outside the session,
 * such as a research result, must stay loaded).  An id the kept part (from
 * @p end_idx) also uses is left out: it can't tell the rows apart.
 *
 * @return A new array of id strings (caller owns; empty when none), or NULL
 */
struct json_object *llm_compaction_tail_call_ids(struct json_object *history,
                                                 int start_idx,
                                                 int end_idx);

/**
 * @brief The first row id in the kept part (from @p end_idx), or 0
 *
 * Every summarized row is below it: a raise over tail rows stops there.
 */
int64_t llm_compaction_kept_first_id(struct json_object *history, int end_idx);

/**
 * @brief Whether a stored row belongs to one of the calls in @p ids
 *
 * A role "tool" row answering one, or an assistant row whose tool_calls hold
 * one.
 */
bool llm_compaction_row_in_calls(const char *role,
                                 const char *tool_calls,
                                 const char *tool_call_id,
                                 struct json_object *ids);

/**
 * @brief Drop tool results at @p from that answer no call before them
 *
 * Messages loaded after a compaction point start where the summary ends.  A
 * point recorded inside a tool exchange (by a build that mapped it wrongly)
 * leaves that exchange's later results first, with their call in the
 * summary: every provider rejects a result without its call.  Removes the
 * results (role "tool", or a user message holding only tool_result parts)
 * from @p from up to the first other message.  What they said is in the
 * summary.
 *
 * @param chars_out Receives the length of the removed messages' string
 *                  content (may be NULL)
 * @return How many messages were removed
 */
int llm_history_drop_leading_results(struct json_object *history, int from, size_t *chars_out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_COMPACTION_RANGE_H */
