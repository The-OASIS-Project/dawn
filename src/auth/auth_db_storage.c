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
 * auth.db's storage: checkpoints off the global lock, freed space returned
 * (auth_db_storage.h).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "auth/auth_db_storage.h"

#include <libgen.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static int64_t now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Free bytes on the filesystem holding @p dir, or -1. */
static int64_t free_bytes(const char *dir) {
   struct statvfs vfs;
   if (statvfs(dir, &vfs) != 0) {
      return -1;
   }
   return (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
}

int64_t auth_db_storage_free_bytes(const char *path) {
   if (!path || strlen(path) >= AUTH_DB_STORAGE_PATH_MAX) {
      return -1;
   }
   char dir_buf[AUTH_DB_STORAGE_PATH_MAX];
   snprintf(dir_buf, sizeof(dir_buf), "%s", path);
   return free_bytes(dirname(dir_buf));
}

/* ---------------------------------------------------------------------------
 * The main connection's settings, and the one-time conversion
 * ------------------------------------------------------------------------- */

void auth_db_storage_configure_locked(sqlite3 *db) {
   sqlite3_busy_timeout(db, AUTH_DB_BUSY_TIMEOUT_MS);
   sqlite3_exec(db, "PRAGMA secure_delete=FAST", NULL, NULL, NULL);
   char sql[64];
   snprintf(sql, sizeof(sql), "PRAGMA journal_size_limit=%d", AUTH_DB_WAL_SIZE_LIMIT);
   sqlite3_exec(db, sql, NULL, NULL, NULL);
   /* Takes effect only while the file has no tables: a new database starts
    * in incremental auto-vacuum; an existing one is converted later. */
   sqlite3_exec(db, "PRAGMA auto_vacuum=INCREMENTAL", NULL, NULL, NULL);
}

/* Where SQLite puts a temporary file (its unix lookup order). */
static const char *temp_dir(void) {
   static const char *const k_fixed[] = { "/var/tmp", "/usr/tmp", "/tmp" };
   const char *env[] = { getenv("SQLITE_TMPDIR"), getenv("TMPDIR") };
   for (size_t i = 0; i < sizeof(env) / sizeof(env[0]); i++) {
      struct stat sb;
      if (env[i] && stat(env[i], &sb) == 0 && S_ISDIR(sb.st_mode)) {
         return env[i];
      }
   }
   for (size_t i = 0; i < sizeof(k_fixed) / sizeof(k_fixed[0]); i++) {
      struct stat sb;
      if (stat(k_fixed[i], &sb) == 0 && S_ISDIR(sb.st_mode)) {
         return k_fixed[i];
      }
   }
   return ".";
}

/* The RAM the conversion may use for its temporary copy: the smaller of the
 * ceiling and a quarter of what's available now. */
static int64_t convert_memory_max(void) {
   const long pages = sysconf(_SC_AVPHYS_PAGES);
   const long page = sysconf(_SC_PAGESIZE);
   const int64_t quarter = pages > 0 && page > 0 ? (int64_t)pages * page / 4 : 0;
   return quarter > 0 && quarter < AUTH_DB_CONVERT_MEMORY_MAX ? quarter
                                                              : AUTH_DB_CONVERT_MEMORY_MAX;
}

static bool same_filesystem(const char *a, const char *b) {
   struct stat sa;
   struct stat sb;
   return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev;
}

void auth_db_storage_convert_locked(sqlite3 *db, const char *path) {
   if (!path || strlen(path) >= AUTH_DB_STORAGE_PATH_MAX ||
       auth_db_query_int64(db, "PRAGMA auto_vacuum") != 0) {
      return; /* incremental already (or full, chosen by hand) */
   }
   const int64_t page_size = auth_db_query_int64(db, "PRAGMA page_size");
   const int64_t pages = auth_db_query_int64(db, "PRAGMA page_count");
   const int64_t free_pages = auth_db_query_int64(db, "PRAGMA freelist_count");
   if (page_size <= 0 || pages < 0 || free_pages < 0) {
      return;
   }
   const int64_t live = (pages - free_pages) * page_size;
   /* The VACUUM writes the live data twice: once into the WAL (next to the
    * file) and once into its temporary copy (in memory when it's small
    * enough, else in the temp directory). */
   char dir_buf[AUTH_DB_STORAGE_PATH_MAX];
   snprintf(dir_buf, sizeof(dir_buf), "%s", path);
   const char *dir = dirname(dir_buf);
   const int64_t margin = live / 10 + (int64_t)64 * 1024 * 1024;
   const bool in_memory = live <= convert_memory_max();
   const char *tmp = temp_dir();
   const bool tmp_here = !in_memory && same_filesystem(dir, tmp);
   const int64_t need_here = tmp_here ? 2 * live + margin : live + margin;
   const bool room = free_bytes(dir) >= need_here &&
                     (in_memory || tmp_here || free_bytes(tmp) >= live + margin);
   if (!room) {
      OLOG_WARNING("auth_db: not converting to incremental auto-vacuum yet: it needs ~%lld MB "
                   "free next to the database%s (tried again at the next start)",
                   (long long)(need_here / (1024 * 1024)),
                   in_memory || tmp_here ? "" : ", and as much in the temp directory");
      return;
   }
   OLOG_INFO("auth_db: converting to incremental auto-vacuum (one-time rewrite of %lld MB)",
             (long long)(live / (1024 * 1024)));
   const int64_t t0 = now_ms();
   if (in_memory) {
      sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
   }
   char *errmsg = NULL;
   int rc = sqlite3_exec(db, "PRAGMA auto_vacuum=INCREMENTAL", NULL, NULL, &errmsg);
   if (rc == SQLITE_OK) {
      rc = sqlite3_exec(db, "VACUUM", NULL, NULL, &errmsg);
   }
   if (in_memory) {
      sqlite3_exec(db, "PRAGMA temp_store=DEFAULT", NULL, NULL, NULL);
   }
   /* Nothing else runs yet: write the WAL back and shrink it now (after a
    * failure too: a failed VACUUM leaves a grown WAL). */
   sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
   if (rc != SQLITE_OK) {
      /* VACUUM is atomic: the file is as it was. */
      OLOG_WARNING("auth_db: incremental auto-vacuum conversion failed (%s); tried again at "
                   "the next start",
                   errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return;
   }
   OLOG_INFO("auth_db: converted to incremental auto-vacuum in %lld ms",
             (long long)(now_ms() - t0));
}

/* ---------------------------------------------------------------------------
 * The storage thread
 * ------------------------------------------------------------------------- */

static pthread_t s_thread;
static bool s_started;
/* Leaf lock: taken by the WAL hook inside a commit (the main connection's
 * mutex held) and by the thread; never held while taking another. */
static pthread_mutex_t s_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake_cond; /* on CLOCK_MONOTONIC (start_locked) */
static bool s_wake;
static _Atomic bool s_stop;
static _Atomic int s_wal_frames;
static _Atomic uint64_t s_checkpoints;
static sqlite3 *s_conn; /* the thread's own connection */

/* After each commit, with the main connection's mutex held: record the WAL's
 * size and past the threshold wake the thread (no I/O).  Past the hard limit
 * (a writer outrunning the thread, so the WAL never gets to restart) the
 * commit checkpoints itself, as SQLite's own auto-checkpoint would: PASSIVE,
 * and BUSY at once when the thread is checkpointing (the next commit tries
 * again). */
static int on_commit(void *arg, sqlite3 *db, const char *name, int frames) {
   (void)arg;
   atomic_store(&s_wal_frames, frames);
   if (frames >= AUTH_DB_WAL_HARD_FRAMES) {
      (void)sqlite3_wal_checkpoint_v2(db, name, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
   }
   if (frames >= AUTH_DB_CHECKPOINT_FRAMES) {
      pthread_mutex_lock(&s_wake_mutex);
      s_wake = true;
      pthread_cond_signal(&s_wake_cond);
      pthread_mutex_unlock(&s_wake_mutex);
   }
   return SQLITE_OK;
}

/* One read, so the connection knows the file is in WAL mode: before it, a
 * checkpoint call succeeds and does nothing. */
static bool prime(sqlite3 *conn) {
   return sqlite3_exec(conn, "SELECT 1 FROM sqlite_master LIMIT 1", NULL, NULL, NULL) == SQLITE_OK;
}

/* A PASSIVE checkpoint on the thread's own connection (no global mutex): it
 * copies what no reader still needs, never waiting.  Warns once when it
 * keeps falling behind, and again only after it has caught up. */
static void checkpoint(sqlite3 *conn, int64_t page_size, int *behind, bool *warned) {
   int log = -1;
   int done = -1;
   int rc = sqlite3_wal_checkpoint_v2(conn, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
   if (rc == SQLITE_OK && log < 0 && prime(conn)) {
      rc = sqlite3_wal_checkpoint_v2(conn, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
   }
   if (rc == SQLITE_BUSY) {
      return; /* another checkpoint is running (an admin one, or a commit's) */
   }
   if (rc != SQLITE_OK || log < 0) {
      if (!*warned) {
         OLOG_WARNING("auth_db: checkpoint failed (rc=%d, log=%d): %s", rc, log,
                      sqlite3_errmsg(conn));
         *warned = true;
      }
      return;
   }
   atomic_fetch_add(&s_checkpoints, 1);
   if (done >= log) {
      *behind = 0;
      *warned = false;
      return;
   }
   if ((++*behind >= 5 || (int64_t)log * page_size > AUTH_DB_WAL_WARN_BYTES) && !*warned) {
      OLOG_WARNING("auth_db: checkpoints are falling behind (%d of %d WAL frames written "
                   "back): a read holding an old snapshot, or writes outrunning them",
                   done, log);
      *warned = true;
   }
}

/* The checkpoint state the thread keeps across calls. */
typedef struct {
   sqlite3 *conn;
   int64_t page_size;
   int behind;
   bool warned;
} checkpointer_t;

static int vacuum_pass(checkpointer_t *cp, int budget_ms, int *freed_out);

static void *storage_thread(void *arg) {
   checkpointer_t cp = { .conn = arg };
   cp.page_size = auth_db_query_int64(cp.conn, "PRAGMA page_size");
   if (cp.page_size <= 0) {
      cp.page_size = 4096;
   }
   int64_t last_checkpoint = 0;
   int64_t last_vacuum = now_ms();
   for (;;) {
      pthread_mutex_lock(&s_wake_mutex);
      if (!s_wake && !atomic_load(&s_stop)) {
         struct timespec until;
         clock_gettime(CLOCK_MONOTONIC, &until);
         until.tv_sec += AUTH_DB_STORAGE_WAKE_SEC;
         pthread_cond_timedwait(&s_wake_cond, &s_wake_mutex, &until);
      }
      s_wake = false;
      pthread_mutex_unlock(&s_wake_mutex);
      if (atomic_load(&s_stop)) {
         break;
      }
      const int64_t gap = now_ms() - last_checkpoint;
      if (gap < AUTH_DB_CHECKPOINT_MIN_GAP_MS) {
         usleep((useconds_t)((AUTH_DB_CHECKPOINT_MIN_GAP_MS - gap) * 1000));
      }
      if (atomic_load(&s_wal_frames) > 0) {
         checkpoint(cp.conn, cp.page_size, &cp.behind, &cp.warned);
      }
      last_checkpoint = now_ms();
      if (last_checkpoint - last_vacuum >= (int64_t)AUTH_DB_STORAGE_WAKE_SEC * 1000) {
         last_vacuum = last_checkpoint;
         int freed = 0;
         (void)vacuum_pass(&cp, AUTH_DB_VACUUM_BUDGET_MS, &freed);
         if (freed > 0) {
            OLOG_INFO("auth_db: returned %d free page(s) to the disk", freed);
         }
      }
   }
   return NULL;
}

int auth_db_storage_start_locked(sqlite3 *db, const char *path) {
   if (s_started || !db || !path || strlen(path) >= AUTH_DB_STORAGE_PATH_MAX) {
      return AUTH_DB_FAILURE;
   }
   /* The thread's connection, open and reading the WAL before the hook turns
    * SQLite's own checkpoint off: on any failure, that one stays on. */
   sqlite3 *conn = NULL;
   if (sqlite3_open_v2(path, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) !=
           SQLITE_OK ||
       !prime(conn)) {
      OLOG_ERROR("auth_db: storage thread's connection failed (%s); checkpoints stay inside "
                 "commits",
                 conn ? sqlite3_errmsg(conn) : "no memory");
      sqlite3_close(conn);
      return AUTH_DB_FAILURE;
   }
   /* The checkpoint's own fsyncs, as the main connection's. */
   sqlite3_exec(conn, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&s_wake_cond, &attr);
   pthread_condattr_destroy(&attr);
   atomic_store(&s_stop, false);
   s_wake = false;
   atomic_store(&s_wal_frames, 0);
   if (pthread_create(&s_thread, NULL, storage_thread, conn) != 0) {
      OLOG_ERROR("auth_db: storage thread not started; checkpoints stay inside commits");
      pthread_cond_destroy(&s_wake_cond);
      sqlite3_close(conn);
      return AUTH_DB_FAILURE;
   }
   s_conn = conn;
   /* From here, commits only record the WAL's size (this turns off the
    * checkpoint SQLite runs inside the commit). */
   sqlite3_wal_hook(db, on_commit, NULL);
   s_started = true;
   return AUTH_DB_SUCCESS;
}

void auth_db_storage_stop(void) {
   if (!s_started) {
      return;
   }
   pthread_mutex_lock(&s_wake_mutex);
   atomic_store(&s_stop, true);
   pthread_cond_signal(&s_wake_cond);
   pthread_mutex_unlock(&s_wake_mutex);
   pthread_join(s_thread, NULL);
   pthread_mutex_lock(&s_db.mutex);
   if (s_db.db) {
      /* SQLite's own checkpoint in the commit again (this replaces the hook). */
      sqlite3_wal_autocheckpoint(s_db.db, AUTH_DB_CHECKPOINT_FRAMES);
   }
   pthread_mutex_unlock(&s_db.mutex);
   sqlite3_close(s_conn);
   s_conn = NULL;
   pthread_cond_destroy(&s_wake_cond);
   atomic_store(&s_stop, false); /* a vacuum pass called without the thread still runs */
   s_started = false;
}

/* ---------------------------------------------------------------------------
 * Draining free pages; leaked reads
 * ------------------------------------------------------------------------- */

/* Chunks of free pages, each one short hold of the mutex, then a pause as
 * long as the chunk took (glibc's mutex isn't fair: without it, this thread
 * takes the mutex straight back and every other caller waits out the whole
 * pass).  The WAL it writes is checkpointed between chunks (@p cp, when the
 * storage thread runs it). */
static int vacuum_pass(checkpointer_t *cp, int budget_ms, int *freed_out) {
   if (freed_out) {
      *freed_out = 0;
   }
   const int64_t start = now_ms();
   int freed = 0;
   char sql[64];
   snprintf(sql, sizeof(sql), "PRAGMA incremental_vacuum(%d)", AUTH_DB_VACUUM_CHUNK_PAGES);
   while (!atomic_load(&s_stop)) {
      const int64_t chunk_start = now_ms();
      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized || !s_db.db) {
         pthread_mutex_unlock(&s_db.mutex);
         return AUTH_DB_FAILURE;
      }
      if (auth_db_query_int64(s_db.db, "PRAGMA auto_vacuum") != 2) {
         pthread_mutex_unlock(&s_db.mutex);
         break; /* not converted yet */
      }
      const int64_t free_pages = auth_db_query_int64(s_db.db, "PRAGMA freelist_count");
      if (free_pages <= AUTH_DB_VACUUM_FLOOR_PAGES) {
         pthread_mutex_unlock(&s_db.mutex);
         break;
      }
      const int rc = sqlite3_exec(s_db.db, sql, NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      if (rc != SQLITE_OK) {
         break;
      }
      freed += free_pages < AUTH_DB_VACUUM_CHUNK_PAGES ? (int)free_pages
                                                       : AUTH_DB_VACUUM_CHUNK_PAGES;
      if (cp && atomic_load(&s_wal_frames) >= AUTH_DB_CHECKPOINT_FRAMES) {
         checkpoint(cp->conn, cp->page_size, &cp->behind, &cp->warned);
      }
      const int64_t took = now_ms() - chunk_start;
      if (now_ms() - start >= budget_ms) {
         break;
      }
      usleep((useconds_t)((took > 1 ? took : 1) * 1000));
   }
   if (freed_out) {
      *freed_out = freed;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_storage_vacuum_pass(int budget_ms, int *freed_out) {
   return vacuum_pass(NULL, budget_ms, freed_out);
}

void auth_db_storage_check_leaked_reads_locked(sqlite3 *db) {
   for (sqlite3_stmt *st = sqlite3_next_stmt(db, NULL); st; st = sqlite3_next_stmt(db, st)) {
      if (sqlite3_stmt_busy(st)) {
         OLOG_WARNING("auth_db: a statement was left mid-read (it holds the WAL from being "
                      "checkpointed): %.160s",
                      sqlite3_sql(st));
      }
   }
}

uint64_t auth_db_storage_checkpoints(void) {
   return atomic_load(&s_checkpoints);
}
