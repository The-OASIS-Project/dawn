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
 * The music tool's actions over a music player.
 */

#ifndef MUSIC_PLAYER_TOOL_H
#define MUSIC_PLAYER_TOOL_H

#include <stdbool.h>

#include "audio/music_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run a music tool action on a player
 *
 * Takes the player's control lock (a direct command preempts a model call
 * waiting for its track to open; a model call waits for other controls).  Replies are
 * written for the model, with a "[queue: ...]" state footer; one that changed
 * nothing is marked with TOOL_RESULT_ERROR_MARK.
 *
 * @param query The packed tool value (base query plus ::items:: etc.)
 * @param for_model The model asked (it may wait up to 15 s for a slow open
 *        and reads the footer); false for a direct command, which mustn't
 *        wait long (the voice loop runs it)
 * @param result_out Receives an allocated reply (caller frees)
 * @return SUCCESS or FAILURE (search and the library aren't player actions)
 */
int music_player_tool_execute(music_player_t *player,
                              const char *action,
                              const char *query,
                              bool for_model,
                              char **result_out);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_TOOL_H */
