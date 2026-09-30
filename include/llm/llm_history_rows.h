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
 * The rows a history message is saved as, whatever format it's in.
 */

#ifndef LLM_HISTORY_ROWS_H
#define LLM_HISTORY_ROWS_H

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Key on a row object holding its stored blocks (assistant rows only). */
#define LLM_HISTORY_ROW_STORED_KEY "_stored"

/**
 * @brief Append the rows @p msg is saved as to @p out
 *
 * Each row is a canonical {role, content, tool_calls?, tool_call_id?} object,
 * the shape every conversation reader and the step-event walk expect:
 *
 *  - an assistant turn is one row: its text (joined, never cut) and its tool
 *    calls, from its blocks without any vendor data; plus the blocks
 *    themselves (llm_turn_blocks_to_stored) under LLM_HISTORY_ROW_STORED_KEY
 *    when the turn has its own blocks and they record the row's calls;
 *  - a Claude user message of tool results is one tool row per result;
 *  - an OpenAI tool message is one tool row;
 *  - anything else is one row of its role and text (a content array's text
 *    parts joined, an image as "[image]").
 *
 * Request context (llm_history_kind.h) is saved as rows of its kind, marked
 * with MESSAGE_KIND_KEY: a context message's rows carry its kind, and each
 * kind part of a question is a row of its own after the question's rows (the
 * order a live turn saves them in).
 * The frozen prefix is never a row.
 *
 * @return Rows appended (0 for a message with no role)
 */
int llm_history_rows_append(struct json_object *msg, struct json_object *out);

/**
 * @brief As llm_history_rows_append, without any stored blocks: the rows a
 *        message is saved as once its reasoning no longer replays (a
 *        compaction's summarized part)
 *
 * @return Rows appended
 */
int llm_history_rows_append_text(struct json_object *msg, struct json_object *out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_HISTORY_ROWS_H */
