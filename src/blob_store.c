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
 * Generic Blob Store — engine.
 *
 * Filesystem-backed blob storage with SQLite metadata, generalized from
 * image_store.c.  Metadata ops hold the auth_db leaf lock; file I/O happens
 * outside it.  One engine serves multiple consumer tables (image, document
 * original, future audio/video), each registered with its own descriptor.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#include "utils/string_utils.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "blob_store.h"
#include "logging.h"

/* Skip a last_accessed write unless the stored value is at least this stale,
 * to limit flash wear from read-driven metadata churn. */
#define BLOB_ACCESS_UPDATE_INTERVAL_SEC 900 /* 15 min */

/* Per-pass row cap for the cleanup/orphan sweeps.  Bounds the stack scratch
 * arrays; the orphan sweep loops until a partial batch drains the queue.  Keep
 * in lockstep with the `LIMIT` in the get_expired/get_cache_lru/get_orphan
 * statements (auth_db_statements.c). */
#define BLOB_CLEANUP_BATCH_SIZE 100

/* Bind params on the create statement: 8 fixed (id, user_id, source/kind,
 * retention, mime, size, filename, created_at) plus one each for the optional
 * content_hash / filename_original columns (see blob_store_save).  Validated
 * against the descriptor flags at registration. */
#define BLOB_CREATE_BASE_PARAMS 8

/* =============================================================================
 * Static State — registry of consumer tables
 * ============================================================================= */

static struct {
   bool initialized;
   char data_dir[BLOB_PATH_MAX];
   blob_store_table_t tables[BLOB_STORE_MAX_TABLES];
   char dirs[BLOB_STORE_MAX_TABLES][BLOB_PATH_MAX]; /* {data_dir}/{subdir} */
   int n_tables;
} s_blob = { 0 };

static blob_store_table_t *tbl(blob_store_handle_t h) {
   if (h < 0 || h >= s_blob.n_tables) {
      return NULL;
   }
   return &s_blob.tables[h];
}

/* =============================================================================
 * Internal helpers (file I/O — no DB)
 * ============================================================================= */

static void build_filepath(const char *dir, const char *filename, char *out, size_t out_size) {
   /* dir is a short config-derived path and filenames are short id.ext names, so
    * this never truncates in practice.  Bound each field to half the output so
    * the result provably fits (-Wformat-truncation). */
   int half = (int)(out_size > 2 ? out_size / 2 - 1 : 0);
   snprintf(out, out_size, "%.*s/%.*s", half, dir, half, filename);
}

/** @brief Atomic write (tmp + fsync + rename), O_NOFOLLOW against symlinks. */
static int write_file_atomic(const char *dir,
                             const char *final_path,
                             const char *filename,
                             const void *data,
                             size_t size) {
   char tmppath[BLOB_PATH_MAX + 16];
   int tp_half = (int)((sizeof(tmppath) - 7) / 2); /* "/." + ".tmp" + NUL = 7 */
   snprintf(tmppath, sizeof(tmppath), "%.*s/.%.*s.tmp", tp_half, dir, tp_half, filename);

   int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0640);
   if (fd < 0) {
      OLOG_ERROR("Blob store: failed to create temp file %s: %s", tmppath, strerror(errno));
      return BLOB_STORE_FAILURE;
   }

   const unsigned char *p = (const unsigned char *)data;
   size_t remaining = size;
   while (remaining > 0) {
      ssize_t written = write(fd, p, remaining);
      if (written < 0) {
         if (errno == EINTR) {
            continue;
         }
         OLOG_ERROR("Blob store: write failed for %s: %s", tmppath, strerror(errno));
         close(fd);
         unlink(tmppath);
         return BLOB_STORE_FAILURE;
      }
      p += written;
      remaining -= (size_t)written;
   }

   if (fsync(fd) != 0) {
      OLOG_WARNING("Blob store: fsync failed for %s: %s", tmppath, strerror(errno));
   }
   close(fd);

   if (rename(tmppath, final_path) != 0) {
      OLOG_ERROR("Blob store: rename failed %s -> %s: %s", tmppath, final_path, strerror(errno));
      unlink(tmppath);
      return BLOB_STORE_FAILURE;
   }
   return BLOB_STORE_SUCCESS;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int blob_store_init(const char *data_dir) {
   if (s_blob.initialized) {
      return BLOB_STORE_SUCCESS;
   }
   if (!auth_db_is_ready()) {
      OLOG_ERROR("Blob store: auth_db not initialized");
      return BLOB_STORE_FAILURE;
   }
   if (!data_dir || !data_dir[0]) {
      OLOG_ERROR("Blob store: no data_dir configured");
      return BLOB_STORE_FAILURE;
   }
   snprintf(s_blob.data_dir, sizeof(s_blob.data_dir), "%s", data_dir);
   s_blob.n_tables = 0;
   s_blob.initialized = true;
   OLOG_INFO("Blob store initialized: data_dir=%s", s_blob.data_dir);
   return BLOB_STORE_SUCCESS;
}

int blob_store_register(const blob_store_table_t *desc, blob_store_handle_t *handle_out) {
   if (!desc || !handle_out || !desc->id_prefix || strlen(desc->id_prefix) != 4 || !desc->subdir ||
       !desc->stmts) {
      return BLOB_STORE_INVALID;
   }
   if (!s_blob.initialized) {
      OLOG_ERROR("Blob store: register before init");
      return BLOB_STORE_FAILURE;
   }
   if (s_blob.n_tables >= BLOB_STORE_MAX_TABLES) {
      OLOG_ERROR("Blob store: too many tables registered (max %d)", BLOB_STORE_MAX_TABLES);
      return BLOB_STORE_FAILURE;
   }

   /* Fail loudly if the externally-owned create statement's bind-parameter count
    * disagrees with the descriptor's column-shape flags.  Otherwise a mismatch
    * surfaces only as a silent SQLITE_RANGE bind on the first save. */
   if (!desc->stmts->create) {
      OLOG_ERROR("Blob store: '%s' descriptor has no create statement",
                 desc->name ? desc->name : "?");
      return BLOB_STORE_INVALID;
   }
   int want_params = BLOB_CREATE_BASE_PARAMS + (desc->has_content_hash ? 1 : 0) +
                     (desc->has_filename_original ? 1 : 0);
   int have_params = sqlite3_bind_parameter_count(desc->stmts->create);
   if (have_params != want_params) {
      OLOG_ERROR("Blob store: '%s' create statement binds %d params, expected %d "
                 "(has_content_hash=%d, has_filename_original=%d)",
                 desc->name ? desc->name : "?", have_params, want_params, desc->has_content_hash,
                 desc->has_filename_original);
      return BLOB_STORE_FAILURE;
   }

   int idx = s_blob.n_tables;
   char *dir = s_blob.dirs[idx];
   /* Bound each field to half the buffer so the result provably fits
    * (-Wformat-truncation); the length check below rejects a pathological dir. */
   int dir_half = (int)(BLOB_PATH_MAX > 2 ? BLOB_PATH_MAX / 2 - 1 : 0);
   snprintf(dir, BLOB_PATH_MAX, "%.*s/%.*s", dir_half, s_blob.data_dir, dir_half, desc->subdir);

   /* Leave room for '/' + filename (id.ext, up to BLOB_FILENAME_MAX). */
   if (strlen(dir) + 1 + BLOB_FILENAME_MAX >= BLOB_PATH_MAX) {
      OLOG_ERROR("Blob store: '%s' dir path too long", desc->name ? desc->name : "?");
      return BLOB_STORE_FAILURE;
   }
   if (mkdir(dir, 0750) != 0 && errno != EEXIST) {
      OLOG_ERROR("Blob store: failed to create %s: %s", dir, strerror(errno));
      return BLOB_STORE_FAILURE;
   }

   s_blob.tables[idx] = *desc; /* shallow copy; pointed-to data is consumer-static */
   s_blob.n_tables++;
   *handle_out = idx;
   OLOG_INFO("Blob store: registered '%s' dir=%s (dedup=%s, max_size=%zu, per_user=%d)",
             desc->name ? desc->name : "?", dir, desc->has_content_hash ? "yes" : "no",
             desc->max_size, desc->max_per_user);
   return BLOB_STORE_SUCCESS;
}

void blob_store_shutdown(void) {
   s_blob.initialized = false;
   s_blob.n_tables = 0;
   OLOG_INFO("Blob store shutdown");
}

bool blob_store_is_ready(void) {
   return s_blob.initialized;
}

/* =============================================================================
 * Query helpers
 * ============================================================================= */

int blob_store_count_user(blob_store_handle_t handle, int user_id, int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   blob_store_table_t *t = tbl(handle);
   if (!t) {
      return BLOB_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->count_user);
   sqlite3_bind_int(t->stmts->count_user, 1, user_id);
   int count = 0;
   if (sqlite3_step(t->stmts->count_user) == SQLITE_ROW) {
      count = sqlite3_column_int(t->stmts->count_user, 0);
   }
   sqlite3_reset(t->stmts->count_user);
   AUTH_DB_UNLOCK();

   if (count_out) {
      *count_out = count;
   }
   return BLOB_STORE_SUCCESS;
}

/** @brief Sum of stored bytes for a user (aggregate budget); 0 if unsupported. */
static int64_t sum_bytes_user_locked(blob_store_table_t *t, int user_id) {
   if (!t->stmts->sum_bytes_user) {
      return 0;
   }
   sqlite3_reset(t->stmts->sum_bytes_user);
   sqlite3_bind_int(t->stmts->sum_bytes_user, 1, user_id);
   int64_t bytes = 0;
   if (sqlite3_step(t->stmts->sum_bytes_user) == SQLITE_ROW) {
      bytes = sqlite3_column_int64(t->stmts->sum_bytes_user, 0);
   }
   sqlite3_reset(t->stmts->sum_bytes_user);
   return bytes;
}

int blob_store_find_by_hash(blob_store_handle_t handle,
                            int user_id,
                            const char *content_hash,
                            char *id_out) {
   blob_store_table_t *t = tbl(handle);
   if (!t || !content_hash || !id_out) {
      return BLOB_STORE_INVALID;
   }
   if (!t->has_content_hash || !t->stmts->find_by_hash) {
      return BLOB_STORE_NOT_FOUND;
   }

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->find_by_hash);
   sqlite3_bind_int(t->stmts->find_by_hash, 1, user_id);
   sqlite3_bind_text(t->stmts->find_by_hash, 2, content_hash, -1, SQLITE_STATIC);
   int rc = sqlite3_step(t->stmts->find_by_hash);
   int result = BLOB_STORE_NOT_FOUND;
   if (rc == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(t->stmts->find_by_hash, 0);
      if (id) {
         safe_strncpy(id_out, id, BLOB_ID_LEN);
         result = BLOB_STORE_SUCCESS;
      }
   }
   sqlite3_reset(t->stmts->find_by_hash);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Save
 * ============================================================================= */

int blob_store_save(blob_store_handle_t handle,
                    int user_id,
                    const void *data,
                    size_t size,
                    const char *mime_type,
                    int source,
                    blob_retention_t retention,
                    const char *content_hash,
                    const char *filename_original,
                    char *id_out) {
   if (!data || size == 0 || !mime_type || !id_out) {
      return BLOB_STORE_INVALID;
   }
   blob_store_table_t *t = tbl(handle);
   if (!t || !s_blob.initialized) {
      return BLOB_STORE_FAILURE;
   }
   if (t->validate_mime && !t->validate_mime(mime_type)) {
      return BLOB_STORE_INVALID;
   }
   if (t->max_size && size > t->max_size) {
      return BLOB_STORE_TOO_LARGE;
   }
   /* A dedup table's content_hash column is NOT NULL — fail fast at the boundary
    * rather than hitting a SQLITE_CONSTRAINT at INSERT. */
   if (t->has_content_hash && (!content_hash || !content_hash[0])) {
      return BLOB_STORE_INVALID;
   }

   /* Dedup BEFORE charging caps: a repeat upload adds no storage. */
   if (content_hash && t->has_content_hash && t->stmts->find_by_hash) {
      char existing[BLOB_ID_LEN];
      if (blob_store_find_by_hash(handle, user_id, content_hash, existing) == BLOB_STORE_SUCCESS) {
         safe_strncpy(id_out, existing, BLOB_ID_LEN);
         return BLOB_STORE_SUCCESS;
      }
   }

   /* Caps (only on a genuine new write; skip system-wide user_id == 0). */
   if (user_id > 0) {
      if (t->max_per_user > 0) {
         int count = 0;
         if (blob_store_count_user(handle, user_id, &count) != BLOB_STORE_SUCCESS) {
            return BLOB_STORE_FAILURE;
         }
         if (count >= t->max_per_user) {
            return BLOB_STORE_LIMIT_EXCEEDED;
         }
      }
      if (t->max_total_bytes_per_user > 0 && t->stmts->sum_bytes_user) {
         AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
         int64_t used = sum_bytes_user_locked(t, user_id);
         AUTH_DB_UNLOCK();
         if (used + (int64_t)size > t->max_total_bytes_per_user) {
            return BLOB_STORE_LIMIT_EXCEEDED;
         }
      }
   }

   if (blob_generate_id(t->id_prefix, id_out) != BLOB_STORE_SUCCESS) {
      return BLOB_STORE_FAILURE;
   }

   char filename[BLOB_FILENAME_MAX];
   blob_build_filename(id_out, mime_type, t->mimes, t->mime_count, filename, sizeof(filename));

   const char *dir = s_blob.dirs[handle];
   char filepath[BLOB_PATH_MAX];
   build_filepath(dir, filename, filepath, sizeof(filepath));

   /* Write the file OUTSIDE the lock. */
   int write_result = write_file_atomic(dir, filepath, filename, data, size);
   if (write_result != BLOB_STORE_SUCCESS) {
      return write_result;
   }

   /* Insert metadata under the lock.  Columns 1-8 are shared by every table;
    * 9 (content_hash) and 10 (filename_original) bind only when the table
    * declares them, so one statement shape serves images and blobs alike. */
   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   time_t now = time(NULL);
   sqlite3_reset(t->stmts->create);
   sqlite3_bind_text(t->stmts->create, 1, id_out, -1, SQLITE_STATIC);
   sqlite3_bind_int(t->stmts->create, 2, user_id);
   sqlite3_bind_int(t->stmts->create, 3, source);
   sqlite3_bind_int(t->stmts->create, 4, (int)retention);
   sqlite3_bind_text(t->stmts->create, 5, mime_type, -1, SQLITE_STATIC);
   sqlite3_bind_int64(t->stmts->create, 6, (int64_t)size);
   sqlite3_bind_text(t->stmts->create, 7, filename, -1, SQLITE_STATIC);
   sqlite3_bind_int64(t->stmts->create, 8, (int64_t)now);
   if (t->has_content_hash) {
      if (content_hash) {
         sqlite3_bind_text(t->stmts->create, 9, content_hash, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(t->stmts->create, 9);
      }
   }
   if (t->has_filename_original) {
      if (filename_original) {
         sqlite3_bind_text(t->stmts->create, 10, filename_original, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(t->stmts->create, 10);
      }
   }
   int rc = sqlite3_step(t->stmts->create);
   sqlite3_reset(t->stmts->create);
   AUTH_DB_UNLOCK();

   if (rc == SQLITE_DONE) {
      OLOG_INFO("Blob store [%s]: saved %s (%zu bytes, %s, source=%d)", t->name ? t->name : "?",
                id_out, size, mime_type, source);
      return BLOB_STORE_SUCCESS;
   }

   /* A UNIQUE(user_id, content_hash) violation means a concurrent identical
    * save won the race: drop our file and return the winner's id. */
   unlink(filepath);
   if (rc == SQLITE_CONSTRAINT && content_hash && t->has_content_hash) {
      char existing[BLOB_ID_LEN];
      if (blob_store_find_by_hash(handle, user_id, content_hash, existing) == BLOB_STORE_SUCCESS) {
         safe_strncpy(id_out, existing, BLOB_ID_LEN);
         return BLOB_STORE_SUCCESS;
      }
   }
   OLOG_ERROR("Blob store [%s]: failed to save metadata for %s: %s", t->name ? t->name : "?",
              id_out, sqlite3_errmsg(s_db.db));
   return BLOB_STORE_FAILURE;
}

/* =============================================================================
 * Read
 * ============================================================================= */

int blob_store_get_path(blob_store_handle_t handle,
                        const char *id,
                        int user_id,
                        char *path_out,
                        char *mime_out) {
   blob_store_table_t *t = tbl(handle);
   if (!t || !id || !path_out) {
      return BLOB_STORE_INVALID;
   }
   if (!blob_validate_id(id, t->id_prefix)) {
      return BLOB_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->get_file);
   sqlite3_bind_text(t->stmts->get_file, 1, id, -1, SQLITE_STATIC);
   if (sqlite3_step(t->stmts->get_file) != SQLITE_ROW) {
      sqlite3_reset(t->stmts->get_file);
      AUTH_DB_UNLOCK();
      return BLOB_STORE_NOT_FOUND;
   }

   /* Columns: 0 filename, 1 user_id, 2 source, 3 mime, 4 last_accessed. */
   const char *filename = (const char *)sqlite3_column_text(t->stmts->get_file, 0);
   int owner_id = sqlite3_column_int(t->stmts->get_file, 1);
   int source = sqlite3_column_int(t->stmts->get_file, 2);
   const char *mime = (const char *)sqlite3_column_text(t->stmts->get_file, 3);
   time_t last_acc = (time_t)sqlite3_column_int64(t->stmts->get_file, 4);

   if (t->can_read && !t->can_read(user_id, owner_id, source)) {
      sqlite3_reset(t->stmts->get_file);
      AUTH_DB_UNLOCK();
      return BLOB_STORE_FORBIDDEN;
   }

   char filename_buf[BLOB_FILENAME_MAX];
   if (filename && blob_validate_db_filename(filename)) {
      safe_strscpy(filename_buf, filename);
   } else {
      sqlite3_reset(t->stmts->get_file);
      AUTH_DB_UNLOCK();
      return BLOB_STORE_FAILURE;
   }

   if (mime_out && mime) {
      safe_strncpy(mime_out, mime, BLOB_MIME_MAX);
   }
   sqlite3_reset(t->stmts->get_file);

   /* Rate-limit last_accessed writes: only if stale enough (flash wear). */
   time_t now = time(NULL);
   if (now - last_acc > BLOB_ACCESS_UPDATE_INTERVAL_SEC) {
      sqlite3_reset(t->stmts->update_access);
      sqlite3_bind_int64(t->stmts->update_access, 1, (int64_t)now);
      sqlite3_bind_text(t->stmts->update_access, 2, id, -1, SQLITE_STATIC);
      sqlite3_step(t->stmts->update_access);
      sqlite3_reset(t->stmts->update_access);
   }
   AUTH_DB_UNLOCK();

   build_filepath(s_blob.dirs[handle], filename_buf, path_out, BLOB_PATH_MAX);
   return BLOB_STORE_SUCCESS;
}

int blob_store_get_metadata(blob_store_handle_t handle, const char *id, blob_metadata_t *out) {
   blob_store_table_t *t = tbl(handle);
   if (!t || !id || !out) {
      return BLOB_STORE_INVALID;
   }
   if (!blob_validate_id(id, t->id_prefix)) {
      return BLOB_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->get);
   sqlite3_bind_text(t->stmts->get, 1, id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(t->stmts->get);
   if (rc != SQLITE_ROW) {
      sqlite3_reset(t->stmts->get);
      AUTH_DB_UNLOCK();
      return BLOB_STORE_NOT_FOUND;
   }

   /* Columns: id,user_id,mime,size,filename,source,retention,created_at,last_accessed. */
   const char *cid = (const char *)sqlite3_column_text(t->stmts->get, 0);
   safe_strscpy(out->id, cid ? cid : "");
   out->user_id = sqlite3_column_int(t->stmts->get, 1);
   const char *mime = (const char *)sqlite3_column_text(t->stmts->get, 2);
   safe_strscpy(out->mime_type, mime ? mime : "");
   out->size = (size_t)sqlite3_column_int64(t->stmts->get, 3);
   const char *fn = (const char *)sqlite3_column_text(t->stmts->get, 4);
   safe_strscpy(out->filename, fn ? fn : "");
   out->source = sqlite3_column_int(t->stmts->get, 5);
   out->retention_policy = (blob_retention_t)sqlite3_column_int(t->stmts->get, 6);
   out->created_at = (time_t)sqlite3_column_int64(t->stmts->get, 7);
   out->last_accessed = (time_t)sqlite3_column_int64(t->stmts->get, 8);
   /* Column 9 (filename_original) exists only on tables that declare it. */
   out->filename_original[0] = '\0';
   if (t->has_filename_original) {
      const char *fo = (const char *)sqlite3_column_text(t->stmts->get, 9);
      if (fo) {
         safe_strscpy(out->filename_original, fo);
      }
   }

   sqlite3_reset(t->stmts->get);
   AUTH_DB_UNLOCK();
   return BLOB_STORE_SUCCESS;
}

/* =============================================================================
 * Mutate
 * ============================================================================= */

int blob_store_update_retention(blob_store_handle_t handle,
                                const char *id,
                                int user_id,
                                blob_retention_t retention) {
   blob_store_table_t *t = tbl(handle);
   if (!t || !id || !blob_validate_id(id, t->id_prefix)) {
      return BLOB_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->update_retention);
   sqlite3_bind_int(t->stmts->update_retention, 1, (int)retention);
   sqlite3_bind_text(t->stmts->update_retention, 2, id, -1, SQLITE_STATIC);
   sqlite3_bind_int(t->stmts->update_retention, 3, user_id);
   sqlite3_bind_int(t->stmts->update_retention, 4, user_id);
   int rc = sqlite3_step(t->stmts->update_retention);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(t->stmts->update_retention);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return BLOB_STORE_FAILURE;
   }
   return (changes > 0) ? BLOB_STORE_SUCCESS : BLOB_STORE_NOT_FOUND;
}

int blob_store_delete(blob_store_handle_t handle, const char *id, int user_id) {
   blob_store_table_t *t = tbl(handle);
   if (!t || !id || !blob_validate_id(id, t->id_prefix)) {
      return BLOB_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->get_file);
   sqlite3_bind_text(t->stmts->get_file, 1, id, -1, SQLITE_STATIC);
   if (sqlite3_step(t->stmts->get_file) != SQLITE_ROW) {
      sqlite3_reset(t->stmts->get_file);
      AUTH_DB_UNLOCK();
      return BLOB_STORE_NOT_FOUND;
   }
   const char *filename = (const char *)sqlite3_column_text(t->stmts->get_file, 0);
   int owner_id = sqlite3_column_int(t->stmts->get_file, 1);

   /* user_id == 0 bypasses the owner check (admin / maintenance). */
   if (user_id != 0 && owner_id != user_id) {
      sqlite3_reset(t->stmts->get_file);
      AUTH_DB_UNLOCK();
      return BLOB_STORE_FORBIDDEN;
   }

   char filename_buf[BLOB_FILENAME_MAX] = { 0 };
   if (filename && blob_validate_db_filename(filename)) {
      safe_strscpy(filename_buf, filename);
   }
   sqlite3_reset(t->stmts->get_file);

   sqlite3_reset(t->stmts->del);
   sqlite3_bind_text(t->stmts->del, 1, id, -1, SQLITE_STATIC);
   sqlite3_bind_int(t->stmts->del, 2, owner_id);
   int rc = sqlite3_step(t->stmts->del);
   sqlite3_reset(t->stmts->del);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return BLOB_STORE_FAILURE;
   }
   if (filename_buf[0]) {
      char filepath[BLOB_PATH_MAX];
      build_filepath(s_blob.dirs[handle], filename_buf, filepath, sizeof(filepath));
      if (unlink(filepath) != 0 && errno != ENOENT) {
         OLOG_WARNING("Blob store: failed to unlink %s: %s", filepath, strerror(errno));
      }
   }
   OLOG_INFO("Blob store [%s]: deleted %s", t->name ? t->name : "?", id);
   return BLOB_STORE_SUCCESS;
}

int blob_store_delete_user(blob_store_handle_t handle, int user_id) {
   blob_store_table_t *t = tbl(handle);
   if (!t) {
      return BLOB_STORE_INVALID;
   }
   if (user_id <= 0) {
      return BLOB_STORE_INVALID;
   }
   if (!s_blob.initialized) {
      return BLOB_STORE_SUCCESS;
   }

   /* Collect filenames (heap, grows) so files can be unlinked after the row
    * delete + lock release.  Bounded by the per-user cap.  Uses ad-hoc prepared
    * statements keyed on the table name (delete_user is a rare account path). */
   const char *table = t->sql_table_name ? t->sql_table_name : "";
   if (!table[0]) {
      OLOG_ERROR("Blob store [%s]: delete_user has no sql_table_name", t->name ? t->name : "?");
      return BLOB_STORE_INVALID;
   }
   char sql_sel[128];
   char sql_del[128];
   snprintf(sql_sel, sizeof(sql_sel), "SELECT filename FROM %s WHERE user_id = ?", table);
   snprintf(sql_del, sizeof(sql_del), "DELETE FROM %s WHERE user_id = ?", table);

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_stmt *sel = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql_sel, -1, &sel, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return BLOB_STORE_FAILURE;
   }
   sqlite3_bind_int(sel, 1, user_id);

   char(*filenames)[BLOB_FILENAME_MAX] = NULL;
   int n = 0, cap = 0;
   while (sqlite3_step(sel) == SQLITE_ROW) {
      const char *fn = (const char *)sqlite3_column_text(sel, 0);
      if (!fn || !blob_validate_db_filename(fn)) {
         continue;
      }
      if (n == cap) {
         int new_cap = cap ? cap * 2 : 32;
         char(*grown)[BLOB_FILENAME_MAX] = realloc(filenames, (size_t)new_cap * BLOB_FILENAME_MAX);
         if (!grown) {
            break;
         }
         filenames = grown;
         cap = new_cap;
      }
      safe_strscpy(filenames[n], fn);
      n++;
   }
   sqlite3_finalize(sel);

   sqlite3_stmt *del = NULL;
   int deleted = 0;
   if (sqlite3_prepare_v2(s_db.db, sql_del, -1, &del, NULL) == SQLITE_OK) {
      sqlite3_bind_int(del, 1, user_id);
      if (sqlite3_step(del) == SQLITE_DONE) {
         deleted = sqlite3_changes(s_db.db);
      }
      sqlite3_finalize(del);
   }
   AUTH_DB_UNLOCK();

   for (int i = 0; i < n; i++) {
      char filepath[BLOB_PATH_MAX];
      build_filepath(s_blob.dirs[handle], filenames[i], filepath, sizeof(filepath));
      if (unlink(filepath) != 0 && errno != ENOENT) {
         OLOG_WARNING("Blob store: delete_user failed to unlink %s: %s", filepath, strerror(errno));
      }
   }
   free(filenames);
   filenames = NULL;
   OLOG_INFO("Blob store [%s]: deleted %d blob(s) for user %d", t->name ? t->name : "?", deleted,
             user_id);
   return BLOB_STORE_SUCCESS;
}

/* =============================================================================
 * Maintenance — retention + orphan sweeps
 * ============================================================================= */

int blob_store_cleanup(blob_store_handle_t handle, int *deleted_out) {
   if (deleted_out) {
      *deleted_out = 0;
   }
   blob_store_table_t *t = tbl(handle);
   if (!t) {
      return BLOB_STORE_INVALID;
   }
   if (!s_blob.initialized) {
      return BLOB_STORE_SUCCESS;
   }
   const char *dir = s_blob.dirs[handle];
   int total_deleted = 0;

   /* Phase 1: DEFAULT retention — delete blobs older than retention_days. */
   if (t->retention_days > 0) {
      time_t cutoff = time(NULL) - ((time_t)t->retention_days * 86400);
      AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);

      sqlite3_reset(t->stmts->get_expired_ids);
      sqlite3_bind_int64(t->stmts->get_expired_ids, 1, (int64_t)cutoff);
      char filenames[BLOB_CLEANUP_BATCH_SIZE][BLOB_FILENAME_MAX];
      int batch = 0;
      while (sqlite3_step(t->stmts->get_expired_ids) == SQLITE_ROW &&
             batch < BLOB_CLEANUP_BATCH_SIZE) {
         const char *fn = (const char *)sqlite3_column_text(t->stmts->get_expired_ids, 1);
         if (fn && blob_validate_db_filename(fn)) {
            safe_strscpy(filenames[batch], fn);
            batch++;
         }
      }
      sqlite3_reset(t->stmts->get_expired_ids);

      sqlite3_reset(t->stmts->delete_old);
      sqlite3_bind_int64(t->stmts->delete_old, 1, (int64_t)cutoff);
      sqlite3_bind_int64(t->stmts->delete_old, 2, (int64_t)cutoff);
      sqlite3_step(t->stmts->delete_old);
      int deleted = sqlite3_changes(s_db.db);
      sqlite3_reset(t->stmts->delete_old);
      AUTH_DB_UNLOCK();

      for (int i = 0; i < batch; i++) {
         char filepath[BLOB_PATH_MAX];
         build_filepath(dir, filenames[i], filepath, sizeof(filepath));
         if (unlink(filepath) != 0 && errno != ENOENT) {
            OLOG_WARNING("Blob store: cleanup failed to unlink %s: %s", filepath, strerror(errno));
         }
      }
      total_deleted += deleted;
   }

   /* Phase 2: CACHE retention — LRU eviction above the cap. */
   if (t->cache_size_mb > 0) {
      AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
      sqlite3_reset(t->stmts->cache_total_size);
      int64_t cache_bytes = 0;
      if (sqlite3_step(t->stmts->cache_total_size) == SQLITE_ROW) {
         cache_bytes = sqlite3_column_int64(t->stmts->cache_total_size, 0);
      }
      sqlite3_reset(t->stmts->cache_total_size);
      int64_t cap = (int64_t)t->cache_size_mb * 1024 * 1024;

      if (cache_bytes > cap) {
         sqlite3_reset(t->stmts->get_cache_lru_ids);
         char cache_ids[BLOB_CLEANUP_BATCH_SIZE][BLOB_ID_LEN];
         char cache_filenames[BLOB_CLEANUP_BATCH_SIZE][BLOB_FILENAME_MAX];
         int evict = 0;
         while (sqlite3_step(t->stmts->get_cache_lru_ids) == SQLITE_ROW && cache_bytes > cap &&
                evict < BLOB_CLEANUP_BATCH_SIZE) {
            const char *cid = (const char *)sqlite3_column_text(t->stmts->get_cache_lru_ids, 0);
            const char *fn = (const char *)sqlite3_column_text(t->stmts->get_cache_lru_ids, 1);
            int64_t row_size = sqlite3_column_int64(t->stmts->get_cache_lru_ids, 2);
            if (cid && fn) {
               safe_strscpy(cache_ids[evict], cid);
               safe_strscpy(cache_filenames[evict], fn);
               evict++;
               cache_bytes -= row_size;
            }
         }
         sqlite3_reset(t->stmts->get_cache_lru_ids);

         for (int i = 0; i < evict; i++) {
            sqlite3_reset(t->stmts->delete_cache_lru);
            sqlite3_bind_text(t->stmts->delete_cache_lru, 1, cache_ids[i], -1, SQLITE_STATIC);
            if (sqlite3_step(t->stmts->delete_cache_lru) == SQLITE_DONE) {
               total_deleted++;
            }
            sqlite3_reset(t->stmts->delete_cache_lru);
         }
         AUTH_DB_UNLOCK();

         for (int i = 0; i < evict; i++) {
            char filepath[BLOB_PATH_MAX];
            build_filepath(dir, cache_filenames[i], filepath, sizeof(filepath));
            if (unlink(filepath) != 0 && errno != ENOENT) {
               OLOG_WARNING("Blob store: cache cleanup failed to unlink %s: %s", filepath,
                            strerror(errno));
            }
         }
      } else {
         AUTH_DB_UNLOCK();
      }
   }

   if (total_deleted > 0) {
      OLOG_INFO("Blob store [%s]: cleaned up %d blob(s)", t->name ? t->name : "?", total_deleted);
   }
   if (deleted_out) {
      *deleted_out = total_deleted;
   }
   return BLOB_STORE_SUCCESS;
}

int blob_store_cleanup_orphans(blob_store_handle_t handle, int grace_sec, int *deleted_out) {
   if (deleted_out) {
      *deleted_out = 0;
   }
   blob_store_table_t *t = tbl(handle);
   if (!t) {
      return BLOB_STORE_INVALID;
   }
   if (!s_blob.initialized || !t->stmts->get_orphan_ids) {
      return BLOB_STORE_SUCCESS; /* table has no orphan concept */
   }
   const char *dir = s_blob.dirs[handle];
   time_t cutoff = time(NULL) - (time_t)grace_sec;
   int total_deleted = 0;

   /* Batches of 100; loop until a pass finds none (drains a large backlog). */
   for (;;) {
      AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
      sqlite3_reset(t->stmts->get_orphan_ids);
      sqlite3_bind_int64(t->stmts->get_orphan_ids, 1, (int64_t)cutoff);
      char ids[BLOB_CLEANUP_BATCH_SIZE][BLOB_ID_LEN];
      char filenames[BLOB_CLEANUP_BATCH_SIZE][BLOB_FILENAME_MAX];
      int batch = 0;
      while (sqlite3_step(t->stmts->get_orphan_ids) == SQLITE_ROW &&
             batch < BLOB_CLEANUP_BATCH_SIZE) {
         const char *cid = (const char *)sqlite3_column_text(t->stmts->get_orphan_ids, 0);
         const char *fn = (const char *)sqlite3_column_text(t->stmts->get_orphan_ids, 1);
         if (cid && fn && blob_validate_db_filename(fn)) {
            safe_strscpy(ids[batch], cid);
            safe_strscpy(filenames[batch], fn);
            batch++;
         }
      }
      sqlite3_reset(t->stmts->get_orphan_ids);

      /* Delete by id only (orphan sweep has no owner handy). */
      for (int i = 0; i < batch; i++) {
         sqlite3_reset(t->stmts->delete_by_id);
         sqlite3_bind_text(t->stmts->delete_by_id, 1, ids[i], -1, SQLITE_STATIC);
         sqlite3_step(t->stmts->delete_by_id);
         sqlite3_reset(t->stmts->delete_by_id);
      }
      AUTH_DB_UNLOCK();

      for (int i = 0; i < batch; i++) {
         char filepath[BLOB_PATH_MAX];
         build_filepath(dir, filenames[i], filepath, sizeof(filepath));
         if (unlink(filepath) != 0 && errno != ENOENT) {
            OLOG_WARNING("Blob store: orphan sweep failed to unlink %s: %s", filepath,
                         strerror(errno));
         }
      }
      total_deleted += batch;
      if (batch < BLOB_CLEANUP_BATCH_SIZE) {
         break;
      }
   }

   if (total_deleted > 0) {
      OLOG_INFO("Blob store [%s]: reclaimed %d orphan(s)", t->name ? t->name : "?", total_deleted);
   }
   if (deleted_out) {
      *deleted_out = total_deleted;
   }
   return BLOB_STORE_SUCCESS;
}

int blob_store_stats(blob_store_handle_t handle, int *total_count, int64_t *total_bytes) {
   blob_store_table_t *t = tbl(handle);
   if (!t || !total_count || !total_bytes) {
      return BLOB_STORE_INVALID;
   }
   *total_count = 0;
   *total_bytes = 0;

   AUTH_DB_LOCK_OR_RETURN(BLOB_STORE_FAILURE);
   sqlite3_reset(t->stmts->stats);
   if (sqlite3_step(t->stmts->stats) == SQLITE_ROW) {
      *total_count = sqlite3_column_int(t->stmts->stats, 0);
      *total_bytes = sqlite3_column_int64(t->stmts->stats, 1);
   }
   sqlite3_reset(t->stmts->stats);
   AUTH_DB_UNLOCK();
   return BLOB_STORE_SUCCESS;
}
