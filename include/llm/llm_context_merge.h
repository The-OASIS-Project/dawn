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
 * Merging a background compaction result into the history a later turn runs on.
 */

#ifndef LLM_CONTEXT_MERGE_H
#define LLM_CONTEXT_MERGE_H

#include <json-c/json.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Replace the summarized part of @p history with @p compacted
 *
 * @p snapshot_last is the last message of the history the summary was computed
 * from.  Every turn rebuilds its history array but keeps the message objects,
 * so that message is found again by identity in @p history: everything up to it
 * is what was summarized, everything after arrived since.  The result, in place:
 * @p history's current leading system messages (this turn's prompt), then
 * @p compacted without its system messages, then the messages after the
 * boundary.  System messages inside @p compacted past its leading prompt (device
 * notices) are kept.  Caller holds the history's lock.
 *
 * @return true when merged; false when @p snapshot_last is not in @p history
 *         past its leading system messages (the history changed underneath:
 *         the result is stale)
 */
bool llm_context_merge_compacted(struct json_object *history,
                                 struct json_object *compacted,
                                 struct json_object *snapshot_last);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CONTEXT_MERGE_H */
