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

/**
 * @brief Claim the user's extraction slot, never queueing: for a run whose
 *        request can't wait in the queue (a range run)
 * @return true when the slot is the caller's; false when the user's
 *         extraction is running, held or waited for, every slot is busy, or
 *         the daemon is stopping
 */
bool extraction_slot_claim(int user_id);

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

/** A range run (memory_extraction_range.c): its bounds, and what its dry run
 *  found. */
typedef struct {
   int64_t from; /* first row of the range */
   int64_t to;   /* last row of the range */
   bool dry;     /* find what it would read; claim nothing, write nothing */
   int outside;  /* out: the outside kind the batch carries */
} extraction_range_t;

/**
 * @brief Extract rows from..to of a conversation behind its cursor, through
 *        the trigger every extraction takes (memory_extraction.c)
 * @param history The conversation's stored rows through @p range->to
 *                (borrowed)
 * @return SUCCESS when started (or, dry, when it would start), MEMORY_EXTRACTION_BUSY,
 *         MEMORY_EXTRACT_RANGE_NO_USER_ROW, or another refusal (non-zero)
 */
int extraction_run_range(int user_id,
                         int64_t conversation_id,
                         struct json_object *history,
                         extraction_range_t *range);

/**
 * @brief Record that rows from..to of a conversation were extracted, unless a
 *        recorded range overlaps them (one statement, so two runs can't both
 *        pass)
 * @return SUCCESS, MEMORY_EXTRACT_RANGE_OVERLAP, or FAILURE
 */
int extraction_range_record(int64_t conversation_id, int64_t from, int64_t to);

/** Remove the record of a range run that stored nothing. */
void extraction_range_unrecord(int64_t conversation_id, int64_t from, int64_t to);

#endif /* MEMORY_EXTRACTION_INTERNAL_H */
