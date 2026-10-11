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
 * The entities every turn implies: the user's own and the assistant's.
 */

#ifndef MEMORY_IMPLIED_H
#define MEMORY_IMPLIED_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_db_entities.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The entities every turn implies, with all their names: the user's own
 * entity and the assistant's (named general.ai_name).  Naming one doesn't make
 * it what a turn is about ("Friday, what's 2+2?"); asking about it can't be
 * told apart by meaning either, since the name dominates a short turn's
 * embedding, so a named one is left out of the turn's context.
 */
typedef struct {
   int64_t ids[2 * MEMORY_ENTITY_CLASS_MAX];
   int count;
} memory_implied_entities_t;

/** @brief Look up the implied entities of @p user_id (none found: count 0). */
void memory_implied_entities_resolve(int user_id, memory_implied_entities_t *out);

/** @brief Whether @p entity_id is one of @p implied's (no database access). */
bool memory_implied_entities_has(const memory_implied_entities_t *implied, int64_t entity_id);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_IMPLIED_H */
