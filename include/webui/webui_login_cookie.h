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
 * One login cookie per app, so front-ends open in the same browser (the WebUI
 * and Aurora) keep separate logins: a browser sends every cookie of the host
 * to every port and path, so the app a request is from names its cookie.
 * The WebUI's is "__Host-dawn_session"; app "aurora" has
 * "__Host-dawn_session_aurora".
 * Pure string handling.
 */

#ifndef WEBUI_LOGIN_COOKIE_H
#define WEBUI_LOGIN_COOKIE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The WebUI's login cookie; an app's is this, '_', and the app's name.  The
 * __Host- prefix makes the browser refuse one not set by this host over a
 * secure connection with Path=/ and no Domain, so a cookie set by a sibling
 * subdomain or for a narrower path can't shadow the login. */
#define WEBUI_LOGIN_COOKIE_BASE "__Host-dawn_session"
/* The WebUI's own app name (the same as naming none). */
#define WEBUI_APP_DEFAULT "webui"
/* An app name: 1-16 of [a-z0-9_]. */
#define WEBUI_APP_NAME_MAX 16
/* Room for any login cookie's name, with its NUL. */
#define WEBUI_LOGIN_COOKIE_NAME_MAX (sizeof(WEBUI_LOGIN_COOKIE_BASE) + 1 + WEBUI_APP_NAME_MAX)

/**
 * @brief The login cookie's name for @p app: NULL, "" or "webui" is the
 *        WebUI's; a valid app name its own.
 * @return false for an invalid app name (the request then has no login).
 */
bool webui_login_cookie_name(const char *app, char *out, size_t out_size);

/**
 * @brief The value of cookie @p name in a Cookie header (exact name match,
 *        "name=value" pairs separated by ';' and optional spaces).
 * @return false if it is absent, empty, or too long for @p out.
 */
bool webui_cookie_value(const char *header, const char *name, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_LOGIN_COOKIE_H */
