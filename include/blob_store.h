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
 * Generic Blob Store
 *
 * Filesystem-backed blob storage with SQLite metadata, generalized from the
 * image store so images, document originals, and future audio/video all share
 * one engine.  Each consumer registers a descriptor (id prefix, on-disk subdir,
 * mime->ext map, validation + access hooks, per-store caps, and a set of
 * prepared statements owned by auth_db) and receives a handle.  The engine owns
 * atomic file I/O, metadata CRUD, content-hash dedup, retention (DEFAULT-age /
 * PERMANENT / CACHE-LRU), and an orphan sweep.
 *
 * Thread safety: metadata ops hold the auth_db leaf lock; file I/O and the
 * SHA-256 hash happen OUTSIDE the lock.  Mirrors the image-store discipline.
 */

#ifndef BLOB_STORE_H
#define BLOB_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sqlite3_stmt is forward-declared so this header is includable without
 * pulling in sqlite3.h.  Only consumers that build a descriptor (themselves
 * Layer-2 store modules) reference these pointers. */
struct sqlite3_stmt;

/* =============================================================================
 * Constants
 * ============================================================================= */

/** @brief Blob ID buffer: prefix(4) + 12 chars + NUL. */
#define BLOB_ID_LEN 17

/** @brief Max MIME length (document MIMEs are long, e.g. the OOXML wordml type). */
#define BLOB_MIME_MAX 96

/** @brief Max on-disk filename: id(16) + '.' + ext + NUL. */
#define BLOB_FILENAME_MAX 40

/** @brief Max user-facing original filename (for download Content-Disposition). */
#define BLOB_FILENAME_ORIGINAL_MAX 256

/** @brief Max filesystem path length for a blob file. */
#define BLOB_PATH_MAX 512

/** @brief SHA-256 hex string length including NUL (mirror DAWN_SHA256_HEX_LEN). */
#define BLOB_HASH_HEX_LEN 65

/** @brief Maximum registered consumer tables (image, document, future a/v). */
#define BLOB_STORE_MAX_TABLES 4

/* =============================================================================
 * Error Codes (mirror image_store for drop-in wrapping)
 * ============================================================================= */

#define BLOB_STORE_SUCCESS 0
#define BLOB_STORE_FAILURE 1
#define BLOB_STORE_NOT_FOUND 2
#define BLOB_STORE_FORBIDDEN 3
#define BLOB_STORE_LIMIT_EXCEEDED 4
#define BLOB_STORE_INVALID 5
#define BLOB_STORE_TOO_LARGE 6

/* =============================================================================
 * Types
 * ============================================================================= */

/** @brief Retention policy — generic across all consumers. */
typedef enum {
   BLOB_RETAIN_DEFAULT = 0,   /* Global retention_days applies (0 = forever) */
   BLOB_RETAIN_PERMANENT = 1, /* Never auto-delete */
   BLOB_RETAIN_CACHE = 2,     /* LRU eviction at size cap */
   BLOB_RETAIN_UNBOUND = 3,   /* Awaiting the row that names it: orphan-swept after a grace */
} blob_retention_t;

/** @brief One MIME -> file-extension mapping entry. */
typedef struct {
   const char *mime;
   const char *ext;
} blob_mime_map_t;

/** @brief Generic blob metadata returned by blob_store_get_metadata(). */
typedef struct {
   char id[BLOB_ID_LEN];
   int user_id;
   char mime_type[BLOB_MIME_MAX];
   size_t size;
   char filename[BLOB_FILENAME_MAX];
   int source; /* consumer-defined source/kind tag */
   blob_retention_t retention_policy;
   time_t created_at;
   time_t last_accessed;
   /* Populated only when the table has a filename_original column (else ""). */
   char filename_original[BLOB_FILENAME_ORIGINAL_MAX];
} blob_metadata_t;

/**
 * @brief Prepared statements backing one consumer's table.
 *
 * Pointers are owned by auth_db (prepared once at auth_db init, finalized at
 * shutdown); the descriptor copies them at registration time.  Statements that
 * a consumer doesn't support are left NULL: `find_by_hash`/`sum_bytes_user`
 * gate dedup + the aggregate-byte budget; `get_orphan_ids` gates the orphan
 * sweep.  Column orders are documented in blob_store.c next to each bind/read.
 */
typedef struct {
   struct sqlite3_stmt *create;            /* INSERT row */
   struct sqlite3_stmt *get;               /* full metadata by id */
   struct sqlite3_stmt *get_file;          /* filename,user_id,source,mime,last_accessed by id */
   struct sqlite3_stmt *del;               /* DELETE by id + owner */
   struct sqlite3_stmt *update_access;     /* last_accessed by id */
   struct sqlite3_stmt *update_retention;  /* retention by id + owner */
   struct sqlite3_stmt *count_user;        /* COUNT by user_id */
   struct sqlite3_stmt *count_user_source; /* COUNT by user_id + source (nullable) */
   struct sqlite3_stmt *sum_bytes_user;    /* SUM(size) by user_id (nullable) */
   struct sqlite3_stmt *find_by_hash;      /* id by user_id + content_hash (nullable) */
   struct sqlite3_stmt *delete_old;        /* DEFAULT-age delete by cutoff */
   struct sqlite3_stmt *cache_total_size;  /* SUM(size) for RETAIN_CACHE */
   struct sqlite3_stmt *delete_cache_lru;  /* DELETE one by id (LRU eviction) */
   struct sqlite3_stmt *delete_by_id;      /* DELETE one by id (orphan reclamation) */
   struct sqlite3_stmt *get_expired_ids;   /* id+filename of DEFAULT-age expired */
   struct sqlite3_stmt *get_cache_lru_ids; /* id+filename+size of LRU overflow */
   struct sqlite3_stmt *get_orphan_ids;    /* orphans by cutoff, past a cursor (nullable):
                                              binds ?1 cutoff, ?2/?3 the last row's
                                              created_at/id; reads id, filename,
                                              created_at, user_id in (created_at, id)
                                              order, LIMIT 100 */
   struct sqlite3_stmt *delete_orphan;     /* DELETE one by id while still an orphan (nullable:
                                              needed by orphans_held) */
   struct sqlite3_stmt *stats;             /* COUNT + SUM(size) */
} blob_store_stmts_t;

/**
 * @brief Consumer descriptor — supplied once at registration.
 *
 * Behaviour that genuinely diverges between consumers is injected here: the
 * MIME whitelist (`validate_mime`), the read-access policy (`can_read`), and
 * the per-table schema shape via `has_content_hash` / `has_filename_original`
 * (the engine binds those columns only when present, so one engine serves both
 * the legacy `images` table and the richer `blobs` table).
 */
typedef struct {
   const char *name;             /* "image" / "document_original" — logs only */
   const char *id_prefix;        /* "img_" / "blb_" (4 chars incl. underscore) */
   const char *subdir;           /* "images" / "blobs" — under data_dir */
   const char *sql_table_name;   /* SQL table for ad-hoc delete_user (e.g. "blobs") */
   const blob_mime_map_t *mimes; /* MIME -> ext table */
   size_t mime_count;

   bool has_content_hash;      /* table has a content_hash column (dedup) */
   bool has_filename_original; /* table has a filename_original column */

   /* Policy hooks (NULL => permissive default). */
   bool (*validate_mime)(const char *mime);
   bool (*can_read)(int user_id, int owner_id, int source);

   const blob_store_stmts_t *stmts; /* prepared statements for this table */

   /* Whether something outside the database still holds each of the @p n
    * orphans in @p ids, owned by @p owners[i] (held[i] = true keeps it this
    * sweep).  Called with no lock held; NULL = nothing does (needs
    * stmts->delete_orphan). */
   void (*orphans_held)(const char *const ids[], const int owners[], int n, bool held[]);

   /* Per-store caps. */
   size_t max_size;  /* per-blob byte cap (0 = unlimited) */
   int max_per_user; /* per-user count cap (0 = unlimited) */
   /* One source counted and capped apart (needs stmts->count_user_source):
    * its saves are held to apart_max_per_user and never count toward
    * max_per_user.  apart_max_per_user 0 = no source apart. */
   int apart_source;
   int apart_max_per_user;
   int64_t max_total_bytes_per_user; /* per-user aggregate byte cap (0 = unlimited) */
   int retention_days;               /* DEFAULT-age days (0 = forever) */
   int cache_size_mb;                /* RETAIN_CACHE LRU cap (0 = disabled) */
} blob_store_table_t;

/** @brief Opaque handle to a registered table (>= 0 valid; < 0 invalid). */
typedef int blob_store_handle_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the blob store root.  Idempotent.  Must run after
 *        auth_db_init().  Each registered table creates {data_dir}/{subdir}/.
 */
int blob_store_init(const char *data_dir);

/**
 * @brief Register a consumer table.  Copies the descriptor, creates its subdir,
 *        and returns a handle.  @p desc and the structs it points at must
 *        outlive the store (use static storage in the consumer module).
 */
int blob_store_register(const blob_store_table_t *desc, blob_store_handle_t *handle_out);

void blob_store_shutdown(void);
bool blob_store_is_ready(void);

/* =============================================================================
 * Operations
 * ============================================================================= */

/**
 * @brief Save a blob.  Hashing + file write happen outside the auth_db lock.
 *
 * When @p content_hash is non-NULL and the table supports dedup, an existing
 * blob with the same (user_id, hash) is returned without a second write.  Caps
 * are charged only on a genuine new write.  On a UNIQUE race (a concurrent
 * identical save won) the just-written file is unlinked and the winner's id is
 * returned as SUCCESS.
 *
 * @param id_out Buffer of at least BLOB_ID_LEN bytes.
 */
int blob_store_save(blob_store_handle_t handle,
                    int user_id,
                    const void *data,
                    size_t size,
                    const char *mime_type,
                    int source,
                    blob_retention_t retention,
                    const char *content_hash,
                    const char *filename_original,
                    char *id_out);

/** @brief Resolve a blob id to its filesystem path (+ MIME), applying the
 *         consumer's `can_read` hook.  Updates last_accessed (rate-limited). */
int blob_store_get_path(blob_store_handle_t handle,
                        const char *id,
                        int user_id,
                        char *path_out,
                        char *mime_out);

int blob_store_get_metadata(blob_store_handle_t handle, const char *id, blob_metadata_t *out);
int blob_store_delete(blob_store_handle_t handle, const char *id, int user_id);
int blob_store_update_retention(blob_store_handle_t handle,
                                const char *id,
                                int user_id,
                                blob_retention_t retention);
int blob_store_count_user(blob_store_handle_t handle, int user_id, int *count_out);
int blob_store_delete_user(blob_store_handle_t handle, int user_id);

/**
 * @brief Unlink the file @p filename (a stored name, validated) of a row the
 *        caller already deleted in its own transaction.  No database work.
 * @return BLOB_STORE_SUCCESS (or already gone), BLOB_STORE_INVALID or
 *         BLOB_STORE_FAILURE.
 */
int blob_store_unlink_file(blob_store_handle_t handle, const char *filename);

/** @brief Look up an existing blob id by (user_id, content_hash).  Returns
 *         BLOB_STORE_NOT_FOUND when absent or the table has no dedup support. */
int blob_store_find_by_hash(blob_store_handle_t handle,
                            int user_id,
                            const char *content_hash,
                            char *id_out);

/** @brief Retention sweep: DEFAULT-age delete + CACHE-LRU eviction. */
int blob_store_cleanup(blob_store_handle_t handle, int *deleted_out);

/** @brief Orphan sweep: delete blobs with no referrer older than @p grace_sec.
 *         No-op when the table declares no `get_orphan_ids` statement.
 *
 * Walks the orphans oldest first by a (created_at, id) cursor, so a batch a
 * live session holds whole doesn't stop the ones after it; at most
 * BLOB_ORPHAN_SWEEP_MAX_BATCHES batches per call (the next sweep goes on). */
int blob_store_cleanup_orphans(blob_store_handle_t handle, int grace_sec, int *deleted_out);

int blob_store_stats(blob_store_handle_t handle, int *total_count, int64_t *total_bytes);

/* =============================================================================
 * Low-level helpers (pure — no DB; exposed for consumers + unit tests)
 * ============================================================================= */

/** @brief Generate "<prefix>" + 12 base62 chars into @p out (>= BLOB_ID_LEN). */
int blob_generate_id(const char *prefix, char *out);

/** @brief Validate "<prefix>" + 12 alphanumeric (prefix is 4 chars incl. '_'). */
bool blob_validate_id(const char *id, const char *prefix);

/** @brief MIME -> extension via @p map (fallback "bin"). */
const char *blob_mime_to_ext(const char *mime_type, const blob_mime_map_t *map, size_t map_count);

/** @brief Build "<id>.<ext>" into @p out. */
void blob_build_filename(const char *id,
                         const char *mime_type,
                         const blob_mime_map_t *map,
                         size_t map_count,
                         char *out,
                         size_t out_size);

/** @brief Reject a DB-sourced filename containing path traversal. */
bool blob_validate_db_filename(const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_STORE_H */
