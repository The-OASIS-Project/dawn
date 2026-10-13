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
 * Conversations waiting for memory extraction: asked for while their user's
 * extraction ran, or while every extraction slot was busy.  Oldest first, one
 * entry per conversation.
 *
 * memory_extraction_slots.c's alone.  Not locked here: it calls every function
 * under its extraction mutex, so taking an entry and claiming its slot are one
 * step (a lock of its own would only nest under that one).  Nothing else may
 * call these.
 */

#ifndef MEMORY_EXTRACTION_QUEUE_H
#define MEMORY_EXTRACTION_QUEUE_H

#ifndef MEMORY_EXTRACTION_QUEUE_ALLOWED
#error "memory_extraction_queue.h is private to memory_extraction_slots.c"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_extraction.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Conversations that can wait at once. */
#define EXTRACTION_QUEUE_MAX 32

/** One waiting extraction: re-read from the database when it runs. */
typedef struct {
   int user_id;
   int64_t conversation_id;
   char session_id[MEMORY_SESSION_ID_MAX];
   bool has_fallback;
   memory_extraction_fallback_t fallback;
} extraction_queue_entry_t;

/**
 * @brief Add @p e, or refresh the entry its conversation already has
 * @return false when the queue is full
 */
bool extraction_queue_push(const extraction_queue_entry_t *e);

/**
 * @brief Take the oldest entry @p ready accepts (it may claim the entry's
 *        extraction slot as it accepts)
 * @return true with the entry in @p out, false when none was ready
 */
bool extraction_queue_take(bool (*ready)(int user_id, void *ctx),
                           void *ctx,
                           extraction_queue_entry_t *out);

/** Drop the user's entries for the conversations in @p ids. */
void extraction_queue_drop_conversations(int user_id, const int64_t *ids, int n);

/** Drop every entry of the user. */
void extraction_queue_drop_user(int user_id);

/** Drop every entry. */
void extraction_queue_clear(void);

/** Entries waiting. */
int extraction_queue_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_QUEUE_H */
