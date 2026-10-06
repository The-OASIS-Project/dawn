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
 * What the model is told when a preview can't be staged or a confirm finds no
 * item to carry out: the same words from every tool that stages for a confirm.
 */

#ifndef TOOL_PENDING_H
#define TOOL_PENDING_H

#include "core/pending_slots.h"
#include "core/turn_origin.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Why a preview of @p what ("call", "deletion") wasn't staged
 *
 * @return An error result (caller frees), or NULL when out of memory
 */
char *tool_pending_stage_refusal(pending_stage_rc_t rc, const char *what);

/** @brief A confirm of @p what that named no pending_id (caller frees). */
char *tool_pending_missing_id(const char *what);

/**
 * @brief Why a confirm of @p what found nothing to carry out
 *
 * @param rc  Any pending_slots_take() result but PENDING_FOUND
 * @param orc The turn check's result, for PENDING_NOT_NOW
 * @return An error result (caller frees), or NULL when out of memory
 */
char *tool_pending_take_refusal(pending_find_rc_t rc, turn_origin_rc_t orc, const char *what);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_PENDING_H */
