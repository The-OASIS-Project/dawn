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
 * A consolidation batch as a reader shows it, from its recorded ops.
 */

#include "memory/memory_consolidation_view.h"

#include <string.h>

const char *const memory_consolidation_count_kinds[MEMORY_CONSOLIDATION_COUNT_KINDS] = {
   "added",  "reinforced", "merged",  "replaced", "closed",
   "linked", "proposed",   "dropped", "deferred", "suggested_forget"
};

static bool is(const char *a, const char *b) {
   return strcmp(a, b) == 0;
}

/* What the user may do with @p o, @p undoable computed once. */
static const char *action_of(const memory_consolidation_op_row_t *o, bool undoable) {
   if (undoable) {
      return "undo";
   }
   if (memory_consolidation_op_keepable(o)) {
      return "keep";
   }
   if (is(o->op, "suggested_forget") && is(o->status, "applied") && o->fact_id > 0) {
      return "forget";
   }
   return NULL;
}

void memory_consolidation_view(const memory_consolidation_op_row_t *rows,
                               int n,
                               memory_consolidation_op_view_t *ops,
                               memory_consolidation_batch_view_t *batch) {
   memset(batch, 0, sizeof(*batch));
   int undone = 0;
   int open = 0; /* still undoable, or found changed since */
   for (int i = 0; i < n; i++) {
      const memory_consolidation_op_row_t *o = &rows[i];
      memory_consolidation_op_view_t *v = &ops[i];
      memset(v, 0, sizeof(*v));
      if (is(o->status, "refused")) {
         batch->refused++;
         continue;
      }
      for (int k = 0; k < MEMORY_CONSOLIDATION_COUNT_KINDS; k++) {
         batch->counts[k] += is(o->op, memory_consolidation_count_kinds[k]);
      }
      const bool proposed = is(o->op, "proposed");
      const bool undoable = memory_consolidation_op_undoable(o);
      v->listed = true;
      v->op = proposed ? "linked" : o->op;
      v->status = proposed ? "proposed" : o->status;
      v->link = proposed || is(o->op, "linked");
      v->action = action_of(o, undoable);
      undone += is(o->status, "undone");
      open += undoable || is(o->status, "stale");
   }
   batch->status = undone == 0 ? "applied" : open == 0 ? "undone" : "partial";
}
