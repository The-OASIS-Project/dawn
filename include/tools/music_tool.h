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
 * @brief Auto-advance to next track after natural playback completion
 *
 * Called by the playback thread (playFlacAudio) when a track finishes
 * naturally (not stopped by user). Advances to the next track in the
 * playlist, wrapping around at the end.
 *
 * Thread-safe: called from the playback thread context.
 */
void music_tool_auto_advance(void);

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
