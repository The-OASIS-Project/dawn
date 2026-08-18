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
 * WebUI Document Library Handlers
 *
 * WebSocket message handlers for the Document Library panel (RAG).
 * Handles list, delete, and index operations.
 */

#ifndef WEBUI_DOC_LIBRARY_H
#define WEBUI_DOC_LIBRARY_H

#include "webui/webui_internal.h"

/**
 * @brief List documents accessible to the current user
 * Response type: doc_library_list_response
 */
void handle_doc_library_list(ws_connection_t *conn, json_object *payload);

/**
 * @brief Fetch a document's reassembled full text for reading (owner-scoped, v63+).
 * Payload: { "id": <document_id> }
 * Response type: doc_library_get_response
 * { success, id, filename, filetype, text } on success; success:false otherwise.
 */
void handle_doc_library_get(ws_connection_t *conn, json_object *payload);

/**
 * @brief Delete a document (and all its chunks)
 * Payload: { "id": <document_id> }
 * Response type: doc_library_delete_response
 */
void handle_doc_library_delete(ws_connection_t *conn, json_object *payload);

/**
 * @brief Index a document: chunk text, embed, store in DB
 * Payload: { "filename": "...", "filetype": "...", "text": "...", "is_global": false }
 * Response type: doc_library_index_response
 */
void handle_doc_library_index(ws_connection_t *conn, json_object *payload);

/**
 * @brief Toggle a document's global visibility flag
 * Payload: { "id": <document_id>, "is_global": true/false }
 * Response type: doc_library_toggle_global_response
 * Requires: document owner or admin
 */
void handle_doc_library_toggle_global(ws_connection_t *conn, json_object *payload);

/**
 * @brief Save a new note (single-chunk document, filename == label) (v61).
 * Payload: { "label": "...", "text": "..." }
 * Response type: doc_library_note_save_response
 * Rejects a duplicate label (edits go through note_update).
 */
void handle_doc_library_note_save(ws_connection_t *conn, json_object *payload);

/**
 * @brief Edit an existing note in place — stable id (v61).
 * Payload: { "id": <doc_id>, "label": "...", "text": "..." }
 * Response type: doc_library_note_update_response
 */
void handle_doc_library_note_update(ws_connection_t *conn, json_object *payload);

/**
 * @brief List a document/note's archived versions, newest first (v62).
 * Payload: { "id": <doc_id> }.  Response type: doc_library_version_list_response
 */
void handle_doc_library_version_list(ws_connection_t *conn, json_object *payload);

/**
 * @brief List recently-deleted items for undo (v62).  No payload.
 * Response type: doc_library_deleted_list_response
 */
void handle_doc_library_deleted_list(ws_connection_t *conn, json_object *payload);

/**
 * @brief Restore a note to an archived version (v62) — in place if it still
 * exists (archiving the current content first), or re-created if it was deleted.
 * Payload: { "version_id": <id> }.  Response: doc_library_version_restore_response
 */
void handle_doc_library_version_restore(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_DOC_LIBRARY_H */
