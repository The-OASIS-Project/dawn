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
 * Logging Bridge - Connects dawn_common logging to daemon logging
 *
 * This module provides the bridge between the common library's callback-based
 * logging and the daemon's logging infrastructure. Call logging_bridge_init()
 * early in main() to enable logging from common library code.
 */

#ifndef LOGGING_BRIDGE_H
#define LOGGING_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the logging bridge
 *
 * Registers the daemon's logging callback with the common library.
 * Must be called early in main() before any common library functions
 * are used.
 *
 * Thread Safety: This function is NOT thread-safe. Call it once at
 * initialization before starting any other threads.
 */
void logging_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LOGGING_BRIDGE_H */
