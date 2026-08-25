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
 * WebUI Watches (SAGE proactive attention) — per-user attention_rules CRUD.
 * The visual counterpart to the conversational `attention` LLM tool; both drive
 * the same attention_watch_* core API.  See src/tools/attention_tool.c.
 *
 * Wire-name mapping: the client-facing "watch_*" WS message types (watch_list /
 * watch_add / watch_update / watch_set_enabled / watch_remove) are handled here;
 * the file is named "attention" to mirror the core it drives, the client module
 * is DawnWatches (www/js/ui/watches.js) to mirror the user-facing "Watches" tab.
 */

#ifndef WEBUI_ATTENTION_H
#define WEBUI_ATTENTION_H

#include "webui/webui_internal.h"

/** List the user's watches + the metric catalog + the master-switch state. */
void handle_watch_list(ws_connection_t *conn);

/** Create (or, per the one-watch-per-metric model, update) a watch. */
void handle_watch_add(ws_connection_t *conn, json_object *payload);

/** Update an existing watch's threshold / direction / notify level (by id). */
void handle_watch_update(ws_connection_t *conn, json_object *payload);

/** Enable or disable a watch (by id). */
void handle_watch_set_enabled(ws_connection_t *conn, json_object *payload);

/** Delete a watch (by id). */
void handle_watch_remove(ws_connection_t *conn, json_object *payload);

/** Opt this connection in/out of the 1 Hz live watch_readings gauge stream. */
void handle_watch_readings_subscribe(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_ATTENTION_H */
