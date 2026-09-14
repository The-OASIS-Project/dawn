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
 * Document Original Store — document-original consumer of the generic blob
 * store.  Owns the descriptor (blb_ ids, blobs subdir, owner-only read), maps
 * the v68 stmt_blob_* statements into the engine, and confines auth_db internal
 * access to Layer 2.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <stdbool.h>
#include <string.h>

#include "document_original_store.h"
#include "logging.h"
#include "utils/string_utils.h"

/* kind tag for document originals in the shared blobs table. */
#define DOC_ORIGINAL_KIND 0

static const blob_mime_map_t s_doc_mimes[] = {
   { "application/pdf", "pdf" },
   { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx" },
   { "application/msword", "doc" },
   { "text/plain", "txt" },
   { "text/markdown", "md" },
   { "application/rtf", "rtf" },
};

static blob_store_stmts_t s_doc_stmts;
static blob_store_table_t s_doc_desc;
static blob_store_handle_t s_doc_handle = -1;
static bool s_doc_ready = false;

/** @brief Originals are private: owner-only, service token (user_id 0) denied. */
static bool doc_can_read(int user_id, int owner_id, int source) {
   (void)source;
   return (user_id > 0 && owner_id == user_id);
}

int document_originals_init(const documents_config_t *cfg, const char *data_dir) {
   if (s_doc_ready) {
      return BLOB_STORE_SUCCESS;
   }
   if (!cfg || !data_dir || !data_dir[0]) {
      OLOG_ERROR("document_original_store: missing config/data_dir");
      return BLOB_STORE_FAILURE;
   }
   if (blob_store_init(data_dir) != BLOB_STORE_SUCCESS) {
      return BLOB_STORE_FAILURE;
   }

   s_doc_stmts = (blob_store_stmts_t){
      .create = s_db.stmt_blob_create,
      .get = s_db.stmt_blob_get,
      .get_file = s_db.stmt_blob_get_file,
      .del = s_db.stmt_blob_delete,
      .update_access = s_db.stmt_blob_update_access,
      .update_retention = s_db.stmt_blob_update_retention,
      .count_user = s_db.stmt_blob_count_user,
      .sum_bytes_user = s_db.stmt_blob_sum_bytes_user,
      .find_by_hash = s_db.stmt_blob_find_by_hash,
      .delete_old = s_db.stmt_blob_delete_old,
      .cache_total_size = s_db.stmt_blob_cache_total_size,
      .delete_cache_lru = s_db.stmt_blob_delete_by_id, /* unused (no CACHE) but valid */
      .delete_by_id = s_db.stmt_blob_delete_by_id,     /* orphan reclamation */
      .get_expired_ids = s_db.stmt_blob_get_expired_ids,
      .get_cache_lru_ids = s_db.stmt_blob_get_cache_lru_ids,
      .get_orphan_ids = s_db.stmt_blob_get_orphan_ids,
      .stats = s_db.stmt_blob_stats,
   };

   int64_t total_cap = (int64_t)cfg->max_originals_total_mb_per_user * 1024 * 1024;
   s_doc_desc = (blob_store_table_t){
      .name = "document_original",
      .id_prefix = "blb_",
      .subdir = "blobs",
      .sql_table_name = "blobs",
      .mimes = s_doc_mimes,
      .mime_count = sizeof(s_doc_mimes) / sizeof(s_doc_mimes[0]),
      .has_content_hash = true,
      .has_filename_original = true,
      .validate_mime = NULL, /* upload path gates type via extension + extraction */
      .can_read = doc_can_read,
      .stmts = &s_doc_stmts,
      .max_size = (size_t)cfg->max_original_size_mb * 1024 * 1024,
      .max_per_user = cfg->max_originals_per_user,
      .max_total_bytes_per_user = total_cap,
      .retention_days = cfg->original_retention_days,
      .cache_size_mb = 0, /* documents use keep-forever + orphan sweep, not LRU */
   };

   if (blob_store_register(&s_doc_desc, &s_doc_handle) != BLOB_STORE_SUCCESS) {
      OLOG_ERROR("document_original_store: failed to register blob table");
      return BLOB_STORE_FAILURE;
   }
   s_doc_ready = true;
   OLOG_INFO("Document original store initialized (blob handle %d)", s_doc_handle);
   return BLOB_STORE_SUCCESS;
}

void document_originals_shutdown(void) {
   /* The shared blob store is torn down once by the daemon (blob_store_shutdown);
    * just drop our readiness so post-shutdown calls are rejected. */
   s_doc_ready = false;
}

bool document_originals_ready(void) {
   return s_doc_ready;
}

int document_original_save(int user_id,
                           const void *data,
                           size_t size,
                           const char *mime_type,
                           const char *content_hash,
                           const char *filename_original,
                           char *id_out) {
   return blob_store_save(s_doc_handle, user_id, data, size, mime_type, DOC_ORIGINAL_KIND,
                          BLOB_RETAIN_DEFAULT, content_hash, filename_original, id_out);
}

int document_original_get_path(const char *blob_id, int user_id, char *path_out, char *mime_out) {
   return blob_store_get_path(s_doc_handle, blob_id, user_id, path_out, mime_out);
}

int document_original_get_filename(const char *blob_id, int user_id, char *out, size_t out_size) {
   if (!out || out_size == 0) {
      return BLOB_STORE_INVALID;
   }
   out[0] = '\0';
   /* Single metadata read.  get_metadata does NOT apply the access policy, so
    * enforce it here via the same doc_can_read hook (keeps this safe for any
    * caller and avoids a redundant get_path query on the download path). */
   blob_metadata_t md;
   int rc = blob_store_get_metadata(s_doc_handle, blob_id, &md);
   if (rc != BLOB_STORE_SUCCESS) {
      return rc;
   }
   if (!doc_can_read(user_id, md.user_id, md.source)) {
      return BLOB_STORE_FORBIDDEN;
   }
   safe_strncpy(out, md.filename_original, out_size);
   return BLOB_STORE_SUCCESS;
}

int document_original_delete_user(int user_id) {
   return blob_store_delete_user(s_doc_handle, user_id);
}

int document_original_count_user(int user_id, int *count_out) {
   return blob_store_count_user(s_doc_handle, user_id, count_out);
}

int document_original_cleanup(int *deleted_out) {
   return blob_store_cleanup(s_doc_handle, deleted_out);
}

int document_original_cleanup_orphans(int grace_sec, int *deleted_out) {
   return blob_store_cleanup_orphans(s_doc_handle, grace_sec, deleted_out);
}

bool document_original_validate_id(const char *blob_id) {
   return blob_validate_id(blob_id, "blb_");
}
