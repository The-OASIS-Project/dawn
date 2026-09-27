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
 * Bulk fact import (memory restore / paste-in).  The one place an importer
 * creates facts, so every importer gets the same filtering, de-duplication and
 * embedding without having to remember any of it.
 */

#ifndef MEMORY_IMPORT_H
#define MEMORY_IMPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of importing one fact. */
typedef enum {
   MEMORY_IMPORT_ACCEPTED,  /**< would be / was created */
   MEMORY_IMPORT_EMPTY,     /**< no text */
   MEMORY_IMPORT_BLOCKED,   /**< rejected by the injection filter */
   MEMORY_IMPORT_DUPLICATE, /**< already known (exact or near-duplicate) */
   MEMORY_IMPORT_FAILED,    /**< commit requested but the DB write failed */
} memory_import_status_t;

/** Confidence for an imported fact or preference that doesn't give one. */
#define MEMORY_IMPORT_DEFAULT_CONFIDENCE 0.8f

/**
 * @brief Confidence from untrusted import data, clamped to [0, 1]
 *
 * @param raw     Value as parsed (any double, including NaN/inf)
 * @return @p raw clamped to [0, 1], or MEMORY_IMPORT_DEFAULT_CONFIDENCE when
 *         it isn't a finite number
 */
float memory_import_confidence(double raw);

/**
 * @brief Make truncated import text valid UTF-8, in place
 *
 * Drops a multi-byte character cut short at the end (by truncation to a field
 * limit) and replaces any other invalid byte, so the text is safe to store and
 * to echo back in JSON.
 */
void memory_import_clean_text(char *s);

/**
 * @brief Import one fact for a user
 *
 * Truncates to MEMORY_FACT_TEXT_MAX (on a UTF-8 character boundary), clamps
 * @p confidence (memory_import_confidence), applies the injection filter and duplicate
 * check, and when @p commit is set creates the fact (source "import") and queues
 * it for embedding, so it reaches semantic search and focus injection.  With
 * @p commit false nothing is written (preview).
 *
 * @param user_id    Owner
 * @param text       Fact text
 * @param confidence Stored confidence
 * @param commit     Write the fact (false = dry run)
 * @param out_text   Receives the text as it would be stored (for previews)
 * @return Outcome
 */
memory_import_status_t memory_import_fact(int user_id,
                                          const char *text,
                                          float confidence,
                                          bool commit,
                                          char out_text[MEMORY_FACT_TEXT_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_IMPORT_H */
