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
 * @brief Pause the local speaker's music, keeping its place for a resume
 *
 * For an interruption such as a phone call: nothing resumes it but the user.
 * Without the music tool there's no local music, so nothing to pause.
 *
 * @return true if music was playing and is now paused
 */
#ifdef DAWN_ENABLE_MUSIC_TOOL
bool music_tool_pause(void);
#else
static inline bool music_tool_pause(void) {
   return false;
}
#endif

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
