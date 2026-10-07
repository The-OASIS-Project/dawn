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
 * WebUI Email Account Management — per-user IMAP/SMTP account CRUD
 */

#ifndef WEBUI_EMAIL_H
#define WEBUI_EMAIL_H

#include "webui/webui_internal.h"

/** List email accounts for the current user */
void handle_email_list_accounts(ws_connection_t *conn, json_object *payload);

/** Add a new email account */
void handle_email_add_account(ws_connection_t *conn, json_object *payload);

/** Update an email account */
void handle_email_update_account(ws_connection_t *conn, json_object *payload);

/** Remove an email account */
void handle_email_remove_account(ws_connection_t *conn, json_object *payload);

/** Test IMAP+SMTP connection */
void handle_email_test_connection(ws_connection_t *conn, json_object *payload);

/** Set account read-only flag */
void handle_email_set_read_only(ws_connection_t *conn, json_object *payload);

/** Set account enabled flag */
void handle_email_set_enabled(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_EMAIL_H */
