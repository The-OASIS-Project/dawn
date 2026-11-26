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
 * Which WebUI messages are the user's own use of a login: each one moves the
 * login's expiry on (conn_login_used).
 */

#ifndef WEBUI_USER_ACTION_H
#define WEBUI_USER_ACTION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a WebSocket message type is only ever sent by a user's action
 *
 * True for a click, typing or a control (a turn, a music control, a mail
 * action, a settings save); false for anything a client sends by itself:
 * init/reconnect, ping, the requests made on every connect, timer polls and
 * refreshes after a server push, which would keep an unattended tab signed in.
 *
 * @param type The message's type (NULL: false)
 * @return true if the message counts as use
 */
bool webui_is_user_action(const char *type);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_USER_ACTION_H */
