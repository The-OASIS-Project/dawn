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
 * Music tool search surface: shared filter parsing and the LLM-facing text for
 * search pages, batch searches and the per-artist album listing.
 */

#include "tools/music_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "tools/tool_registry.h"

/* Parse one sanity-bounded year param from the tool value, 0 if absent/invalid. */
static int parse_year_param(const char *value, const char *field) {
   char ybuf[16] = "";
   if (tool_param_extract_custom(value ? value : "", field, ybuf, sizeof(ybuf))) {
      long y = strtol(ybuf, NULL, 10);
      if (y > 0 && y < MUSIC_QUERY_YEAR_MAX) {
         return (int)y;
      }
   }
   return 0;
}

void music_search_parse_filters(const char *value, music_search_filters_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   const char *v = value ? value : "";
   tool_param_extract_custom(v, "genre", out->genre, sizeof(out->genre));
   tool_param_extract_custom(v, "artist", out->artist, sizeof(out->artist));
   tool_param_extract_custom(v, "title", out->title, sizeof(out->title));
   tool_param_extract_custom(v, "album", out->album, sizeof(out->album));
   out->year_min = parse_year_param(v, "year_min");
   out->year_max = parse_year_param(v, "year_max");
}

bool music_search_filters_any(const music_search_filters_t *f) {
   return f && (f->genre[0] || f->artist[0] || f->title[0] || f->album[0] || f->year_min > 0 ||
                f->year_max > 0);
}

int music_search_parse_int(const char *value, const char *field, int fallback) {
   char num[16] = "";
   if (!tool_param_extract_custom(value ? value : "", field, num, sizeof(num))) {
      return fallback;
   }
   char *endptr;
   long parsed = strtol(num, &endptr, 10);
   if (endptr == num || *endptr != '\0' || parsed < 0) {
      return fallback;
   }
   return parsed > MUSIC_SEARCH_MAX_PAGE ? MUSIC_SEARCH_MAX_PAGE : (int)parsed;
}

void music_search_build_query(music_query_t *out,
                              const char *text,
                              const music_search_filters_t *f) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   if (text && text[0]) {
      out->text = text;
   }
   if (f) {
      out->genre = f->genre[0] ? f->genre : NULL;
      out->artist = f->artist[0] ? f->artist : NULL;
      out->title = f->title[0] ? f->title : NULL;
      out->album = f->album[0] ? f->album : NULL;
      out->year_min = f->year_min;
      out->year_max = f->year_max;
   }
}

/* Echo the query back ("query 'x', artist 'y', years 1980-1989") so the LLM can
 * see exactly what was searched when reading a count or an empty result. */
static void describe_query(strbuf_t *sb, const char *text, const music_search_filters_t *f) {
   const char *sep = "";
   if (text && text[0]) {
      strbuf_appendf(sb, "query '%s'", text);
      sep = ", ";
   }
   if (f) {
      const struct {
         const char *name;
         const char *val;
      } fields[] = {
         { "artist", f->artist },
         { "title", f->title },
         { "album", f->album },
         { "genre", f->genre },
      };
      for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
         if (fields[i].val[0]) {
            strbuf_appendf(sb, "%s%s '%s'", sep, fields[i].name, fields[i].val);
            sep = ", ";
         }
      }
      if (f->year_min > 0 || f->year_max > 0) {
         strbuf_appendf(sb, "%syears ", sep);
         if (f->year_min > 0) {
            strbuf_appendf(sb, "%d", f->year_min);
         }
         strbuf_appendf(sb, "-");
         if (f->year_max > 0) {
            strbuf_appendf(sb, "%d", f->year_max);
         }
      }
   }
}

/* Append a tag-derived string with control characters (CR/LF/tab/other C0)
 * flattened to spaces, so a hostile tag can't forge its own result line or a
 * fake "Found ..." header in text the LLM is told to trust. */
static void append_tag(strbuf_t *sb, const char *s) {
   const char *run = s;
   for (const char *p = s;; p++) {
      if (*p == '\0' || (unsigned char)*p < 0x20 || *p == 0x7F) {
         if (p > run) {
            strbuf_append_n(sb, run, (size_t)(p - run));
         }
         if (*p == '\0') {
            return;
         }
         strbuf_append_n(sb, " ", 1);
         run = p + 1;
      }
   }
}

/* One result line: "Artist - Title  (Album, Year)  [path]". */
static void append_track_row(strbuf_t *sb, const music_search_result_t *r) {
   strbuf_append(sb, "  ");
   append_tag(sb, r->display_name);
   if (r->album[0]) {
      strbuf_append(sb, "  (");
      append_tag(sb, r->album);
      if (r->year > 0) {
         strbuf_appendf(sb, ", %u", r->year);
      }
      strbuf_append(sb, ")");
   } else if (r->year > 0) {
      strbuf_appendf(sb, "  (%u)", r->year);
   }
   strbuf_append(sb, "  [");
   append_tag(sb, r->path);
   strbuf_append(sb, "]\n");
}

/* "Found N tracks for <query>" — or, for approximate results, a lead that says
 * plainly no track matched every word. */
static void append_lead(strbuf_t *sb,
                        const music_query_page_t *page,
                        const char *text,
                        const music_search_filters_t *f) {
   if (page->approximate) {
      strbuf_appendf(sb, "No track matches every word of ");
      describe_query(sb, text, f);
      strbuf_appendf(sb, "; found %d closest match%s (one word unmatched)", page->total,
                     page->total == 1 ? "" : "es");
   } else {
      strbuf_appendf(sb, "Found %d track%s for ", page->total, page->total == 1 ? "" : "s");
      describe_query(sb, text, f);
   }
}

char *music_search_page_text(const char *text,
                             const music_search_filters_t *f,
                             int page_no,
                             int limit) {
   if (limit < 1 || limit > MUSIC_SEARCH_MAX_LIMIT) {
      limit = MUSIC_SEARCH_MAX_LIMIT;
   }
   if (page_no < 1) {
      page_no = 1;
   } else if (page_no > MUSIC_SEARCH_MAX_PAGE) {
      page_no = MUSIC_SEARCH_MAX_PAGE;
   }
   music_search_result_t *res = malloc((size_t)limit * sizeof(*res));
   if (!res) {
      return NULL;
   }

   music_query_t mq;
   music_search_build_query(&mq, text, f);
   mq.allow_partial = true; /* shown to the LLM, labeled approximate */
   music_query_page_t page;
   int offset = (page_no - 1) * limit;
   if (music_db_query_page(&mq, offset, res, limit, &page) != SUCCESS) {
      free(res);
      return NULL;
   }

   /* Always lead with the real total and page count, so a truncated view is
    * never mistaken for the whole answer. */
   strbuf_t sb;
   strbuf_init(&sb, 512);
   int pages = (page.total + limit - 1) / limit;
   if (page.total == 0) {
      strbuf_appendf(&sb, "No music found for ");
      describe_query(&sb, text, f);
      strbuf_appendf(&sb, ".");
   } else if (page.count == 0) {
      append_lead(&sb, &page, text, f);
      strbuf_appendf(&sb, ", but page %d is past the end (last page is %d).", page_no, pages);
   } else {
      append_lead(&sb, &page, text, f);
      strbuf_appendf(&sb, " - showing %d-%d (page %d of %d).", offset + 1, offset + page.count,
                     page_no, pages);
      if (page_no < pages) {
         strbuf_appendf(&sb, " Use page:%d for more.", page_no + 1);
      }
      strbuf_appendf(&sb, "\n");
      for (int i = 0; i < page.count; i++) {
         append_track_row(&sb, &res[i]);
      }
   }
   free(res);

   char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
   if (!out) {
      strbuf_free(&sb);
   }
   return out;
}

void music_search_append_batch_entry(strbuf_t *sb,
                                     const char *q,
                                     const music_search_filters_t *f,
                                     music_search_result_t *buf) {
   if (!sb || !q || !q[0] || !buf) {
      return;
   }
   music_query_t mq;
   music_search_build_query(&mq, q, f);
   mq.allow_partial = true; /* shown to the LLM, labeled below */
   music_query_page_t page;
   if (music_db_query_page(&mq, 0, buf, MUSIC_BATCH_SEARCH_LIMIT, &page) != SUCCESS) {
      strbuf_appendf(sb, "%s:\n  Search failed (music database error)\n", q);
      return;
   }
   if (page.count <= 0) {
      strbuf_appendf(sb, "%s:\n  No match: %s\n", q, q);
      return;
   }
   strbuf_appendf(sb, "%s (%s%d found", q, page.approximate ? "no exact match, closest " : "",
                  page.total);
   if (page.total > page.count) {
      strbuf_appendf(sb, ", showing top %d - search it alone with page:2 for more", page.count);
   }
   strbuf_appendf(sb, "):\n");
   for (int j = 0; j < page.count; j++) {
      append_track_row(sb, &buf[j]);
   }
}

char *music_library_albums_text(const char *artist, int page, int per_page) {
   if (!artist || !artist[0]) {
      return NULL;
   }
   if (per_page < 1 || per_page > MUSIC_SEARCH_MAX_LIMIT) {
      per_page = MUSIC_SEARCH_MAX_LIMIT;
   }
   if (page < 1) {
      page = 1;
   } else if (page > MUSIC_SEARCH_MAX_PAGE) {
      page = MUSIC_SEARCH_MAX_PAGE;
   }
   music_album_info_t *albums = malloc((size_t)per_page * sizeof(*albums));
   if (!albums) {
      return NULL;
   }
   int count = 0;
   int total = 0;
   int offset = (page - 1) * per_page;
   if (music_db_list_albums_by_artist(artist, albums, per_page, offset, &count, &total) !=
       SUCCESS) {
      free(albums);
      return NULL;
   }

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   int pages = (total + per_page - 1) / per_page;
   if (total == 0) {
      strbuf_appendf(&sb, "No albums found for artist '%s'.", artist);
   } else if (count == 0) {
      strbuf_appendf(&sb,
                     "Found %d album%s for artist '%s', but page %d is past the end (last "
                     "page is %d).",
                     total, total == 1 ? "" : "s", artist, page, pages);
   } else {
      strbuf_appendf(&sb,
                     "Found %d album%s for artist '%s' (including credits that contain the name, "
                     "such as bands and collaborations) - showing %d-%d (page %d of %d), oldest "
                     "first. Editions are merged; track counts are distinct titles.",
                     total, total == 1 ? "" : "s", artist, offset + 1, offset + count, page, pages);
      if (page < pages) {
         strbuf_appendf(&sb, " Use page:%d for more.", page + 1);
      }
      strbuf_appendf(&sb, "\n");
      for (int i = 0; i < count; i++) {
         const music_album_info_t *a = &albums[i];
         strbuf_append(&sb, "- ");
         append_tag(&sb, a->name);
         if (a->year > 0) {
            strbuf_appendf(&sb, " (%d)", a->year);
         }
         strbuf_append(&sb, " - ");
         append_tag(&sb, a->artist);
         if (a->artist_count > 1) {
            strbuf_appendf(&sb, " + %d other artist%s", a->artist_count - 1,
                           a->artist_count == 2 ? "" : "s");
         }
         strbuf_appendf(&sb, ", %d track%s", a->track_count, a->track_count == 1 ? "" : "s");
         if (a->editions > 1) {
            strbuf_appendf(&sb, ", %d editions", a->editions);
         }
         strbuf_appendf(&sb, "\n");
      }
      strbuf_appendf(&sb, "To list an album's tracks: search album:'<name>'.");
   }
   free(albums);

   char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
   if (!out) {
      strbuf_free(&sb);
   }
   return out;
}
