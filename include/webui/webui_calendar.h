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
 * WebUI Calendar Account Management — per-user CalDAV account CRUD
 */

#ifndef WEBUI_CALENDAR_H
#define WEBUI_CALENDAR_H

#include "webui/webui_internal.h"

/** List calendar accounts for the current user */
void handle_calendar_list_accounts(ws_connection_t *conn);

/** Add a new CalDAV account for the current user */
void handle_calendar_add_account(ws_connection_t *conn, json_object *payload);

/** Edit a CalDAV account (name, URL, username, optionally password) */
void handle_calendar_edit_account(ws_connection_t *conn, json_object *payload);

/** Remove a CalDAV account (user must own it) */
void handle_calendar_remove_account(ws_connection_t *conn, json_object *payload);

/** Test connection to a CalDAV account */
void handle_calendar_test_account(ws_connection_t *conn, json_object *payload);

/** Trigger immediate sync for a CalDAV account */
void handle_calendar_sync_account(ws_connection_t *conn, json_object *payload);

/** List calendars for a CalDAV account */
void handle_calendar_list_calendars(ws_connection_t *conn, json_object *payload);

/** Toggle a calendar's active state */
void handle_calendar_toggle_calendar(ws_connection_t *conn, json_object *payload);

/** Toggle an account's read-only flag */
void handle_calendar_toggle_read_only(ws_connection_t *conn, json_object *payload);

/** Set an account's enabled flag */
void handle_calendar_set_enabled(ws_connection_t *conn, json_object *payload);

/**
 * @brief List the user's active calendars flat across accounts (the
 *        calendar_id -> {name,color} map a panel needs), in one call.
 * @param conn Authenticated WebSocket connection (self-scoped to its user).
 */
void handle_calendar_list_my_calendars(ws_connection_t *conn);

/**
 * @brief Read a window of upcoming occurrences (read-only pull for a panel).
 * @param conn    Authenticated WebSocket connection (self-scoped to its user).
 * @param payload {days} or explicit {start,end} epoch window + optional
 *                calendar_name filter.
 */
void handle_calendar_upcoming_events(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_CALENDAR_H */
