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
 * Image Store Module
 *
 * Provides filesystem-backed image storage with SQLite metadata.
 * Images are stored as files on disk; the database holds metadata only.
 *
 * Thread Safety: All functions are thread-safe. File I/O happens outside
 * the auth_db mutex; only metadata operations hold the lock.
 */

#ifndef IMAGE_STORE_H
#define IMAGE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* =============================================================================
 * Constants
 * ============================================================================= */

/** @brief Image ID format: "img_" + 12 alphanumeric chars */
#define IMAGE_ID_LEN 17 /* 4 + 12 + 1 null */

/** @brief Maximum MIME type length */
#define IMAGE_MIME_MAX 32

/** @brief Maximum filename length (id + '.' + ext + null) */
#define IMAGE_FILENAME_MAX 32

/** @brief Maximum filesystem path length for image files */
#define IMAGE_PATH_MAX 512

/** @brief Default max image size (4MB) */
#define IMAGE_MAX_SIZE_DEFAULT (4 * 1024 * 1024)

/** @brief Default max images per user */
#define IMAGE_MAX_PER_USER_DEFAULT 1000

/** @brief Default retention days (90 days, 0 = forever) */
#define IMAGE_RETENTION_DAYS_DEFAULT 90

/** @brief Default per-user cap on a tool's captures (IMAGE_SOURCE_CAPTURE),
 *  counted apart from max_per_user */
#define IMAGE_CAPTURE_MAX_PER_USER_DEFAULT 1000

/** @brief Default cache size cap in MB for RETAIN_CACHE images */
#define IMAGE_CACHE_SIZE_MB_DEFAULT 200

/** @brief How long an unbound image (IMAGE_RETAIN_UNBOUND) is kept for the row
 *  that will name it: a voice conversation is saved when it goes idle, and
 *  retried later when that fails.  Counted from the capture; one a live
 *  session still holds past it is kept until that session saves (a voice
 *  session that never idles for a day: session_images_held). */
#define IMAGE_UNBOUND_GRACE_SEC (24 * 60 * 60)

/* =============================================================================
 * Error Codes
 * ============================================================================= */

#define IMAGE_STORE_SUCCESS 0
#define IMAGE_STORE_FAILURE 1
#define IMAGE_STORE_NOT_FOUND 2
#define IMAGE_STORE_FORBIDDEN 3
#define IMAGE_STORE_LIMIT_EXCEEDED 4
#define IMAGE_STORE_INVALID 5
#define IMAGE_STORE_TOO_LARGE 6

/* =============================================================================
 * Types
 * ============================================================================= */

/**
 * @brief Image source — how the image entered the system
 */
typedef enum {
   IMAGE_SOURCE_UPLOAD = 0,    /* User upload (vision) */
   IMAGE_SOURCE_GENERATED = 1, /* sd.cpp output */
   IMAGE_SOURCE_SEARCH = 2,    /* Web image search cache */
   IMAGE_SOURCE_MMS = 3,       /* Received MMS */
   IMAGE_SOURCE_DOCUMENT = 4,  /* Extracted from document */
   IMAGE_SOURCE_CAPTURE = 5,   /* An image a tool returned (a camera capture): owner-only */
} image_source_t;

/**
 * @brief Retention policy — how long to keep the image
 */
typedef enum {
   IMAGE_RETAIN_DEFAULT = 0,   /* Global retention_days applies */
   IMAGE_RETAIN_PERMANENT = 1, /* Never auto-delete */
   IMAGE_RETAIN_CACHE = 2,     /* LRU eviction at size cap */
   IMAGE_RETAIN_UNBOUND = 3,   /* No row refers to it yet: reclaimed after
                                  IMAGE_UNBOUND_GRACE_SEC unless a row binds it
                                  (made PERMANENT with the row that names it) */
} image_retention_t;

/**
 * @brief Image metadata structure
 */
typedef struct {
   char id[IMAGE_ID_LEN];
   int user_id;
   char mime_type[IMAGE_MIME_MAX];
   size_t size;
   char filename[IMAGE_FILENAME_MAX];
   image_source_t source;
   image_retention_t retention_policy;
   time_t created_at;
   time_t last_accessed;
} image_metadata_t;

/**
 * @brief Image store configuration
 */
typedef struct {
   size_t max_size;           /**< Maximum image size in bytes */
   int max_per_user;          /**< Maximum images per user (a tool's captures aside) */
   int max_captures_per_user; /**< Maximum IMAGE_SOURCE_CAPTURE images per user
                               *   (0 = IMAGE_CAPTURE_MAX_PER_USER_DEFAULT) */
   int retention_days;        /**< Auto-delete DEFAULT images after N days (0 = forever) */
   int cache_size_mb;         /**< LRU cache cap for RETAIN_CACHE images in MB */
   const char *data_dir;      /**< Base data directory (images stored in <data_dir>/images/) */
} image_store_config_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the image store
 *
 * Must be called after auth_db_init(). Creates <data_dir>/images/ if needed.
 *
 * @param config Configuration (NULL for defaults)
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_init(const image_store_config_t *config);

/**
 * @brief Shutdown the image store
 *
 * Safe to call multiple times or if not initialized.
 */
void image_store_shutdown(void);

/**
 * @brief Check if image store is initialized
 *
 * @return true if ready, false otherwise
 */
bool image_store_is_ready(void);

/* =============================================================================
 * Image Operations
 * ============================================================================= */

/**
 * @brief Save an image (backward-compatible wrapper)
 *
 * Calls image_store_save_ex() with SOURCE_UPLOAD, RETAIN_DEFAULT.
 */
int image_store_save(int user_id,
                     const void *data,
                     size_t size,
                     const char *mime_type,
                     char *id_out);

/**
 * @brief Save an image with source and retention policy
 *
 * Writes file to disk atomically (tmp+fsync+rename), then inserts metadata
 * into the database. File I/O happens outside the auth_db mutex.
 *
 * @param user_id User who owns the image (0 for system-wide)
 * @param data Image binary data
 * @param size Size of image data in bytes
 * @param mime_type MIME type (e.g., "image/jpeg")
 * @param source How the image entered the system
 * @param retention Retention policy for cleanup
 * @param id_out Buffer to receive generated ID (must be IMAGE_ID_LEN bytes)
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_TOO_LARGE, IMAGE_STORE_LIMIT_EXCEEDED,
 *         IMAGE_STORE_INVALID, or IMAGE_STORE_FAILURE
 */
int image_store_save_ex(int user_id,
                        const void *data,
                        size_t size,
                        const char *mime_type,
                        image_source_t source,
                        image_retention_t retention,
                        char *id_out);

/**
 * @brief Get the filesystem path for an image
 *
 * Returns the full path for zero-copy HTTP serving via lws_serve_http_file().
 * Updates last_accessed timestamp.
 *
 * @param id Image ID
 * @param user_id User ID for access check (0 to skip check)
 * @param path_out Buffer to receive path (must be IMAGE_PATH_MAX bytes)
 * @param mime_out Buffer for MIME type (must be IMAGE_MIME_MAX bytes, can be NULL)
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, IMAGE_STORE_FORBIDDEN,
 *         or IMAGE_STORE_FAILURE
 */
int image_store_get_path(const char *id, int user_id, char *path_out, char *mime_out);

/**
 * @brief Get image metadata without loading data
 *
 * @param id Image ID
 * @param metadata_out Buffer to receive metadata
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, or IMAGE_STORE_FAILURE
 */
int image_store_get_metadata(const char *id, image_metadata_t *metadata_out);

/**
 * @brief Delete an image (file + metadata)
 *
 * @param id Image ID
 * @param user_id User ID for access check (0 to skip check, admin only)
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, IMAGE_STORE_FORBIDDEN,
 *         or IMAGE_STORE_FAILURE
 */
int image_store_delete(const char *id, int user_id);

/**
 * @brief Update an image's retention policy
 *
 * @param id Image ID
 * @param user_id User ID for ownership check (0 to skip)
 * @param retention New retention policy
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, IMAGE_STORE_FORBIDDEN,
 *         or IMAGE_STORE_FAILURE
 */
int image_store_update_retention(const char *id, int user_id, image_retention_t retention);

/**
 * @brief Count images for a user (every source, captures included)
 *
 * @param user_id   User ID
 * @param count_out Output: number of images (may be NULL)
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_count_user(int user_id, int *count_out);

/**
 * @brief Delete ALL images owned by a user (rows + files).
 *
 * For account deletion: the images table FK cascades rows on user delete but
 * leaks the files, so this purges both. Self-contained (own lock); call it
 * before deleting the user so the rows are still present to enumerate.
 *
 * @param user_id User whose images to purge (> 0).
 * @return IMAGE_STORE_SUCCESS (best-effort), IMAGE_STORE_INVALID, or IMAGE_STORE_FAILURE.
 */
int image_store_delete_user(int user_id);

/**
 * @brief Unlink an image's file whose row the caller deleted in its own
 *        transaction (conv_db_delete_ex); call with no database lock held.
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_INVALID or IMAGE_STORE_FAILURE.
 */
int image_store_unlink_file(const char *filename);

/* =============================================================================
 * Validation
 * ============================================================================= */

/**
 * @brief Validate an image ID format
 *
 * Valid format: "img_" followed by 12 alphanumeric characters.
 *
 * @param id Image ID to validate
 * @return true if valid, false otherwise
 */
bool image_store_validate_id(const char *id);

/**
 * @brief Check if MIME type is allowed
 *
 * Allowed: image/jpeg, image/png, image/gif, image/webp
 * NOT allowed: image/svg+xml (XSS risk)
 *
 * @param mime_type MIME type to check
 * @return true if allowed, false otherwise
 */
bool image_store_validate_mime(const char *mime_type);

/* =============================================================================
 * Maintenance
 * ============================================================================= */

/**
 * @brief Run cleanup of images based on retention policies
 *
 * - RETAIN_DEFAULT: delete images older than retention_days
 * - RETAIN_CACHE: LRU eviction when total cache exceeds cache_size_mb
 * - RETAIN_PERMANENT: never deleted
 * - RETAIN_UNBOUND: reclaimed after IMAGE_UNBOUND_GRACE_SEC (image_store_reclaim_unbound)
 *
 * Deletes both files and metadata within a transaction.
 *
 * @param deleted_out Output: number of images deleted (may be NULL)
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_cleanup(int *deleted_out);

/**
 * @brief Reclaim unbound images (IMAGE_RETAIN_UNBOUND) older than @p grace_sec
 *        (rows + files): stored for a turn whose row never named them (a
 *        cancelled or taken-back turn, a save that failed for good, a crash)
 *
 * Runs at init (with IMAGE_UNBOUND_GRACE_SEC) and in image_store_cleanup().
 * One a live session of its owner still holds stays (session_images_held: a
 * voice session is saved only when it goes idle, which a busy one may not do
 * within the grace; a job's history too); one bound while the sweep runs stays
 * too.  Call with no lock held: the hold check takes the session registry's
 * and each session's history lock.
 *
 * @param deleted_out Output: number reclaimed (may be NULL)
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_reclaim_unbound(int grace_sec, int *deleted_out);

/**
 * @brief Get storage statistics
 *
 * @param total_count Output: total number of images
 * @param total_bytes Output: total storage used in bytes
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_stats(int *total_count, int64_t *total_bytes);

#endif /* IMAGE_STORE_H */
