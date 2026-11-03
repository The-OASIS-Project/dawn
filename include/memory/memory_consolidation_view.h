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
 * A consolidation batch as a reader shows it: each op's kind and status as
 * shown, what the user may do with it, and the batch's counts and status.
 * Pure functions of its recorded ops; surfaces only serialize the result.
 */

#ifndef MEMORY_CONSOLIDATION_VIEW_H
#define MEMORY_CONSOLIDATION_VIEW_H

#include <stdbool.h>

#include "memory/memory_db_consolidation.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The kinds a batch's counts give, in order. */
#define MEMORY_CONSOLIDATION_COUNT_KINDS 10
extern const char *const memory_consolidation_count_kinds[MEMORY_CONSOLIDATION_COUNT_KINDS];

/** One op as shown. */
typedef struct {
   bool listed;        /* shown (a refused op is only counted) */
   const char *op;     /* its kind; a link that became a proposal is "linked" */
   const char *status; /* its status; that link's is "proposed" */
   const char *action; /* "undo", "keep", "forget", or NULL */
   bool link;          /* its counterpart is an entity, not a fact */
} memory_consolidation_op_view_t;

/** A batch as shown. */
typedef struct {
   int counts[MEMORY_CONSOLIDATION_COUNT_KINDS];
   int refused;
   const char *status; /* "applied", "partial" (some undone) or "undone" (all of it) */
} memory_consolidation_batch_view_t;

/**
 * @brief The view of @p n ops @p rows (as memory_db_consolidation_ops_list
 *        reads them): one entry per op into @p ops (@p n of them), and the
 *        batch's counts and status into @p batch
 */
void memory_consolidation_view(const memory_consolidation_op_row_t *rows,
                               int n,
                               memory_consolidation_op_view_t *ops,
                               memory_consolidation_batch_view_t *batch);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONSOLIDATION_VIEW_H */
