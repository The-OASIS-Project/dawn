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
 * Music DB internals shared between music_db.c (lifecycle, scan, browse) and
 * music_db_query.c (search, ranked query, album inventory). Not a public API.
 */

#ifndef MUSIC_DB_INTERNAL_H
#define MUSIC_DB_INTERNAL_H

#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>

#include "audio/music_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The module's single connection, the lock serializing every use of it, and
 *  whether init completed. Defined in music_db.c. */
extern sqlite3 *g_music_db;
extern pthread_mutex_t g_music_db_mutex;
extern bool g_music_db_initialized;

/** The single result-row projection shared by every row-returning query. Column
 *  order MUST match music_db_populate_row()'s reads (0 path … 7 year); keeping it
 *  in one macro is what stops a future column from drifting one query out of
 *  contract. */
#define MUSIC_ROW_COLS "path, title, artist, album, genre, duration_sec, source, year"

/** Same-source rows count as one track only when their durations are known and
 *  agree this closely, so a Plex item indexed twice collapses but two distinct
 *  tracks that share a title on one album (e.g. a repeated "Interlude") survive.
 *  Collapse is pairwise, so a chain of copies each within the slack of the
 *  previous one collapses to its earliest row. */
#define MUSIC_DEDUP_DURATION_SLACK_SEC 2
#define MUSIC_DB_STR_(x) #x
#define MUSIC_DB_STR(x) MUSIC_DB_STR_(x)

/** Dedup condition: a row is hidden when another row has the same artist+album+
 *  title (NOCASE) and either comes from a higher-priority source (lower enum
 *  value), or is an earlier copy from the SAME source with the same genre and year
 *  and a matching known duration (a library indexed twice). Requiring equal genre
 *  and year means a same-source copy is never hidden by one that a genre/year
 *  filter would reject. The "source <=" bound makes this ONE idx_music_dedup range
 *  seek (the "+" keeps the id test off the index); the OR then filters the few
 *  rows it finds. IS (not =) so NULL IS NULL. @p cross_extra is ANDed onto the
 *  cross-source branch. */
#define MUSIC_DEDUP_EXISTS_(cross_extra)                                                  \
   "NOT EXISTS ("                                                                         \
   "   SELECT 1 FROM music_metadata m2 "                                                  \
   "   WHERE m2.artist IS music_metadata.artist COLLATE NOCASE "                          \
   "   AND m2.album IS music_metadata.album COLLATE NOCASE "                              \
   "   AND m2.title IS music_metadata.title COLLATE NOCASE "                              \
   "   AND m2.source <= music_metadata.source AND +m2.id <> music_metadata.id "           \
   "   AND ((m2.source < music_metadata.source" cross_extra ") "                          \
   "        OR (m2.id < music_metadata.id "                                               \
   "            AND m2.genre IS music_metadata.genre "                                    \
   "            AND m2.year IS music_metadata.year "                                      \
   "            AND m2.duration_sec > 0 AND music_metadata.duration_sec > 0 "             \
   "            AND abs(m2.duration_sec - music_metadata.duration_sec) <= " MUSIC_DB_STR( \
       MUSIC_DEDUP_DURATION_SLACK_SEC) "))"                                               \
                                       ") "

/** Plain dedup (browse, stats, unfiltered search). */
#define MUSIC_DEDUP_EXISTS MUSIC_DEDUP_EXISTS_("")

/** Dedup for queries filtering on genre or year: a higher-priority source only
 *  hides a copy whose genre and year it shares, so a local copy with different
 *  (or missing) tags can't hide the Plex copy that matches the filter. Such
 *  copies may then both appear — only under these filters. */
#define MUSIC_DEDUP_EXISTS_TAGGED                                              \
   MUSIC_DEDUP_EXISTS_(" AND m2.genre IS music_metadata.genre AND m2.year IS " \
                       "music_metadata.year")

/** Dedup clause appended after an existing WHERE condition. */
#define MUSIC_DEDUP_CLAUSE "AND " MUSIC_DEDUP_EXISTS

/** Dedup clause for queries that have no preceding WHERE condition. */
#define MUSIC_DEDUP_WHERE "WHERE " MUSIC_DEDUP_EXISTS

/**
 * @brief Populate a result row from a statement using the MUSIC_ROW_COLS projection
 */
void music_db_populate_row(sqlite3_stmt *stmt, music_search_result_t *r);

/**
 * @brief Register the music matching SQL functions on a connection
 *
 * Adds music_score(artist, title, album, genre, needle), music_match(field,
 * needle), music_album_key(album, artist) and music_fold(text), all deterministic
 * and backed by music_rank.c. They exist ONLY on the connection passed here, so
 * they must only be used in ad-hoc queries run on g_music_db — never in a schema
 * object (index, view, trigger, generated column, CHECK), or other connections
 * (the Plex sync handle, the sqlite3 CLI) would fail with "no such function".
 *
 * @return SUCCESS or FAILURE
 */
int music_db_register_functions(sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_DB_INTERNAL_H */
