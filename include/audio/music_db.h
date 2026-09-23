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
#include "audio/music_source.h"

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
   char genre[AUDIO_METADATA_STRING_MAX];            /**< Genre (comma-separated if multiple) */
   char display_name[AUDIO_METADATA_STRING_MAX * 2]; /**< "Artist - Title" or filename */
   uint32_t duration_sec;                            /**< Duration in seconds */
   uint32_t year;                                    /**< Release year (0 if unknown) */
   music_source_t source;                            /**< Which source this track came from */
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
 * @param count_out Output for track count
 * @return SUCCESS or FAILURE
 */
int music_db_get_track_count(int *count_out);

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
 * @brief Structured music query: fielded filters + relevance-ranked free text
 *
 * Fields left NULL / 0 are ignored. Matching is punctuation-insensitive and
 * token-based (see music_rank.h): @ref text matches rows whose artist/title/
 * album/genre hold every significant query token, ranked best-first;
 * artist/title/album are word-boundary-strict filters requiring every word;
 * genre is a substring filter; year bounds are inclusive. The file path is never
 * matched, so folder names don't leak into results.
 */
typedef struct {
   const char *text;   /**< Free text, ranked across artist/title/album/genre (NULL = none) */
   const char *artist; /**< Artist filter, whole words (NULL = none) */
   const char *title;  /**< Title filter, whole words (NULL = none) */
   const char *album;  /**< Album filter, whole words (NULL = none) */
   const char *genre;  /**< Genre filter, substring (NULL = none) */
   int year_min;       /**< Inclusive lower year bound (0 = unbounded) */
   int year_max;       /**< Inclusive upper year bound (0 = unbounded) */
   bool allow_partial; /**< When no row matches every word of @ref text, fall back to
                            rows missing one word (flagged approximate). Only for
                            surfaces that SHOW results and can say so — never for
                            play/enqueue/resolve, which act on them. */
} music_query_t;

/** Upper sanity bound for a parsed release year (exclusive); rejects garbage. */
#define MUSIC_QUERY_YEAR_MAX 3000

/** One page of a ranked query. */
typedef struct {
   int count;        /**< Rows written to the results array */
   int total;        /**< Total matching rows across all pages (exact) */
   bool approximate; /**< No row matched every query word; these are the closest
                          (one word unmatched) — say so rather than presenting them
                          as exact hits */
} music_query_page_t;

/**
 * @brief Run a structured, relevance-ranked query and return one page
 *
 * Ranks every matching row (duplicates across and within sources collapsed) by
 * relevance to the query's most specific term — text, else artist/title/album —
 * then alphabetically, and returns rows [@p offset, @p offset + @p max_results).
 * With @c allow_partial, if no row matches every word of a long free-text query,
 * the rows missing just one word are returned instead, flagged @c approximate. A
 * query with no constraints returns nothing.
 *
 * @param query       The structured query
 * @param offset      Rows to skip (< 0 treated as 0)
 * @param results     Output array (caller-owned, holds at least @p max_results)
 * @param max_results Page size / capacity of @p results
 * @param page_out    Output: count, exact total, approximate flag
 * @return SUCCESS or FAILURE
 */
int music_db_query_page(const music_query_t *query,
                        int offset,
                        music_search_result_t *results,
                        int max_results,
                        music_query_page_t *page_out);

/**
 * @brief First page of music_db_query_page() without a total (strict matching
 *        unless the query sets allow_partial)
 */
int music_db_query(const music_query_t *query,
                   music_search_result_t *results,
                   int max_results,
                   int *count_out);


/**
 * @brief Get metadata for a specific file from the database
 *
 * @param path Full path to audio file
 * @param result Output for metadata
 * @param found_out Output: true if track was found, false if not found
 * @return SUCCESS or FAILURE
 */
int music_db_get_by_path(const char *path, music_search_result_t *result, bool *found_out);

/**
 * @brief List tracks in the database (no search filtering)
 *
 * Returns tracks ordered by artist, album, title.
 *
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @param count_out Output for number of results found
 * @return SUCCESS or FAILURE
 */
int music_db_list(music_search_result_t *results, int max_results, int *count_out);

/**
 * @brief List tracks with pagination
 *
 * Returns tracks ordered by artist, album, title with offset support.
 *
 * @param results Output array for results
 * @param max_results Maximum number of results to return
 * @param offset Number of tracks to skip (for pagination, 0 = start)
 * @param count_out Output for number of results found
 * @return SUCCESS or FAILURE
 */
int music_db_list_paged(music_search_result_t *results,
                        int max_results,
                        int offset,
                        int *count_out);

/**
 * @brief List unique artists in the database
 *
 * Returns distinct artist names, ordered alphabetically.
 *
 * @param artists Output array of artist name buffers
 * @param max_artists Maximum number of artists to return
 * @param offset Number of artists to skip (for pagination, 0 = start)
 * @param count_out Output for number of artists found
 * @return SUCCESS or FAILURE
 */
int music_db_list_artists(char (*artists)[AUDIO_METADATA_STRING_MAX],
                          int max_artists,
                          int offset,
                          int *count_out);

/**
 * @brief List unique albums in the database
 *
 * Returns distinct album names, ordered alphabetically.
 *
 * @param albums Output array of album name buffers
 * @param max_albums Maximum number of albums to return
 * @param offset Number of albums to skip (for pagination, 0 = start)
 * @param count_out Output for number of albums found
 * @return SUCCESS or FAILURE
 */
int music_db_list_albums(char (*albums)[AUDIO_METADATA_STRING_MAX],
                         int max_albums,
                         int offset,
                         int *count_out);

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
   int year;         /**< Earliest release year (0 = unknown; by-artist listing only) */
   int editions;     /**< Distinct album names folded into this entry (by-artist only) */
   int artist_count; /**< Distinct track artists on the album (by-artist only) */
} music_album_info_t;

/**
 * @brief List artists with statistics (album count, track count)
 *
 * @param artists Output array for artist info
 * @param max_artists Maximum number of artists to return
 * @param offset Number of artists to skip (for pagination)
 * @param count_out Output for number of artists found
 * @return SUCCESS or FAILURE
 */
int music_db_list_artists_with_stats(music_artist_info_t *artists,
                                     int max_artists,
                                     int offset,
                                     int *count_out);

/**
 * @brief List albums with statistics (track count, artist)
 *
 * @param albums Output array for album info
 * @param max_albums Maximum number of albums to return
 * @param offset Number of albums to skip (for pagination)
 * @param count_out Output for number of albums found
 * @return SUCCESS or FAILURE
 */
int music_db_list_albums_with_stats(music_album_info_t *albums,
                                    int max_albums,
                                    int offset,
                                    int *count_out);

/**
 * @brief List an artist's albums (discography-style inventory)
 *
 * Albums with at least one track whose artist matches @p artist as whole words
 * (punctuation-insensitive, so "Ben Folds" also covers "Ben Folds Five" and
 * "Ben Folds Presents: …"). Editions/formats of one album ("(Expanded Edition)",
 * "(EP)", "[Clean]", " - Disc 1") fold into a single entry whose @c track_count is
 * the number of distinct titles across them. Oldest first; unknown years last.
 *
 * @param artist     Artist to match (required)
 * @param albums     Output array
 * @param max_albums Page size / capacity of @p albums
 * @param offset     Albums to skip (pagination)
 * @param count_out  Output: albums written
 * @param total_out  Output: total albums for the artist (NULL = don't compute)
 * @return SUCCESS or FAILURE
 */
int music_db_list_albums_by_artist(const char *artist,
                                   music_album_info_t *albums,
                                   int max_albums,
                                   int offset,
                                   int *count_out,
                                   int *total_out);

/**
 * @brief Get all tracks by a specific artist
 *
 * @param artist Artist name (exact match)
 * @param results Output array for results
 * @param max_results Maximum number of results
 * @param count_out Output for number of tracks found
 * @return SUCCESS or FAILURE
 */
int music_db_get_by_artist(const char *artist,
                           music_search_result_t *results,
                           int max_results,
                           int *count_out);

/**
 * @brief Get all tracks in a specific album
 *
 * @param album Album name (exact match)
 * @param results Output array for results
 * @param max_results Maximum number of results
 * @param count_out Output for number of tracks found
 * @return SUCCESS or FAILURE
 */
int music_db_get_by_album(const char *album,
                          music_search_result_t *results,
                          int max_results,
                          int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_DB_H */
