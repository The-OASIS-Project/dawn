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
 * Music tool search surface: the shared filter params and the LLM-facing text
 * for 'search' pages, batch searches and the per-artist album listing. Used by
 * both the voice executor (music_tool.c) and the WebUI executor
 * (webui_music_tool.c) so they parse and report identically.
 */

#ifndef MUSIC_SEARCH_H
#define MUSIC_SEARCH_H

#include "audio/music_db.h"
#include "core/strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Rows per 'search' page when the caller gives no limit. */
#define MUSIC_SEARCH_DEFAULT_LIMIT 25

/** Largest 'search' page (also what limit:0 means). */
#define MUSIC_SEARCH_MAX_LIMIT 100

/** Rows shown per query in an items[] batch search. */
#define MUSIC_BATCH_SEARCH_LIMIT 25

/** Most queries accepted in one items[] batch search (each is a full-library scan). */
#define MUSIC_BATCH_MAX_QUERIES 50

/** Highest page number accepted; keeps (page - 1) * limit far from int overflow. */
#define MUSIC_SEARCH_MAX_PAGE 100000

/**
 * @brief Parsed 'search' filter params (fielded filters + year range)
 *
 * Backing storage for the optional fielded filters so the voice and WebUI
 * executors parse them identically. Empty string / 0 means "not set".
 */
typedef struct {
   char genre[AUDIO_METADATA_STRING_MAX];
   char artist[AUDIO_METADATA_STRING_MAX];
   char title[AUDIO_METADATA_STRING_MAX];
   char album[AUDIO_METADATA_STRING_MAX];
   int year_min;
   int year_max;
} music_search_filters_t;

/**
 * @brief Parse the shared 'search' filter params from a tool value string
 *
 * Reads genre / artist / title / album / year_min / year_max out of @p value
 * into @p out (zeroed first). Year values are sanity-bounded; absent params stay
 * empty/0.
 *
 * @param value The tool value string (may be NULL)
 * @param out   Output filters (must be non-NULL)
 */
void music_search_parse_filters(const char *value, music_search_filters_t *out);

/** @return true when any filter in @p f is set */
bool music_search_filters_any(const music_search_filters_t *f);

/**
 * @brief Parse a non-negative integer tool param ("limit", "page")
 *
 * @return the value (clamped to MUSIC_SEARCH_MAX_PAGE), or @p fallback when absent,
 *         negative, or not a whole number
 */
int music_search_parse_int(const char *value, const char *field, int fallback);

/**
 * @brief Assemble a music_query_t from free text + parsed filters
 *
 * Maps @p text and the fielded filters into @p out with the empty-string→NULL
 * convention, so all search executors build the query identically. @p out borrows
 * pointers into @p text and @p f — both must outlive @p out's use.
 *
 * @param out  Query to populate (zeroed first; must be non-NULL)
 * @param text Free-text term (NULL/empty → no text)
 * @param f    Parsed filters (may be NULL)
 */
void music_search_build_query(music_query_t *out,
                              const char *text,
                              const music_search_filters_t *f);

/**
 * @brief Run one page of a search and render it for the LLM
 *
 * The text always leads with the real total and page count ("Found 480 tracks
 * for artist 'Ben Folds' - showing 26-50 (page 2 of 20). Use page:3 for
 * more."), then one line per track with album, year and path. An empty result
 * or a page past the end says so and echoes the query.
 *
 * @param text    Free text (may be empty when filters are set)
 * @param f       Filters (may be NULL)
 * @param page_no 1-based page (clamped to [1, MUSIC_SEARCH_MAX_PAGE])
 * @param limit   Rows per page (clamped to [1, MUSIC_SEARCH_MAX_LIMIT])
 * @return allocated text (caller frees), or NULL on DB/alloc failure
 */
char *music_search_page_text(const char *text,
                             const music_search_filters_t *f,
                             int page_no,
                             int limit);

/**
 * @brief Run one query of a batch search and append its block to @p sb
 *
 * Writes "<query> (N found[, showing top K - search it alone with page:2 for
 * more]):" followed by one line per track, or a "No match" line.
 *
 * @param sb  Output buffer
 * @param q   Free-text query (non-empty)
 * @param f   Filters applied to every query in the batch (may be NULL)
 * @param buf Scratch result buffer of at least MUSIC_BATCH_SEARCH_LIMIT rows
 */
void music_search_append_batch_entry(strbuf_t *sb,
                                     const char *q,
                                     const music_search_filters_t *f,
                                     music_search_result_t *buf);

/**
 * @brief List one page of an artist's albums for the LLM ('library' action)
 *
 * One line per album (editions merged) with year, credited artist(s), distinct
 * track count and edition count, led by the total and page count.
 *
 * @param artist   Artist to match (every word, whole words)
 * @param page     1-based page (clamped to [1, MUSIC_SEARCH_MAX_PAGE])
 * @param per_page Albums per page (clamped to [1, MUSIC_SEARCH_MAX_LIMIT])
 * @return allocated text (caller frees), or NULL on DB/alloc failure
 */
char *music_library_albums_text(const char *artist, int page, int per_page);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SEARCH_H */
