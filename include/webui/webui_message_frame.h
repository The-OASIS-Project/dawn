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
 * The message_appended frame: its JSON, the origin connection's copy, and
 * which session a persisted row names.  Pure (json-c only), so it can be
 * tested without a WebSocket server.
 */

#ifndef WEBUI_MESSAGE_FRAME_H
#define WEBUI_MESSAGE_FRAME_H

#include <json-c/json.h>
#include <stdint.h>

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The session a row written by @p session's turn names, or NULL
 *
 * A job session belongs to no browser (its stream reaches every viewer of
 * the job), so its rows name none.
 */
const session_t *webui_message_origin(const session_t *session);

/**
 * @brief Build a message_appended frame
 * @param role      NULL is sent as "assistant"
 * @param text      The row's text; must not be NULL
 * @param reasoning Sent only when non-empty
 * @param origin    Sent as payload.session_id when non-NULL
 * @param email_ref A user row's email_ref (webui_row_add_email_ref), or NULL
 * @return The frame ({type, payload}); the caller owns it
 */
json_object *webui_message_appended_frame(int64_t conv_id,
                                          int64_t msg_id,
                                          const char *role,
                                          const char *text,
                                          const char *reasoning,
                                          unsigned stream_id,
                                          const session_t *origin,
                                          const char *email_ref);

/**
 * @brief The origin connection's copy of @p frame, naming its turn
 *
 * @p frame is left as it was.
 * @return The serialized copy with payload.client_ref (caller frees), or NULL
 *         when there is no origin, no client_ref (NULL or ""), or no memory:
 *         every connection then gets @p frame as is
 */
char *webui_message_appended_origin_json(json_object *frame,
                                         const session_t *origin,
                                         const char *client_ref);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MESSAGE_FRAME_H */
