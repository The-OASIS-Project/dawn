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
 * auth.db's storage: WAL checkpoints off the global lock, and freed space
 * returned to the disk.
 *
 * - A commit only records the WAL's size (a WAL hook, which also turns off
 *   SQLite's checkpoint inside the commit that crosses the threshold, where
 *   it ran under the global mutex); only past AUTH_DB_WAL_HARD_FRAMES does a
 *   commit checkpoint itself.  The storage thread checkpoints from its
 *   own connection: PASSIVE only, without the global mutex, never blocking
 *   the main connection's readers or writer.  The main connection restarts
 *   and truncates the WAL (journal_size_limit) at its next write.
 * - The file is in incremental auto-vacuum (converted once at startup), and
 *   the storage thread drains its free pages in short chunks, releasing the
 *   mutex between them, so deleted data leaves the file within about a
 *   minute.  secure_delete=FAST zeroes freed space inside pages rewritten
 *   anyway (it doesn't zero whole freed pages: draining removes those from
 *   the file; their old blocks are the filesystem's, like a deleted file's).
 */

#ifndef AUTH_DB_STORAGE_H
#define AUTH_DB_STORAGE_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WAL frames past which the storage thread checkpoints (SQLite's default). */
#define AUTH_DB_CHECKPOINT_FRAMES 1000
/* The WAL file is truncated to this when the writer restarts it. */
#define AUTH_DB_WAL_SIZE_LIMIT (16 * 1024 * 1024)
/* The storage thread wakes at least this often (checkpoint, free pages). */
#define AUTH_DB_STORAGE_WAKE_SEC 60
/* Checkpoints at most this often (a hot writer can't spin the thread). */
#define AUTH_DB_CHECKPOINT_MIN_GAP_MS 100
/* The WAL size that is logged as a checkpoint falling behind. */
#define AUTH_DB_WAL_WARN_BYTES (64 * 1024 * 1024)
/* WAL frames past which a commit checkpoints itself (PASSIVE): a writer
 * outrunning the storage thread would otherwise keep the WAL from ever
 * restarting. */
#define AUTH_DB_WAL_HARD_FRAMES 16384
/* The longest database path the storage thread takes. */
#define AUTH_DB_STORAGE_PATH_MAX 1024
/* How the main connection waits for SQLite's own locks (a checkpoint in
 * progress on the storage thread's connection, at an admin or shutdown
 * TRUNCATE checkpoint). */
#define AUTH_DB_BUSY_TIMEOUT_MS 2000
/* Free pages are drained when more than this many are free (all of them),
 * this many per chunk (one short hold of the mutex each, then a pause as
 * long), for at most this long per run. */
#define AUTH_DB_VACUUM_FLOOR_PAGES 0
#define AUTH_DB_VACUUM_CHUNK_PAGES 256
#define AUTH_DB_VACUUM_BUDGET_MS 2000
/* The one-time conversion to incremental auto-vacuum builds its temporary
 * copy in memory up to this much live data, or a quarter of the RAM
 * available if that's less (above it, in the temp directory, which then
 * needs the space). */
#define AUTH_DB_CONVERT_MEMORY_MAX ((int64_t)512 * 1024 * 1024)

/**
 * @brief Settings on the main connection, before the schema exists: the busy
 *        timeout, secure_delete=FAST, the WAL size limit, and incremental
 *        auto-vacuum (which a new, empty file takes at once).
 *        Caller holds the mutex.
 */
void auth_db_storage_configure_locked(sqlite3 *db);

/**
 * @brief Convert an existing file to incremental auto-vacuum (a one-time
 *        VACUUM), after the migrations, when there's room for it; the file's
 *        header records it, so it runs once.  Caller holds the mutex; no
 *        statements are prepared yet.
 */
void auth_db_storage_convert_locked(sqlite3 *db, const char *path);

/**
 * @brief Register the WAL hook on the main connection and start the storage
 *        thread (its own connection to @p path).  Caller holds the mutex.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (the database still works,
 *         checkpointing inside commits as before)
 */
int auth_db_storage_start_locked(sqlite3 *db, const char *path);

/**
 * @brief Stop and join the storage thread, and give checkpoints back to
 *        SQLite (inside the commit, as without the thread).  Called WITHOUT
 *        the mutex (the thread takes it to drain free pages).
 */
void auth_db_storage_stop(void);

/**
 * @brief Drain free pages in chunks until the freelist is at the floor or
 *        @p budget_ms is spent (the storage thread's step; tests call it).
 * @param freed_out Pages freed (may be NULL)
 */
int auth_db_storage_vacuum_pass(int budget_ms, int *freed_out);

/**
 * @brief Warn about any statement on the main connection left mid-read (it
 *        holds a snapshot the WAL can't be checkpointed past).  Caller holds
 *        the mutex, so no other thread is using a statement.
 */
void auth_db_storage_check_leaked_reads_locked(sqlite3 *db);

/** @brief Checkpoints the storage thread has completed (tests). */
uint64_t auth_db_storage_checkpoints(void);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_STORAGE_H */
