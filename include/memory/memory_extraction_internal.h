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
 * What memory_extraction.c (the extraction) and memory_extraction_slots.c
 * (who may extract now) share.  Nothing else may include it: the slots and
 * the queue are only consistent under their own mutex.
 */

#ifndef MEMORY_EXTRACTION_INTERNAL_H
#define MEMORY_EXTRACTION_INTERNAL_H

#ifndef MEMORY_EXTRACTION_INTERNAL_ALLOWED
#error "memory_extraction_internal.h is private to the memory extraction module"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_extraction.h"

struct json_object;

/**
 * @brief Claim the user's extraction slot, or else queue @p conversation_id
 *        (not for memory recovery's own requests) to run at the next release
 * @return true when the slot is the caller's (release it with
 *         extraction_slot_release); false when refused (queued, or left for
 *         recovery when the queue is full)
 */
bool extraction_slot_claim_or_queue(int user_id,
                                    int64_t conversation_id,
                                    const char *session_id,
                                    const memory_extraction_fallback_t *fallback);

/** Release the user's slot, then start what the queue holds that can now run. */
void extraction_slot_release(int user_id);

/** Note the user's last extraction outcome (memory_extraction_consume_last_transient). */
void extraction_record_outcome(int user_id, bool was_transient);

/**
 * @brief Run a queued conversation's extraction on the slot already claimed
 *        for it (memory_extraction.c); the slot is released when it ends
 */
int extraction_run_queued(int user_id,
                          int64_t conversation_id,
                          const char *session_id,
                          struct json_object *history,
                          int message_count,
                          const memory_extraction_fallback_t *fallback);

#endif /* MEMORY_EXTRACTION_INTERNAL_H */
