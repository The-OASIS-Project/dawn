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
 * WebUI Code Projects handlers (coding harness).
 *
 * WebSocket message handlers for the Coding popover: list visible projects,
 * import a repo, refresh (re-index), and delete. Live status changes are pushed
 * via the code_project_status_changed broadcast (webui_broadcasts.c).
 */

#ifndef WEBUI_CODE_PROJECTS_H
#define WEBUI_CODE_PROJECTS_H

#include "webui/webui_internal.h"

/** @brief List code projects visible to the current user.
 *  Response type: code_projects_list_response */
void handle_code_projects_list(ws_connection_t *conn, json_object *payload);

/** @brief Import a repo. Payload: { url, name?, global? }.
 *  Response type: code_projects_import_response */
void handle_code_projects_import(ws_connection_t *conn, json_object *payload);

/** @brief Queue a re-index of a project. Payload: { name }.
 *  Response type: code_projects_action_response */
void handle_code_projects_refresh(ws_connection_t *conn, json_object *payload);

/** @brief Delete a project (owner or admin). Payload: { name }.
 *  Response type: code_projects_action_response */
void handle_code_projects_delete(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_CODE_PROJECTS_H */
