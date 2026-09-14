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
 * Image Store — thin wrapper over the generic blob store.
 *
 * The filesystem + SQLite engine now lives in blob_store.c; this file keeps the
 * image-specific policy (MIME whitelist, source-based read access) and the
 * stable public API.  The `images` table and on-disk layout are unchanged — the
 * descriptor points at the existing stmt_image_* prepared statements.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#include "utils/string_utils.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <stdbool.h>
#include <string.h>

#include "blob_store.h"
#include "image_store.h"
#include "logging.h"

/* The wrapper returns blob-store codes directly and casts enums by value, so the
 * two code spaces and the MIME buffer bound must line up. */
_Static_assert(IMAGE_STORE_SUCCESS == BLOB_STORE_SUCCESS &&
                   IMAGE_STORE_FAILURE == BLOB_STORE_FAILURE &&
                   IMAGE_STORE_NOT_FOUND == BLOB_STORE_NOT_FOUND &&
                   IMAGE_STORE_FORBIDDEN == BLOB_STORE_FORBIDDEN &&
                   IMAGE_STORE_LIMIT_EXCEEDED == BLOB_STORE_LIMIT_EXCEEDED &&
                   IMAGE_STORE_INVALID == BLOB_STORE_INVALID &&
                   IMAGE_STORE_TOO_LARGE == BLOB_STORE_TOO_LARGE,
               "image/blob error codes must match for direct return");
_Static_assert(IMAGE_RETAIN_DEFAULT == (int)BLOB_RETAIN_DEFAULT &&
                   IMAGE_RETAIN_PERMANENT == (int)BLOB_RETAIN_PERMANENT &&
                   IMAGE_RETAIN_CACHE == (int)BLOB_RETAIN_CACHE,
               "image/blob retention enums must match");
_Static_assert(IMAGE_MIME_MAX <= BLOB_MIME_MAX, "image MIME buffer must fit blob MIME");
_Static_assert(IMAGE_ID_LEN == BLOB_ID_LEN, "image/blob id buffer must match");

/* =============================================================================
 * Image-specific policy + descriptor
 * ============================================================================= */

static const blob_mime_map_t s_image_mimes[] = {
   { "image/jpeg", "jpg" },
   { "image/png", "png" },
   { "image/gif", "gif" },
   { "image/webp", "webp" },
};

static blob_store_stmts_t s_image_stmts;
static blob_store_table_t s_image_desc;
static blob_store_handle_t s_image_handle = -1;
static bool s_image_ready = false;

/** @brief Read access: UPLOAD and MMS are private (owner-only, service token
 *  denied); other sources are shareable.  Reproduces the pre-extraction rule. */
static bool image_can_read(int user_id, int owner_id, int source) {
   if ((source == IMAGE_SOURCE_UPLOAD || source == IMAGE_SOURCE_MMS) &&
       (user_id == 0 || owner_id != user_id)) {
      return false;
   }
   return true;
}

bool image_store_validate_mime(const char *mime_type) {
   if (!mime_type) {
      return false;
   }
   /* Allowed types — NO SVG (XSS risk). */
   return (strcmp(mime_type, "image/jpeg") == 0 || strcmp(mime_type, "image/png") == 0 ||
           strcmp(mime_type, "image/gif") == 0 || strcmp(mime_type, "image/webp") == 0);
}

bool image_store_validate_id(const char *id) {
   return blob_validate_id(id, "img_");
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int image_store_init(const image_store_config_t *config) {
   if (s_image_ready) {
      return IMAGE_STORE_SUCCESS;
   }
   if (!config || !config->data_dir || !config->data_dir[0]) {
      OLOG_ERROR("Image store: no data_dir configured");
      return IMAGE_STORE_FAILURE;
   }
   if (blob_store_init(config->data_dir) != BLOB_STORE_SUCCESS) {
      return IMAGE_STORE_FAILURE;
   }

   /* Point the descriptor at the existing image prepared statements (prepared
    * during auth_db_init, which runs before this). */
   s_image_stmts = (blob_store_stmts_t){
      .create = s_db.stmt_image_create,
      .get = s_db.stmt_image_get,
      .get_file = s_db.stmt_image_get_file,
      .del = s_db.stmt_image_delete,
      .update_access = s_db.stmt_image_update_access,
      .update_retention = s_db.stmt_image_update_retention,
      .count_user = s_db.stmt_image_count_user,
      .sum_bytes_user = NULL, /* images have no aggregate-byte budget */
      .find_by_hash = NULL,   /* images have no content-hash dedup */
      .delete_old = s_db.stmt_image_delete_old,
      .cache_total_size = s_db.stmt_image_cache_total_size,
      .delete_cache_lru = s_db.stmt_image_delete_cache_lru,
      .delete_by_id =
          s_db.stmt_image_delete_cache_lru, /* id-only DELETE (images: no orphan sweep) */
      .get_expired_ids = s_db.stmt_image_get_expired_ids,
      .get_cache_lru_ids = s_db.stmt_image_get_cache_lru_ids,
      .get_orphan_ids = NULL, /* images are not orphan-swept */
      .stats = s_db.stmt_image_stats,
   };

   s_image_desc = (blob_store_table_t){
      .name = "image",
      .id_prefix = "img_",
      .subdir = "images",
      .sql_table_name = "images",
      .mimes = s_image_mimes,
      .mime_count = sizeof(s_image_mimes) / sizeof(s_image_mimes[0]),
      .has_content_hash = false,
      .has_filename_original = false,
      .validate_mime = image_store_validate_mime,
      .can_read = image_can_read,
      .stmts = &s_image_stmts,
      .max_size = config->max_size > 0 ? config->max_size : IMAGE_MAX_SIZE_DEFAULT,
      .max_per_user = config->max_per_user > 0 ? config->max_per_user : IMAGE_MAX_PER_USER_DEFAULT,
      .max_total_bytes_per_user = 0,
      .retention_days = config->retention_days,
      .cache_size_mb = config->cache_size_mb > 0 ? config->cache_size_mb
                                                 : IMAGE_CACHE_SIZE_MB_DEFAULT,
   };

   if (blob_store_register(&s_image_desc, &s_image_handle) != BLOB_STORE_SUCCESS) {
      OLOG_ERROR("Image store: failed to register blob table");
      return IMAGE_STORE_FAILURE;
   }
   s_image_ready = true;
   OLOG_INFO("Image store initialized (blob handle %d)", s_image_handle);
   return IMAGE_STORE_SUCCESS;
}

void image_store_shutdown(void) {
   /* The shared blob store is torn down once by the daemon (blob_store_shutdown);
    * here we only drop image readiness so post-shutdown calls are rejected. */
   s_image_ready = false;
   OLOG_INFO("Image store shutdown");
}

bool image_store_is_ready(void) {
   return s_image_ready;
}

/* =============================================================================
 * Operations — delegate to the engine
 * ============================================================================= */

int image_store_save(int user_id,
                     const void *data,
                     size_t size,
                     const char *mime_type,
                     char *id_out) {
   return image_store_save_ex(user_id, data, size, mime_type, IMAGE_SOURCE_UPLOAD,
                              IMAGE_RETAIN_DEFAULT, id_out);
}

int image_store_save_ex(int user_id,
                        const void *data,
                        size_t size,
                        const char *mime_type,
                        image_source_t source,
                        image_retention_t retention,
                        char *id_out) {
   return blob_store_save(s_image_handle, user_id, data, size, mime_type, (int)source,
                          (blob_retention_t)retention, NULL /*hash*/, NULL /*filename_original*/,
                          id_out);
}

int image_store_get_path(const char *id, int user_id, char *path_out, char *mime_out) {
   /* The engine writes up to BLOB_MIME_MAX into mime_out; image callers pass an
    * IMAGE_MIME_MAX buffer, so stage through a local and truncate. */
   char mime_buf[BLOB_MIME_MAX];
   int rc = blob_store_get_path(s_image_handle, id, user_id, path_out, mime_out ? mime_buf : NULL);
   if (rc == BLOB_STORE_SUCCESS && mime_out) {
      safe_strncpy(mime_out, mime_buf, IMAGE_MIME_MAX);
   }
   return rc;
}

int image_store_get_metadata(const char *id, image_metadata_t *metadata_out) {
   if (!metadata_out) {
      return IMAGE_STORE_INVALID;
   }
   blob_metadata_t m;
   int rc = blob_store_get_metadata(s_image_handle, id, &m);
   if (rc != BLOB_STORE_SUCCESS) {
      return rc;
   }
   safe_strscpy(metadata_out->id, m.id);
   metadata_out->user_id = m.user_id;
   safe_strscpy(metadata_out->mime_type, m.mime_type);
   metadata_out->size = m.size;
   safe_strscpy(metadata_out->filename, m.filename);
   metadata_out->source = (image_source_t)m.source;
   metadata_out->retention_policy = (image_retention_t)m.retention_policy;
   metadata_out->created_at = m.created_at;
   metadata_out->last_accessed = m.last_accessed;
   return IMAGE_STORE_SUCCESS;
}

int image_store_delete(const char *id, int user_id) {
   return blob_store_delete(s_image_handle, id, user_id);
}

int image_store_update_retention(const char *id, int user_id, image_retention_t retention) {
   return blob_store_update_retention(s_image_handle, id, user_id, (blob_retention_t)retention);
}

int image_store_count_user(int user_id, int *count_out) {
   return blob_store_count_user(s_image_handle, user_id, count_out);
}

int image_store_delete_user(int user_id) {
   return blob_store_delete_user(s_image_handle, user_id);
}

int image_store_cleanup(int *deleted_out) {
   return blob_store_cleanup(s_image_handle, deleted_out);
}

int image_store_stats(int *total_count, int64_t *total_bytes) {
   return blob_store_stats(s_image_handle, total_count, total_bytes);
}
