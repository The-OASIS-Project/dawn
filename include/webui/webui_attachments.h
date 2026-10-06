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
 * The documents a text turn attaches, as the client sends them (a text
 * frame's `attachments`), built into the text the turn carries.
 */

#ifndef WEBUI_ATTACHMENTS_H
#define WEBUI_ATTACHMENTS_H

#include <json-c/json.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A malformed attachment, or a blob_id that names no original of the user's. */
#define WEBUI_ATTACHMENTS_INVALID 2

/** Longest attachment filename, in bytes. */
#define WEBUI_ATTACHMENT_FILENAME_MAX 255

/** The stored original an attachment names no longer exists (reclaimed or
 *  deleted): the document is kept, without its link to the original. */
#define WEBUI_ATTACHMENT_BLOB_GONE 3

/** Who may name a stored original as an attachment's blob_id: SUCCESS when
 *  @p blob_id is one @p user_id may read; WEBUI_ATTACHMENT_BLOB_GONE when it
 *  no longer exists; anything else refuses the turn (another user's).  The
 *  daemon's is the document original store's access check; tests pass their
 *  own. */
typedef int (*webui_attachment_owner_fn)(const char *blob_id, int user_id);

/**
 * @brief The text of a turn's attached documents
 *
 * Each entry of @p attachments is {filename, size, content, blob_id?}.  The
 * documents are third-party text: each body and filename has DAWN's markers
 * defused (llm_context_neutralize), and a body can't end itself early (a
 * document-marker line inside it is quoted).  The result is the inlined form
 * clients used to build themselves, one block per document separated by a
 * blank line:
 *
 *   [ATTACHED DOCUMENT: <filename> (<size> bytes)[ blob:<blob_id>]]
 *   <content>
 *   [END DOCUMENT]
 *
 * so the saved row, its reload, and the stored-original sweep read it as
 * before.
 *
 * @param attachments  The frame's `attachments` array.
 * @param user_id      The turn's user (owner of any blob_id).
 * @param max_docs     Most documents a turn may attach.
 * @param max_content  Longest content of one document, in bytes.
 * @param owner        Ownership check for a blob_id; NULL refuses any blob_id.
 * @param out          Set to the built text (caller frees); NULL on failure.
 * @return SUCCESS; WEBUI_ATTACHMENTS_INVALID when an entry is malformed, too
 *         many, too large, or names a blob the user can't read; FAILURE on
 *         allocation failure.
 */
int webui_attachments_build(struct json_object *attachments,
                            int user_id,
                            int max_docs,
                            size_t max_content,
                            webui_attachment_owner_fn owner,
                            char **out);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_ATTACHMENTS_H */
