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
 * Extracting a range of a conversation's rows that lie behind its extraction
 * cursor: rows an earlier extraction marked done without reading (a session
 * whose history lacked another session's turns).  The admin names the range;
 * the run goes through the extraction every conversation takes, leaves the
 * cursor and the conversation's summary alone, and is recorded
 * (memory_extract_ranges) so the same rows are never learned twice.
 */

#ifndef MEMORY_EXTRACTION_RANGE_H
#define MEMORY_EXTRACTION_RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a range is refused (SUCCESS: it runs, or, dry, would). */
#define MEMORY_EXTRACT_RANGE_NOT_FOUND 10   /**< no such conversation of the user's */
#define MEMORY_EXTRACT_RANGE_PRIVATE 11     /**< the conversation is private */
#define MEMORY_EXTRACT_RANGE_BAD_RANGE 12   /**< from is not at least 1 and at most to */
#define MEMORY_EXTRACT_RANGE_PAST_CURSOR 13 /**< to is past the extraction cursor */
#define MEMORY_EXTRACT_RANGE_NO_USER_ROW 14 /**< nothing in it the user said */
#define MEMORY_EXTRACT_RANGE_OVERLAP 15     /**< an earlier range run covers part of it */
#define MEMORY_EXTRACT_RANGE_BUSY 16        /**< the user's extraction is running or held */
#define MEMORY_EXTRACT_RANGE_JOB 17         /**< a background job's conversation */
#define MEMORY_EXTRACT_RANGE_DISABLED 18    /**< memory is turned off */
#define MEMORY_EXTRACT_RANGE_FORGOTTEN 19   /**< it reaches rows the user had forgotten */

/** What a range holds, counts only. */
typedef struct {
   bool counted;   /**< the counts below were read */
   int user;       /**< the user's messages */
   int assistant;  /**< replies */
   int tool;       /**< tool results */
   int other;      /**< other rows, DAWN's request context among them */
   int outside;    /**< outside_kind_t the batch carries; -1 not known */
   bool overlap;   /**< an earlier range run covers part of it */
   int64_t cursor; /**< the conversation's extraction cursor */
} memory_extract_range_report_t;

/**
 * @brief Check, and with @p confirm start, an extraction of rows
 *        @p from..@p to of conversation @p conversation_id
 *
 * Refused when the conversation isn't the user's (or doesn't exist), is
 * private or a background job's; when @p to is past its extraction cursor
 * (the range must be behind it, where no run reads); when the range holds
 * nothing the user said; when an earlier range run overlaps it; and, with
 * @p confirm, when the user's extraction is running or held (a range run
 * waits in no queue).
 *
 * Without @p confirm nothing is written: @p out gets the range's counts, the
 * outside kind the batch would carry, and the result is the refusal that
 * would apply.  With it, the run starts on its own thread: the cursor and the
 * summary are left alone, provenance names the range, and the range is
 * recorded before anything is stored.
 *
 * @return SUCCESS, a MEMORY_EXTRACT_RANGE_* refusal, or FAILURE
 */
int memory_extract_range(int user_id,
                         int64_t conversation_id,
                         int64_t from,
                         int64_t to,
                         bool confirm,
                         memory_extract_range_report_t *out);

/** A refusal's text, for the admin's reply ("" for SUCCESS). */
const char *memory_extract_range_refusal_text(int code);

/**
 * @brief The admin's reply to memory_extract_range(): the counts when read,
 *        then what happened or would (counts and ids only, never row text)
 * @param code What memory_extract_range() returned
 */
void memory_extract_range_describe(int code,
                                   bool confirm,
                                   int64_t from,
                                   int64_t to,
                                   const memory_extract_range_report_t *r,
                                   char *buf,
                                   size_t size);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_RANGE_H */
