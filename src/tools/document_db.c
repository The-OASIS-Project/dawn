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
 * Document Database Layer - SQLite CRUD for documents and document_chunks
 *
 * Part of the RAG document search system. Accesses s_db directly
 * (same pattern as scheduler_db.c). All functions acquire the auth_db mutex.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/document_db.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_bm25.h"
#include "memory/memory_stem.h"

extern dawn_config_t g_config;

/* v62 versioning: archive the pre-mutation content of a document before an
 * in-place update or delete.  Defined later (near document_db_note_update) but
 * also called from document_db_delete_indexed above it. */
static void version_archive_locked(int64_t doc_id,
                                   int user_id,
                                   const char *filename,
                                   const char *text);

/* Max length of a built FTS5 MATCH expression (OR-of-quoted-stems). */
#define DOC_CHUNK_MATCH_EXPR_MAX 2048

/* =============================================================================
 * Helper: safe string copy from SQLite column
 * ============================================================================= */

static void col_text_copy(char *dst, size_t dst_size, sqlite3_stmt *stmt, int col) {
   const char *src = (const char *)sqlite3_column_text(stmt, col);
   if (src) {
      size_t len = strlen(src);
      if (len >= dst_size)
         len = dst_size - 1;
      memcpy(dst, src, len);
      dst[len] = '\0';
   } else {
      dst[0] = '\0';
   }
}

/* =============================================================================
 * Helper: populate document_t from a SELECT row
 * Columns: id, user_id, filename, filepath, filetype, file_hash,
 *          num_chunks, is_global, created_at
 * ============================================================================= */

static void row_to_document(sqlite3_stmt *stmt, document_t *doc) {
   doc->id = sqlite3_column_int64(stmt, 0);
   doc->user_id = sqlite3_column_int(stmt, 1);
   col_text_copy(doc->filename, sizeof(doc->filename), stmt, 2);
   col_text_copy(doc->filepath, sizeof(doc->filepath), stmt, 3);
   col_text_copy(doc->filetype, sizeof(doc->filetype), stmt, 4);
   col_text_copy(doc->file_hash, sizeof(doc->file_hash), stmt, 5);
   doc->num_chunks = sqlite3_column_int(stmt, 6);
   doc->is_global = sqlite3_column_int(stmt, 7) != 0;
   doc->created_at = sqlite3_column_int64(stmt, 8);
}

/* =============================================================================
 * Document CRUD
 * ============================================================================= */

int document_db_create(int user_id,
                       const char *filename,
                       const char *filepath,
                       const char *filetype,
                       const char *file_hash,
                       int num_chunks,
                       bool is_global,
                       int64_t *id_out) {
   if (!filename || !filepath || !filetype || !file_hash || !id_out)
      return FAILURE;

   *id_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_create;
   sqlite3_reset(stmt);

   if (user_id > 0)
      sqlite3_bind_int(stmt, 1, user_id);
   else
      sqlite3_bind_null(stmt, 1);
   sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, filepath, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, filetype, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, file_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 6, num_chunks);
   sqlite3_bind_int(stmt, 7, is_global ? 1 : 0);
   sqlite3_bind_int64(stmt, 8, (int64_t)time(NULL));

   int rc = sqlite3_step(stmt);
   int result = FAILURE;
   if (rc == SQLITE_DONE) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("document_db: create failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_get(int64_t doc_id, document_t *out) {
   if (!out)
      return FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, doc_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, out);
      result = SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_find_by_hash(const char *file_hash, int user_id, int64_t *id_out) {
   if (!file_hash || !id_out)
      return FAILURE;

   *id_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_get_by_hash;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, file_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, user_id);

   /* *id_out remains 0 (not found) unless a row is returned */
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *id_out = sqlite3_column_int64(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int document_db_list(int user_id, document_t *out, int limit, int offset, int *count_out) {
   if (!out || limit <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;
   if (limit > DOC_MAX_RESULTS)
      limit = DOC_MAX_RESULTS;
   if (offset < 0)
      offset = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, limit);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < limit && sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, &out[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

int document_db_list_all(document_t *out, int limit, int offset, int *count_out) {
   if (!out || limit <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;
   if (limit > DOC_MAX_RESULTS)
      limit = DOC_MAX_RESULTS;
   if (offset < 0)
      offset = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_list_all;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, limit);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (count < limit && sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, &out[count]);
      /* Column 9 = COALESCE(u.username, '') from the JOIN */
      col_text_copy(out[count].owner_name, sizeof(out[count].owner_name), stmt, 9);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

int document_db_update_global(int64_t doc_id, bool is_global) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_update_global;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, is_global ? 1 : 0);
   sqlite3_bind_int64(stmt, 2, doc_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_DONE) {
      result = (sqlite3_changes(s_db.db) > 0) ? SUCCESS : FAILURE;
   } else {
      OLOG_ERROR("document_db: update_global failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_delete(int64_t doc_id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, doc_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_DONE) {
      result = (sqlite3_changes(s_db.db) > 0) ? SUCCESS : FAILURE;
   } else {
      OLOG_ERROR("document_db: delete failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_count_user(int user_id, int *count_out) {
   if (!count_out)
      return FAILURE;
   *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_count_user;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *count_out = sqlite3_column_int(stmt, 0);
      result = SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Document Lookup by Name
 * ============================================================================= */

int document_db_find_by_name(int user_id, const char *name, document_t *out) {
   if (!name || !out)
      return FAILURE;

   /* If name is all digits, try direct ID lookup first */
   bool all_digits = true;
   for (const char *p = name; *p; p++) {
      if (*p < '0' || *p > '9') {
         all_digits = false;
         break;
      }
   }
   if (all_digits && name[0] != '\0') {
      int64_t doc_id = strtoll(name, NULL, 10);
      if (document_db_get(doc_id, out) == SUCCESS) {
         /* Verify user access: must be owner or global */
         if (out->user_id == user_id || out->is_global)
            return SUCCESS;
      }
   }

   /* Escape LIKE wildcards in name */
   size_t name_len = strlen(name);
   char escaped[DOC_FILENAME_MAX * 2];
   size_t esc_len = 0;
   for (size_t i = 0; i < name_len && esc_len + 1 < sizeof(escaped); i++) {
      if (name[i] == '%' || name[i] == '_' || name[i] == '\\') {
         escaped[esc_len++] = '\\';
      }
      escaped[esc_len++] = name[i];
   }
   escaped[esc_len] = '\0';

   /* Build LIKE pattern: %escaped_name% */
   char pattern[DOC_FILENAME_MAX * 2 + 3];
   if (esc_len + 2 >= sizeof(pattern))
      return FAILURE;
   pattern[0] = '%';
   memcpy(pattern + 1, escaped, esc_len);
   pattern[esc_len + 1] = '%';
   pattern[esc_len + 2] = '\0';

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_find_by_name;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);

   int result = FAILURE;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_to_document(stmt, out);
      result = SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Chunk Read (Paginated)
 * ============================================================================= */

int document_db_chunk_read(int64_t document_id,
                           document_chunk_t *chunks,
                           int max_count,
                           int start_chunk,
                           int *count_out) {
   if (!chunks || max_count <= 0 || start_chunk < 0 || !count_out)
      return FAILURE;

   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_read;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, document_id);
   sqlite3_bind_int(stmt, 2, max_count);
   sqlite3_bind_int(stmt, 3, start_chunk);

   int count = 0;
   while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
      document_chunk_t *c = &chunks[count];
      memset(c, 0, sizeof(*c));
      c->chunk_index = sqlite3_column_int(stmt, 0);
      col_text_copy(c->text, sizeof(c->text), stmt, 1);
      c->document_id = document_id;
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

int document_db_chunk_read_range(int64_t document_id,
                                 int lo_idx,
                                 int hi_idx,
                                 document_chunk_t *chunks,
                                 int max_count,
                                 int *count_out) {
   if (!chunks || max_count <= 0 || !count_out)
      return FAILURE;

   *count_out = 0;
   if (hi_idx < lo_idx)
      return SUCCESS; /* empty range — not an error */
   if (lo_idx < 0)
      lo_idx = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_read_range;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, document_id);
   sqlite3_bind_int(stmt, 2, lo_idx);
   sqlite3_bind_int(stmt, 3, hi_idx);

   int count = 0;
   while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
      document_chunk_t *c = &chunks[count];
      memset(c, 0, sizeof(*c));
      c->chunk_index = sqlite3_column_int(stmt, 0);
      col_text_copy(c->text, sizeof(c->text), stmt, 1);
      c->document_id = document_id;
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

int document_db_chunk_grep(int user_id,
                           const char *needle,
                           bool case_sensitive,
                           int offset,
                           doc_grep_hit_t *hits,
                           int max_count,
                           int *count_out,
                           bool *more_out) {
   if (!needle || needle[0] == '\0' || !hits || max_count <= 0 || !count_out || !more_out)
      return FAILURE;
   if (offset < 0)
      offset = 0;

   *count_out = 0;
   *more_out = false;

   /* For the case-insensitive (LIKE) path, escape wildcards and wrap in %..%.
    * The case-sensitive (instr) path binds the needle literally. */
   char pattern[DOC_CHUNK_TEXT_MAX * 2 + 3];
   if (!case_sensitive) {
      size_t nlen = strlen(needle);
      char escaped[DOC_CHUNK_TEXT_MAX * 2];
      size_t esc_len = 0;
      for (size_t i = 0; i < nlen && esc_len + 2 < sizeof(escaped); i++) {
         if (needle[i] == '%' || needle[i] == '_' || needle[i] == '\\')
            escaped[esc_len++] = '\\';
         escaped[esc_len++] = needle[i];
      }
      escaped[esc_len] = '\0';
      if (esc_len + 2 >= sizeof(pattern))
         return FAILURE;
      pattern[0] = '%';
      memcpy(pattern + 1, escaped, esc_len);
      pattern[esc_len + 1] = '%';
      pattern[esc_len + 2] = '\0';
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = case_sensitive ? s_db.stmt_doc_chunk_grep_cs : s_db.stmt_doc_chunk_grep_ci;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   if (case_sensitive)
      sqlite3_bind_text(stmt, 2, needle, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
   /* Fetch one extra row to detect "more matches exist" without a COUNT(*). */
   sqlite3_bind_int(stmt, 3, max_count + 1);
   sqlite3_bind_int(stmt, 4, offset);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (count >= max_count) {
         *more_out = true; /* the (max_count+1)th row — there are more */
         break;
      }
      doc_grep_hit_t *h = &hits[count];
      memset(h, 0, sizeof(*h));
      h->chunk_index = sqlite3_column_int(stmt, 0);
      h->document_id = sqlite3_column_int64(stmt, 1);
      col_text_copy(h->filename, sizeof(h->filename), stmt, 2);
      h->num_chunks = sqlite3_column_int(stmt, 3);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

/* =============================================================================
 * Chunk CRUD
 * ============================================================================= */

int document_db_chunk_create(int64_t document_id,
                             int chunk_index,
                             const char *text,
                             const float *embedding,
                             int dims,
                             float embedding_norm,
                             int64_t created_at,
                             int64_t *id_out) {
   if (!text || !embedding || dims <= 0 || !id_out)
      return FAILURE;

   *id_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_create;
   sqlite3_reset(stmt);

   sqlite3_bind_int64(stmt, 1, document_id);
   sqlite3_bind_int(stmt, 2, chunk_index);
   sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
   sqlite3_bind_blob(stmt, 4, embedding, dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 5, (double)embedding_norm);
   sqlite3_bind_int64(stmt, 6, created_at); /* 0 = unknown — schema default also 0 */

   int rc = sqlite3_step(stmt);
   int result = FAILURE;
   if (rc == SQLITE_DONE) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("document_db: chunk_create failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int document_db_chunk_search_load(int user_id,
                                  document_chunk_t *chunks,
                                  float *embedding_buf,
                                  int dims,
                                  int max_count,
                                  int *count_out) {
   if (!chunks || !embedding_buf || dims <= 0 || max_count <= 0 || !count_out)
      return FAILURE;

   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   int expected_blob_size = dims * (int)sizeof(float);

   while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
      /* Verify embedding dimensions match */
      int blob_size = sqlite3_column_bytes(stmt, 3);
      if (blob_size != expected_blob_size)
         continue;

      document_chunk_t *c = &chunks[count];
      c->id = sqlite3_column_int64(stmt, 0);
      c->chunk_index = sqlite3_column_int(stmt, 1);
      col_text_copy(c->text, sizeof(c->text), stmt, 2);

      /* Copy embedding into the flat buffer */
      const void *blob = sqlite3_column_blob(stmt, 3);
      float *emb_dest = embedding_buf + (count * dims);
      memcpy(emb_dest, blob, (size_t)blob_size);
      c->embedding = emb_dest;
      c->embedding_norm = (float)sqlite3_column_double(stmt, 4);

      c->document_id = sqlite3_column_int64(stmt, 5);
      col_text_copy(c->doc_filename, sizeof(c->doc_filename), stmt, 6);
      col_text_copy(c->doc_filetype, sizeof(c->doc_filetype), stmt, 7);
      c->created_at = sqlite3_column_int64(stmt, 8); /* v35 — 0 if column was NULL/0 */

      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

/* =============================================================================
 * v61: document_chunks_fts (BM25 lexical index) sync + indexed delete
 *
 * Mirrors the memory_facts_fts discipline (src/memory/memory_db_facts.c):
 *   - the FTS5 index is CONTENTLESS, so deletes use the 'delete' command and
 *     need the ORIGINAL stems (re-derived from the live row before the DELETE);
 *   - stemming runs OUTSIDE the auth_db lock (memory_stem owns its own mutex; the
 *     auth_db mutex must stay a leaf — see ARCHITECTURE.md lock ordering), so the
 *     _locked helpers take PRE-STEMMED strings and the public entry points stem
 *     before (re)acquiring the lock.
 * Global-IDF caveat carried from memory_facts_fts: a single index across users;
 * per-user safety is the search statement's JOIN + filter, not the index.
 * ============================================================================= */

/* Insert one chunk's pre-stemmed (label, body) into document_chunks_fts.  Caller
 * holds AUTH_DB_LOCK.  Soft: a NULL stmt (v61 migration not yet run) or an insert
 * error is logged and swallowed so search degrades rather than crashing. */
static int fts5_insert_chunk_stems_locked(int64_t chunk_id,
                                          const char *label_stems,
                                          const char *body_stems) {
   if (!s_db.stmt_doc_chunk_fts_insert)
      return FAILURE;
   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_fts_insert;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, chunk_id);
   sqlite3_bind_text(stmt, 2, label_stems ? label_stems : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, body_stems ? body_stems : "", -1, SQLITE_TRANSIENT);
   int ok = (sqlite3_step(stmt) == SQLITE_DONE);
   if (!ok)
      OLOG_WARNING("document_db: FTS insert (chunk %lld) failed: %s", (long long)chunk_id,
                   sqlite3_errmsg(s_db.db));
   sqlite3_reset(stmt);
   return ok ? SUCCESS : FAILURE;
}

/* Delete one chunk's row from the contentless FTS5 index.  Requires the ORIGINAL
 * stems (the 'delete' command decrements postings for those exact terms — wrong
 * terms corrupt the index).  Caller holds AUTH_DB_LOCK. */
static int fts5_delete_chunk_stems_locked(int64_t chunk_id,
                                          const char *label_stems,
                                          const char *body_stems) {
   if (!s_db.stmt_doc_chunk_fts_delete)
      return FAILURE;
   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_fts_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, chunk_id);
   sqlite3_bind_text(stmt, 2, label_stems ? label_stems : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, body_stems ? body_stems : "", -1, SQLITE_TRANSIENT);
   int ok = (sqlite3_step(stmt) == SQLITE_DONE);
   if (!ok)
      OLOG_WARNING("document_db: FTS delete (chunk %lld) failed: %s", (long long)chunk_id,
                   sqlite3_errmsg(s_db.db));
   sqlite3_reset(stmt);
   return ok ? SUCCESS : FAILURE;
}

/* Index one freshly-created chunk into the FTS5 table.  Public so the ingest
 * pipeline can call it right after document_db_chunk_create (keeping
 * chunk_create's signature stable).  Stems are computed by the CALLER outside the
 * lock; this only does the locked insert. */
int document_db_chunk_index_fts(int64_t chunk_id, const char *label_stems, const char *body_stems) {
   AUTH_DB_LOCK_OR_FAIL();
   int result = fts5_insert_chunk_stems_locked(chunk_id, label_stems, body_stems);
   AUTH_DB_UNLOCK();
   return result;
}

/* Delete a document AND its contentless-FTS rows in one transaction.  The
 * document_chunks FK ON DELETE CASCADE does NOT cascade to document_chunks_fts
 * (no SQL trigger can call the C stemmer), so orphan FTS rows would bias global
 * IDF and let a reused rowid re-attribute postings.  This snapshots each chunk's
 * (id, text) + the doc filename under the lock, RELEASES the lock to stem (leaf-
 * lock rule), then re-locks to delete the FTS rows and the document together.
 * Permission checks stay in the caller (mirrors document_db_delete).  On an
 * allocation failure it falls back to a plain delete (chunks cascade; FTS orphans
 * are recoverable via the admin rebuild pass). */
int document_db_delete_indexed(int64_t doc_id) {
   /* --- Phase 1 (locked): snapshot chunk ids + text + the filename. --- */
   AUTH_DB_LOCK_OR_FAIL();
   char filename[DOC_FILENAME_MAX] = "";
   int owner_user_id = 0; /* for the v62 pre-delete version snapshot */
   int64_t *chunk_ids = NULL;
   char **chunk_texts = NULL;
   int n = 0, cap = 0;
   bool oom = false;
   sqlite3_stmt *sel = NULL;
   int prep = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.text, d.filename, d.user_id FROM document_chunks c "
       "JOIN documents d ON d.id = c.document_id "
       "WHERE c.document_id = ?",
       -1, &sel, NULL);
   if (prep == SQLITE_OK) {
      sqlite3_bind_int64(sel, 1, doc_id);
      while (!oom && sqlite3_step(sel) == SQLITE_ROW) {
         if (n == cap) {
            int ncap = cap ? cap * 2 : 8;
            int64_t *nids = realloc(chunk_ids, (size_t)ncap * sizeof(*nids));
            char **ntexts = realloc(chunk_texts, (size_t)ncap * sizeof(*ntexts));
            if (nids)
               chunk_ids = nids;
            if (ntexts)
               chunk_texts = ntexts;
            if (!nids || !ntexts) {
               oom = true;
               break;
            }
            cap = ncap;
         }
         chunk_ids[n] = sqlite3_column_int64(sel, 0);
         const unsigned char *t = sqlite3_column_text(sel, 1);
         chunk_texts[n] = strdup(t ? (const char *)t : "");
         if (!chunk_texts[n]) {
            oom = true;
            break;
         }
         if (!filename[0]) {
            const unsigned char *fn = sqlite3_column_text(sel, 2);
            if (fn) {
               strncpy(filename, (const char *)fn, sizeof(filename) - 1);
               filename[sizeof(filename) - 1] = '\0';
            }
            owner_user_id = sqlite3_column_int(sel, 3);
         }
         n++;
      }
   }
   if (sel)
      sqlite3_finalize(sel);
   AUTH_DB_UNLOCK();

   /* OOM fallback: free, then plain delete (leaves recoverable FTS orphans). */
   if (oom) {
      for (int i = 0; i < n; i++)
         free(chunk_texts[i]);
      free(chunk_texts);
      free(chunk_ids);
      OLOG_WARNING("document_db: delete_indexed OOM (doc %lld) — falling back to plain delete; "
                   "FTS orphans recoverable via admin rebuild",
                   (long long)doc_id);
      return document_db_delete(doc_id);
   }

   /* --- Phase 2 (unlocked): stem the label once + each chunk body. --- */
   char label_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(filename, label_stems, sizeof(label_stems));
   char **body_stems = (n > 0) ? calloc((size_t)n, sizeof(char *)) : NULL;
   bool stem_oom = (n > 0 && !body_stems);
   for (int i = 0; i < n && !stem_oom; i++) {
      body_stems[i] = malloc(MEMORY_FACT_STEMS_MAX);
      if (!body_stems[i]) {
         stem_oom = true;
         break;
      }
      (void)memory_stem_string(chunk_texts[i], body_stems[i], MEMORY_FACT_STEMS_MAX);
   }

   /* Build the pre-delete content snapshot (concat of chunks, newline-joined).
    * Single-chunk notes are exact; multi-chunk docs are lossy at overlap
    * boundaries — acceptable for a recovery aid.  NULL on OOM → snapshot skipped
    * (the delete still proceeds; versioning is best-effort). */
   char *snapshot = NULL;
   if (g_config.documents.version_retention_days > 0) {
      size_t total = 0;
      for (int i = 0; i < n; i++)
         total += strlen(chunk_texts[i]) + 1;
      snapshot = malloc(total + 1);
      if (snapshot) {
         size_t pos = 0;
         for (int i = 0; i < n; i++) {
            size_t l = strlen(chunk_texts[i]);
            memcpy(snapshot + pos, chunk_texts[i], l);
            pos += l;
            if (i + 1 < n)
               snapshot[pos++] = '\n';
         }
         snapshot[pos] = '\0';
      }
   }

   /* --- Phase 3 (locked): FTS deletes + document delete in one transaction.
    * Plain lock (not AUTH_DB_LOCK_OR_FAIL) + an initialized guard so an early
    * return can't leak the heap buffers allocated above. */
   int result = FAILURE;
   if (!stem_oom) {
      pthread_mutex_lock(&s_db.mutex);
      if (s_db.initialized) {
         (void)sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
         /* Snapshot the pre-delete content so the deletion is undoable.  Skip
          * when there's nothing meaningful to archive (snapshot OOM, or a
          * zero-chunk doc) — an empty version row is junk that "recovers" to "". */
         if (snapshot && n > 0)
            version_archive_locked(doc_id, owner_user_id, filename, snapshot);
         for (int i = 0; i < n; i++)
            (void)fts5_delete_chunk_stems_locked(chunk_ids[i], label_stems, body_stems[i]);
         sqlite3_stmt *del = s_db.stmt_doc_delete;
         sqlite3_reset(del);
         sqlite3_bind_int64(del, 1, doc_id);
         if (sqlite3_step(del) == SQLITE_DONE)
            result = (sqlite3_changes(s_db.db) > 0) ? SUCCESS : FAILURE;
         else
            OLOG_ERROR("document_db: delete_indexed delete failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_reset(del);
         /* Commit only if the document delete actually happened; otherwise roll
          * back so the FTS deletes + the version row don't outlive a no-op
          * delete (orphaned contentless-FTS postings, spurious version row). */
         (void)sqlite3_exec(s_db.db, result == SUCCESS ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
      }
      AUTH_DB_UNLOCK();
   } else {
      OLOG_WARNING("document_db: delete_indexed stem OOM (doc %lld) — plain delete fallback",
                   (long long)doc_id);
      result = document_db_delete(doc_id);
   }

   free(snapshot);
   for (int i = 0; i < n; i++) {
      free(chunk_texts[i]);
      if (body_stems)
         free(body_stems[i]);
   }
   free(body_stems);
   free(chunk_texts);
   free(chunk_ids);
   return result;
}

/* Rebuild the entire document_chunks_fts index from scratch (v61 recovery path).
 * Recovers from a partial migration backfill (the v48-style policy advances the
 * schema version even if the backfill is interrupted) and from FTS orphans left
 * when delete_indexed hits its OOM fallback (it leaves rows whose chunks are gone).
 *
 * Clears the whole contentless index with the 'delete-all' command (valid for
 * contentless/external-content FTS5 tables) so we never need each row's ORIGINAL
 * stems, then re-stems + re-inserts every live chunk.  Processes one row per
 * lock-cycle (keyset cursor on the PK): stemming runs OUTSIDE the auth_db leaf
 * lock, the locked insert reuses document_db_chunk_index_fts.  One-shot admin op,
 * so per-row prepares are acceptable; *count_out (optional) gets rows indexed. */
int document_db_rebuild_fts(int *count_out) {
   if (count_out)
      *count_out = 0;

   if (memory_stem_init() != SUCCESS) {
      OLOG_ERROR("document_db: rebuild_fts could not init stemmer");
      return FAILURE;
   }

   /* Phase 1: clear the whole contentless index in one statement. */
   pthread_mutex_lock(&s_db.mutex);
   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return FAILURE;
   }
   int rc = sqlite3_exec(
       s_db.db, "INSERT INTO document_chunks_fts(document_chunks_fts) VALUES('delete-all')", NULL,
       NULL, NULL);
   pthread_mutex_unlock(&s_db.mutex);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("document_db: rebuild_fts delete-all failed (v61 migration not run?): rc=%d",
                   rc);
      return FAILURE;
   }

   /* Phase 2: keyset cursor over every live chunk, one row per lock-cycle. */
   int64_t cursor = 0;
   int total = 0;
   for (;;) {
      int64_t id = 0;
      char filename[DOC_FILENAME_MAX];
      char *text = NULL;
      bool got = false;
      bool oom = false;

      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         return FAILURE;
      }
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT c.id, c.text, d.filename FROM document_chunks c "
                             "JOIN documents d ON d.id = c.document_id "
                             "WHERE c.id > ? ORDER BY c.id ASC LIMIT 1",
                             -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, cursor);
         if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int64(stmt, 0);
            const char *t = (const char *)sqlite3_column_text(stmt, 1);
            const char *fn = (const char *)sqlite3_column_text(stmt, 2);
            text = strdup(t ? t : "");
            if (text) {
               snprintf(filename, sizeof(filename), "%s", fn ? fn : "");
               got = true;
            } else {
               oom = true;
            }
         }
      }
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);

      if (oom) {
         OLOG_ERROR("document_db: rebuild_fts OOM at cursor %lld", (long long)cursor);
         return FAILURE;
      }
      if (!got)
         break;
      cursor = id;

      /* Stem outside the lock (leaf-lock rule). */
      char label_stems[MEMORY_FACT_STEMS_MAX];
      char body_stems[MEMORY_FACT_STEMS_MAX];
      (void)memory_stem_string(filename, label_stems, sizeof(label_stems));
      (void)memory_stem_string(text, body_stems, sizeof(body_stems));
      free(text);

      if (document_db_chunk_index_fts(id, label_stems, body_stems) == SUCCESS)
         total++;
   }

   if (count_out)
      *count_out = total;
   OLOG_INFO("document_db: rebuild_fts indexed %d chunk(s)", total);
   return SUCCESS;
}

/* Lexical (BM25) chunk search — the v61 keyword candidate set.  Mirrors
 * memory_db_fact_search_bm25_since: stem the query, build the OR-of-quoted-stems
 * MATCH expression, run the column-weighted bm25() statement, and sigmoid-
 * normalize each (negated) raw score to [0, 1] for fusion with cosine. */
int document_db_chunk_search_bm25(int user_id,
                                  const char *query,
                                  float label_weight,
                                  float body_weight,
                                  doc_bm25_hit_t *out,
                                  float *out_scores,
                                  int max_hits,
                                  int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!query || !out || !out_scores || max_hits <= 0)
      return FAILURE;

   /* Stem the query and build the MATCH expression OUTSIDE the lock. */
   char stems[MEMORY_FACT_STEMS_MAX];
   int n_terms = memory_stem_string(query, stems, sizeof(stems));
   if (n_terms <= 0)
      return SUCCESS; /* nothing to search; not an error */

   char match_expr[DOC_CHUNK_MATCH_EXPR_MAX];
   int n_emitted = memory_bm25_build_match_expr(stems, match_expr, sizeof(match_expr));
   if (n_emitted <= 0)
      return SUCCESS;

   float midpoint = 0.0f, steepness = 0.0f;
   memory_bm25_get_params(n_emitted, &midpoint, &steepness);

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_search_bm25;
   if (!stmt) {
      /* v61 migration not yet complete — degrade to "no lexical candidates". */
      AUTH_DB_UNLOCK();
      return SUCCESS;
   }
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, match_expr, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 2, (double)label_weight);
   sqlite3_bind_double(stmt, 3, (double)body_weight);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int(stmt, 5, max_hits);

   int count = 0;
   while (count < max_hits && sqlite3_step(stmt) == SQLITE_ROW) {
      doc_bm25_hit_t *h = &out[count];
      h->id = sqlite3_column_int64(stmt, 0);
      h->chunk_index = sqlite3_column_int(stmt, 1);
      col_text_copy(h->text, sizeof(h->text), stmt, 2);
      h->document_id = sqlite3_column_int64(stmt, 3);
      col_text_copy(h->filename, sizeof(h->filename), stmt, 4);
      col_text_copy(h->filetype, sizeof(h->filetype), stmt, 5);
      h->num_chunks = sqlite3_column_int(stmt, 6);
      h->created_at = sqlite3_column_int64(stmt, 7);
      /* Column 8 = bm25(), negative-for-relevant; flip sign before sigmoid. */
      double raw = sqlite3_column_double(stmt, 8);
      out_scores[count] = memory_bm25_normalize((float)(-raw), midpoint, steepness);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return SUCCESS;
}

/* =============================================================================
 * v62: document_versions — soft-archive of pre-mutation content for undo/restore
 * ============================================================================= */

/* Archive the pre-mutation content of a document as a restorable version, then
 * prune that document's history to the keep-last-N cap.  CALLER HOLDS THE LOCK
 * and is inside a transaction (the snapshot must be atomic with the mutation it
 * precedes).  Best-effort: versioning is a safety net, so a failure here is
 * logged and swallowed — it must never abort the mutation.  No-op when
 * versioning is disabled (version_retention_days <= 0). */
static void version_archive_locked(int64_t doc_id,
                                   int user_id,
                                   const char *filename,
                                   const char *text) {
   if (g_config.documents.version_retention_days <= 0)
      return;
   if (!text)
      text = "";

   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO document_versions"
                          "(document_id, user_id, filename, text, archived_at) VALUES(?,?,?,?,?)",
                          -1, &ins, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(ins, 1, doc_id);
      sqlite3_bind_int(ins, 2, user_id);
      sqlite3_bind_text(ins, 3, filename ? filename : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 4, text, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(ins, 5, (int64_t)time(NULL));
      if (sqlite3_step(ins) != SQLITE_DONE)
         OLOG_WARNING("document_db: version archive failed (doc %lld): %s", (long long)doc_id,
                      sqlite3_errmsg(s_db.db));
      sqlite3_finalize(ins);
   }

   /* Keep only the N newest versions for this document. */
   sqlite3_stmt *pr = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "DELETE FROM document_versions WHERE document_id = ?1 AND id NOT IN "
                          "(SELECT id FROM document_versions WHERE document_id = ?1 "
                          " ORDER BY archived_at DESC, id DESC LIMIT ?2)",
                          -1, &pr, NULL) == SQLITE_OK) {
      int keep = g_config.documents.version_keep_per_doc;
      if (keep < 1)
         keep = 1;
      sqlite3_bind_int64(pr, 1, doc_id);
      sqlite3_bind_int(pr, 2, keep);
      (void)sqlite3_step(pr);
      sqlite3_finalize(pr);
   }
}

/* List a document's archived versions, newest first, owner-scoped.  Returns
 * metadata + a short text preview (full text fetched on restore via
 * document_db_version_get_text). */
int document_db_version_list(int user_id,
                             int64_t doc_id,
                             document_version_meta_t *out,
                             int max,
                             int *count_out) {
   if (!out || max <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int count = 0;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id, document_id, filename, substr(text, 1, ?), archived_at "
                          "FROM document_versions WHERE document_id = ? AND user_id = ? "
                          "ORDER BY archived_at DESC, id DESC LIMIT ?",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, DOC_VERSION_PREVIEW_MAX - 1);
      sqlite3_bind_int64(st, 2, doc_id);
      sqlite3_bind_int(st, 3, user_id);
      sqlite3_bind_int(st, 4, max);
      while (count < max && sqlite3_step(st) == SQLITE_ROW) {
         out[count].id = sqlite3_column_int64(st, 0);
         out[count].document_id = sqlite3_column_int64(st, 1);
         col_text_copy(out[count].filename, sizeof(out[count].filename), st, 2);
         col_text_copy(out[count].preview, sizeof(out[count].preview), st, 3);
         out[count].archived_at = sqlite3_column_int64(st, 4);
         count++;
      }
   }
   if (st)
      sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

/* List RECENTLY-DELETED items: the newest archived version of each document that
 * no longer exists (the snapshot outlived a delete), owner-scoped, newest first.
 * The returned `id` is the version id to restore via document_db_version_get_text
 * (the original document is gone, so restore re-creates it).  SQLite's
 * bare-column-from-MAX-row rule makes the GROUP BY pick the newest version's row.
 *
 * Deletion is inferred via `document_id NOT IN documents`.  documents.id is a
 * plain INTEGER PRIMARY KEY (no AUTOINCREMENT), so SQLite can reuse a freed
 * rowid for an unrelated new doc; if that happens before retention prunes the
 * dead snapshots, the deleted item drops off this list and its versions would
 * be mis-attributed to the new doc.  Accepted as a low-likelihood edge (recover
 * re-points snapshots off the dead id via document_db_version_reattach, and
 * retention sweeps the rest); the durable fix would be an explicit deleted_at
 * marker instead of the NOT IN heuristic — deferred until it bites in practice. */
int document_db_version_list_deleted(int user_id,
                                     document_version_meta_t *out,
                                     int max,
                                     int *count_out) {
   if (!out || max <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int count = 0;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id, document_id, filename, substr(text, 1, ?), MAX(archived_at) "
                          "FROM document_versions "
                          "WHERE user_id = ? AND document_id NOT IN (SELECT id FROM documents) "
                          "GROUP BY document_id ORDER BY MAX(archived_at) DESC LIMIT ?",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, DOC_VERSION_PREVIEW_MAX - 1);
      sqlite3_bind_int(st, 2, user_id);
      sqlite3_bind_int(st, 3, max);
      while (count < max && sqlite3_step(st) == SQLITE_ROW) {
         out[count].id = sqlite3_column_int64(st, 0);
         out[count].document_id = sqlite3_column_int64(st, 1);
         col_text_copy(out[count].filename, sizeof(out[count].filename), st, 2);
         col_text_copy(out[count].preview, sizeof(out[count].preview), st, 3);
         out[count].archived_at = sqlite3_column_int64(st, 4);
         count++;
      }
   }
   if (st)
      sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}

/* Fetch a version's FULL text (for restore), owner-scoped.  *text_out is a heap
 * string the caller frees; filename_out/doc_id_out are optional. */
int document_db_version_get_text(int64_t version_id,
                                 int user_id,
                                 char **text_out,
                                 char *filename_out,
                                 size_t fn_sz,
                                 int64_t *doc_id_out) {
   if (!text_out)
      return FAILURE;
   *text_out = NULL;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = FAILURE;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT text, filename, document_id FROM document_versions "
                          "WHERE id = ? AND user_id = ?",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, version_id);
      sqlite3_bind_int(st, 2, user_id);
      if (sqlite3_step(st) == SQLITE_ROW) {
         const unsigned char *t = sqlite3_column_text(st, 0);
         *text_out = strdup(t ? (const char *)t : "");
         if (filename_out)
            col_text_copy(filename_out, fn_sz, st, 1);
         if (doc_id_out)
            *doc_id_out = sqlite3_column_int64(st, 2);
         result = *text_out ? SUCCESS : FAILURE;
      }
   }
   if (st)
      sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

/* Re-point a deleted document's surviving version rows onto the live document
 * created when it was recovered.  Without this the snapshots still reference the
 * dead document_id, so they keep showing in "Recently deleted" (their JOIN to
 * documents fails) and a second restore would create another duplicate; moving
 * them to the new id both clears the ghost and carries history through the
 * delete/recover cycle.  Owner-scoped.  Best-effort (the recover already
 * succeeded); returns SUCCESS even if no rows matched. */
int document_db_version_reattach(int user_id, int64_t old_doc_id, int64_t new_doc_id) {
   if (old_doc_id <= 0 || new_doc_id <= 0 || old_doc_id == new_doc_id)
      return FAILURE;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = FAILURE;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE document_versions SET document_id = ? "
                          "WHERE document_id = ? AND user_id = ?",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, new_doc_id);
      sqlite3_bind_int64(st, 2, old_doc_id);
      sqlite3_bind_int(st, 3, user_id);
      if (sqlite3_step(st) == SQLITE_DONE)
         result = SUCCESS;
      sqlite3_finalize(st);
   }
   AUTH_DB_UNLOCK();
   return result;
}

/* Correct a document's stored chunk count after ingest when embed failures mean
 * fewer chunk rows were written than the chunker produced.  Keeps num_chunks in
 * step with the actual chunk rows (it gates note-vs-document editing). */
int document_db_set_num_chunks(int64_t doc_id, int num_chunks) {
   if (doc_id <= 0 || num_chunks < 0)
      return FAILURE;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = FAILURE;
   if (sqlite3_prepare_v2(s_db.db, "UPDATE documents SET num_chunks = ? WHERE id = ?", -1, &st,
                          NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, num_chunks);
      sqlite3_bind_int64(st, 2, doc_id);
      if (sqlite3_step(st) == SQLITE_DONE)
         result = SUCCESS;
      sqlite3_finalize(st);
   }
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * v63: document_full_text — canonical un-chunked text for multi-chunk doc edits
 * ============================================================================= */

/* UPSERT a document's full text.  Caller holds the lock (use inside the
 * doc-replace transaction or ingest). */
static int full_text_set_locked(int64_t doc_id, const char *text) {
   sqlite3_stmt *st = NULL;
   int result = FAILURE;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO document_full_text(document_id, text) VALUES(?,?) "
                          "ON CONFLICT(document_id) DO UPDATE SET text = excluded.text",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, doc_id);
      sqlite3_bind_text(st, 2, text ? text : "", -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_DONE)
         result = SUCCESS;
      sqlite3_finalize(st);
   }
   return result;
}

/* Store/replace a document's canonical full text (own lock — used by ingest). */
int document_db_full_text_set(int64_t doc_id, const char *text) {
   if (!text)
      return FAILURE;
   AUTH_DB_LOCK_OR_FAIL();
   int result = full_text_set_locked(doc_id, text);
   AUTH_DB_UNLOCK();
   return result;
}

/* Fetch a document's canonical full text, owner-scoped.  *text_out is a heap
 * string (caller frees); FAILURE if the document has no stored full text (a
 * pre-v63 upload that must be re-saved before it can be edited). */
int document_db_full_text_get(int64_t doc_id, int user_id, char **text_out) {
   if (!text_out)
      return FAILURE;
   *text_out = NULL;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = FAILURE;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT f.text FROM document_full_text f "
                          "JOIN documents d ON d.id = f.document_id "
                          "WHERE f.document_id = ? AND d.user_id = ?",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, doc_id);
      sqlite3_bind_int(st, 2, user_id);
      if (sqlite3_step(st) == SQLITE_ROW) {
         const unsigned char *t = sqlite3_column_text(st, 0);
         *text_out = strdup(t ? (const char *)t : "");
         result = *text_out ? SUCCESS : FAILURE;
      }
      sqlite3_finalize(st);
   }
   AUTH_DB_UNLOCK();
   return result;
}

/* Read a multi-chunk document for in-place editing (B1b): owner-checked, must
 * have stored full text.  Returns the filename, full text (heap), and the old
 * chunk ids + body texts (heap arrays, for the caller to stem outside the lock
 * before the atomic replace).  Caller frees full_text_out, old_ids_out, and each
 * old_texts_out[i] + the array. */
int document_db_doc_read_for_edit(int user_id,
                                  int64_t doc_id,
                                  char *filename_out,
                                  size_t fn_sz,
                                  char **full_text_out,
                                  int64_t **old_ids_out,
                                  char ***old_texts_out,
                                  int *n_out) {
   if (!full_text_out || !old_ids_out || !old_texts_out || !n_out)
      return FAILURE;
   *full_text_out = NULL;
   *old_ids_out = NULL;
   *old_texts_out = NULL;
   *n_out = 0;

   AUTH_DB_LOCK_OR_FAIL();
   int result = FAILURE;
   int64_t *ids = NULL;
   char **texts = NULL;
   int n = 0, cap = 0;
   bool oom = false;

   /* Owner-checked full text — also rejects pre-v63 docs (no row) and other
    * users' / global docs. */
   sqlite3_stmt *ft = NULL;
   char *full = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT f.text, d.filename FROM document_full_text f "
                          "JOIN documents d ON d.id = f.document_id "
                          "WHERE f.document_id = ? AND d.user_id = ?",
                          -1, &ft, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(ft, 1, doc_id);
      sqlite3_bind_int(ft, 2, user_id);
      if (sqlite3_step(ft) == SQLITE_ROW) {
         const unsigned char *t = sqlite3_column_text(ft, 0);
         full = strdup(t ? (const char *)t : "");
         if (filename_out)
            col_text_copy(filename_out, fn_sz, ft, 1);
      }
   }
   if (ft)
      sqlite3_finalize(ft);

   if (full) {
      sqlite3_stmt *cs = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT id, text FROM document_chunks WHERE document_id = ? "
                             "ORDER BY chunk_index",
                             -1, &cs, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(cs, 1, doc_id);
         while (!oom && sqlite3_step(cs) == SQLITE_ROW) {
            if (n == cap) {
               int ncap = cap ? cap * 2 : 8;
               int64_t *nids = realloc(ids, (size_t)ncap * sizeof(*nids));
               char **ntexts = realloc(texts, (size_t)ncap * sizeof(*ntexts));
               if (nids)
                  ids = nids;
               if (ntexts)
                  texts = ntexts;
               if (!nids || !ntexts) {
                  oom = true;
                  break;
               }
               cap = ncap;
            }
            ids[n] = sqlite3_column_int64(cs, 0);
            const unsigned char *t = sqlite3_column_text(cs, 1);
            texts[n] = strdup(t ? (const char *)t : "");
            if (!texts[n]) {
               oom = true;
               break;
            }
            n++;
         }
      }
      if (cs)
         sqlite3_finalize(cs);
   }
   AUTH_DB_UNLOCK();

   if (!full || oom) {
      for (int i = 0; i < n; i++)
         free(texts[i]);
      free(texts);
      free(ids);
      free(full);
      return FAILURE;
   }

   *full_text_out = full;
   *old_ids_out = ids;
   *old_texts_out = texts;
   *n_out = n;
   result = SUCCESS;
   return result;
}

/* Atomic in-place replace of a multi-chunk document's content (B1b).  In one
 * transaction: archive the OLD full text as a restorable version, FTS-delete the
 * old chunks, swap in the new chunks + their FTS rows, update num_chunks +
 * file_hash, and store the new full text — keeping doc_id / is_global stable.
 * All stems are computed by the caller (leaf-lock); label_stems is the same for
 * every chunk (the filename is unchanged). */
int document_db_doc_replace(int user_id,
                            int64_t doc_id,
                            const char *new_full_text,
                            const char *new_hash,
                            const char *filename,
                            const char *label_stems,
                            const int64_t *old_ids,
                            const char *const *old_body_stems,
                            int n_old,
                            const doc_replace_chunk_t *new_chunks,
                            int n_new,
                            int dims) {
   if (!new_full_text || !new_hash || n_new <= 0 || dims <= 0)
      return FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   int result = FAILURE;
   if (!s_db.initialized) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   /* Read the old full text for the version snapshot (best-effort). */
   char *old_full = NULL;
   sqlite3_stmt *of = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT text FROM document_full_text WHERE document_id = ?", -1,
                          &of, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(of, 1, doc_id);
      if (sqlite3_step(of) == SQLITE_ROW) {
         const unsigned char *t = sqlite3_column_text(of, 0);
         old_full = strdup(t ? (const char *)t : "");
      }
      sqlite3_finalize(of);
   }

   (void)sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
   bool ok = true;

   version_archive_locked(doc_id, user_id, filename, old_full ? old_full : "");

   for (int i = 0; i < n_old; i++)
      (void)fts5_delete_chunk_stems_locked(old_ids[i], label_stems,
                                           old_body_stems ? old_body_stems[i] : "");

   /* Drop all old chunks. */
   sqlite3_stmt *dc = NULL;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM document_chunks WHERE document_id = ?", -1, &dc,
                          NULL) == SQLITE_OK) {
      sqlite3_bind_int64(dc, 1, doc_id);
      if (sqlite3_step(dc) != SQLITE_DONE)
         ok = false;
      sqlite3_finalize(dc);
   } else {
      ok = false;
   }

   /* Insert the new chunks + FTS rows. */
   sqlite3_stmt *ic = NULL;
   if (ok && sqlite3_prepare_v2(s_db.db,
                                "INSERT INTO document_chunks"
                                "(document_id, chunk_index, text, embedding, embedding_norm, "
                                " created_at) VALUES(?,?,?,?,?,?)",
                                -1, &ic, NULL) == SQLITE_OK) {
      int64_t ts = (int64_t)time(NULL);
      for (int i = 0; i < n_new && ok; i++) {
         sqlite3_reset(ic);
         sqlite3_bind_int64(ic, 1, doc_id);
         sqlite3_bind_int(ic, 2, i);
         sqlite3_bind_text(ic, 3, new_chunks[i].text, -1, SQLITE_TRANSIENT);
         sqlite3_bind_blob(ic, 4, new_chunks[i].embedding, dims * (int)sizeof(float),
                           SQLITE_TRANSIENT);
         sqlite3_bind_double(ic, 5, (double)new_chunks[i].norm);
         sqlite3_bind_int64(ic, 6, ts);
         if (sqlite3_step(ic) != SQLITE_DONE) {
            ok = false;
            break;
         }
         int64_t new_id = sqlite3_last_insert_rowid(s_db.db);
         (void)fts5_insert_chunk_stems_locked(new_id, label_stems, new_chunks[i].body_stems);
      }
      sqlite3_finalize(ic);
   } else {
      ok = false;
   }

   /* Update the document row (num_chunks + hash) — ownership re-checked. */
   sqlite3_stmt *du = NULL;
   if (ok && sqlite3_prepare_v2(s_db.db,
                                "UPDATE documents SET num_chunks = ?, file_hash = ? "
                                "WHERE id = ? AND user_id = ?",
                                -1, &du, NULL) == SQLITE_OK) {
      sqlite3_bind_int(du, 1, n_new);
      sqlite3_bind_text(du, 2, new_hash, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(du, 3, doc_id);
      sqlite3_bind_int(du, 4, user_id);
      if (sqlite3_step(du) != SQLITE_DONE || sqlite3_changes(s_db.db) == 0)
         ok = false;
      sqlite3_finalize(du);
   } else {
      ok = false;
   }

   if (ok && full_text_set_locked(doc_id, new_full_text) != SUCCESS)
      ok = false;

   if (ok) {
      (void)sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
      result = SUCCESS;
   } else {
      (void)sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      OLOG_ERROR("document_db: doc_replace (doc %lld) failed — rolled back", (long long)doc_id);
   }

   AUTH_DB_UNLOCK();
   free(old_full);
   return result;
}

/* Retention sweep: drop versions older than retention_days (global).  The
 * per-document keep-last-N cap is enforced at archive time; this is the age
 * bound.  No-op when retention_days <= 0 (versioning disabled). */
int document_db_version_prune_expired(int retention_days, int *deleted_out) {
   if (deleted_out)
      *deleted_out = 0;
   if (retention_days <= 0)
      return SUCCESS;

   int64_t cutoff = (int64_t)time(NULL) - (int64_t)retention_days * 86400;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int deleted = 0;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM document_versions WHERE archived_at < ?", -1, &st,
                          NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, cutoff);
      if (sqlite3_step(st) == SQLITE_DONE)
         deleted = sqlite3_changes(s_db.db);
      sqlite3_finalize(st);
   }
   AUTH_DB_UNLOCK();
   if (deleted_out)
      *deleted_out = deleted;
   return SUCCESS;
}

/* Stable-id in-place edit of a single-chunk note (v61, M-5).  Gated on
 * filetype == "note" && num_chunks == 1 so a forged request can't rewrite an
 * uploaded multi-chunk document (whose filepath is a real filesystem path).
 * Keeps doc_id / is_global / any inbound pointer stable: swaps the chunk's text +
 * embedding, rebuilds its two FTS rows, and updates the document's filename +
 * file_hash — all in one transaction.  The new embedding is computed by the
 * caller (needs the engine); stemming + hashing-input stay here.  new_hash is
 * supplied by the caller (which already has the sha256 helper). */
int document_db_note_update(int user_id,
                            int64_t doc_id,
                            const char *new_label,
                            const char *new_text,
                            const float *new_emb,
                            int dims,
                            float norm,
                            const char *new_hash) {
   if (!new_label || !new_text || !new_emb || dims <= 0 || !new_hash)
      return FAILURE;

   /* --- Phase 1 (locked): verify note + single chunk + ownership; snapshot the
    * old filename + chunk (id, text) so we can rebuild the contentless FTS. --- */
   char old_filename[DOC_FILENAME_MAX] = "";
   char *old_text = NULL;
   int64_t chunk_id = 0;
   int gate = FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *g = s_db.stmt_doc_get;
   sqlite3_reset(g);
   sqlite3_bind_int64(g, 1, doc_id);
   if (sqlite3_step(g) == SQLITE_ROW) {
      document_t doc;
      row_to_document(g, &doc);
      if (doc.user_id == user_id && doc.num_chunks == 1 && strcmp(doc.filetype, "note") == 0) {
         strncpy(old_filename, doc.filename, sizeof(old_filename) - 1);
         old_filename[sizeof(old_filename) - 1] = '\0';
         gate = SUCCESS;
      }
   }
   sqlite3_reset(g);

   if (gate == SUCCESS) {
      sqlite3_stmt *cs = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT id, text FROM document_chunks WHERE document_id = ? "
                             "ORDER BY chunk_index LIMIT 1",
                             -1, &cs, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(cs, 1, doc_id);
         if (sqlite3_step(cs) == SQLITE_ROW) {
            chunk_id = sqlite3_column_int64(cs, 0);
            const unsigned char *t = sqlite3_column_text(cs, 1);
            old_text = strdup(t ? (const char *)t : "");
         } else {
            gate = FAILURE; /* note row claims 1 chunk but none found */
         }
      }
      if (cs)
         sqlite3_finalize(cs);
   }
   AUTH_DB_UNLOCK();

   if (gate != SUCCESS || !old_text) {
      free(old_text);
      return FAILURE;
   }

   /* --- Phase 2 (unlocked): stem old + new label/body (leaf-lock rule). --- */
   char old_label_stems[MEMORY_FACT_STEMS_MAX], old_body_stems[MEMORY_FACT_STEMS_MAX];
   char new_label_stems[MEMORY_FACT_STEMS_MAX], new_body_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(old_filename, old_label_stems, sizeof(old_label_stems));
   (void)memory_stem_string(old_text, old_body_stems, sizeof(old_body_stems));
   (void)memory_stem_string(new_label, new_label_stems, sizeof(new_label_stems));
   (void)memory_stem_string(new_text, new_body_stems, sizeof(new_body_stems));

   /* --- Phase 3 (locked): snapshot the OLD note for undo, then FTS swap + chunk
    * update + document meta update — all atomic in one transaction.  old_text is
    * kept alive through here for the snapshot and freed after. --- */
   int result = FAILURE;
   pthread_mutex_lock(&s_db.mutex);
   if (s_db.initialized) {
      (void)sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
      bool ok = true;

      /* Snapshot the pre-edit content so this overwrite/edit/append is undoable. */
      version_archive_locked(doc_id, user_id, old_filename, old_text);

      (void)fts5_delete_chunk_stems_locked(chunk_id, old_label_stems, old_body_stems);

      if (s_db.stmt_doc_chunk_update) {
         sqlite3_stmt *cu = s_db.stmt_doc_chunk_update;
         sqlite3_reset(cu);
         sqlite3_bind_text(cu, 1, new_text, -1, SQLITE_TRANSIENT);
         sqlite3_bind_blob(cu, 2, new_emb, dims * (int)sizeof(float), SQLITE_TRANSIENT);
         sqlite3_bind_double(cu, 3, (double)norm);
         sqlite3_bind_int64(cu, 4, (int64_t)time(NULL));
         sqlite3_bind_int64(cu, 5, chunk_id);
         if (sqlite3_step(cu) != SQLITE_DONE)
            ok = false;
         sqlite3_reset(cu);
      } else {
         ok = false;
      }

      /* filename == filepath for notes (the label is the whole identity). */
      sqlite3_stmt *du = NULL;
      if (ok && sqlite3_prepare_v2(s_db.db,
                                   "UPDATE documents SET filename = ?, filepath = ?, "
                                   "file_hash = ? WHERE id = ? AND user_id = ?",
                                   -1, &du, NULL) == SQLITE_OK) {
         sqlite3_bind_text(du, 1, new_label, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(du, 2, new_label, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(du, 3, new_hash, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(du, 4, doc_id);
         sqlite3_bind_int(du, 5, user_id);
         if (sqlite3_step(du) != SQLITE_DONE || sqlite3_changes(s_db.db) == 0)
            ok = false;
      } else {
         ok = false;
      }
      if (du)
         sqlite3_finalize(du);

      (void)fts5_insert_chunk_stems_locked(chunk_id, new_label_stems, new_body_stems);

      if (ok) {
         (void)sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
         result = SUCCESS;
      } else {
         (void)sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         OLOG_ERROR("document_db: note_update (doc %lld) failed — rolled back", (long long)doc_id);
      }
   }
   AUTH_DB_UNLOCK();
   free(old_text);
   return result;
}

/* Exact-label lookup (v61): find a document whose filename equals `label`
 * case-insensitively.  Unlike document_db_find_by_name (substring LIKE), this is
 * EXACT — used for note overwrite routing and delete-by-label so "Bio" never
 * resolves to "Public Bio".  When note_only is true, restricts to filetype
 * 'note'.  Ad-hoc prepared (not a hot path). */
int document_db_find_by_label_exact(int user_id,
                                    const char *label,
                                    bool note_only,
                                    document_t *out) {
   if (!label || !out)
      return FAILURE;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       note_only ? "SELECT id, user_id, filename, filepath, filetype, file_hash, num_chunks, "
                   "is_global, created_at FROM documents WHERE (user_id = ? OR is_global = 1) "
                   "AND filename = ? COLLATE NOCASE AND filetype = 'note' "
                   "ORDER BY created_at DESC LIMIT 1"
                 : "SELECT id, user_id, filename, filepath, filetype, file_hash, num_chunks, "
                   "is_global, created_at FROM documents WHERE (user_id = ? OR is_global = 1) "
                   "AND filename = ? COLLATE NOCASE ORDER BY created_at DESC LIMIT 1";
   int result = FAILURE;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, label, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         row_to_document(stmt, out);
         result = SUCCESS;
      }
   }
   if (stmt)
      sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* List documents filtered by kind (v61): kind 0 = all, 1 = documents only
 * (filetype != 'note'), 2 = notes only.  Paginated; own docs + global.  Ad-hoc
 * prepared (list is not a hot path); mirrors document_db_list's columns. */
int document_db_list_filtered(int user_id,
                              doc_kind_t kind,
                              document_t *out,
                              int limit,
                              int offset,
                              int *count_out) {
   if (!out || limit <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;
   if (limit > DOC_MAX_RESULTS)
      limit = DOC_MAX_RESULTS;
   if (offset < 0)
      offset = 0;

   const char *filter = (kind == DOC_KIND_DOCS)    ? "AND filetype != 'note' "
                        : (kind == DOC_KIND_NOTES) ? "AND filetype = 'note' "
                                                   : "";
   char sql[512];
   snprintf(sql, sizeof(sql),
            "SELECT id, user_id, filename, filepath, filetype, file_hash, num_chunks, is_global, "
            "created_at FROM documents WHERE (user_id = ? OR is_global = 1) %s"
            "ORDER BY created_at DESC LIMIT ? OFFSET ?",
            filter);

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int count = 0;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int(stmt, 2, limit);
      sqlite3_bind_int(stmt, 3, offset);
      while (count < limit && sqlite3_step(stmt) == SQLITE_ROW) {
         row_to_document(stmt, &out[count]);
         count++;
      }
   }
   if (stmt)
      sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = count;
   return SUCCESS;
}
