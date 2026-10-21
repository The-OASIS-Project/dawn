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
 * Memory extraction input: which of a session's messages are sent to the
 * extraction model, and the check that they belong to the conversation.
 */

#ifndef MEMORY_EXTRACTION_INPUT_H
#define MEMORY_EXTRACTION_INPUT_H

#include <stdbool.h>
#include <stdint.h>

struct json_object;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The messages of @p conversation_history to send for extraction
 *
 * Messages after @p last_msg_id (plus id-less ones: compaction markers, a turn's
 * intermediate tool entries, unpersisted satellite sessions), minus system
 * messages and DAWN's own injected events; images replaced by a placeholder,
 * note-filed text redacted, each message capped.
 *
 * For a DB conversation (@p conversation_id > 0) the rows themselves are checked
 * to belong to it and to no private or background-job conversation, and every
 * user message must carry a row id.  Any disagreement refuses the whole input
 * (fail closed; memory_recovery re-extracts eligible conversations from the DB).
 *
 * @p conversation_id 0 means "no DB conversation" and skips that check: pass it
 * only for histories that are never persisted (satellite sessions).  A caller
 * with a persisted history it can't attribute must not extract it at all.
 *
 * @param user_id              Owner
 * @param conversation_id      Conversation being extracted (0 = none)
 * @param conversation_history Session history (read only)
 * @param last_msg_id          Extraction cursor for the conversation
 * @return New JSON array (caller owns), or NULL when refused or out of memory
 */
/**
 * @brief Whether an extraction input holds anything the user said
 *
 * Without a question there is nothing to learn (the assistant is never a
 * source), so the caller skips the model call.
 */
bool memory_extraction_input_has_question(struct json_object *input);

/**
 * @brief The highest row id in a conversation history (0 when none has one)
 *
 * Marking a conversation extracted after checking a history goes only this
 * far: a row added since (a reply arriving meanwhile) was never checked.
 */
int64_t memory_extraction_history_max_id(struct json_object *history);

struct json_object *memory_extraction_build_input(int user_id,
                                                  int64_t conversation_id,
                                                  struct json_object *conversation_history,
                                                  int64_t last_msg_id);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_INPUT_H */
