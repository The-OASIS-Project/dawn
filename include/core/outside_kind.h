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
 * What outside text (text someone other than the user or DAWN wrote: a page,
 * an email, a document) something carried.  A stored memory row carries the
 * kind its extraction batch read (memory_facts / memory_relations /
 * memory_summaries .outside), so code can keep such rows from acting as the
 * user's own word.
 *
 * The values are kinds, not bits: where two combine, the tainted one wins,
 * the first seen.  UNKNOWN marks rows stored before the kind was recorded,
 * which can't be judged: guard them like clean rows (outside text can't
 * overwrite them) and read them as not tainted (they don't say "outside").
 */

#ifndef OUTSIDE_KIND_H
#define OUTSIDE_KIND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What outside text something carried.  The values are stored (memory
 *        rows, conversations.outside, exports): append new kinds at the end,
 *        never renumber one.
 */
typedef enum {
   OUTSIDE_NONE = 0,      /**< only the user's words and DAWN's own */
   OUTSIDE_WEB = 1,       /**< a web page or search result */
   OUTSIDE_EMAIL = 2,     /**< an email */
   OUTSIDE_RESEARCH = 3,  /**< a research report */
   OUTSIDE_MESSAGING = 4, /**< a message read from a chat channel */
   OUTSIDE_DOCUMENT = 5,  /**< a document the user attached or DAWN read */
   OUTSIDE_MCP = 6,       /**< an MCP server's result */
   OUTSIDE_OTHER = 7,     /**< a tool result of any other tool */
   OUTSIDE_UNKNOWN = 8,   /**< stored before the kind was recorded */
} outside_kind_t;

/** The highest outside_kind_t value (a stored value above it is invalid). */
#define OUTSIDE_KIND_MAX OUTSIDE_UNKNOWN

/** The values that are not tainted, as a SQL list: `outside IN (...)`. */
#define OUTSIDE_UNTAINTED_SQL "0, 8"
#ifndef __cplusplus
_Static_assert(OUTSIDE_NONE == 0 && OUTSIDE_UNKNOWN == 8, "OUTSIDE_UNTAINTED_SQL names these");
#endif

/** Whether @p v marks outside text (any kind but NONE and UNKNOWN). */
static inline bool outside_is_tainted(int v) {
   return v != OUTSIDE_NONE && v != OUTSIDE_UNKNOWN;
}

/**
 * @brief @p a and @p b read together: the first tainted kind, else UNKNOWN
 *        when either is (what can't be judged stays so), else NONE
 */
static inline int outside_combine(int a, int b) {
   if (outside_is_tainted(a)) {
      return a;
   }
   if (outside_is_tainted(b)) {
      return b;
   }
   return (a == OUTSIDE_UNKNOWN || b == OUTSIDE_UNKNOWN) ? OUTSIDE_UNKNOWN : OUTSIDE_NONE;
}

/** Whether @p v is known to carry no outside text. */
static inline bool outside_is_clean(int v) {
   return v == OUTSIDE_NONE;
}

#ifdef __cplusplus
}
#endif

#endif /* OUTSIDE_KIND_H */
