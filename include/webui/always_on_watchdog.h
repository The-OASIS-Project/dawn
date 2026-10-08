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
 * When always-on gives up waiting for a turn's answer (webui_always_on.c).
 */

#ifndef ALWAYS_ON_WATCHDOG_H
#define ALWAYS_ON_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nothing for this long while no turn is running: the turn never started, or
 * its end was missed. */
#define ALWAYS_ON_PROCESSING_TIMEOUT_MS 30000

/* However busy the turn: one wedged in a tool must not leave always-on deaf
 * for good. */
#define ALWAYS_ON_PROCESSING_MAX_MS 300000

/**
 * @brief Whether always-on stops waiting for a turn's answer.
 * @param now Now, in ms
 * @param last_progress When the turn last showed progress (PROCESSING began, a
 *        sentence was spoken, or it was seen running), in ms
 * @param since When PROCESSING began, in ms
 * @param turn_running Whether a turn is running on the session now
 */
static inline bool always_on_processing_expired(int64_t now,
                                                int64_t last_progress,
                                                int64_t since,
                                                bool turn_running) {
   if (now - since >= ALWAYS_ON_PROCESSING_MAX_MS)
      return true;
   if (turn_running)
      return false;
   return now - last_progress >= ALWAYS_ON_PROCESSING_TIMEOUT_MS;
}

#ifdef __cplusplus
}
#endif

#endif /* ALWAYS_ON_WATCHDOG_H */
