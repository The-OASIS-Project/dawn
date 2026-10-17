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

#include "core/user_tz.h"

#include "auth/auth_db.h"

tz_t *user_tz(int user_id) {
   auth_user_settings_t settings;
   tz_t *zone = NULL;
   if (user_id > 0 && auth_db_get_user_settings(user_id, &settings) == AUTH_DB_SUCCESS)
      zone = tz_lookup(settings.timezone);
   return zone ? zone : tz_daemon();
}
