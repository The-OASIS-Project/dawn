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
 * Music Metadata Database
 *
 * SQLite-based cache for audio file metadata (artist, title, album).
 * Enables fast search by metadata fields instead of just filename.
 *
 * Features:
 *   - Incremental scanning (only reparse changed files based on mtime)
 *   - Indexed search by artist, title, album
 *   - Automatic cleanup of deleted files
 *
 * Thread Safety:
 *   - init/cleanup are NOT thread-safe (call from main thread)
 *   - scan/search are thread-safe (use internal mutex)
 */

#ifndef MUSIC_DB_H
#define MUSIC_DB_H

#include <stdbool.h>
#include <stddef.h>

#include "audio/audio_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/** Maximum path length for music files */
#define MUSIC_DB_PATH_MAX 1024

/** Maximum search results returned */
#define MUSIC_DB_MAX_RESULTS 100

/* =============================================================================
 * Types
 * ============================================================================= */

/**
 * @brief Search result entry
 *
 * Contains file path and cached metadata for display.
 */
typedef struct {
   char path[MUSIC_DB_PATH_MAX];                     /**< Full path to audio file */
   char title[AUDIO_METADATA_STRING_MAX];            /**< Track title */
   char artist[AUDIO_METADATA_STRING_MAX];           /**< Artist name */
   char album[AUDIO_METADATA_STRING_MAX];            /**< Album name */
   char display_name[AUDIO_METADATA_STRING_MAX * 2]; /**< "Artist - Title" or filename */
   uint32_t duration_sec;                            /**< Duration in seconds */
} music_search_result_t;

/**
 * @brief Scan statistics
 */
typedef struct {
   int files_scanned; /**< Total files found in directory */
   int files_added;   /**< New files added to database */
   int files_updated; /**< Files updated (mtime changed) */
   int files_removed; /**< Deleted files removed from database */
   int files_skipped; /**< Files unchanged (no reparse needed) */
} music_db_scan_stats_t;

/* =============================================================================
 * Initialization / Cleanup
 * ============================================================================= */

/**
 * @brief Initialize the music database
 *
 * Opens or creates the SQLite database at the specified path.
 * Creates tables and indexes if they don't exist.
 *
 * @param db_path Path to SQLite database file (created if doesn't exist)
 * @return 0 on success, non-zero on failure
 */
int music_db_init(const char *db_path);

/**
 * @brief Close the music database
 *
 * Releases all resources. Safe to call multiple times.
 */
void music_db_cleanup(void);

/**
 * @brief Check if database is initialized
 *
 * @return true if database is open and ready
 */
bool music_db_is_initialized(void);

/* =============================================================================
 * Scanning
 * ============================================================================= */

/**
 * @brief Scan a directory for music files and update the database
 *
 * Performs incremental scanning:
 *   1. Walks the directory recursively finding audio files
 *   2. For new files: parse metadata and insert
 *   3. For existing files with changed mtime: reparse and update
 *   4. For deleted files: remove from database
 *
 * This operation can be slow for large music libraries on first scan,
 * but subsequent scans are fast due to mtime checking.
 *
 * @param music_dir Directory to scan (absolute path)
 * @param stats Optional output for scan statistics (can be NULL)
 * @return 0 on success, non-zero on failure
 */
int music_db_scan(const char *music_dir, music_db_scan_stats_t *stats);

/**
 * @brief Get the number of tracks in the database
 *
 * @return Number of indexed tracks, or -1 on error
 */
int music_db_get_track_count(void);

/**
 * @brief Database statistics
 */
typedef struct {
   int track_count;  /**< Total number of tracks */
   int artist_count; /**< Number of unique artists */
   int album_count;  /**< Number of unique albums */
} music_db_stats_t;

/**
 * @brief Get database statistics
 *
 * @param stats Output for statistics
 * @return 0 on success, non-zero on failure
 */
int music_db_get_stats(music_db_stats_t *stats);

/* =============================================================================
 * Search
 * ============================================================================= */

/**
 * @brief Search for music by pattern
 *
 * Searches artist, title, and album fields using SQL LIKE patterns.
 * The pattern is matched against each field independently.
 *
 * @param pattern Search pattern (wildcards: % for any chars, _ for single char)
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @return Number of results found, or -1 on error
 */
int music_db_search(const char *pattern, music_search_result_t *results, int max_results);

/**
 * @brief Get metadata for a specific file from the database
 *
 * @param path Full path to audio file
 * @param result Output for metadata
 * @return 0 on success (found), 1 if not found, -1 on error
 */
int music_db_get_by_path(const char *path, music_search_result_t *result);

/**
 * @brief List tracks in the database (no search filtering)
 *
 * Returns tracks ordered by artist, album, title.
 *
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @return Number of results found, or -1 on error
 */
int music_db_list(music_search_result_t *results, int max_results);

/**
 * @brief List tracks with pagination
 *
 * Returns tracks ordered by artist, album, title with offset support.
 *
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @param offset Number of tracks to skip (for pagination, 0 = start)
 * @return Number of results found, or -1 on error
 */
int music_db_list_paged(music_search_result_t *results, int max_results, int offset);

/**
 * @brief List unique artists in the database
 *
 * Returns distinct artist names, ordered alphabetically.
 *
 * @param artists Output array of artist name buffers
 * @param max_artists Maximum number of artists to return
 * @param offset Number of artists to skip (for pagination, 0 = start)
 * @return Number of artists found, or -1 on error
 */
int music_db_list_artists(char (*artists)[AUDIO_METADATA_STRING_MAX], int max_artists, int offset);

/**
 * @brief List unique albums in the database
 *
 * Returns distinct album names, ordered alphabetically.
 *
 * @param albums Output array of album name buffers
 * @param max_albums Maximum number of albums to return
 * @param offset Number of albums to skip (for pagination, 0 = start)
 * @return Number of albums found, or -1 on error
 */
int music_db_list_albums(char (*albums)[AUDIO_METADATA_STRING_MAX], int max_albums, int offset);

/**
 * @brief Artist info with statistics
 */
typedef struct {
   char name[AUDIO_METADATA_STRING_MAX]; /**< Artist name */
   int album_count;                      /**< Number of albums */
   int track_count;                      /**< Number of tracks */
} music_artist_info_t;

/**
 * @brief Album info with statistics
 */
typedef struct {
   char name[AUDIO_METADATA_STRING_MAX];   /**< Album name */
   char artist[AUDIO_METADATA_STRING_MAX]; /**< Primary artist */
   int track_count;                        /**< Number of tracks */
} music_album_info_t;

/**
 * @brief List artists with statistics (album count, track count)
 *
 * @param artists Output array for artist info
 * @param max_artists Maximum number of artists to return
 * @return Number of artists found, or -1 on error
 */
int music_db_list_artists_with_stats(music_artist_info_t *artists, int max_artists);

/**
 * @brief List albums with statistics (track count, artist)
 *
 * @param albums Output array for album info
 * @param max_albums Maximum number of albums to return
 * @return Number of albums found, or -1 on error
 */
int music_db_list_albums_with_stats(music_album_info_t *albums, int max_albums);

/**
 * @brief Get all tracks by a specific artist
 *
 * @param artist Artist name (exact match)
 * @param results Output array for results
 * @param max_results Maximum number of results
 * @return Number of tracks found, or -1 on error
 */
int music_db_get_by_artist(const char *artist, music_search_result_t *results, int max_results);

/**
 * @brief Get all tracks in a specific album
 *
 * @param album Album name (exact match)
 * @param results Output array for results
 * @param max_results Maximum number of results
 * @return Number of tracks found, or -1 on error
 */
int music_db_get_by_album(const char *album, music_search_result_t *results, int max_results);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_DB_H */
