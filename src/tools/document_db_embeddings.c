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
 * Reading the chunk embeddings a user can access, page by page, and the
 * generation that says when a copy of them is stale (document_embed_cache.c).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <sqlite3.h>
#include <stdbool.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"

/* The documents a user can read: their own and shared ones. */
#define DOC_ACCESSIBLE_DOCS                                                             \
   "(SELECT id FROM documents WHERE user_id = ?1 UNION SELECT id FROM documents WHERE " \
   "is_global = 1)"

int document_db_chunk_count(int user_id, int *count_out) {
   if (!count_out) {
      return FAILURE;
   }
   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db, "SELECT COUNT(*) FROM document_chunks WHERE document_id IN " DOC_ACCESSIBLE_DOCS,
       -1, &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
         *count_out = sqlite3_column_int(stmt, 0);
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW ? SUCCESS : FAILURE;
}

int document_db_chunk_embeddings_page(int user_id,
                                      int dims,
                                      document_chunk_cursor_t *cursor,
                                      int max,
                                      int64_t *ids_out,
                                      float *norms_out,
                                      float *embs_out,
                                      int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   if (!cursor || dims <= 0 || max <= 0 || !ids_out || !norms_out || !embs_out || !count_out) {
      return FAILURE;
   }
   if (cursor->done) {
      return SUCCESS;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *doc_stmt = NULL;
   sqlite3_stmt *chunk_stmt = NULL;
   bool ok = sqlite3_prepare_v2(s_db.db,
                                "SELECT id FROM documents WHERE id >= ?2 AND "
                                "(user_id = ?1 OR is_global = 1) ORDER BY id LIMIT 1",
                                -1, &doc_stmt, NULL) == SQLITE_OK &&
             sqlite3_prepare_v2(s_db.db,
                                "SELECT id, embedding, embedding_norm FROM document_chunks "
                                "WHERE document_id = ?1 AND id > ?2 ORDER BY id LIMIT ?3",
                                -1, &chunk_stmt, NULL) == SQLITE_OK;
   if (!ok) {
      OLOG_ERROR("document_db: prepare chunk_embeddings_page failed: %s", sqlite3_errmsg(s_db.db));
   }
   const int expected = dims * (int)sizeof(float);
   int n = 0;
   int read = 0;
   while (ok && read < max && !cursor->done) {
      /* The document the cursor is in, or the next accessible one. */
      sqlite3_reset(doc_stmt);
      sqlite3_bind_int(doc_stmt, 1, user_id);
      sqlite3_bind_int64(doc_stmt, 2, cursor->doc_id);
      int rc = sqlite3_step(doc_stmt);
      if (rc == SQLITE_DONE) {
         cursor->done = true;
         break;
      }
      if (rc != SQLITE_ROW) {
         ok = false;
         break;
      }
      const int64_t doc = sqlite3_column_int64(doc_stmt, 0);
      if (doc != cursor->doc_id) {
         cursor->doc_id = doc;
         cursor->chunk_id = 0;
      }
      const int want = max - read;
      sqlite3_reset(chunk_stmt);
      sqlite3_bind_int64(chunk_stmt, 1, doc);
      sqlite3_bind_int64(chunk_stmt, 2, cursor->chunk_id);
      sqlite3_bind_int(chunk_stmt, 3, want);
      int got = 0;
      while ((rc = sqlite3_step(chunk_stmt)) == SQLITE_ROW) {
         got++;
         cursor->chunk_id = sqlite3_column_int64(chunk_stmt, 0);
         const void *emb = sqlite3_column_blob(chunk_stmt, 1);
         if (!emb || sqlite3_column_bytes(chunk_stmt, 1) != expected) {
            continue;
         }
         ids_out[n] = cursor->chunk_id;
         norms_out[n] = (float)sqlite3_column_double(chunk_stmt, 2);
         memcpy(embs_out + (size_t)n * (size_t)dims, emb, (size_t)expected);
         n++;
      }
      if (rc != SQLITE_DONE) {
         ok = false;
         break;
      }
      read += got;
      if (got < want) {
         /* This document is finished: move past it. */
         cursor->doc_id = doc + 1;
         cursor->chunk_id = 0;
      }
   }
   sqlite3_finalize(doc_stmt);
   sqlite3_finalize(chunk_stmt);
   AUTH_DB_UNLOCK();
   if (!ok) {
      return FAILURE;
   }
   *count_out = n;
   return SUCCESS;
}

int document_db_chunk_generation(int user_id, document_chunk_gen_t *gen_out) {
   if (!gen_out) {
      return FAILURE;
   }
   memset(gen_out, 0, sizeof(*gen_out));
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = s_db.stmt_doc_chunk_generation; /* NULL before the v89 migration */
   int rc = SQLITE_ERROR;
   if (stmt) {
      sqlite3_reset(stmt);
      sqlite3_bind_int(stmt, 1, user_id);
      rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
         gen_out->own = sqlite3_column_int64(stmt, 0);
         gen_out->shared = sqlite3_column_int64(stmt, 1);
      }
      sqlite3_reset(stmt);
   }
   AUTH_DB_UNLOCK();
   return (rc == SQLITE_ROW) ? SUCCESS : FAILURE;
}
