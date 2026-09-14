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
 * Music Metadata Database Implementation
 *
 * SQLite-based cache for audio file metadata enabling fast search by
 * artist, title, and album fields instead of just filename.
 */

#include "audio/music_db.h"

#include <dirent.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_source.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/** Maximum directory depth for recursive scanning */
#define MAX_DIRECTORY_DEPTH 10

/** Yield mutex every N files to allow searches during scan */
#define SCAN_YIELD_INTERVAL 50

/** Priority-based dedup clause: exclude rows where a higher-priority source
 *  (lower enum value) has the same artist+album+title. Uses idx_music_dedup.
 *  COLLATE NOCASE handles metadata case differences between sources.
 *  Uses IS instead of = so that NULL IS NULL evaluates to TRUE (SQLite). */
#define DEDUP_CLAUSE                                             \
   "AND NOT EXISTS ("                                            \
   "   SELECT 1 FROM music_metadata m2 "                         \
   "   WHERE m2.artist IS music_metadata.artist COLLATE NOCASE " \
   "   AND m2.album IS music_metadata.album COLLATE NOCASE "     \
   "   AND m2.title IS music_metadata.title COLLATE NOCASE "     \
   "   AND m2.source < music_metadata.source"                    \
   ") "

/** Dedup clause for queries that have no preceding WHERE condition */
#define DEDUP_WHERE                                              \
   "WHERE NOT EXISTS ("                                          \
   "   SELECT 1 FROM music_metadata m2 "                         \
   "   WHERE m2.artist IS music_metadata.artist COLLATE NOCASE " \
   "   AND m2.album IS music_metadata.album COLLATE NOCASE "     \
   "   AND m2.title IS music_metadata.title COLLATE NOCASE "     \
   "   AND m2.source < music_metadata.source"                    \
   ") "

/* SQL statements */
static const char *SQL_CREATE_TABLE = "CREATE TABLE IF NOT EXISTS music_metadata ("
                                      "   id INTEGER PRIMARY KEY,"
                                      "   path TEXT UNIQUE NOT NULL,"
                                      "   mtime INTEGER NOT NULL,"
                                      "   title TEXT,"
                                      "   artist TEXT,"
                                      "   album TEXT,"
                                      "   duration_sec INTEGER"
                                      ")";

static const char *SQL_CREATE_INDEX_ALBUM =
    "CREATE INDEX IF NOT EXISTS idx_music_album ON music_metadata(album)";
static const char *SQL_CREATE_INDEX_TITLE =
    "CREATE INDEX IF NOT EXISTS idx_music_title ON music_metadata(title)";

static const char *SQL_INSERT_OR_REPLACE =
    "INSERT OR REPLACE INTO music_metadata (path, mtime, title, artist, album, duration_sec) "
    "VALUES (?, ?, ?, ?, ?, ?)";

static const char *SQL_SELECT_BY_PATH = "SELECT path, title, artist, album, duration_sec "
                                        "FROM music_metadata WHERE path = ?";

static const char *SQL_SELECT_MTIME = "SELECT mtime FROM music_metadata WHERE path = ?";

static const char *SQL_COUNT = "SELECT COUNT(*) FROM music_metadata";

static const char *SQL_STATS = "SELECT COUNT(*), "
                               "COUNT(DISTINCT CASE WHEN artist != '' THEN artist END), "
                               "COUNT(DISTINCT CASE WHEN album != '' THEN album END) "
                               "FROM music_metadata " DEDUP_WHERE;

/* Search query: match pattern against title, artist, album, genre, or filename.
 * Dedup: exclude rows where a higher-priority source has the same track. */
static const char *SQL_SEARCH = "SELECT path, title, artist, album, genre, duration_sec, source "
                                "FROM music_metadata "
                                "WHERE (title LIKE ? ESCAPE '\\' OR artist LIKE ? ESCAPE '\\' "
                                "   OR album LIKE ? ESCAPE '\\' OR genre LIKE ? ESCAPE '\\' "
                                "   OR path LIKE ? ESCAPE '\\') " DEDUP_CLAUSE
                                "ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, "
                                "title COLLATE NOCASE "
                                "LIMIT ?";


static const char *SQL_LIST_PAGED =
    "SELECT path, title, artist, album, genre, duration_sec, source "
    "FROM music_metadata " DEDUP_WHERE
    "ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, title COLLATE NOCASE "
    "LIMIT ? OFFSET ?";

/* List unique artists (deduped) */
static const char *SQL_LIST_ARTISTS = "SELECT DISTINCT artist FROM music_metadata "
                                      "WHERE artist != '' " DEDUP_CLAUSE
                                      "ORDER BY artist COLLATE NOCASE "
                                      "LIMIT ? OFFSET ?";

/* List unique albums (deduped) */
static const char *SQL_LIST_ALBUMS = "SELECT DISTINCT album FROM music_metadata "
                                     "WHERE album != '' " DEDUP_CLAUSE
                                     "ORDER BY album COLLATE NOCASE "
                                     "LIMIT ? OFFSET ?";

/* List artists with stats (album count, track count) - dedup applied before GROUP BY */
static const char *SQL_LIST_ARTISTS_WITH_STATS =
    "SELECT artist, "
    "       COUNT(DISTINCT CASE WHEN album != '' THEN album END) as album_count, "
    "       COUNT(*) as track_count "
    "FROM music_metadata "
    "WHERE artist != '' " DEDUP_CLAUSE "GROUP BY artist "
    "ORDER BY artist COLLATE NOCASE "
    "LIMIT ? OFFSET ?";

/* List albums with stats (track count, artist) - dedup applied before GROUP BY */
static const char *SQL_LIST_ALBUMS_WITH_STATS = "SELECT album, "
                                                "       COUNT(*) as track_count, "
                                                "       MAX(artist) as artist "
                                                "FROM music_metadata "
                                                "WHERE album != '' " DEDUP_CLAUSE "GROUP BY album "
                                                "ORDER BY album COLLATE NOCASE "
                                                "LIMIT ? OFFSET ?";

/* Get tracks by artist (deduped) */
static const char *SQL_GET_BY_ARTIST =
    "SELECT path, title, artist, album, genre, duration_sec, source "
    "FROM music_metadata "
    "WHERE artist = ? " DEDUP_CLAUSE "ORDER BY album COLLATE NOCASE, title COLLATE NOCASE "
    "LIMIT ?";

/* Get tracks by album (deduped) */
static const char *SQL_GET_BY_ALBUM =
    "SELECT path, title, artist, album, genre, duration_sec, source "
    "FROM music_metadata "
    "WHERE album = ? " DEDUP_CLAUSE "ORDER BY path "
    "LIMIT ?";

/* =============================================================================
 * Module State
 * ============================================================================= */

static sqlite3 *g_db = NULL;
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

/* =============================================================================
 * Internal Helper Functions
 * ============================================================================= */

/**
 * @brief Build display name from metadata: "Artist - Title" or filename fallback
 */
static void build_display_name(music_search_result_t *result) {
   if (result->artist[0] && result->title[0]) {
      /* Truncate artist/title to fit in display_name with " - " separator */
      int written = snprintf(result->display_name, sizeof(result->display_name), "%.200s - %.200s",
                             result->artist, result->title);
      if (written >= (int)sizeof(result->display_name)) {
         result->display_name[sizeof(result->display_name) - 1] = '\0';
      }
   } else if (result->title[0]) {
      safe_strncpy(result->display_name, result->title, sizeof(result->display_name));
   } else {
      /* Fallback to filename */
      const char *fname = strrchr(result->path, '/');
      safe_strncpy(result->display_name, fname ? fname + 1 : result->path,
                   sizeof(result->display_name));
   }
}

/**
 * @brief Execute a simple SQL statement (no results)
 */
static int exec_sql(const char *sql) {
   char *err_msg = NULL;
   int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err_msg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("SQL error: %s (query: %s)", err_msg, sql);
      sqlite3_free(err_msg);
      return FAILURE;
   }
   return SUCCESS;
}

/**
 * @brief Check if a file extension is supported
 */
static bool is_supported_extension(const char *filename) {
   const char *dot = strrchr(filename, '.');
   if (!dot)
      return false;

   const char **extensions = audio_decoder_get_extensions();
   for (int i = 0; extensions[i] != NULL; i++) {
      if (strcasecmp(dot, extensions[i]) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Get file modification time
 */
static time_t get_file_mtime(const char *path) {
   struct stat st;
   if (stat(path, &st) != 0) {
      return 0;
   }
   return st.st_mtime;
}

/**
 * @brief Get stored mtime for a path from database
 * @return mtime, or 0 if not found
 */
static time_t get_db_mtime(const char *path) {
   sqlite3_stmt *stmt = NULL;
   time_t mtime = 0;

   if (sqlite3_prepare_v2(g_db, SQL_SELECT_MTIME, -1, &stmt, NULL) != SQLITE_OK) {
      return 0;
   }

   sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      mtime = (time_t)sqlite3_column_int64(stmt, 0);
   }

   sqlite3_finalize(stmt);
   return mtime;
}

/**
 * @brief Insert or update a track in the database
 */
static int insert_track(const char *path, time_t mtime, const audio_metadata_t *meta) {
   sqlite3_stmt *stmt = NULL;
   int rc;

   rc = sqlite3_prepare_v2(g_db, SQL_INSERT_OR_REPLACE, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Failed to prepare insert statement: %s", sqlite3_errmsg(g_db));
      return FAILURE;
   }

   sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, (sqlite3_int64)mtime);
   sqlite3_bind_text(stmt, 3, meta->title[0] ? meta->title : NULL, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, meta->artist[0] ? meta->artist : NULL, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, meta->album[0] ? meta->album : NULL, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 6, (int)meta->duration_sec);

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("Failed to insert track: %s", sqlite3_errmsg(g_db));
      return FAILURE;
   }

   return SUCCESS;
}

/* Forward declaration */
static void scan_directory_recursive(const char *dir_path,
                                     music_db_scan_stats_t *stats,
                                     char **seen_paths,
                                     int *seen_count,
                                     int max_seen,
                                     int depth);

/**
 * @brief Yield mutex to allow pending searches during long scans
 *
 * Commits current transaction, releases mutex briefly, then re-acquires
 * and starts a new transaction. This reduces lock contention.
 */
static void scan_yield_for_searches(void) {
   exec_sql("COMMIT");
   pthread_mutex_unlock(&g_db_mutex);

   /* Brief yield to allow pending searches to acquire mutex */
   struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
   nanosleep(&ts, NULL);

   pthread_mutex_lock(&g_db_mutex);
   exec_sql("BEGIN TRANSACTION");
}

/**
 * @brief Recursively scan a directory for music files
 */
static void scan_directory_recursive(const char *dir_path,
                                     music_db_scan_stats_t *stats,
                                     char **seen_paths,
                                     int *seen_count,
                                     int max_seen,
                                     int depth) {
   if (depth >= MAX_DIRECTORY_DEPTH) {
      OLOG_WARNING("Max directory depth reached at: %s", dir_path);
      return;
   }

   DIR *dir = opendir(dir_path);
   if (!dir) {
      return;
   }

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      /* Skip . and .. */
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
         continue;
      }

      /* Skip symlinks to prevent escaping music directory */
      if (entry->d_type == DT_LNK) {
         continue;
      }

      /* Build full path */
      char full_path[MUSIC_DB_PATH_MAX];
      int written = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
      if (written >= (int)sizeof(full_path)) {
         continue; /* Path too long */
      }

      /* Handle DT_UNKNOWN (some filesystems) by checking with lstat */
      unsigned char d_type = entry->d_type;
      if (d_type == DT_UNKNOWN) {
         struct stat st;
         if (lstat(full_path, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
               continue; /* Skip symlinks */
            } else if (S_ISDIR(st.st_mode)) {
               d_type = DT_DIR;
            } else if (S_ISREG(st.st_mode)) {
               d_type = DT_REG;
            }
         }
      }

      if (d_type == DT_DIR) {
         /* Recurse into subdirectory */
         scan_directory_recursive(full_path, stats, seen_paths, seen_count, max_seen, depth + 1);
      } else if (d_type == DT_REG && is_supported_extension(entry->d_name)) {
         stats->files_scanned++;

         /* Track this path as "seen" for cleanup */
         if (*seen_count < max_seen) {
            seen_paths[*seen_count] = strdup(full_path);
            if (seen_paths[*seen_count]) {
               (*seen_count)++;
            }
         }

         /* Check if file needs updating */
         time_t file_mtime = get_file_mtime(full_path);
         time_t db_mtime = get_db_mtime(full_path);

         if (db_mtime == 0) {
            /* New file - parse and insert */
            audio_metadata_t meta = { 0 };
            audio_decoder_get_metadata(full_path, &meta);
            if (insert_track(full_path, file_mtime, &meta) == 0) {
               stats->files_added++;
            }
         } else if (file_mtime > db_mtime) {
            /* File changed - reparse and update */
            audio_metadata_t meta = { 0 };
            audio_decoder_get_metadata(full_path, &meta);
            if (insert_track(full_path, file_mtime, &meta) == 0) {
               stats->files_updated++;
            }
         } else {
            /* File unchanged */
            stats->files_skipped++;
         }

         /* Periodically yield mutex to allow pending searches */
         if (stats->files_scanned % SCAN_YIELD_INTERVAL == 0) {
            scan_yield_for_searches();
         }
      }
   }

   closedir(dir);
}

/* =============================================================================
 * Public API Implementation
 * ============================================================================= */

int music_db_init(const char *db_path) {
   pthread_mutex_lock(&g_db_mutex);

   if (g_initialized) {
      OLOG_WARNING("Music database already initialized");
      pthread_mutex_unlock(&g_db_mutex);
      return SUCCESS;
   }

   /* Expand tilde in path (e.g., ~/.config/dawn/music.db) */
   char expanded_path[MUSIC_DB_PATH_MAX];
   if (!path_expand_tilde(db_path, expanded_path, sizeof(expanded_path))) {
      OLOG_ERROR("Failed to expand database path: %s", db_path);
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Ensure parent directory exists */
   if (!path_ensure_parent_dir(expanded_path)) {
      OLOG_ERROR("Failed to create database directory for: %s", expanded_path);
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   int rc = sqlite3_open(expanded_path, &g_db);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Failed to open music database '%s': %s", expanded_path, sqlite3_errmsg(g_db));
      sqlite3_close(g_db);
      g_db = NULL;
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Create tables and indexes */
   if (exec_sql(SQL_CREATE_TABLE) != 0 || exec_sql(SQL_CREATE_INDEX_ALBUM) != 0 ||
       exec_sql(SQL_CREATE_INDEX_TITLE) != 0) {
      OLOG_ERROR("Failed to create music database schema");
      sqlite3_close(g_db);
      g_db = NULL;
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Schema migration: add columns for unified multi-source DB.
    * Use sqlite3_exec() directly — NOT exec_sql() — because ALTER TABLE returns
    * SQLITE_ERROR for duplicate columns on restart. We check the error message to
    * differentiate expected "duplicate column" from real failures. */
   {
      static const char *migrations[] = {
         "ALTER TABLE music_metadata ADD COLUMN source INTEGER DEFAULT 0",
         "ALTER TABLE music_metadata ADD COLUMN genre TEXT",
         "ALTER TABLE music_metadata ADD COLUMN rating_key TEXT",
         "ALTER TABLE music_metadata ADD COLUMN sync_gen INTEGER DEFAULT 0",
      };
      for (int i = 0; i < (int)(sizeof(migrations) / sizeof(migrations[0])); i++) {
         char *err = NULL;
         sqlite3_exec(g_db, migrations[i], NULL, NULL, &err);
         if (err) {
            if (!strstr(err, "duplicate column"))
               OLOG_WARNING("Schema migration: %s", err);
            sqlite3_free(err);
         }
      }
   }

   /* New indexes for multi-source queries */
   exec_sql("CREATE INDEX IF NOT EXISTS idx_music_source ON music_metadata(source)");
   exec_sql("CREATE INDEX IF NOT EXISTS idx_music_genre ON music_metadata(genre)");
   exec_sql("CREATE INDEX IF NOT EXISTS idx_music_rating_key ON music_metadata(rating_key)");

   /* Composite index for dedup subquery — single B-tree seek for NOT EXISTS check.
    * COLLATE NOCASE must match the dedup clause collation for index to be used. */
   exec_sql("DROP INDEX IF EXISTS idx_music_dedup");
   exec_sql("CREATE INDEX IF NOT EXISTS idx_music_dedup "
            "ON music_metadata(artist COLLATE NOCASE, album COLLATE NOCASE, "
            "title COLLATE NOCASE, source)");

   /* Drop redundant individual artist index (covered by composite idx_music_dedup) */
   sqlite3_exec(g_db, "DROP INDEX IF EXISTS idx_music_artist", NULL, NULL, NULL);

   /* Enable WAL mode for better concurrent access */
   exec_sql("PRAGMA journal_mode=WAL");
   exec_sql("PRAGMA synchronous=NORMAL");

   /* Set busy timeout to prevent query blocking during scans (5 seconds) */
   sqlite3_busy_timeout(g_db, 5000);

   g_initialized = true;
   OLOG_INFO("Music database initialized: %s", expanded_path);

   pthread_mutex_unlock(&g_db_mutex);
   return SUCCESS;
}

void music_db_cleanup(void) {
   pthread_mutex_lock(&g_db_mutex);

   if (g_db) {
      sqlite3_close(g_db);
      g_db = NULL;
   }
   g_initialized = false;

   pthread_mutex_unlock(&g_db_mutex);
   OLOG_INFO("Music database closed");
}

bool music_db_is_initialized(void) {
   return g_initialized;
}

int music_db_scan(const char *music_dir, music_db_scan_stats_t *stats) {
   if (!music_dir) {
      return FAILURE;
   }

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      OLOG_ERROR("Music database not initialized");
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Initialize stats */
   music_db_scan_stats_t local_stats = { 0 };

   /* Allocate array to track seen files for cleanup
    * Using 10k limit balances memory usage (~80KB for pointers) with typical library sizes.
    * For larger libraries, some deleted files may not be detected until next scan. */
   int max_seen = 10000;
   char **seen_paths = calloc(max_seen, sizeof(char *));
   int seen_count = 0;

   if (!seen_paths) {
      OLOG_ERROR("Failed to allocate seen paths array");
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Begin transaction for better performance */
   exec_sql("BEGIN TRANSACTION");

   /* Scan directory recursively */
   scan_directory_recursive(music_dir, &local_stats, seen_paths, &seen_count, max_seen, 0);

   /* Warn if we hit the tracking limit */
   if (seen_count >= max_seen) {
      OLOG_WARNING("File tracking limit reached (%d files). Some deleted files may persist in DB.",
                   max_seen);
   }

   /* Remove files that no longer exist */
   /* Build a list of paths to keep and delete everything else */
   /* For efficiency, we delete paths NOT in the seen list */
   if (seen_count > 0) {
      /* Delete files not in our seen list using a temporary table approach */
      exec_sql("CREATE TEMP TABLE IF NOT EXISTS seen_paths (path TEXT PRIMARY KEY)");
      exec_sql("DELETE FROM seen_paths");

      sqlite3_stmt *insert_stmt = NULL;
      sqlite3_prepare_v2(g_db, "INSERT INTO seen_paths (path) VALUES (?)", -1, &insert_stmt, NULL);

      for (int i = 0; i < seen_count; i++) {
         sqlite3_bind_text(insert_stmt, 1, seen_paths[i], -1, SQLITE_STATIC);
         sqlite3_step(insert_stmt);
         sqlite3_reset(insert_stmt);
      }
      sqlite3_finalize(insert_stmt);

      /* Count stale local files (don't touch remote source rows) */
      char count_sql[256];
      snprintf(count_sql, sizeof(count_sql),
               "SELECT COUNT(*) FROM music_metadata "
               "WHERE source = %d "
               "AND path NOT IN (SELECT path FROM seen_paths)",
               MUSIC_SOURCE_LOCAL);
      sqlite3_stmt *count_stmt = NULL;
      sqlite3_prepare_v2(g_db, count_sql, -1, &count_stmt, NULL);
      if (sqlite3_step(count_stmt) == SQLITE_ROW) {
         local_stats.files_removed = sqlite3_column_int(count_stmt, 0);
      }
      sqlite3_finalize(count_stmt);

      /* Delete stale local files (don't touch remote source rows) */
      char delete_sql[256];
      snprintf(delete_sql, sizeof(delete_sql),
               "DELETE FROM music_metadata "
               "WHERE source = %d "
               "AND path NOT IN (SELECT path FROM seen_paths)",
               MUSIC_SOURCE_LOCAL);
      exec_sql(delete_sql);
      exec_sql("DROP TABLE seen_paths");
   }

   /* Commit transaction */
   exec_sql("COMMIT");

   /* Free seen paths */
   for (int i = 0; i < seen_count; i++) {
      free(seen_paths[i]);
   }
   free(seen_paths);

   pthread_mutex_unlock(&g_db_mutex);

   /* Return stats if requested */
   if (stats) {
      *stats = local_stats;
   }

   OLOG_INFO("Music scan complete: %d scanned, %d added, %d updated, %d removed, %d unchanged",
             local_stats.files_scanned, local_stats.files_added, local_stats.files_updated,
             local_stats.files_removed, local_stats.files_skipped);

   return SUCCESS;
}

int music_db_get_track_count(int *count_out) {
   if (!count_out) {
      return FAILURE;
   }
   *count_out = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;

   if (sqlite3_prepare_v2(g_db, SQL_COUNT, -1, &stmt, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *count_out = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);

   pthread_mutex_unlock(&g_db_mutex);
   return SUCCESS;
}

int music_db_get_stats(music_db_stats_t *stats) {
   if (!stats) {
      return FAILURE;
   }

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   memset(stats, 0, sizeof(*stats));
   sqlite3_stmt *stmt = NULL;

   /* Get all stats in a single query */
   if (sqlite3_prepare_v2(g_db, SQL_STATS, -1, &stmt, NULL) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         stats->track_count = sqlite3_column_int(stmt, 0);
         stats->artist_count = sqlite3_column_int(stmt, 1);
         stats->album_count = sqlite3_column_int(stmt, 2);
      }
      sqlite3_finalize(stmt);
   }

   pthread_mutex_unlock(&g_db_mutex);
   return SUCCESS;
}

int music_db_search(const char *pattern,
                    music_search_result_t *results,
                    int max_results,
                    int *count_out) {
   if (!pattern || !results || max_results <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;

   /* Validate pattern: reject overly short or broad searches to prevent DoS */
   size_t len = strlen(pattern);
   if (len > AUDIO_METADATA_STRING_MAX) {
      OLOG_WARNING("music_db_search: Pattern too long (%zu chars)", len);
      return FAILURE;
   }

   /* Count non-wildcard characters to ensure meaningful search */
   size_t content_chars = 0;
   for (size_t i = 0; i < len; i++) {
      if (pattern[i] != ' ' && pattern[i] != '*') {
         content_chars++;
      }
   }
   if (content_chars < 2) {
      OLOG_WARNING("music_db_search: Pattern too broad (need at least 2 characters)");
      return SUCCESS; /* Return empty results rather than error */
   }

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Convert search pattern to SQL LIKE pattern (add % wildcards).
    * Escape literal % and _ to prevent LIKE wildcard injection.
    * Limit wildcards to prevent excessive pattern complexity. */
   char sql_pattern[AUDIO_METADATA_STRING_MAX * 2];
   size_t j = 0;
   int wildcard_count = 0;
   bool last_was_wildcard = false; /* Track actual wildcards, not escaped literals */
   const int max_wildcards = 10;   /* Limit total wildcards to prevent query slowdown */

   sql_pattern[j++] = '%';
   wildcard_count++;
   last_was_wildcard = true;

   for (size_t i = 0; i < len && j < sizeof(sql_pattern) - 4; i++) {
      if (pattern[i] == ' ' || pattern[i] == '*') {
         /* Skip consecutive wildcards and respect limit */
         if (!last_was_wildcard && wildcard_count < max_wildcards) {
            sql_pattern[j++] = '%';
            wildcard_count++;
            last_was_wildcard = true;
         }
      } else {
         /* Escape literal LIKE wildcards */
         if (pattern[i] == '%' || pattern[i] == '_' || pattern[i] == '\\') {
            sql_pattern[j++] = '\\';
         }
         sql_pattern[j++] = pattern[i];
         last_was_wildcard = false;
      }
   }

   /* Add trailing wildcard if not already present */
   if (!last_was_wildcard && wildcard_count < max_wildcards) {
      sql_pattern[j++] = '%';
   }
   sql_pattern[j] = '\0';

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_SEARCH, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Failed to prepare search: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   /* Bind pattern to all search columns */
   sqlite3_bind_text(stmt, 1, sql_pattern, -1, SQLITE_STATIC); /* title */
   sqlite3_bind_text(stmt, 2, sql_pattern, -1, SQLITE_STATIC); /* artist */
   sqlite3_bind_text(stmt, 3, sql_pattern, -1, SQLITE_STATIC); /* album */
   sqlite3_bind_text(stmt, 4, sql_pattern, -1, SQLITE_STATIC); /* genre */
   sqlite3_bind_text(stmt, 5, sql_pattern, -1, SQLITE_STATIC); /* path */
   sqlite3_bind_int(stmt, 6, max_results);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
      music_search_result_t *r = &results[count];
      memset(r, 0, sizeof(*r));

      const char *path = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *artist = (const char *)sqlite3_column_text(stmt, 2);
      const char *album = (const char *)sqlite3_column_text(stmt, 3);
      const char *genre = (const char *)sqlite3_column_text(stmt, 4);
      int duration = sqlite3_column_int(stmt, 5);
      int source = sqlite3_column_int(stmt, 6);

      if (path)
         safe_strncpy(r->path, path, sizeof(r->path));
      if (title)
         safe_strncpy(r->title, title, sizeof(r->title));
      if (artist)
         safe_strncpy(r->artist, artist, sizeof(r->artist));
      if (album)
         safe_strncpy(r->album, album, sizeof(r->album));
      if (genre)
         safe_strncpy(r->genre, genre, sizeof(r->genre));
      r->duration_sec = (uint32_t)duration;
      r->source = (music_source_t)source;

      build_display_name(r);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

/* Case-insensitive substring test (avoids depending on GNU strcasestr here). */
static bool music_ci_contains(const char *haystack, const char *needle) {
   size_t nlen = strlen(needle);
   if (nlen == 0) {
      return true;
   }
   for (const char *p = haystack; *p; p++) {
      if (strncasecmp(p, needle, nlen) == 0) {
         return true;
      }
   }
   return false;
}

int music_db_pick_best_match(const music_search_result_t *results,
                             int count,
                             const char *title_query,
                             const char *artist) {
   if (!results || count <= 0) {
      return 0;
   }
   if (!title_query) {
      title_query = "";
   }

   size_t qlen = strlen(title_query);
   int best = 0;
   long best_score = -1; /* so candidate 0 always initializes the winner */
   size_t best_len = 0;

   for (int i = 0; i < count; i++) {
      const char *t = results[i].title;
      long score = 0;

      /* Title closeness: exact > prefix > substring. */
      if (qlen > 0) {
         if (strcasecmp(t, title_query) == 0) {
            score += 300;
         } else if (strncasecmp(t, title_query, qlen) == 0) {
            score += 200;
         } else if (music_ci_contains(t, title_query)) {
            score += 100;
         }
      }

      /* A matching artist dominates (disambiguates "Artist - Title"). */
      if (artist && artist[0] && music_ci_contains(results[i].artist, artist)) {
         score += 1000;
      }

      size_t len = strlen(t);
      /* Higher score wins; among equally-scored REAL matches prefer the shorter
       * (closer) title. When nothing matches (score 0 for all), this keeps
       * candidate 0 — i.e. the DB's first row — rather than an arbitrary shortest. */
      bool better = (score > best_score) || (score == best_score && score > 0 && len < best_len);
      if (better) {
         best_score = score;
         best_len = len;
         best = i;
      }
   }
   return best;
}

int music_db_get_by_path(const char *path, music_search_result_t *result, bool *found_out) {
   if (!path || !result || !found_out) {
      return FAILURE;
   }
   *found_out = false;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_SELECT_BY_PATH, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      memset(result, 0, sizeof(*result));

      const char *p = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *artist = (const char *)sqlite3_column_text(stmt, 2);
      const char *album = (const char *)sqlite3_column_text(stmt, 3);
      int duration = sqlite3_column_int(stmt, 4);

      if (p)
         safe_strncpy(result->path, p, sizeof(result->path));
      if (title)
         safe_strncpy(result->title, title, sizeof(result->title));
      if (artist)
         safe_strncpy(result->artist, artist, sizeof(result->artist));
      if (album)
         safe_strncpy(result->album, album, sizeof(result->album));
      result->duration_sec = (uint32_t)duration;

      build_display_name(result);
      *found_out = true;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   return SUCCESS;
}

int music_db_list(music_search_result_t *results, int max_results, int *count_out) {
   return music_db_list_paged(results, max_results, 0, count_out);
}

int music_db_list_paged(music_search_result_t *results,
                        int max_results,
                        int offset,
                        int *count_out) {
   if (!results || max_results <= 0 || !count_out)
      return FAILURE;
   *count_out = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_LIST_PAGED, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_list_paged: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_int(stmt, 1, max_results);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
      music_search_result_t *r = &results[count];
      memset(r, 0, sizeof(*r));

      const char *path = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *artist = (const char *)sqlite3_column_text(stmt, 2);
      const char *album = (const char *)sqlite3_column_text(stmt, 3);
      const char *genre = (const char *)sqlite3_column_text(stmt, 4);
      int duration = sqlite3_column_int(stmt, 5);
      int source = sqlite3_column_int(stmt, 6);

      if (path)
         safe_strncpy(r->path, path, sizeof(r->path));
      if (title)
         safe_strncpy(r->title, title, sizeof(r->title));
      if (artist)
         safe_strncpy(r->artist, artist, sizeof(r->artist));
      if (album)
         safe_strncpy(r->album, album, sizeof(r->album));
      if (genre)
         safe_strncpy(r->genre, genre, sizeof(r->genre));
      r->duration_sec = (uint32_t)duration;
      r->source = (music_source_t)source;

      build_display_name(r);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

int music_db_list_artists(char (*artists)[AUDIO_METADATA_STRING_MAX],
                          int max_artists,
                          int offset,
                          int *count_out) {
   if (!artists || max_artists <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;
   if (offset < 0)
      offset = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_LIST_ARTISTS, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_list_artists: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_int(stmt, 1, max_artists);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_artists) {
      const char *artist = (const char *)sqlite3_column_text(stmt, 0);
      if (artist) {
         safe_strncpy(artists[count], artist, AUDIO_METADATA_STRING_MAX);
         count++;
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

int music_db_list_albums(char (*albums)[AUDIO_METADATA_STRING_MAX],
                         int max_albums,
                         int offset,
                         int *count_out) {
   if (!albums || max_albums <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;
   if (offset < 0)
      offset = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_LIST_ALBUMS, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_list_albums: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_int(stmt, 1, max_albums);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_albums) {
      const char *album = (const char *)sqlite3_column_text(stmt, 0);
      if (album) {
         safe_strncpy(albums[count], album, AUDIO_METADATA_STRING_MAX);
         count++;
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

int music_db_list_artists_with_stats(music_artist_info_t *artists,
                                     int max_artists,
                                     int offset,
                                     int *count_out) {
   if (!artists || max_artists <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;
   if (offset < 0)
      offset = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_LIST_ARTISTS_WITH_STATS, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_list_artists_with_stats: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_int(stmt, 1, max_artists);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_artists) {
      const char *name = (const char *)sqlite3_column_text(stmt, 0);
      if (name) {
         safe_strncpy(artists[count].name, name, AUDIO_METADATA_STRING_MAX);
         artists[count].album_count = sqlite3_column_int(stmt, 1);
         artists[count].track_count = sqlite3_column_int(stmt, 2);
         count++;
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

int music_db_list_albums_with_stats(music_album_info_t *albums,
                                    int max_albums,
                                    int offset,
                                    int *count_out) {
   if (!albums || max_albums <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;
   if (offset < 0)
      offset = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_LIST_ALBUMS_WITH_STATS, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_list_albums_with_stats: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_int(stmt, 1, max_albums);
   sqlite3_bind_int(stmt, 2, offset);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_albums) {
      const char *name = (const char *)sqlite3_column_text(stmt, 0);
      if (name) {
         safe_strncpy(albums[count].name, name, AUDIO_METADATA_STRING_MAX);
         albums[count].track_count = sqlite3_column_int(stmt, 1);
         const char *artist = (const char *)sqlite3_column_text(stmt, 2);
         if (artist) {
            safe_strncpy(albums[count].artist, artist, AUDIO_METADATA_STRING_MAX);
         } else {
            albums[count].artist[0] = '\0';
         }
         count++;
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

int music_db_get_by_artist(const char *artist,
                           music_search_result_t *results,
                           int max_results,
                           int *count_out) {
   if (!artist || !results || max_results <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_GET_BY_ARTIST, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_get_by_artist: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_text(stmt, 1, artist, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, max_results);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
      music_search_result_t *r = &results[count];
      memset(r, 0, sizeof(*r));

      const char *path = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *art = (const char *)sqlite3_column_text(stmt, 2);
      const char *album = (const char *)sqlite3_column_text(stmt, 3);
      const char *genre = (const char *)sqlite3_column_text(stmt, 4);
      int duration = sqlite3_column_int(stmt, 5);
      int source = sqlite3_column_int(stmt, 6);

      if (path)
         safe_strncpy(r->path, path, sizeof(r->path));
      if (title)
         safe_strncpy(r->title, title, sizeof(r->title));
      if (art)
         safe_strncpy(r->artist, art, sizeof(r->artist));
      if (album)
         safe_strncpy(r->album, album, sizeof(r->album));
      if (genre)
         safe_strncpy(r->genre, genre, sizeof(r->genre));
      r->duration_sec = (uint32_t)duration;
      r->source = (music_source_t)source;

      build_display_name(r);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}

int music_db_get_by_album(const char *album,
                          music_search_result_t *results,
                          int max_results,
                          int *count_out) {
   if (!album || !results || max_results <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;

   pthread_mutex_lock(&g_db_mutex);

   if (!g_initialized) {
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_db, SQL_GET_BY_ALBUM, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("music_db_get_by_album: prepare failed: %s", sqlite3_errmsg(g_db));
      pthread_mutex_unlock(&g_db_mutex);
      return FAILURE;
   }

   sqlite3_bind_text(stmt, 1, album, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, max_results);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
      music_search_result_t *r = &results[count];
      memset(r, 0, sizeof(*r));

      const char *path = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *artist = (const char *)sqlite3_column_text(stmt, 2);
      const char *alb = (const char *)sqlite3_column_text(stmt, 3);
      const char *genre = (const char *)sqlite3_column_text(stmt, 4);
      int duration = sqlite3_column_int(stmt, 5);
      int source = sqlite3_column_int(stmt, 6);

      if (path)
         safe_strncpy(r->path, path, sizeof(r->path));
      if (title)
         safe_strncpy(r->title, title, sizeof(r->title));
      if (artist)
         safe_strncpy(r->artist, artist, sizeof(r->artist));
      if (alb)
         safe_strncpy(r->album, alb, sizeof(r->album));
      if (genre)
         safe_strncpy(r->genre, genre, sizeof(r->genre));
      r->duration_sec = (uint32_t)duration;
      r->source = (music_source_t)source;

      build_display_name(r);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&g_db_mutex);

   *count_out = count;
   return SUCCESS;
}
