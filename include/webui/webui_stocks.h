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
 * WebUI stocks panel — WS verbs + a dedicated refresher thread that fans the
 * owner's Schwab portfolio snapshot to subscribed browsers on a cadence.
 */

#ifndef WEBUI_STOCKS_H
#define WEBUI_STOCKS_H

#include "webui/webui_internal.h"

/* WS handlers (owner-scoped; run on the lws service thread, do no blocking I/O). */
void handle_stocks_portfolio_subscribe(ws_connection_t *conn, struct json_object *payload);
void handle_stocks_portfolio_unsubscribe(ws_connection_t *conn, struct json_object *payload);
void handle_stocks_portfolio_get(ws_connection_t *conn, struct json_object *payload);

/* Lifecycle: start/stop the dedicated refresher thread (idempotent). */
void webui_stocks_start(void);
void webui_stocks_stop(void);

#endif /* WEBUI_STOCKS_H */
