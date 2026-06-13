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
 * Extracted from webui_doc_library.c so both WebUI upload and the
 * document_index LLM tool can share the same indexing pipeline.
 */

#include "tools/document_index_pipeline.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_stem.h"
#include "tools/document_chunker.h"
#include "tools/document_db.h"

/* Briefly yield the CPU every N chunks during the embed pass so latency-
 * sensitive threads (notably the single WebSocket service thread) get
 * scheduled.  A long synchronous local-embedding burst would otherwise starve
 * them: observed 2026-06-13, a ~93s index of 323 chunks dropped and reconnected
 * a connected satellite mid-pass because its app-level keepalive lapsed.  The
 * cost is trivial (a few hundred ms across a multi-second index) and only
 * applies to the embed loop. */
#define DOC_INDEX_YIELD_INTERVAL_CHUNKS 8
#define DOC_INDEX_YIELD_MS 5

/* =============================================================================
 * Helpers
 * ============================================================================= */

static void sha256_hex(const char *data, size_t len, char *out_hex) {
   unsigned char hash[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)data, len, hash);
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      snprintf(out_hex + (i * 2), 3, "%02x", hash[i]);
   }
   out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static void set_error(doc_index_result_t *out, int code, const char *msg) {
   out->error_code = code;
   out->doc_id = -1;
   snprintf(out->error_msg, sizeof(out->error_msg), "%s", msg);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

const char *document_index_error_string(int error_code) {
   switch (error_code) {
      case DOC_INDEX_SUCCESS:
         return "Success";
      case DOC_INDEX_ERROR_EMPTY:
         return "Document text is empty";
      case DOC_INDEX_ERROR_TOO_LARGE:
         return "Document text exceeds maximum size";
      case DOC_INDEX_ERROR_LIMIT:
         return "Document limit reached";
      case DOC_INDEX_ERROR_NO_EMBEDDING:
         return "Embedding engine not available";
      case DOC_INDEX_ERROR_DUPLICATE:
         return "Document already indexed (duplicate content)";
      case DOC_INDEX_ERROR_CHUNK_FAIL:
         return "Failed to chunk document text";
      case DOC_INDEX_ERROR_DB_FAIL:
         return "Failed to create document record";
      case DOC_INDEX_ERROR_ALLOC:
         return "Memory allocation failed";
      default:
         return "Unknown indexing error";
   }
}

int document_index_text(int user_id,
                        const char *filename,
                        const char *filetype,
                        const char *text,
                        size_t text_len,
                        bool is_global,
                        doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;

   memset(out, 0, sizeof(*out));
   out->doc_id = -1;

   /* Validate text */
   if (!text || text_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Document text is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }

   if (text_len > (size_t)g_config.documents.max_index_size_kb * 1024) {
      set_error(out, DOC_INDEX_ERROR_TOO_LARGE, "Document text exceeds maximum size");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }

   /* Check user document count limit */
   int user_doc_count = 0;
   document_db_count_user(user_id, &user_doc_count);
   if (user_doc_count >= g_config.documents.max_indexed_documents) {
      char msg[128];
      snprintf(msg, sizeof(msg), "Document limit reached (%d max)",
               g_config.documents.max_indexed_documents);
      set_error(out, DOC_INDEX_ERROR_LIMIT, msg);
      return DOC_INDEX_ERROR_LIMIT;
   }

   /* Check embedding engine */
   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   /* Compute file hash for dedup */
   char file_hash[65];
   sha256_hex(text, text_len, file_hash);

   /* Check for duplicate */
   int64_t existing = 0;
   document_db_find_by_hash(file_hash, user_id, &existing);
   if (existing > 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "Document already indexed (id=%lld)", (long long)existing);
      set_error(out, DOC_INDEX_ERROR_DUPLICATE, msg);
      return DOC_INDEX_ERROR_DUPLICATE;
   }

   /* Chunk the text */
   chunk_config_t chunk_cfg = CHUNK_CONFIG_DEFAULT;
   chunk_result_t chunks;
   if (document_chunk_text(text, &chunk_cfg, &chunks) != 0 || chunks.count == 0) {
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to chunk document text");
      return DOC_INDEX_ERROR_CHUNK_FAIL;
   }

   int dims = embedding_engine_dims();

   /* Create document record */
   int64_t doc_id = 0;
   if (document_db_create(user_id, filename, filename, filetype, file_hash, chunks.count, is_global,
                          &doc_id) != SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to create document record");
      chunk_result_free(&chunks);
      return DOC_INDEX_ERROR_DB_FAIL;
   }

   /* Embed and store each chunk */
   float *emb_buf = malloc((size_t)dims * sizeof(float));
   int embedded_count = 0;
   int failed_count = 0;

   if (emb_buf) {
      /* Capture ingest time once so all chunks of the same document share an
       * identical created_at.  Calling time(NULL) per-chunk would give chunks
       * slightly different timestamps across a slow embedding pass, which
       * contradicts the "inherit from the document's ingest time" intent. */
      int64_t ingest_ts = (int64_t)time(NULL);

      /* v61: stem the filename/label ONCE (same for every chunk) outside the
       * auth_db lock (leaf-lock rule).  Each chunk's body is stemmed in-loop,
       * also outside the lock; document_db_chunk_index_fts does only the locked
       * insert.  FTS indexing is best-effort — a failure (e.g. v61 migration not
       * yet run) never fails the ingest; search degrades to pure-semantic. */
      char label_stems[MEMORY_FACT_STEMS_MAX];
      (void)memory_stem_string(filename, label_stems, sizeof(label_stems));

      for (int i = 0; i < chunks.count; i++) {
         int out_dims = 0;
         int rc = embedding_engine_embed(chunks.chunks[i], emb_buf, dims, &out_dims);
         if (rc != 0 || out_dims != dims) {
            failed_count++;
            continue;
         }

         float norm = embedding_engine_l2_norm(emb_buf, dims);
         int64_t chunk_id = 0;
         if (document_db_chunk_create(doc_id, i, chunks.chunks[i], emb_buf, dims, norm, ingest_ts,
                                      &chunk_id) == SUCCESS) {
            embedded_count++;
            char body_stems[MEMORY_FACT_STEMS_MAX];
            (void)memory_stem_string(chunks.chunks[i], body_stems, sizeof(body_stems));
            (void)document_db_chunk_index_fts(chunk_id, label_stems, body_stems);
         } else {
            failed_count++;
         }

         /* Yield a CPU window to other threads between embed batches (no lock is
          * held here — embedding_engine_embed releases s_embed_mutex internally). */
         if ((i + 1) % DOC_INDEX_YIELD_INTERVAL_CHUNKS == 0) {
            usleep((useconds_t)DOC_INDEX_YIELD_MS * 1000u);
         }
      }
      free(emb_buf);
   } else {
      /* Memory allocation failed — delete the document record */
      document_db_delete(doc_id);
      chunk_result_free(&chunks);
      set_error(out, DOC_INDEX_ERROR_ALLOC, "Memory allocation failed");
      return DOC_INDEX_ERROR_ALLOC;
   }

   /* If no chunk embedded, the document row is useless (it would also archive a
    * junk version row when later deleted) — drop it and report the failure.  If
    * only some embedded, correct num_chunks so it matches the actual chunk rows
    * (it gates note-vs-document editing). */
   if (embedded_count == 0) {
      document_db_delete(doc_id);
      chunk_result_free(&chunks);
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "No chunks could be embedded");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }
   if (embedded_count != chunks.count)
      (void)document_db_set_num_chunks(doc_id, embedded_count);

   chunk_result_free(&chunks);

   /* v63: store the canonical un-chunked text so the document can later be edited
    * in place (find/replace → re-chunk).  Best-effort — a failure here doesn't
    * fail the ingest; the doc just won't be editable until re-saved.  Skipped for
    * GLOBAL docs: the LLM/WebUI edit path only touches owned docs, so full_text on
    * a global would be pure dead storage (no one can edit it in place). */
   if (!is_global)
      (void)document_db_full_text_set(doc_id, text);

   OLOG_INFO("document_index_pipeline: indexed '%s' — %d chunks embedded, %d failed%s", filename,
             embedded_count, failed_count, is_global ? " [GLOBAL]" : "");

   out->doc_id = doc_id;
   out->num_chunks = embedded_count;
   out->failed_chunks = failed_count;
   out->error_code = DOC_INDEX_SUCCESS;
   out->error_msg[0] = '\0';
   return DOC_INDEX_SUCCESS;
}

/* v61: save a short authored "note" — a single-chunk document whose filename IS
 * the user's label.  Bypasses the chunker entirely (M-1): rejects anything the
 * chunker would split so num_chunks == 1 holds by construction on every path
 * (WebUI and tool).  No hash-dedup: identical bodies under different labels are
 * legitimate, and the tool layer handles same-label overwrite via note_update. */
int document_index_note(int user_id,
                        const char *label,
                        const char *text,
                        size_t text_len,
                        bool is_global,
                        doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;
   memset(out, 0, sizeof(*out));
   out->doc_id = -1;

   if (!label || !label[0]) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Note label is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }
   if (!text || text_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Note text is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }

   chunk_config_t cfg = CHUNK_CONFIG_DEFAULT;
   if (chunk_estimate_tokens(text, (int)text_len) > cfg.max_tokens) {
      set_error(out, DOC_INDEX_ERROR_TOO_LARGE,
                "Note is too long to file as a single note — save it as a document instead (use "
                "action save_text), or shorten it");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }

   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   int user_doc_count = 0;
   if (document_db_count_user(user_id, &user_doc_count) == SUCCESS &&
       user_doc_count >= g_config.documents.max_indexed_documents) {
      set_error(out, DOC_INDEX_ERROR_LIMIT, "Document limit reached");
      return DOC_INDEX_ERROR_LIMIT;
   }

   int dims = embedding_engine_dims();
   if (dims <= 0) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   char file_hash[65];
   sha256_hex(text, text_len, file_hash);

   int64_t doc_id = 0;
   if (document_db_create(user_id, label, label, "note", file_hash, 1, is_global, &doc_id) !=
       SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to create note record");
      return DOC_INDEX_ERROR_DB_FAIL;
   }

   float *emb = malloc((size_t)dims * sizeof(float));
   if (!emb) {
      document_db_delete(doc_id);
      set_error(out, DOC_INDEX_ERROR_ALLOC, "Memory allocation failed");
      return DOC_INDEX_ERROR_ALLOC;
   }
   int out_dims = 0;
   if (embedding_engine_embed(text, emb, dims, &out_dims) != 0 || out_dims != dims) {
      free(emb);
      document_db_delete(doc_id);
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to embed note text");
      return DOC_INDEX_ERROR_CHUNK_FAIL;
   }
   float norm = embedding_engine_l2_norm(emb, dims);
   int64_t chunk_id = 0;
   int rc = document_db_chunk_create(doc_id, 0, text, emb, dims, norm, (int64_t)time(NULL),
                                     &chunk_id);
   free(emb);
   if (rc != SUCCESS) {
      document_db_delete(doc_id);
      set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to store note chunk");
      return DOC_INDEX_ERROR_DB_FAIL;
   }

   char label_stems[MEMORY_FACT_STEMS_MAX], body_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(label, label_stems, sizeof(label_stems));
   (void)memory_stem_string(text, body_stems, sizeof(body_stems));
   (void)document_db_chunk_index_fts(chunk_id, label_stems, body_stems);

   out->doc_id = doc_id;
   out->num_chunks = 1;
   out->failed_chunks = 0;
   out->error_code = DOC_INDEX_SUCCESS;
   out->error_msg[0] = '\0';
   OLOG_INFO("document_index_pipeline: saved note '%s' (doc %lld)", label, (long long)doc_id);
   return DOC_INDEX_SUCCESS;
}

/* v61: edit an existing note in place (re-embed + delegate the stable-id DB
 * swap).  Same single-chunk cap as create.  document_db_note_update enforces the
 * note + ownership gate. */
int document_note_update(int user_id,
                         int64_t doc_id,
                         const char *new_label,
                         const char *new_text,
                         size_t new_len,
                         doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;
   memset(out, 0, sizeof(*out));
   out->doc_id = doc_id;

   if (!new_label || !new_label[0] || !new_text || new_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Note label and text are required");
      return DOC_INDEX_ERROR_EMPTY;
   }
   chunk_config_t cfg = CHUNK_CONFIG_DEFAULT;
   if (chunk_estimate_tokens(new_text, (int)new_len) > cfg.max_tokens) {
      set_error(out, DOC_INDEX_ERROR_TOO_LARGE,
                "Note is too long to file as a single note — save it as a document instead (use "
                "action save_text), or shorten it");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }
   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }
   int dims = embedding_engine_dims();
   if (dims <= 0) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }
   float *emb = malloc((size_t)dims * sizeof(float));
   if (!emb) {
      set_error(out, DOC_INDEX_ERROR_ALLOC, "Memory allocation failed");
      return DOC_INDEX_ERROR_ALLOC;
   }
   int out_dims = 0;
   if (embedding_engine_embed(new_text, emb, dims, &out_dims) != 0 || out_dims != dims) {
      free(emb);
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to embed note text");
      return DOC_INDEX_ERROR_CHUNK_FAIL;
   }
   float norm = embedding_engine_l2_norm(emb, dims);
   char new_hash[65];
   sha256_hex(new_text, new_len, new_hash);
   int rc = document_db_note_update(user_id, doc_id, new_label, new_text, emb, dims, norm,
                                    new_hash);
   free(emb);
   if (rc != SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL,
                "Failed to update note (not a note, not yours, or a database error)");
      return DOC_INDEX_ERROR_DB_FAIL;
   }
   out->doc_id = doc_id;
   out->num_chunks = 1;
   out->error_code = DOC_INDEX_SUCCESS;
   out->error_msg[0] = '\0';
   return DOC_INDEX_SUCCESS;
}

/* v63 (B1b): replace a MULTI-chunk document's content in place — re-chunk +
 * re-embed the new full text and atomically swap all chunks, keeping doc_id
 * stable.  Requires stored full text (document_db_doc_read_for_edit rejects
 * pre-v63 uploads).  Stems run outside the auth_db leaf lock; the swap is one
 * transaction in document_db_doc_replace, which also archives the old content. */
int document_doc_update(int user_id,
                        int64_t doc_id,
                        const char *new_text,
                        size_t new_len,
                        doc_index_result_t *out) {
   if (!out)
      return DOC_INDEX_ERROR_ALLOC;
   memset(out, 0, sizeof(*out));
   out->doc_id = doc_id;

   if (!new_text || new_len == 0) {
      set_error(out, DOC_INDEX_ERROR_EMPTY, "Document text is empty");
      return DOC_INDEX_ERROR_EMPTY;
   }
   if (new_len > (size_t)g_config.documents.max_index_size_kb * 1024) {
      set_error(out, DOC_INDEX_ERROR_TOO_LARGE, "Document text exceeds maximum size");
      return DOC_INDEX_ERROR_TOO_LARGE;
   }
   if (!embedding_engine_available()) {
      set_error(out, DOC_INDEX_ERROR_NO_EMBEDDING, "Embedding engine not available");
      return DOC_INDEX_ERROR_NO_EMBEDDING;
   }

   /* Read old chunks (for FTS delete) + filename; owner-checked, requires full text. */
   char filename[DOC_FILENAME_MAX] = "";
   char *old_full = NULL;
   int64_t *old_ids = NULL;
   char **old_texts = NULL;
   int n_old = 0;
   if (document_db_doc_read_for_edit(user_id, doc_id, filename, sizeof(filename), &old_full,
                                     &old_ids, &old_texts, &n_old) != SUCCESS) {
      set_error(out, DOC_INDEX_ERROR_DB_FAIL,
                "This document can't be edited in place — re-save it to enable editing");
      return DOC_INDEX_ERROR_DB_FAIL;
   }
   free(old_full); /* doc_replace re-reads it for the version snapshot */

   /* Stem the label once + each OLD chunk body (for FTS delete), outside the lock. */
   char label_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(filename, label_stems, sizeof(label_stems));
   char **old_body_stems = (n_old > 0) ? calloc((size_t)n_old, sizeof(char *)) : NULL;
   bool oom = (n_old > 0 && !old_body_stems);
   for (int i = 0; i < n_old && !oom; i++) {
      old_body_stems[i] = malloc(MEMORY_FACT_STEMS_MAX);
      if (!old_body_stems[i]) {
         oom = true;
         break;
      }
      (void)memory_stem_string(old_texts[i], old_body_stems[i], MEMORY_FACT_STEMS_MAX);
   }

   /* Chunk + embed + stem the NEW text. */
   int dims = embedding_engine_dims();
   chunk_config_t cfg = CHUNK_CONFIG_DEFAULT;
   chunk_result_t chunks;
   bool chunks_inited = (!oom && document_chunk_text(new_text, &cfg, &chunks) == 0);
   int n_new = chunks_inited ? chunks.count : 0;
   doc_replace_chunk_t *nc = NULL;
   float *embs = NULL;
   char **new_stems = NULL;
   bool build_ok = false;
   if (n_new > 0) {
      nc = calloc((size_t)n_new, sizeof(*nc));
      embs = malloc((size_t)n_new * (size_t)dims * sizeof(float));
      new_stems = calloc((size_t)n_new, sizeof(char *));
      build_ok = (nc && embs && new_stems);
      for (int i = 0; build_ok && i < n_new; i++) {
         float *slot = embs + (size_t)i * dims;
         int od = 0;
         if (embedding_engine_embed(chunks.chunks[i], slot, dims, &od) != 0 || od != dims) {
            build_ok = false;
            break;
         }
         new_stems[i] = malloc(MEMORY_FACT_STEMS_MAX);
         if (!new_stems[i]) {
            build_ok = false;
            break;
         }
         (void)memory_stem_string(chunks.chunks[i], new_stems[i], MEMORY_FACT_STEMS_MAX);
         nc[i].text = chunks.chunks[i];
         nc[i].embedding = slot;
         nc[i].norm = embedding_engine_l2_norm(slot, dims);
         nc[i].body_stems = new_stems[i];
      }
   }

   int rc = DOC_INDEX_ERROR_CHUNK_FAIL;
   if (build_ok) {
      char new_hash[65];
      sha256_hex(new_text, new_len, new_hash);
      if (document_db_doc_replace(user_id, doc_id, new_text, new_hash, filename, label_stems,
                                  old_ids, (const char *const *)old_body_stems, n_old, nc, n_new,
                                  dims) == SUCCESS) {
         out->doc_id = doc_id;
         out->num_chunks = n_new;
         out->error_code = DOC_INDEX_SUCCESS;
         rc = DOC_INDEX_SUCCESS;
      } else {
         set_error(out, DOC_INDEX_ERROR_DB_FAIL, "Failed to apply the edit");
         rc = DOC_INDEX_ERROR_DB_FAIL;
      }
   } else {
      set_error(out, DOC_INDEX_ERROR_CHUNK_FAIL, "Failed to chunk/embed the edited text");
   }

   /* Cleanup. */
   if (chunks_inited)
      chunk_result_free(&chunks);
   if (new_stems)
      for (int i = 0; i < n_new; i++)
         free(new_stems[i]);
   free(new_stems);
   free(embs);
   free(nc);
   if (old_body_stems)
      for (int i = 0; i < n_old; i++)
         free(old_body_stems[i]);
   free(old_body_stems);
   for (int i = 0; i < n_old; i++)
      free(old_texts[i]);
   free(old_texts);
   free(old_ids);
   return rc;
}
