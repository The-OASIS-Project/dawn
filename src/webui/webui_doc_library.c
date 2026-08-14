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
 * WebSocket handlers for the Document Library panel. Provides list, delete,
 * and index (chunk + embed + store) operations for RAG document search.
 */

#include "webui/webui_doc_library.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "blob_store.h" /* BLOB_ID_LEN */
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_note_bridge.h"
#include "tools/document_db.h"
#include "tools/document_index_pipeline.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Helper: quiet admin check (no error response sent)
 * ============================================================================= */

static bool conn_check_admin_quiet(ws_connection_t *conn) {
   if (!conn->authenticated)
      return false;
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS)
      return false;
   return session.is_admin;
}

/* =============================================================================
 * List Documents
 * ============================================================================= */

#define DOC_LIBRARY_DEFAULT_LIMIT 20
#define DOC_LIBRARY_MAX_LIMIT 100
/* Over-fetch ratio for the BM25 candidate pool before dedup-to-document. */
#define DOC_LIBRARY_BM25_CAND_FACTOR 4

void handle_doc_library_list(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_list_response"));

   json_object *resp_payload = json_object_new_object();

   /* Parse pagination + v61 scope/query params */
   int limit = DOC_LIBRARY_DEFAULT_LIMIT;
   int offset = 0;
   bool show_all = false;
   doc_kind_t kind = DOC_KIND_ALL;
   const char *query = NULL;
   if (payload) {
      json_object *limit_obj, *offset_obj, *show_all_obj, *scope_obj, *query_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > DOC_LIBRARY_MAX_LIMIT)
            limit = DOC_LIBRARY_DEFAULT_LIMIT;
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0)
            offset = 0;
      }
      if (json_object_object_get_ex(payload, "show_all", &show_all_obj))
         show_all = json_object_get_boolean(show_all_obj);
      if (json_object_object_get_ex(payload, "scope", &scope_obj)) {
         const char *s = json_object_get_string(scope_obj);
         if (s && strcmp(s, "documents") == 0)
            kind = DOC_KIND_DOCS;
         else if (s && strcmp(s, "notes") == 0)
            kind = DOC_KIND_NOTES;
      }
      if (json_object_object_get_ex(payload, "query", &query_obj)) {
         const char *q = json_object_get_string(query_obj);
         if (q && q[0])
            query = q;
      }
   }

   /* show_all requires admin — silently fall back to user-only if not admin */
   if (show_all && !conn_check_admin_quiet(conn))
      show_all = false;

   /* The lexical search path is owner-scoped (own + global) and BM25 hits carry
    * no owner identity, so admin cross-user search isn't supported yet — force
    * show_all off on a query so the response never emits bogus owner fields. */
   if (query)
      show_all = false;

   if (limit > DOC_LIBRARY_DEFAULT_LIMIT)
      limit = DOC_LIBRARY_DEFAULT_LIMIT;

   document_t docs[DOC_LIBRARY_DEFAULT_LIMIT]; /* Stack-friendly: sized to typical page */
   int count = 0;
   bool ok = true;
   bool has_more = false;

   /* v61 search path: label/body BM25 (lexical) over the user's docs, deduped to
    * one row per document, then scope-filtered.  Single page (no offset). */
   if (query) {
      int max_hits = DOC_LIBRARY_DEFAULT_LIMIT * DOC_LIBRARY_BM25_CAND_FACTOR;
      doc_bm25_hit_t *hits = calloc((size_t)max_hits, sizeof(doc_bm25_hit_t));
      float *scores = calloc((size_t)max_hits, sizeof(float));
      int hit_count = 0;
      if (hits && scores &&
          document_db_chunk_search_bm25(conn->auth_user_id, query,
                                        g_config.documents.fts_label_weight,
                                        g_config.documents.fts_body_weight, hits, scores, max_hits,
                                        &hit_count) == SUCCESS) {
         for (int i = 0; i < hit_count && count < limit; i++) {
            bool is_note = (strcmp(hits[i].filetype, "note") == 0);
            if ((kind == DOC_KIND_DOCS && is_note) || (kind == DOC_KIND_NOTES && !is_note))
               continue;
            bool dup = false; /* dedup to one row per document */
            for (int j = 0; j < count; j++)
               if (docs[j].id == hits[i].document_id) {
                  dup = true;
                  break;
               }
            if (dup)
               continue;
            /* Pull authoritative metadata (is_global, created_at, owner scoping)
             * from the document row — the BM25 hit carries only search-facing
             * fields, so building the row from it would misreport is_global and
             * take created_at from the chunk rather than the document. */
            if (document_db_get(hits[i].document_id, &docs[count]) != SUCCESS)
               continue;
            count++;
         }
      } else {
         ok = false;
      }
      free(hits);
      free(scores);
   } else {
      int list_rc = show_all ? document_db_list_all(docs, limit, offset, &count)
                             : document_db_list_filtered(conn->auth_user_id, kind, docs, limit,
                                                         offset, &count);
      ok = (list_rc == SUCCESS);
      has_more = (count == limit);
   }

   if (!ok) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list documents"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      json_object *docs_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *doc = json_object_new_object();
         bool is_note = (strcmp(docs[i].filetype, "note") == 0);
         json_object_object_add(doc, "id", json_object_new_int64(docs[i].id));
         json_object_object_add(doc, "filename", json_object_new_string(docs[i].filename));
         json_object_object_add(doc, "filetype", json_object_new_string(docs[i].filetype));
         json_object_object_add(doc, "is_note", json_object_new_boolean(is_note));
         /* Notes are single-chunk + small: include the body so the client can
          * preview it and pre-fill the editor without a second round-trip. */
         if (is_note) {
            document_chunk_t chunk;
            int cc = 0;
            if (document_db_chunk_read(docs[i].id, &chunk, 1, 0, &cc) == SUCCESS && cc > 0)
               json_object_object_add(doc, "text", json_object_new_string(chunk.text));
         }
         json_object_object_add(doc, "num_chunks", json_object_new_int(docs[i].num_chunks));
         json_object_object_add(doc, "is_global", json_object_new_boolean(docs[i].is_global));
         json_object_object_add(doc, "created_at", json_object_new_int64(docs[i].created_at));
         /* Documents (not notes) may have a stored original file: surface its blob id +
          * a has_original flag so a client can fetch/download the original. Notes carry
          * their body inline (above) and have no separate original file. Emit ONLY for
          * docs the requester owns: the download path (doc_can_read) is owner-only, so
          * advertising has_original on a listed GLOBAL doc would offer a download the
          * server always refuses. Gating here also skips the accessor query for those. */
         if (!is_note && docs[i].user_id == conn->auth_user_id) {
            char blob_id[BLOB_ID_LEN] = { 0 };
            bool has_original = document_db_get_original_blob_id(docs[i].id, blob_id,
                                                                 sizeof(blob_id)) == SUCCESS &&
                                blob_id[0];
            json_object_object_add(doc, "has_original", json_object_new_boolean(has_original));
            if (has_original)
               json_object_object_add(doc, "original_blob_id", json_object_new_string(blob_id));
         }
         if (show_all) {
            json_object_object_add(doc, "user_id", json_object_new_int(docs[i].user_id));
            json_object_object_add(doc, "owner_name", json_object_new_string(docs[i].owner_name));
         }
         json_object_array_add(docs_array, doc);
      }
      json_object_object_add(resp_payload, "documents", docs_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(has_more));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Get Document Full Text (the reassembled body of a multi-chunk document)
 * ============================================================================= */

void handle_doc_library_get(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_get_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing document id"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);
      /* Echo the id on every branch (incl. errors) so a client can correlate the async
       * response - "unavailable" is a common, expected outcome for global/legacy docs. */
      json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
      /* Short-circuit: metadata read (unscoped, for filename/filetype) THEN the
       * owner-scoped full-text read. A missing id and a not-owned/global/pre-v63 doc
       * both fall to ONE generic error, so this verb is not a document-existence oracle
       * (full_text_get already gates content by owner). */
      document_t doc;
      char *text = NULL;
      bool ok = document_db_get(doc_id, &doc) == SUCCESS &&
                document_db_full_text_get(doc_id, conn->auth_user_id, &text) == SUCCESS && text;
      if (!ok) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document unavailable"));
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "filename", json_object_new_string(doc.filename));
         json_object_object_add(resp_payload, "filetype", json_object_new_string(doc.filetype));
         json_object_object_add(resp_payload, "text", json_object_new_string(text));
      }
      free(text); /* NULL-safe on every error path */
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete Document
 * ============================================================================= */

void handle_doc_library_delete(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_delete_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing document id"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);

      /* Verify ownership: get the doc first */
      document_t doc;
      if (document_db_get(doc_id, &doc) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document not found"));
      } else if (doc.user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Permission denied"));
      } else {
         /* Drop the memory→note bridge gloss (v61) BEFORE the row is deleted (the
          * FK nulls note_doc_id on delete).  Owner-scoped to the note's owner so
          * an admin deleting another user's note removes that user's gloss.
          * Best-effort + harmless for non-note docs (no gloss exists). */
         (void)memory_note_bridge_delete_gloss(doc.user_id, doc_id);
         /* v61: delete_indexed also removes the contentless FTS rows (no orphan
          * postings); applies to notes AND uploaded docs, all now FTS-indexed. */
         int rc = document_db_delete_indexed(doc_id);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(rc == SUCCESS));
         if (rc != 0) {
            json_object_object_add(resp_payload, "error", json_object_new_string("Delete failed"));
         } else {
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            if (doc.user_id != conn->auth_user_id) {
               OLOG_INFO("doc_library: admin user %d deleted document %lld (%s) owned by user %d",
                         conn->auth_user_id, (long long)doc_id, doc.filename, doc.user_id);
            } else {
               OLOG_INFO("doc_library: deleted document %lld (%s)", (long long)doc_id,
                         doc.filename);
            }
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Index Document (delegates to shared pipeline)
 * ============================================================================= */

void handle_doc_library_index(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_index_response"));
   json_object *resp_payload = json_object_new_object();

   /* Extract fields */
   json_object *filename_obj, *filetype_obj, *text_obj, *global_obj;
   if (!json_object_object_get_ex(payload, "filename", &filename_obj) ||
       !json_object_object_get_ex(payload, "filetype", &filetype_obj) ||
       !json_object_object_get_ex(payload, "text", &text_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing required fields"));
      goto done;
   }

   const char *filename = json_object_get_string(filename_obj);
   const char *filetype = json_object_get_string(filetype_obj);
   const char *text = json_object_get_string(text_obj);
   bool is_global = json_object_object_get_ex(payload, "is_global", &global_obj) &&
                    json_object_get_boolean(global_obj);

   /* Optional original-file blob id, echoed back by the browser from the upload
    * response so the indexed document links to its stored source file (v68). */
   json_object *blob_obj = NULL;
   const char *original_blob_id = NULL;
   if (json_object_object_get_ex(payload, "original_blob_id", &blob_obj)) {
      original_blob_id = json_object_get_string(blob_obj);
   }

   /* Only admins can mark documents as global */
   if (is_global && !conn_check_admin_quiet(conn))
      is_global = false;

   {
      size_t text_len = text ? strlen(text) : 0;
      doc_index_result_t result;
      int rc = document_index_text(conn->auth_user_id, filename, filetype, text, text_len,
                                   is_global, original_blob_id, &result);

      if (rc != DOC_INDEX_SUCCESS) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string(result.error_msg[0]
                                                           ? result.error_msg
                                                           : document_index_error_string(rc)));
         if (rc == DOC_INDEX_ERROR_DUPLICATE && result.doc_id > 0) {
            /* Legacy: WebUI checks for existing_id on duplicate */
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "id", json_object_new_int64(result.doc_id));
         json_object_object_add(resp_payload, "filename", json_object_new_string(filename));
         json_object_object_add(resp_payload, "num_chunks", json_object_new_int(result.num_chunks));
         if (result.failed_chunks > 0) {
            json_object_object_add(resp_payload, "failed_chunks",
                                   json_object_new_int(result.failed_chunks));
         }
      }
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Toggle Global Visibility
 * ============================================================================= */

void handle_doc_library_toggle_global(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_toggle_global_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *global_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_global", &global_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing id or is_global"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);
      bool new_global = json_object_get_boolean(global_obj);

      document_t doc;
      if (document_db_get(doc_id, &doc) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document not found"));
      } else if (doc.user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Permission denied"));
      } else {
         int rc = document_db_update_global(doc_id, new_global);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(rc == SUCCESS));
         if (rc != 0) {
            json_object_object_add(resp_payload, "error", json_object_new_string("Update failed"));
         } else {
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            json_object_object_add(resp_payload, "is_global", json_object_new_boolean(new_global));
            OLOG_INFO("doc_library: user %d toggled document %lld (%s) global=%s",
                      conn->auth_user_id, (long long)doc_id, doc.filename,
                      new_global ? "true" : "false");
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Notes — save (create) and update (edit) (v61)
 *
 * A note is a single-chunk document whose filename IS the label.  Deletion goes
 * through the existing handle_doc_library_delete (now delete_indexed).  The
 * WebUI gates deletion with its own confirm modal (user approval).
 * ============================================================================= */

void handle_doc_library_note_save(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_note_save_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *label_obj, *text_obj;
   if (!payload || !json_object_object_get_ex(payload, "label", &label_obj) ||
       !json_object_object_get_ex(payload, "text", &text_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("A label and text are required"));
   } else {
      const char *label = json_object_get_string(label_obj);
      const char *text = json_object_get_string(text_obj);

      /* Reject a duplicate label up front (the editor is "new note"; edits use
       * note_update).  Server-side guard, not just the client pre-check. */
      document_t existing;
      if (document_db_find_by_label_exact(conn->auth_user_id, label, true, &existing) == SUCCESS) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("A note with that label already exists"));
      } else {
         doc_index_result_t result;
         int rc = document_index_note(conn->auth_user_id, label, text, text ? strlen(text) : 0,
                                      false, &result);
         if (rc != DOC_INDEX_SUCCESS) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(result.error_msg[0]
                                                              ? result.error_msg
                                                              : document_index_error_string(rc)));
         } else {
            /* Memory→note bridge (v61): gloss pointer for fuzzy recall.  Best-effort. */
            if (result.doc_id > 0)
               (void)memory_note_bridge_upsert_gloss(conn->auth_user_id, result.doc_id, label);
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "id", json_object_new_int64(result.doc_id));
            json_object_object_add(resp_payload, "label", json_object_new_string(label));
            OLOG_INFO("doc_library: user %d saved note '%s' (doc %lld)", conn->auth_user_id, label,
                      (long long)result.doc_id);
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_doc_library_note_update(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_note_update_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *label_obj, *text_obj;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "label", &label_obj) ||
       !json_object_object_get_ex(payload, "text", &text_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("id, label, and text are required"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);
      const char *label = json_object_get_string(label_obj);
      const char *text = json_object_get_string(text_obj);

      /* If the label changed, guard against colliding with a DIFFERENT note. */
      document_t clash;
      bool collide = (document_db_find_by_label_exact(conn->auth_user_id, label, true, &clash) ==
                          SUCCESS &&
                      clash.id != doc_id);
      if (collide) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Another note already uses that label"));
      } else {
         doc_index_result_t result;
         int rc = document_note_update(conn->auth_user_id, doc_id, label, text,
                                       text ? strlen(text) : 0, &result);
         if (rc != DOC_INDEX_SUCCESS) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(result.error_msg[0]
                                                              ? result.error_msg
                                                              : document_index_error_string(rc)));
         } else {
            /* Memory→note bridge (v61): refresh the gloss (label may have changed →
             * delete+recreate inside the bridge).  Best-effort. */
            (void)memory_note_bridge_upsert_gloss(conn->auth_user_id, doc_id, label);
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            json_object_object_add(resp_payload, "label", json_object_new_string(label));
            OLOG_INFO("doc_library: user %d updated note %lld ('%s')", conn->auth_user_id,
                      (long long)doc_id, label);
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* v62: list a document/note's archived versions (newest first), owner-scoped.
 * Payload: { id: <document_id> }. */
void handle_doc_library_version_list(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_version_list_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj = NULL;
   int64_t doc_id = 0;
   if (!payload || !json_object_object_get_ex(payload, "id", &id_obj) ||
       (doc_id = json_object_get_int64(id_obj)) <= 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("a valid id is required"));
   } else {
      document_version_meta_t v[DOC_VERSION_MAX_LIST];
      int n = 0;
      document_db_version_list(conn->auth_user_id, doc_id, v, DOC_VERSION_MAX_LIST, &n);

      json_object *arr = json_object_new_array();
      for (int i = 0; i < n; i++) {
         json_object *row = json_object_new_object();
         json_object_object_add(row, "id", json_object_new_int64(v[i].id));
         json_object_object_add(row, "filename", json_object_new_string(v[i].filename));
         json_object_object_add(row, "preview", json_object_new_string(v[i].preview));
         json_object_object_add(row, "archived_at", json_object_new_int64(v[i].archived_at));
         json_object_array_add(arr, row);
      }
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
      json_object_object_add(resp_payload, "versions", arr);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* v62: list recently-deleted items (their surviving snapshots) for undo, owner-
 * scoped.  No payload.  Restore one via doc_library_version_restore (the item no
 * longer exists, so restore re-creates it). */
void handle_doc_library_deleted_list(ws_connection_t *conn, json_object *payload) {
   (void)payload;
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_deleted_list_response"));
   json_object *resp_payload = json_object_new_object();

   document_version_meta_t v[DOC_VERSION_MAX_LIST];
   int n = 0;
   document_db_version_list_deleted(conn->auth_user_id, v, DOC_VERSION_MAX_LIST, &n);

   json_object *arr = json_object_new_array();
   for (int i = 0; i < n; i++) {
      json_object *row = json_object_new_object();
      json_object_object_add(row, "version_id", json_object_new_int64(v[i].id));
      json_object_object_add(row, "filename", json_object_new_string(v[i].filename));
      json_object_object_add(row, "preview", json_object_new_string(v[i].preview));
      json_object_object_add(row, "archived_at", json_object_new_int64(v[i].archived_at));
      json_object_array_add(arr, row);
   }
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "deleted", arr);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* v62: restore a note OR document to an archived version, owner-scoped.  Payload:
 * { version_id: <id> }.  If the item still exists it is updated in place (which
 * archives the CURRENT content first, so the restore itself is undoable): a
 * single-chunk note via note_update, a multi-chunk document via doc_update
 * (v63).  If it was deleted, it is re-created from the snapshot — as a note, or
 * as a multi-chunk document when the snapshot is too long for one note. */
void handle_doc_library_version_restore(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_version_restore_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *vid_obj = NULL;
   int64_t version_id = 0;
   if (!payload || !json_object_object_get_ex(payload, "version_id", &vid_obj) ||
       (version_id = json_object_get_int64(vid_obj)) <= 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("a valid version_id is required"));
      goto send;
   }
   char *text = NULL;
   char fname[DOC_FILENAME_MAX] = "";
   int64_t doc_id = 0;
   if (document_db_version_get_text(version_id, conn->auth_user_id, &text, fname, sizeof(fname),
                                    &doc_id) != SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Version not found (or not yours)"));
      goto send;
   }

   document_t doc;
   bool exists = (document_db_get(doc_id, &doc) == SUCCESS && doc.user_id == conn->auth_user_id);
   doc_index_result_t result;
   int rc = DOC_INDEX_SUCCESS + 1; /* non-success sentinel */
   const char *label = exists ? doc.filename : fname;
   int64_t deleted_doc_id = 0; /* dead id whose snapshots we re-point on recover */
   bool recreated_as_note = false;

   if (exists && strcmp(doc.filetype, "note") == 0 && doc.num_chunks == 1) {
      rc = document_note_update(conn->auth_user_id, doc_id, doc.filename, text, strlen(text),
                                &result);
   } else if (exists) {
      /* Existing multi-chunk document — restore in place (re-chunk + re-embed). */
      rc = document_doc_update(conn->auth_user_id, doc_id, text, strlen(text), &result);
   } else {
      /* The item was deleted — re-create it from the snapshot under its label.
       * Try a note first; fall back to a multi-chunk document if the snapshot is
       * too long for one note (mirrors the tool's recover path). */
      rc = document_index_note(conn->auth_user_id, fname, text, strlen(text), false, &result);
      recreated_as_note = (rc == DOC_INDEX_SUCCESS);
      if (rc != DOC_INDEX_SUCCESS)
         rc = document_index_text(conn->auth_user_id, fname, "text", text, strlen(text), false,
                                  NULL, &result);
      if (rc == DOC_INDEX_SUCCESS) {
         deleted_doc_id = doc_id; /* re-point its snapshots onto the new doc */
         doc_id = result.doc_id;
      }
   }

   free(text);

   if (rc != DOC_INDEX_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(result.error_msg[0]
                                                        ? result.error_msg
                                                        : document_index_error_string(rc)));
   } else {
      /* Refresh the memory→note gloss only for notes (an existing single-chunk
       * note, or a deleted item re-created as a note) — a multi-chunk document
       * has no gloss. */
      bool restored_note = exists ? (strcmp(doc.filetype, "note") == 0 && doc.num_chunks == 1)
                                  : recreated_as_note;
      if (restored_note)
         (void)memory_note_bridge_upsert_gloss(conn->auth_user_id, doc_id, label);
      /* Re-point the deleted item's surviving snapshots onto the re-created doc
       * so it leaves "Recently deleted" and a repeat restore can't dup it (#5). */
      if (deleted_doc_id > 0)
         (void)document_db_version_reattach(conn->auth_user_id, deleted_doc_id, doc_id);
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
      json_object_object_add(resp_payload, "label", json_object_new_string(label));
      json_object_object_add(resp_payload, "recreated", json_object_new_boolean(!exists));
      OLOG_INFO("doc_library: user %d restored item %lld ('%s') from version %lld",
                conn->auth_user_id, (long long)doc_id, label, (long long)version_id);
   }

send:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
