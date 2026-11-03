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
 * What memory learned per consolidation batch, for the WebUI: the batch
 * object (docs/WEBSOCKET_PROTOCOL.md, Memory, Consolidation), the
 * list_memory_batches reply, and the replies to undo_memory_batch and
 * keep_memory_op.  The connection's own user only.
 */

#ifndef WEBUI_MEMORY_BATCHES_H
#define WEBUI_MEMORY_BATCHES_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_consolidation_undo.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Batches one list_memory_batches page returns when the request names none. */
#define WEBUI_MEMORY_BATCHES_LIMIT 20
/** The most one page returns, whatever the request asks. */
#define WEBUI_MEMORY_BATCHES_LIMIT_MAX 50
/** Bytes of batches one page holds before it stops (has_more: the rest on
 *  the next page); a page always holds at least one batch. */
#define WEBUI_MEMORY_BATCHES_PAGE_BYTES (256 * 1024)

/** The memory_batches flag: memory is enabled (a build without the WebUI has
 *  no flags). */
bool webui_memory_batches_enabled(void);

/** Batch @p batch_id of @p user_id as the batch object, or NULL (not the
 *  user's, gone, or the database). */
json_object *webui_memory_batch_json(int user_id, int64_t batch_id);

/** The list_memory_batches_response payload for @p request (its
 *  conversation_id, before_batch_id and limit). */
json_object *webui_memory_batches_list_payload(int user_id, json_object *request);

/** The undo_memory_batch_response or keep_memory_op_response payload for an
 *  action's outcome. */
json_object *webui_memory_batch_outcome_payload(const memory_consolidation_outcome_t *io);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MEMORY_BATCHES_H */
