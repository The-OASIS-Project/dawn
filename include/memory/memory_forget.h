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
 * Forget what memory learned from a conversation (offered when a user marks a
 * conversation private).
 */

#ifndef MEMORY_FORGET_H
#define MEMORY_FORGET_H

#include <stdint.h>

#include "memory/memory_db_provenance.h"

#ifdef __cplusplus
extern "C" {
#endif

/** memory_forget_conversation(): an extraction for the user was still running
 *  when the wait ran out; nothing was deleted, retry later. */
#define MEMORY_FORGET_BUSY 2

/** The conversation has more than CONV_CHAIN_MAX continuations; nothing was
 *  counted or deleted. */
#define MEMORY_FORGET_TOO_LONG 3

/** Longest a count or forget waits for the user's in-flight extraction. */
#define MEMORY_FORGET_WAIT_SEC 120

/**
 * @brief Count what memory learned from a conversation and its continuations
 *
 * Waits (up to MEMORY_FORGET_WAIT_SEC) for an extraction already running for the
 * user, so rows it is still writing are counted.  Blocking: call off the WebUI
 * service thread.
 *
 * @return SUCCESS, MEMORY_FORGET_TOO_LONG, or FAILURE
 */
int memory_conversation_learned(int user_id, int64_t conv_id, memory_conv_learned_t *out);

/**
 * @brief Delete every memory learned from a conversation and its continuations
 *
 * Covers the conversation and every conversation continuing it (they share its
 * context).  Waits for an extraction already running for the user and keeps a
 * new one from starting until done, then deletes in one transaction (see
 * memory_db_conversations_forget).  The caller must already have verified that
 * @p user_id owns the conversation.  Blocking: call off the WebUI service thread.
 *
 * @param deleted_out Counts actually deleted (may be NULL)
 * @return SUCCESS; MEMORY_FORGET_BUSY (extraction still running, nothing
 *         deleted); MEMORY_FORGET_TOO_LONG; or FAILURE (nothing deleted)
 */
int memory_forget_conversation(int user_id, int64_t conv_id, memory_conv_learned_t *deleted_out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FORGET_H */
