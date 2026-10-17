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
 * The zone a user's clock times are read and written in.
 */

#ifndef USER_TZ_H
#define USER_TZ_H

#include "core/tz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The zone @p user_id's times are in: their own setting when it names
 *        a zone, else the daemon's (also for a guest, user_id <= 0).  Never
 *        NULL.
 *
 * Reads the user's settings (the auth database lock): resolve it once per
 * turn, tool call or fired event and pass the zone down, and never call it
 * while holding that lock.
 */
tz_t *user_tz(int user_id);

#ifdef __cplusplus
}
#endif

#endif /* USER_TZ_H */
