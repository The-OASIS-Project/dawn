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
 * The mail panel's verbs: email_list, email_search, email_read,
 * email_set_flags, email_unread_counts (docs/WEBSOCKET_PROTOCOL.md, Email).
 * Each is checked on the lws thread and run on the email executor.
 */

#ifndef WEBUI_EMAIL_PANEL_H
#define WEBUI_EMAIL_PANEL_H

#include <stdbool.h>

#include "webui/webui_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void handle_email_list(ws_connection_t *conn, json_object *payload);
void handle_email_search(ws_connection_t *conn, json_object *payload);
void handle_email_read(ws_connection_t *conn, json_object *payload);
void handle_email_set_flags(ws_connection_t *conn, json_object *payload);
void handle_email_unread_counts(ws_connection_t *conn, json_object *payload);

/** The panel's verbs answer (the email tool is on and its service up): the email_client flag. */
bool webui_email_client_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_EMAIL_PANEL_H */
