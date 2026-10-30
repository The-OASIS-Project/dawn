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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Music Tool - Audio playback control with playlist management
 */

#ifndef MUSIC_TOOL_H
#define MUSIC_TOOL_H

#include <stdbool.h>

#include "core/session_manager.h"

typedef struct music_player music_player_t;

/**
 * @brief Register the music tool with the tool registry
 *
 * Call this during startup to register the music playback tool.
 * The tool handles: play, stop, next, previous actions.
 *
 * @return 0 on success, non-zero on failure
 */
int music_tool_register(void);

/**
 * @brief The player a session has of its own (a WebUI tab, a Tier-1
 *        satellite), referenced: release it with music_player_release()
 *
 * Weak default (no WebUI) returns NULL; the WebUI overrides it, keeping the
 * session's connection alive while it takes the reference.
 *
 * @return The player, or NULL when the session has none
 */
music_player_t *music_player_for_session(struct session *session);

/**
 * @brief Whether a caller without a player of its own may play on the local
 *        speaker
 *
 * Only its owner (the local device's user), and only from the local mic, a
 * messaging channel, or no session (the scheduler, a briefing, MQTT).  A
 * WebUI tab, a satellite (which has a speaker of its own) and a background
 * job never play at home.
 *
 * @param session The caller's session, or NULL
 * @param caller_user_id The user the call acts for (tool_get_current_user_id())
 * @param owner_user_id The speaker's owner, 0 when unknown
 */
static inline bool music_tool_speaker_allowed(const session_t *session,
                                              int caller_user_id,
                                              int owner_user_id) {
   if (owner_user_id <= 0 || caller_user_id != owner_user_id) {
      return false;
   }
   return !session || session->type == SESSION_TYPE_LOCAL ||
          session->type == SESSION_TYPE_MESSAGING;
}

/**
 * @brief Set custom music directory path
 *
 * Sets an override directory for music file searches. If not set,
 * uses the path from dawn.toml [paths].music_dir.
 *
 * This is typically called from command-line argument processing.
 *
 * @param path The directory path (will be copied internally)
 */
void set_music_directory(const char *path);

#endif /* MUSIC_TOOL_H */
