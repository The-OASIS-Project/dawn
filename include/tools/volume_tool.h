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
 * Volume Tool - Control audio volume level
 */

#ifndef VOLUME_TOOL_H
#define VOLUME_TOOL_H

#include <stdbool.h>

/**
 * @brief Register the volume tool with the tool registry
 *
 * @return 0 on success, non-zero on failure
 */
int volume_tool_register(void);

/**
 * @brief Parse a volume level string to a float 0.0-1.0
 *
 * Handles numeric strings ("50", "0.5", "100") and word strings ("fifty").
 * Values > 2.0 are treated as percentages and divided by 100.
 * Result is clamped to 0.0-1.0 (no amplification beyond 100%).
 * Rejects NaN, Infinity, and non-finite values.
 *
 * @param value  Volume string to parse
 * @return float 0.0-1.0 on success, -1.0f on failure
 */
float parse_volume_level(const char *value);

struct session;

/**
 * @brief Run a volume action on a session's own client (a WebUI tab or a
 *        satellite), keeping its connection alive while it does
 *
 * Weak default (no WebUI): not handled.
 *
 * @param handled Out: false when the session has no live client, so the
 *        caller sets the local speaker's volume instead
 * @return The reply (allocated), when handled
 */
char *volume_tool_for_session(struct session *session,
                              const char *action,
                              const char *value,
                              int *should_respond,
                              bool *handled);

#endif /* VOLUME_TOOL_H */
