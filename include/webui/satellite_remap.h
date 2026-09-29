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
 * Satellite user mapping: whose speech a satellite session's history holds,
 * and moving it to another user without mixing two users' speech.
 */

#ifndef SATELLITE_REMAP_H
#define SATELLITE_REMAP_H

#include <stdbool.h>

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether mapping @p session to @p new_user makes its speech someone else's
 *
 * A satellite's speech is its mapped user's; an unmapped one is a guest's (0,
 * see session_effective_user_id()), so any change of mapped user is a change.
 */
bool satellite_owner_changes(session_t *session, int new_user);

/**
 * @brief Apply a mapping to the session now: its user
 *
 * The user's prompt and the satellite's room reach the model with the next
 * turn (its prompt is built for the session's user; the room is a standing
 * direction).  For a mapping that doesn't change whose speech the history is.
 */
void satellite_apply_mapping(session_t *session, int user_id);

/**
 * @brief Move the session to another user, behind any query in progress
 *
 * Queued on the session's turn queue: the running query finishes as the user it
 * began as.  Then the previous user's conversation is saved (a new context
 * starts), or, with nothing to save, a new context starts; then the mapping is
 * applied.  Off the WebSocket service thread.  If it can't be queued, the
 * history is discarded instead of carried over.
 */
void satellite_queue_remap(session_t *session, int user_id);

#ifdef __cplusplus
}
#endif

#endif /* SATELLITE_REMAP_H */
