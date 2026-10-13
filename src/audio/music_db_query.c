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
 * Music DB search: the relevance-ranked, paged structured query, the per-artist
 * album inventory, and the SQLite matching functions behind both. Lifecycle,
 * scanning and browsing live in music_db.c.
 */

#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio/music_db.h"
#include "audio/music_db_internal.h"
#include "audio/music_rank.h"
#include "audio/music_source.h"
#include "dawn_error.h"
#include "logging.h"
#include "utils/string_utils.h"

/**
 * @brief Build a bounded SQL LIKE pattern from a user query
 *
 * Wraps the query in %…% wildcards, turns internal spaces/'*' into wildcards,
 * escapes literal % _ \ (ESCAPE '\\'), and caps total wildcards to keep the LIKE
 * scan cheap. @p out_cap must be >= 5 (leading '%', trailing '%', NUL, plus the
 * loop's own guard headroom); smaller yields an empty pattern.
 */
static void build_like_pattern(const char *pattern, char *out, size_t out_cap) {
   if (!out || out_cap < 5) {
      if (out && out_cap > 0) {
         out[0] = '\0';
      }
      return;
   }
   size_t len = strlen(pattern);
   size_t j = 0;
   int wildcard_count = 0;
   bool last_was_wildcard = false;
   const int max_wildcards = 10;

   out[j++] = '%';
   wildcard_count++;
   last_was_wildcard = true;

   for (size_t i = 0; i < len && j < out_cap - 4; i++) {
      if (pattern[i] == ' ' || pattern[i] == '*') {
         if (!last_was_wildcard && wildcard_count < max_wildcards) {
            out[j++] = '%';
            wildcard_count++;
            last_was_wildcard = true;
         }
      } else {
         if (pattern[i] == '%' || pattern[i] == '_' || pattern[i] == '\\') {
            out[j++] = '\\';
         }
         out[j++] = pattern[i];
         last_was_wildcard = false;
      }
   }

   if (!last_was_wildcard && wildcard_count < max_wildcards) {
      out[j++] = '%';
   }
   out[j] = '\0';
}

/* Count of meaningful (non-wildcard) characters in a query — a search needs at
 * least a couple so it isn't effectively "match everything". */
static size_t content_char_count(const char *s) {
   size_t n = 0;
   for (const char *p = s; *p; p++) {
      if (*p != ' ' && *p != '*') {
         n++;
      }
   }
   return n;
}

/* =============================================================================
 * SQL matching functions (backed by music_rank.c)
 * ============================================================================= */

/* The needle argument is usually a bound constant, so cache its folded/tokenized
 * form per statement via auxdata instead of re-preparing it for every row.
 * SQLite may drop auxdata at any time (even right after set), so the scratch copy
 * is what this call uses; the heap copy only serves later rows. */
static const music_rank_needle_t *needle_arg(sqlite3_context *ctx,
                                             sqlite3_value **argv,
                                             int i,
                                             bool all_required,
                                             music_rank_needle_t *scratch) {
   const music_rank_needle_t *cached = sqlite3_get_auxdata(ctx, i);
   if (cached) {
      return cached;
   }
   music_rank_needle_prepare_ex((const char *)sqlite3_value_text(argv[i]), all_required, scratch);
   music_rank_needle_t *copy = sqlite3_malloc((int)sizeof(*copy));
   if (copy) {
      *copy = *scratch;
      sqlite3_set_auxdata(ctx, i, copy, sqlite3_free);
   }
   return scratch;
}

/* music_score(artist, title, album, genre, needle) → row relevance (0 = no match) */
static void sql_music_score(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   music_rank_needle_t scratch;
   const music_rank_needle_t *n = needle_arg(ctx, argv, 4, false, &scratch);
   sqlite3_result_int(ctx, music_rank_score_fields((const char *)sqlite3_value_text(argv[0]),
                                                   (const char *)sqlite3_value_text(argv[1]),
                                                   (const char *)sqlite3_value_text(argv[2]),
                                                   (const char *)sqlite3_value_text(argv[3]), n));
}

/* music_match(field, needle) → single-field quality tier with EVERY needle word
 * required (fielded-filter precision; 0 = no match) */
static void sql_music_match(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   music_rank_needle_t scratch;
   const music_rank_needle_t *n = needle_arg(ctx, argv, 1, true, &scratch);
   sqlite3_result_int(
       ctx, music_rank_field_quality_prepared((const char *)sqlite3_value_text(argv[0]), n));
}

/* music_album_key(album, artist) → edition-folded grouping key */
static void sql_music_album_key(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   char key[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key((const char *)sqlite3_value_text(argv[0]),
                        (const char *)sqlite3_value_text(argv[1]), key, sizeof(key));
   sqlite3_result_text(ctx, key, (int)strlen(key), SQLITE_TRANSIENT);
}

/* music_fold(text) → folded text (distinct-title counting across sources) */
static void sql_music_fold(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   char out[MUSIC_RANK_FOLD_MAX];
   size_t n = music_rank_fold((const char *)sqlite3_value_text(argv[0]), out, sizeof(out));
   sqlite3_result_text(ctx, out, (int)n, SQLITE_TRANSIENT);
}

#ifdef SQLITE_INNOCUOUS
#define MUSIC_FN_FLAGS (SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS)
#else
#define MUSIC_FN_FLAGS (SQLITE_UTF8 | SQLITE_DETERMINISTIC)
#endif

int music_db_register_functions(sqlite3 *db) {
   if (!db) {
      return FAILURE;
   }
   if (sqlite3_create_function(db, "music_score", 5, MUSIC_FN_FLAGS, NULL, sql_music_score, NULL,
                               NULL) != SQLITE_OK ||
       sqlite3_create_function(db, "music_match", 2, MUSIC_FN_FLAGS, NULL, sql_music_match, NULL,
                               NULL) != SQLITE_OK ||
       sqlite3_create_function(db, "music_album_key", 2, MUSIC_FN_FLAGS, NULL, sql_music_album_key,
                               NULL, NULL) != SQLITE_OK ||
       sqlite3_create_function(db, "music_fold", 1, MUSIC_FN_FLAGS, NULL, sql_music_fold, NULL,
                               NULL) != SQLITE_OK) {
      return FAILURE;
   }
   return SUCCESS;
}

/* =============================================================================
 * Ranked, paged query
 * ============================================================================= */

/* Append to a fixed SQL buffer, failing closed on truncation so a clause added
 * later can never silently produce malformed SQL. */
static bool sql_append(char *sql, size_t cap, int *off, const char *frag) {
   int w = snprintf(sql + *off, cap - (size_t)*off, "%s", frag);
   if (w < 0 || w >= (int)(cap - (size_t)*off)) {
      OLOG_ERROR("music_db: SQL buffer overflow");
      return false;
   }
   *off += w;
   return true;
}

/* Resolved query: which constraints are active, plus the LIKE pattern for genre
 * and the free text without its source word (kept alive for the SQLITE_STATIC
 * binds). */
typedef struct {
   const music_query_t *q;
   bool text, artist, title, album, genre, ymin, ymax, source;
   music_source_t source_id;
   int min_score;         /* text score floor for this pass */
   const char *text_str;  /* free text to match: q->text, or text_buf */
   const char *rank_text; /* term that orders results, or NULL for alpha order */
   char p_genre[AUDIO_METADATA_STRING_MAX * 2];
   char text_buf[AUDIO_METADATA_STRING_MAX * 2];
} resolved_query_t;

#define MUSIC_QUERY_SOURCE_WORD "source:"
/* What ends a source name: whitespace or punctuation typed after the word */
#define MUSIC_QUERY_SOURCE_NAME_END " \t\r\n,.;:!?)\"'"

bool music_query_take_source(const char *text, char *out, size_t cap, music_source_t *source_out) {
   if (!text || !out || !source_out) {
      return false;
   }
   const size_t word_len = sizeof(MUSIC_QUERY_SOURCE_WORD) - 1;
   for (const char *p = text; *p; p++) {
      bool word_start = p == text || isspace((unsigned char)p[-1]) || p[-1] == '(';
      if (!word_start || strncasecmp(p, MUSIC_QUERY_SOURCE_WORD, word_len) != 0) {
         continue;
      }
      const char *name = p + word_len;
      size_t name_len = strcspn(name, MUSIC_QUERY_SOURCE_NAME_END);
      for (int s = 0; s < MUSIC_SOURCE_COUNT; s++) {
         const char *known = music_source_name((music_source_t)s);
         if (name_len != strlen(known) || strncasecmp(name, known, name_len) != 0) {
            continue;
         }
         size_t before = (size_t)(p - text);
         const char *after = name + name_len;
         size_t after_len = strlen(after);
         if (before + after_len >= cap) {
            return false;
         }
         /* memmove: out may be text itself */
         memmove(out, text, before);
         memmove(out + before, after, after_len + 1);
         *source_out = (music_source_t)s;
         return true;
      }
   }
   return false;
}

static bool resolve_query(const music_query_t *q, resolved_query_t *r) {
   memset(r, 0, sizeof(*r));
   r->q = q;
   r->text_str = q->text;
   /* A source word always comes out of the text; the structured field wins */
   if (music_query_take_source(q->text, r->text_buf, sizeof(r->text_buf), &r->source_id)) {
      r->source = true;
      r->text_str = r->text_buf;
   }
   if (q->has_source) {
      r->source = true;
      r->source_id = q->source;
   }
   /* Searchable text: two content characters, and a word once punctuation is
    * folded away (so "(source:plex)" leaves nothing, not "()") */
   size_t text_chars = r->text_str ? content_char_count(r->text_str) : 0;
   char folded[sizeof(r->text_buf)];
   size_t folded_len = r->text_str ? music_rank_fold(r->text_str, folded, sizeof(folded)) : 0;
   r->text = text_chars >= 2 && folded_len > 0;
   r->artist = q->artist && q->artist[0];
   r->title = q->title && q->title[0];
   r->album = q->album && q->album[0];
   /* A genre that's only wildcards/space would become '%' and match every row,
    * sidestepping the "no constraints → nothing" rule. */
   r->genre = q->genre && content_char_count(q->genre) >= 1;
   r->ymin = q->year_min > 0;
   r->ymax = q->year_max > 0;
   if (r->genre) {
      build_like_pattern(q->genre, r->p_genre, sizeof(r->p_genre));
   }
   /* Rank by the most specific search term: free text if given, else the fielded
    * artist/title/album value. Genre/year are pure filters and never rank. */
   r->rank_text = r->text     ? r->text_str
                  : r->artist ? q->artist
                  : r->title  ? q->title
                  : r->album  ? q->album
                              : NULL;
   bool other = r->text || r->artist || r->title || r->album || r->genre || r->ymin || r->ymax;
   /* The source alone lists it only where the caller shows results, and only
    * when nothing else was typed: "source:plex X" matches nothing, as "X" does */
   return other || (r->source && q->list_source && folded_len == 0);
}

/* WHERE body shared by the page and count statements. */
static bool append_where(char *sql, size_t cap, int *off, const resolved_query_t *r) {
   /* Cheapest terms first — SQLite evaluates non-indexed terms in written order,
    * so the year/genre/single-field tests trim rows before the 4-field text score.
    * The correlated dedup subquery is always evaluated last. */
   bool ok = sql_append(sql, cap, off, " FROM music_metadata WHERE 1=1");
   if (ok && r->source) {
      ok = sql_append(sql, cap, off, " AND source = ?");
   }
   if (ok && r->ymin) {
      ok = sql_append(sql, cap, off, " AND year >= ?");
   }
   if (ok && r->ymax) {
      ok = sql_append(sql, cap, off, " AND year <= ?");
   }
   if (ok && r->genre) {
      ok = sql_append(sql, cap, off, " AND genre LIKE ? ESCAPE '\\'");
   }
   /* Fielded filters are the precision lever: every word required, word-boundary
    * strict (artist:"Prince" excludes "Princeton"), punctuation/order-insensitive. */
   if (ok && r->artist) {
      ok = sql_append(sql, cap, off, " AND music_match(artist, ?) >= ?");
   }
   if (ok && r->title) {
      ok = sql_append(sql, cap, off, " AND music_match(title, ?) >= ?");
   }
   if (ok && r->album) {
      ok = sql_append(sql, cap, off, " AND music_match(album, ?) >= ?");
   }
   if (ok && r->text) {
      ok = sql_append(sql, cap, off, " AND music_score(artist, title, album, genre, ?) >= ?");
   }
   /* One source: its rows aren't hidden by another source's copies.  Its
    * same-source branch already requires equal genre and year. */
   if (r->source) {
      return ok && sql_append(sql, cap, off, " AND " MUSIC_DEDUP_EXISTS_SAME_SOURCE);
   }
   /* Under a genre/year filter the dedup must not let a copy that fails the
    * filter hide one that passes it. */
   if (r->genre || r->ymin || r->ymax) {
      return ok && sql_append(sql, cap, off, " AND " MUSIC_DEDUP_EXISTS_TAGGED);
   }
   return ok && sql_append(sql, cap, off, " " MUSIC_DEDUP_CLAUSE);
}

/* Bind the WHERE params in append_where() order; returns the next index. */
static int bind_where(sqlite3_stmt *st, int i, const resolved_query_t *r) {
   const music_query_t *q = r->q;
   if (r->source) {
      sqlite3_bind_int(st, i++, (int)r->source_id);
   }
   if (r->ymin) {
      sqlite3_bind_int(st, i++, q->year_min);
   }
   if (r->ymax) {
      sqlite3_bind_int(st, i++, q->year_max);
   }
   if (r->genre) {
      sqlite3_bind_text(st, i++, r->p_genre, -1, SQLITE_STATIC);
   }
   if (r->artist) {
      sqlite3_bind_text(st, i++, q->artist, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, i++, MUSIC_RANK_FIELD_MIN);
   }
   if (r->title) {
      sqlite3_bind_text(st, i++, q->title, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, i++, MUSIC_RANK_FIELD_MIN);
   }
   if (r->album) {
      sqlite3_bind_text(st, i++, q->album, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, i++, MUSIC_RANK_FIELD_MIN);
   }
   if (r->text) {
      sqlite3_bind_text(st, i++, r->text_str, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, i++, r->min_score);
   }
   return i;
}

/* Run a single-column COUNT statement; caller holds the DB lock. */
static int count_rows(const char *sql, const resolved_query_t *r, int *total_out) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(g_music_db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("music_db: count prepare failed: %s", sqlite3_errmsg(g_music_db));
      return FAILURE;
   }
   if (r) {
      bind_where(st, 1, r);
   }
   int rc = sqlite3_step(st);
   *total_out = (rc == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
      OLOG_ERROR("music_db: count step failed: %s", sqlite3_errmsg(g_music_db));
      return FAILURE;
   }
   return SUCCESS;
}

/* One ranked page at r->min_score. Caller holds the DB lock. The page and the
 * total come from ONE statement — COUNT(*) OVER() is computed over the full match
 * set before LIMIT/OFFSET — so both see the same snapshot (the Plex sync writes
 * through its own connection). The predicate IS the scorer, so the total is exact:
 * every row counted is a row that ranks. */
static int run_page(const resolved_query_t *r,
                    int offset,
                    music_search_result_t *results,
                    int max_results,
                    int *count_out,
                    int *total_out) {
   char sql[2048];
   int off = 0;
   bool ok = sql_append(sql, sizeof(sql), &off, "SELECT " MUSIC_ROW_COLS ", COUNT(*) OVER ()");
   ok = ok && append_where(sql, sizeof(sql), &off, r);
   ok = ok && sql_append(sql, sizeof(sql), &off, " ORDER BY ");
   if (ok && r->rank_text) {
      ok = sql_append(sql, sizeof(sql), &off, "music_score(artist, title, album, genre, ?) DESC, ");
   }
   /* id last: deterministic order so OFFSET pages never repeat or skip rows. */
   ok = ok && sql_append(sql, sizeof(sql), &off,
                         "artist COLLATE NOCASE, album COLLATE NOCASE, title COLLATE NOCASE, id "
                         "LIMIT ? OFFSET ?");
   if (!ok) {
      return FAILURE;
   }

   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(g_music_db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("music_db_query: prepare failed: %s", sqlite3_errmsg(g_music_db));
      return FAILURE;
   }
   int bi = bind_where(st, 1, r);
   if (r->rank_text) {
      sqlite3_bind_text(st, bi++, r->rank_text, -1, SQLITE_STATIC);
   }
   sqlite3_bind_int(st, bi++, max_results);
   sqlite3_bind_int(st, bi++, offset);

   int count = 0;
   int total = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW && count < max_results) {
      music_db_populate_row(st, &results[count]);
      if (count == 0) {
         total = sqlite3_column_int(st, 8);
      }
      count++;
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
      OLOG_ERROR("music_db_query: step failed: %s", sqlite3_errmsg(g_music_db));
      return FAILURE;
   }

   /* A page past the end returns no rows, hence no window total; count directly
    * so the caller can still say how many exist. */
   if (count == 0 && offset > 0) {
      char csql[2048];
      int coff = 0;
      if (!sql_append(csql, sizeof(csql), &coff, "SELECT COUNT(*)") ||
          !append_where(csql, sizeof(csql), &coff, r) || count_rows(csql, r, &total) != SUCCESS) {
         return FAILURE;
      }
   }
   *count_out = count;
   *total_out = total;
   return SUCCESS;
}

/* Bind a row's text field as the DB stores it: an empty tag is NULL there */
static void bind_tag(sqlite3_stmt *st, int i, const char *value) {
   if (value[0]) {
      sqlite3_bind_text(st, i, value, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(st, i);
   }
}

/*
 * Whether a row shown on this page has a copy in another source that duplicates
 * collapsed.  The copy listed comes from the lowest source, so the hidden one is
 * a higher source with the same artist, album and title (one index seek per
 * row); under a genre/year filter only a copy with the same genre and year was
 * collapsed (MUSIC_DEDUP_EXISTS_TAGGED), the rest the filter excluded.  Caller
 * holds the DB lock.
 */
static bool page_hides_other_source(const music_search_result_t *rows,
                                    int count,
                                    bool tag_filtered,
                                    music_source_t *hidden_out) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(g_music_db,
                          "SELECT source FROM music_metadata WHERE artist IS ?1 COLLATE NOCASE "
                          "AND album IS ?2 COLLATE NOCASE AND title IS ?3 COLLATE NOCASE "
                          "AND source > ?4 AND (?5 = 0 OR (genre IS ?6 AND year IS ?7)) LIMIT 1",
                          -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool hid = false;
   for (int i = 0; i < count && !hid; i++) {
      bind_tag(st, 1, rows[i].artist);
      bind_tag(st, 2, rows[i].album);
      bind_tag(st, 3, rows[i].title);
      sqlite3_bind_int(st, 4, (int)rows[i].source);
      sqlite3_bind_int(st, 5, tag_filtered ? 1 : 0);
      bind_tag(st, 6, rows[i].genre);
      if (rows[i].year > 0) {
         sqlite3_bind_int(st, 7, (int)rows[i].year);
      } else {
         sqlite3_bind_null(st, 7);
      }
      if (sqlite3_step(st) == SQLITE_ROW) {
         hid = true;
         *hidden_out = (music_source_t)sqlite3_column_int(st, 0);
      }
      sqlite3_reset(st);
   }
   sqlite3_finalize(st);
   return hid;
}

int music_db_query_page(const music_query_t *q,
                        int offset,
                        music_search_result_t *results,
                        int max_results,
                        music_query_page_t *page_out) {
   if (!q || !results || max_results <= 0 || !page_out) {
      return FAILURE;
   }
   memset(page_out, 0, sizeof(*page_out));
   if (offset < 0) {
      offset = 0;
   }

   resolved_query_t r;
   if (!resolve_query(q, &r)) {
      return SUCCESS; /* No constraints → return nothing rather than the whole library */
   }

   pthread_mutex_lock(&g_music_db_mutex);
   if (!g_music_db_initialized) {
      pthread_mutex_unlock(&g_music_db_mutex);
      return FAILURE;
   }

   /* Strict pass first: every required token matched. Only when that finds
    * nothing — and the caller can show the result as approximate — does the
    * typo-tolerant PARTIAL tier apply, so a near-miss still gets an answer
    * without partials padding real results or being played silently. */
   r.min_score = MUSIC_RANK_PARTIAL + 1;
   int ret = run_page(&r, offset, results, max_results, &page_out->count, &page_out->total);
   if (ret == SUCCESS && r.text && q->allow_partial && page_out->total == 0) {
      r.min_score = MUSIC_RANK_PARTIAL;
      ret = run_page(&r, offset, results, max_results, &page_out->count, &page_out->total);
      page_out->approximate = (page_out->total > 0);
   }
   /* With no source named, say whether copies in another source were hidden,
    * so a caller can tell how to reach them */
   if (ret == SUCCESS && !r.source && page_out->count > 0) {
      page_out->hid_other_source = page_hides_other_source(results, page_out->count,
                                                           r.genre || r.ymin || r.ymax,
                                                           &page_out->hidden_source);
   }
   pthread_mutex_unlock(&g_music_db_mutex);

   if (ret != SUCCESS) {
      memset(page_out, 0, sizeof(*page_out));
   }
   return ret;
}

int music_db_query(const music_query_t *q,
                   music_search_result_t *results,
                   int max_results,
                   int *count_out) {
   if (!count_out) {
      return FAILURE;
   }
   *count_out = 0;
   music_query_page_t page;
   if (music_db_query_page(q, 0, results, max_results, &page) != SUCCESS) {
      return FAILURE;
   }
   *count_out = page.count;
   return SUCCESS;
}

/* =============================================================================
 * Album inventory for an artist
 * ============================================================================= */

/* Grouped by edition-folded album key only (not artist), so a compilation whose
 * tracks carry a dozen different "Artist Presents: X" credits lists once. Track
 * counts are distinct folded titles, which neutralizes duplicate indexing and
 * sums editions to their union. Oldest release first. */
/* Display the shortest name in a group ("Speed Graphic" over "Speed Graphic (EP)"),
 * ties broken alphabetically: min over zero-padded-length || name, prefix removed. */
#define SHORTEST(col) "substr(MIN(printf('%04d', length(" col ")) || " col "), 5)"

#define ALBUMS_BY_ARTIST_FROM                                                                   \
   " FROM music_metadata WHERE album != '' AND music_match(artist, ?) >= ? " MUSIC_DEDUP_CLAUSE \
   "GROUP BY music_album_key(album, artist)"

static const char *SQL_ALBUMS_BY_ARTIST = "SELECT " SHORTEST("album") ", " SHORTEST(
    "artist") ", COUNT(DISTINCT music_fold(title)), "
              "       COALESCE(MIN(NULLIF(year, 0)), 0), COUNT(DISTINCT album), "
              "       COUNT(DISTINCT lower(artist)), COUNT(*) OVER () " ALBUMS_BY_ARTIST_FROM
              " ORDER BY MIN(NULLIF(year, 0)) IS NULL, MIN(NULLIF(year, 0)), 1 COLLATE NOCASE "
              "LIMIT ? OFFSET ?";

static const char *SQL_ALBUMS_BY_ARTIST_COUNT =
    "SELECT COUNT(*) FROM (SELECT 1" ALBUMS_BY_ARTIST_FROM ")";

static void copy_col(sqlite3_stmt *st, int col, char *dst, size_t cap) {
   const char *v = (const char *)sqlite3_column_text(st, col);
   safe_strncpy(dst, v ? v : "", cap);
}

int music_db_list_albums_by_artist(const char *artist,
                                   music_album_info_t *albums,
                                   int max_albums,
                                   int offset,
                                   int *count_out,
                                   int *total_out) {
   if (!artist || !artist[0] || !albums || max_albums <= 0 || !count_out) {
      return FAILURE;
   }
   *count_out = 0;
   if (total_out) {
      *total_out = 0;
   }
   if (offset < 0) {
      offset = 0;
   }

   pthread_mutex_lock(&g_music_db_mutex);
   if (!g_music_db_initialized) {
      pthread_mutex_unlock(&g_music_db_mutex);
      return FAILURE;
   }

   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(g_music_db, SQL_ALBUMS_BY_ARTIST, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("music_db_list_albums_by_artist: prepare failed: %s", sqlite3_errmsg(g_music_db));
      pthread_mutex_unlock(&g_music_db_mutex);
      return FAILURE;
   }
   sqlite3_bind_text(st, 1, artist, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 2, MUSIC_RANK_FIELD_MIN);
   sqlite3_bind_int(st, 3, max_albums);
   sqlite3_bind_int(st, 4, offset);

   int count = 0;
   int total = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW && count < max_albums) {
      music_album_info_t *a = &albums[count];
      memset(a, 0, sizeof(*a));
      copy_col(st, 0, a->name, sizeof(a->name));
      copy_col(st, 1, a->artist, sizeof(a->artist));
      a->track_count = sqlite3_column_int(st, 2);
      a->year = sqlite3_column_int(st, 3);
      a->editions = sqlite3_column_int(st, 4);
      a->artist_count = sqlite3_column_int(st, 5);
      if (count == 0) {
         total = sqlite3_column_int(st, 6);
      }
      count++;
   }
   sqlite3_finalize(st);
   int ret = (rc == SQLITE_ROW || rc == SQLITE_DONE) ? SUCCESS : FAILURE;

   if (ret == SUCCESS && total_out && count == 0 && offset > 0) {
      sqlite3_stmt *cs = NULL;
      if (sqlite3_prepare_v2(g_music_db, SQL_ALBUMS_BY_ARTIST_COUNT, -1, &cs, NULL) == SQLITE_OK) {
         sqlite3_bind_text(cs, 1, artist, -1, SQLITE_STATIC);
         sqlite3_bind_int(cs, 2, MUSIC_RANK_FIELD_MIN);
         int crc = sqlite3_step(cs);
         total = (crc == SQLITE_ROW) ? sqlite3_column_int(cs, 0) : 0;
         sqlite3_finalize(cs);
         if (crc != SQLITE_ROW && crc != SQLITE_DONE) {
            ret = FAILURE;
         }
      } else {
         ret = FAILURE;
      }
   }
   pthread_mutex_unlock(&g_music_db_mutex);

   if (ret != SUCCESS) {
      return FAILURE;
   }
   *count_out = count;
   if (total_out) {
      *total_out = total;
   }
   return SUCCESS;
}
