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
 * Music search relevance ranking (pure; no DB, no locks).
 */

#include "audio/music_rank.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Field weights (percent). Artist/title carry the query intent; album and genre
 * are weaker signals for a free-text track/artist search. */
#define WEIGHT_ARTIST 100
#define WEIGHT_TITLE 100
#define WEIGHT_ALBUM 75
#define WEIGHT_GENRE 60

static bool is_word_char(char c) {
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Best match quality of `needle` within `field`: exact > prefix > whole-word >
 * substring > none. */
int music_rank_field_quality(const char *field, const char *needle) {
   if (!field || !field[0] || !needle || !needle[0]) {
      return 0;
   }
   size_t nlen = strlen(needle);

   if (strcasecmp(field, needle) == 0) {
      return MUSIC_RANK_EXACT;
   }
   /* Prefix only counts when the needle ends at a word boundary — otherwise the
    * field merely starts with a LONGER word (e.g. "Princess" for "Prince"), which
    * is a weak substring, not a prefix match. */
   if (strncasecmp(field, needle, nlen) == 0 && !is_word_char(field[nlen])) {
      return MUSIC_RANK_PREFIX;
   }

   /* Whole-word: a substring occurrence bounded by non-word chars on both sides. */
   const char *first = NULL;
   for (const char *p = field; *p; p++) {
      if (strncasecmp(p, needle, nlen) == 0) {
         if (!first) {
            first = p; /* remember earliest substring hit for the fallback tier */
         }
         bool left_ok = (p == field) || !is_word_char(p[-1]);
         bool right_ok = !is_word_char(p[nlen]);
         if (left_ok && right_ok) {
            return MUSIC_RANK_WORD;
         }
      }
   }
   return first ? MUSIC_RANK_SUBSTRING : 0;
}

static int weighted(int quality, int weight) {
   return quality * weight / 100;
}

int music_rank_score(const music_search_result_t *r, const char *text) {
   if (!r || !text || !text[0]) {
      return 0;
   }
   int best = weighted(music_rank_field_quality(r->artist, text), WEIGHT_ARTIST);
   int t = weighted(music_rank_field_quality(r->title, text), WEIGHT_TITLE);
   int al = weighted(music_rank_field_quality(r->album, text), WEIGHT_ALBUM);
   int g = weighted(music_rank_field_quality(r->genre, text), WEIGHT_GENRE);
   if (t > best) {
      best = t;
   }
   if (al > best) {
      best = al;
   }
   if (g > best) {
      best = g;
   }
   return best;
}

/* Sort context: descending score, ties broken by original index (stable — keeps
 * the SQL alpha order within equal scores). */
typedef struct {
   int score;
   int idx;
} scored_t;

static int scored_cmp(const void *a, const void *b) {
   const scored_t *sa = (const scored_t *)a;
   const scored_t *sb = (const scored_t *)b;
   if (sa->score != sb->score) {
      return (sb->score > sa->score) ? 1 : -1; /* higher score first */
   }
   return (sa->idx > sb->idx) ? 1 : (sa->idx < sb->idx ? -1 : 0); /* stable by index */
}

int music_rank_select(const music_search_result_t *results,
                      int n,
                      const char *text,
                      int max,
                      int *out_order) {
   if (!results || !out_order || n <= 0 || max <= 0) {
      return 0;
   }

   /* No free text → no ranking; keep input (alpha) order, capped at max. */
   if (!text || !text[0]) {
      int keep = (n < max) ? n : max;
      for (int i = 0; i < keep; i++) {
         out_order[i] = i;
      }
      return keep;
   }

   scored_t *scored = malloc((size_t)n * sizeof(*scored));
   if (!scored) {
      /* Degrade gracefully: unranked, input order, capped. */
      int keep = (n < max) ? n : max;
      for (int i = 0; i < keep; i++) {
         out_order[i] = i;
      }
      return keep;
   }

   for (int i = 0; i < n; i++) {
      scored[i].score = music_rank_score(&results[i], text);
      scored[i].idx = i;
   }
   qsort(scored, (size_t)n, sizeof(*scored), scored_cmp);

   /* Keep every actual match, best-first, capped at max. Only true non-matches
    * (score 0) are dropped — partials rank lower so the caller sees breadth. */
   int count = 0;
   for (int i = 0; i < n && count < max; i++) {
      if (scored[i].score <= 0) {
         break; /* sorted desc → everything after is also a non-match */
      }
      out_order[count++] = scored[i].idx;
   }

   free(scored);
   return count;
}
