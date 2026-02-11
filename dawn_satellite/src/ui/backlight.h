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
 * Sysfs backlight abstraction for Pi 7" touchscreen
 */

#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdbool.h>

/**
 * @brief Probe sysfs for a backlight device and read max_brightness
 * @return 0 on success (backlight found), 1 if no backlight available
 */
int backlight_init(void);

/**
 * @brief Get current brightness as a percentage (0-100)
 */
int backlight_get(void);

/**
 * @brief Set brightness percentage (clamped to 10-100 to prevent black screen).
 * Only writes to sysfs when the value actually changes.
 */
void backlight_set(int pct);

/**
 * @brief Check if a backlight device was found
 */
bool backlight_available(void);

/**
 * @brief Open the sysfs brightness fd for low-latency writes during drag.
 * Called when settings panel opens. No-op if already open or unavailable.
 */
void backlight_open(void);

/**
 * @brief Close the sysfs brightness fd.
 * Called when settings panel closes.
 */
void backlight_close(void);

#endif /* BACKLIGHT_H */
