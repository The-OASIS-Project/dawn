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
 * Shared RAG document indexing pipeline — chunk, embed, store
 *
 * Used by both the WebUI Document Library and the document_index LLM tool.
 * Thread-safe: relies on auth_db mutex and embedding engine mutex internally.
 */

#ifndef DOCUMENT_INDEX_PIPELINE_H
#define DOCUMENT_INDEX_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define DOC_INDEX_SUCCESS 0
#define DOC_INDEX_ERROR_EMPTY 1
#define DOC_INDEX_ERROR_TOO_LARGE 2
#define DOC_INDEX_ERROR_LIMIT 3
#define DOC_INDEX_ERROR_NO_EMBEDDING 4
#define DOC_INDEX_ERROR_DUPLICATE 5
#define DOC_INDEX_ERROR_CHUNK_FAIL 6
#define DOC_INDEX_ERROR_DB_FAIL 7
#define DOC_INDEX_ERROR_ALLOC 8

/**
 * @brief Result of document indexing
 */
typedef struct {
   int64_t doc_id;      /* Document ID on success, -1 on failure */
   int num_chunks;      /* Number of successfully embedded chunks */
   int failed_chunks;   /* Number of chunks that failed to embed */
   int error_code;      /* DOC_INDEX_SUCCESS or error code */
   char error_msg[128]; /* Human-readable error message */
} doc_index_result_t;

/**
 * @brief Index document text into the RAG database
 *
 * Validates limits, computes SHA-256 hash for dedup, chunks the text,
 * embeds each chunk, and stores in the document database.
 *
 * @param user_id Owner user ID
 * @param filename Display filename for the document
 * @param filetype File extension (e.g., ".pdf")
 * @param text Extracted text to index
 * @param text_len Length of text in bytes
 * @param is_global Whether document should be visible to all users
 * @param original_blob_id Stored original-file blob id (NULL if none, e.g. notes
 *                         and the LLM tool path)
 * @param out Result struct (always populated). On DOC_INDEX_ERROR_DUPLICATE,
 *            out->doc_id carries the EXISTING document's id (not -1) so a caller
 *            can adopt the already-stored copy; on every other error it is -1.
 * @return DOC_INDEX_SUCCESS or error code
 */
int document_index_text(int user_id,
                        const char *filename,
                        const char *filetype,
                        const char *text,
                        size_t text_len,
                        bool is_global,
                        const char *original_blob_id,
                        doc_index_result_t *out);

/**
 * @brief Get human-readable error message for an indexing error code
 *
 * @param error_code Error code from document_index_text
 * @return Static string describing the error
 */
const char *document_index_error_string(int error_code);

/**
 * @brief Save a short authored note — a single-chunk document, filename == label (v61).
 *
 * Bypasses the chunker and rejects anything that would split (M-1), so the note
 * invariant num_chunks == 1 holds by construction on both the WebUI and tool
 * paths.  filetype is set to "note".  No hash-dedup (same body under different
 * labels is allowed).
 *
 * @param user_id   Owner
 * @param label     Human label (becomes filename); required
 * @param text      Note body; required
 * @param text_len  Byte length of text
 * @param is_global Whether visible to all users (WebUI admin only)
 * @param[out] out  Result (doc_id, error_code, error_msg)
 * @return DOC_INDEX_SUCCESS or a DOC_INDEX_ERROR_* code (also set in out->error_code)
 */
int document_index_note(int user_id,
                        const char *label,
                        const char *text,
                        size_t text_len,
                        bool is_global,
                        doc_index_result_t *out);

/**
 * @brief Edit an existing note in place — re-embed + stable-id DB swap (v61).
 *
 * Re-embeds new_text and delegates to document_db_note_update, which enforces the
 * filetype=="note" && num_chunks==1 && owner gate.  Same single-chunk cap as
 * document_index_note.
 *
 * @param user_id   Owner (gate)
 * @param doc_id    Note document id
 * @param new_label New label (== new filename)
 * @param new_text  New note body
 * @param new_len   Byte length of new_text
 * @param[out] out  Result
 * @return DOC_INDEX_SUCCESS or a DOC_INDEX_ERROR_* code
 */
int document_note_update(int user_id,
                         int64_t doc_id,
                         const char *new_label,
                         const char *new_text,
                         size_t new_len,
                         doc_index_result_t *out);

/**
 * @brief Replace a MULTI-chunk document's content in place (v63, B1b).
 *
 * Re-chunks + re-embeds new_text and atomically swaps all chunks, keeping doc_id
 * stable.  Requires the document to have stored full text (pre-v63 uploads are
 * rejected with a "re-save to enable editing" error).  Archives the old content
 * as a restorable version.  Single-chunk notes use document_note_update instead.
 *
 * @return DOC_INDEX_SUCCESS or a DOC_INDEX_ERROR_* code.
 */
int document_doc_update(int user_id,
                        int64_t doc_id,
                        const char *new_text,
                        size_t new_len,
                        doc_index_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_INDEX_PIPELINE_H */
