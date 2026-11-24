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
#include <strings.h>

#include "audio/music_rank.h"
#include "dawn_error.h"
#include "tools/tool_registry.h"

/* After results that collapsed copies in another source: how to reach them */
static void append_source_hint(strbuf_t *sb, const music_query_page_t *page) {
   if (page->hid_other_source) {
      const char *name = music_source_name(page->hidden_source);
      strbuf_appendf(sb,
                     "Some of these tracks are also in %s, listed once as the copy shown; add "
                     "source:%s to the query for those copies.\n",
                     name, name);
   }
}

/** Candidates the resolver picks from (artist preference + tiebreak) */
#define MUSIC_SEARCH_RESOLVE_CANDIDATES 8

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

int music_search_resolve_item(const char *item,
                              const music_search_filters_t *f,
                              music_search_result_t *out) {
   if (!item || !out) {
      return FAILURE;
   }

   /* A source word comes off first, so it can't land in the artist or skew
    * the picker */
   char clean[AUDIO_METADATA_STRING_MAX * 2];
   music_source_t source = MUSIC_SOURCE_LOCAL;
   bool has_source = music_query_take_source(item, clean, sizeof(clean), &source);
   if (has_source) {
      item = clean;
   }

   /* "Artist - Title": search the title (spaces are word boundaries, so the
    * combined form usually misses) and prefer the matching artist, which picks
    * Toto's "Africa" over an unrelated top match */
   const char *dash = strstr(item, " - ");
   char artist[sizeof(out->artist)] = { 0 };
   const char *title_query = item;
   if (dash) {
      size_t alen = (size_t)(dash - item);
      if (alen >= sizeof(artist)) {
         alen = sizeof(artist) - 1;
      }
      memcpy(artist, item, alen);
      artist[alen] = '\0';
      title_query = dash + 3;
   }

   /* Strict matching, never the approximate fallback: this plays what it finds */
   music_search_result_t r[MUSIC_SEARCH_RESOLVE_CANDIDATES];
   int count = 0;
   music_query_t mq;
   music_search_build_query(&mq, title_query, f);
   mq.has_source = has_source;
   mq.source = source;
   if (music_db_query(&mq, r, MUSIC_SEARCH_RESOLVE_CANDIDATES, &count) != SUCCESS || count <= 0) {
      return FAILURE;
   }
   *out = r[music_rank_pick_best(r, count, title_query, dash ? artist : NULL)];
   return SUCCESS;
}

/* Whether no earlier track of @p tracks carries track @p i's album tag */
static bool first_of_tag(const music_search_result_t *tracks, int i) {
   for (int j = 0; j < i; j++) {
      if (strcasecmp(tracks[j].album, tracks[i].album) == 0) {
         return false;
      }
   }
   return true;
}

/* Whether no earlier track of @p tracks is on track @p i's disc */
static bool first_of_disc(const music_search_result_t *tracks, int i) {
   for (int j = 0; j < i; j++) {
      if (tracks[j].disc_no == tracks[i].disc_no) {
         return false;
      }
   }
   return true;
}

/* How well @p album names @p name with its artist ("The Hits Prince") */
static int names_quality(const char *name, const char *artist, const char *album) {
   char named[AUDIO_METADATA_STRING_MAX * 2 + 2];
   snprintf(named, sizeof(named), "%s %s", name, artist);
   return music_rank_field_quality(named, album);
}

/* Whether the tracks tagged @p tag are exactly one disc: all on one disc number,
 * which no other tag's track is on (an edition tag inside a disc is not a disc) */
static bool tag_is_disc(const music_search_result_t *tracks, int count, const char *tag) {
   uint32_t disc = 0;
   for (int i = 0; i < count; i++) {
      if (strcasecmp(tracks[i].album, tag) == 0) {
         if (disc && tracks[i].disc_no != disc) {
            return false;
         }
         disc = tracks[i].disc_no;
      }
   }
   for (int i = 0; i < count; i++) {
      if (tracks[i].disc_no == disc && strcasecmp(tracks[i].album, tag) != 0) {
         return false;
      }
   }
   return disc != 0;
}

/* The tag @p album names better than every other tag and the set's own name,
 * else -1 */
static int named_tag(const music_search_result_t *tracks,
                     int count,
                     const char *album,
                     const char *artist) {
   int tags = 0;
   int best = 0;
   int best_i = -1;
   bool tie = false;
   for (int i = 0; i < count; i++) {
      if (!first_of_tag(tracks, i)) {
         continue;
      }
      tags++;
      int q = names_quality(tracks[i].album, artist && artist[0] ? artist : tracks[i].artist,
                            album);
      if (q > best) {
         best = q;
         best_i = i;
         tie = false;
      } else if (q == best) {
         tie = true;
      }
   }
   if (tags < 2 || best_i < 0 || tie || best < MUSIC_RANK_FIELD_MIN) {
      return -1;
   }
   char set[AUDIO_METADATA_STRING_MAX];
   music_search_album_set_name(tracks[0].album, set, sizeof(set));
   if (names_quality(set, artist && artist[0] ? artist : tracks[0].artist, album) >= best) {
      return -1; /* "The Hits": the whole set */
   }
   return best_i;
}

int music_search_album_disc(music_search_result_t *tracks,
                            int count,
                            const char *album,
                            const char *artist,
                            int *discs_out) {
   int discs = 0;
   for (int i = 0; i < count; i++) {
      discs += first_of_disc(tracks, i);
   }
   int t = (album && album[0] && discs > 1) ? named_tag(tracks, count, album, artist) : -1;
   if (t >= 0 && tag_is_disc(tracks, count, tracks[t].album)) {
      char tag[AUDIO_METADATA_STRING_MAX];
      snprintf(tag, sizeof(tag), "%s", tracks[t].album);
      int kept = 0;
      for (int i = 0; i < count; i++) {
         if (strcasecmp(tracks[i].album, tag) == 0) {
            if (kept != i) {
               tracks[kept] = tracks[i];
            }
            kept++;
         }
      }
      count = kept;
      discs = 1;
   }
   if (discs_out) {
      *discs_out = discs;
   }
   return count;
}

void music_search_album_set_name(const char *tag, char *out, size_t cap) {
   if (!out || cap == 0) {
      return;
   }
   out[0] = '\0';
   if (!tag) {
      return;
   }
   /* The shortest leading part of the tag that is already its album key: the
    * tag before the qualifiers the key drops */
   char key[MUSIC_RANK_FOLD_MAX];
   music_rank_album_key(tag, NULL, key, sizeof(key));
   size_t len = strlen(tag);
   for (size_t n = 1; n < len && key[0]; n++) {
      if (tag[n] != ' ' && tag[n] != '(' && tag[n] != '[') {
         continue;
      }
      char part[AUDIO_METADATA_STRING_MAX];
      char folded[MUSIC_RANK_FOLD_MAX];
      snprintf(part, sizeof(part), "%.*s", (int)n, tag);
      music_rank_fold(part, folded, sizeof(folded));
      if (strcmp(folded, key) == 0) {
         snprintf(out, cap, "%s", part);
         return;
      }
   }
   snprintf(out, cap, "%s", tag);
}

void music_search_describe_query(strbuf_t *sb, const char *text, const music_search_filters_t *f) {
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
      music_search_describe_query(sb, text, f);
      strbuf_appendf(sb, "; found %d closest match%s (one word unmatched)", page->total,
                     page->total == 1 ? "" : "es");
   } else {
      strbuf_appendf(sb, "Found %d track%s for ", page->total, page->total == 1 ? "" : "s");
      music_search_describe_query(sb, text, f);
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
   mq.list_source = true;
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
      music_search_describe_query(&sb, text, f);
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
      /* Without it, a model asked for Plex concludes there are no Plex copies */
      append_source_hint(&sb, &page);
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
   mq.list_source = true;
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
   append_source_hint(sb, &page);
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
      strbuf_appendf(&sb,
                     "To list an album's tracks: search album:'<name>'; to play or queue it whole: "
                     "play/enqueue album:'<name>'.");
   }
   free(albums);

   char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
   if (!out) {
      strbuf_free(&sb);
   }
   return out;
}
