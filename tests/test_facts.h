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
 * Checks a rendered text by its facts (numbers, ids, paths, the verdict
 * word), so the wording around them can change without breaking a test.
 */

#ifndef TEST_FACTS_H
#define TEST_FACTS_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

/* The first of @p facts (NULL-ended) not in @p text; with @p ordered, each
 * must come after the one before it.  NULL when all are there. */
static inline const char *test_fact_missing(const char *text,
                                            const char *const *facts,
                                            bool ordered) {
   const char *from = text;
   for (; *facts != NULL; facts++) {
      const char *at = from ? strstr(from, *facts) : NULL;
      if (at == NULL)
         return *facts;
      if (ordered)
         from = at + strlen(*facts);
   }
   return NULL;
}

#define ASSERT_FACTS_(text, ordered, ...)                                                     \
   do {                                                                                       \
      const char *t_ = (text);                                                                \
      const char *m_ = test_fact_missing(t_, (const char *const[]){ __VA_ARGS__, NULL },      \
                                         (ordered));                                          \
      if (m_ != NULL) {                                                                       \
         static char msg_[4096];                                                              \
         snprintf(msg_, sizeof(msg_), "%s \"%s\" in: %s",                                     \
                  (ordered) ? "missing or out of order" : "missing", m_, t_ ? t_ : "(null)"); \
         TEST_FAIL_MESSAGE(msg_);                                                             \
      }                                                                                       \
   } while (0)

/** Each fact is somewhere in @p text. */
#define ASSERT_FACTS(text, ...) ASSERT_FACTS_(text, false, __VA_ARGS__)
/** Each fact is in @p text, in the order given (ties a fact to the one before it). */
#define ASSERT_FACTS_IN_ORDER(text, ...) ASSERT_FACTS_(text, true, __VA_ARGS__)

#endif /* TEST_FACTS_H */
