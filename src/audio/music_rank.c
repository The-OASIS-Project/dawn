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
 * Music search relevance matching (pure; no DB, no locks).
 */

#include "audio/music_rank.h"

#include <stdbool.h>
#include <string.h>
#include <strings.h>

/* Field weights (percent). Artist/title carry the query intent; album and genre
 * are weaker signals for a free-text track/artist search. */
#define WEIGHT_ARTIST 100
#define WEIGHT_TITLE 100
#define WEIGHT_ALBUM 75
#define WEIGHT_GENRE 60

/* How a token occurs in a folded field. */
#define OCCURS_NONE 0
#define OCCURS_SUBSTRING 1 /* only inside a longer word */
#define OCCURS_WORD 2      /* bounded by spaces / string ends */

static bool is_ascii_alnum(unsigned char c) {
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Append @p s (length @p n) to out, honoring cap (NUL slot reserved). Returns
 * false when it didn't fit, so the caller stops rather than emitting a torn word. */
static bool fold_put(char *out, size_t cap, size_t *j, const char *s, size_t n) {
   if (*j + n + 1 > cap) {
      return false;
   }
   memcpy(out + *j, s, n);
   *j += n;
   return true;
}

size_t music_rank_fold(const char *in, char *out, size_t cap) {
   if (!out || cap == 0) {
      return 0;
   }
   size_t j = 0;
   bool pending_space = false;
   const unsigned char *p = (const unsigned char *)(in ? in : "");

   for (; *p; p++) {
      unsigned char c = *p;

      /* Apostrophes vanish so "Rockin'" == "Rockin" and "Don't" == "Dont". The
       * short-circuit keeps the 3-byte UTF-8 check from reading past a NUL. */
      if (c == '\'' || c == '`') {
         continue;
      }
      if (c == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) {
         p += 2;
         continue;
      }
      /* Common Unicode punctuation acts like ASCII punctuation (a separator):
       * U+2010-2015 dashes, U+201C/201D quotes, U+2026 ellipsis, and U+00A0
       * no-break space. Otherwise it would glue onto the adjacent word. */
      if (c == 0xE2 && p[1] == 0x80 &&
          ((p[2] >= 0x90 && p[2] <= 0x95) || p[2] == 0x9C || p[2] == 0x9D || p[2] == 0xA6)) {
         p += 2;
         pending_space = true;
         continue;
      }
      if (c == 0xC2 && p[1] == 0xA0) {
         p += 1;
         pending_space = true;
         continue;
      }

      if (c == '&') {
         if (j > 0 && !fold_put(out, cap, &j, " ", 1)) {
            break;
         }
         if (!fold_put(out, cap, &j, "and", 3)) {
            break;
         }
         pending_space = true;
         continue;
      }

      if (!is_ascii_alnum(c) && c < 0x80) {
         pending_space = true; /* separator; collapsed and trimmed */
         continue;
      }

      if (pending_space && j > 0) {
         if (!fold_put(out, cap, &j, " ", 1)) {
            break;
         }
      }
      pending_space = false;
      char ch = (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
      if (!fold_put(out, cap, &j, &ch, 1)) {
         break;
      }
   }
   /* A cap cut can leave a trailing separator; trim it. */
   while (j > 0 && out[j - 1] == ' ') {
      j--;
   }
   out[j] = '\0';
   return j;
}

/* Filler words a request often carries ("… by Ben Folds", "the album …") that
 * still help exact/phrase matching but must not be REQUIRED to match. */
static const char *const FILLER_WORDS[] = {
   "a", "an", "the", "of", "by", "and", "from", "feat", "ft", "featuring", "song", "track", "album",
};

static bool is_filler(const char *s, size_t n) {
   for (size_t w = 0; w < sizeof(FILLER_WORDS) / sizeof(FILLER_WORDS[0]); w++) {
      if (strlen(FILLER_WORDS[w]) == n && memcmp(FILLER_WORDS[w], s, n) == 0) {
         return true;
      }
   }
   return false;
}

void music_rank_needle_prepare(const char *text, music_rank_needle_t *out) {
   music_rank_needle_prepare_ex(text, false, out);
}

void music_rank_needle_prepare_ex(const char *text, bool all_required, music_rank_needle_t *out) {
   if (!out) {
      return;
   }
   out->len = music_rank_fold(text, out->folded, sizeof(out->folded));
   out->ntok = 0;
   out->nsig = 0;
   size_t i = 0;
   while (i < out->len && out->ntok < MUSIC_RANK_MAX_TOKENS) {
      size_t start = i;
      while (i < out->len && out->folded[i] != ' ') {
         i++;
      }
      int t = out->ntok++;
      out->tok[t].off = (unsigned short)start;
      out->tok[t].len = (unsigned short)(i - start);
      out->tok[t].sig = (!all_required && is_filler(out->folded + start, i - start)) ? 0 : 1;
      out->nsig += out->tok[t].sig;
      i++; /* skip the single separating space */
   }
   if (out->nsig == 0) {
      /* All filler ("The The", "The Who"-less "the") — every token is required. */
      for (int t = 0; t < out->ntok; t++) {
         out->tok[t].sig = 1;
      }
      out->nsig = out->ntok;
   }
}

/* Best occurrence of s[0..n) within folded field f[0..flen). Short strings only
 * count as whole words (see MUSIC_RANK_SHORT_TOKEN). */
static int occurs(const char *f, size_t flen, const char *s, size_t n) {
   if (n == 0 || n > flen) {
      return OCCURS_NONE;
   }
   const bool short_tok = (n <= MUSIC_RANK_SHORT_TOKEN);
   int best = OCCURS_NONE;
   const char *end = f + (flen - n); /* last possible start */
   for (const char *q = f; q <= end;) {
      /* Jump to the next candidate first byte instead of comparing at every offset. */
      q = memchr(q, (unsigned char)s[0], (size_t)(end - q) + 1);
      if (!q) {
         break;
      }
      size_t i = (size_t)(q - f);
      q++;
      if (memcmp(f + i + 1, s + 1, n - 1) != 0) {
         continue;
      }
      bool left = (i == 0) || f[i - 1] == ' ';
      bool right = (i + n == flen) || f[i + n] == ' ';
      if (left && right) {
         return OCCURS_WORD;
      }
      if (!short_tok) {
         best = OCCURS_SUBSTRING;
      }
   }
   return best;
}

/* Quality of a prepared needle against an already-folded field. */
static int quality_folded(const char *f, size_t flen, const music_rank_needle_t *n) {
   if (flen == 0 || n->len == 0) {
      return 0;
   }
   if (flen == n->len && memcmp(f, n->folded, flen) == 0) {
      return MUSIC_RANK_EXACT;
   }
   if (flen > n->len && memcmp(f, n->folded, n->len) == 0 && f[n->len] == ' ') {
      return MUSIC_RANK_PREFIX;
   }
   int phrase = occurs(f, flen, n->folded, n->len);
   if (phrase == OCCURS_WORD) {
      return MUSIC_RANK_WORD;
   }
   if (n->ntok == 1) {
      /* The lone token IS the phrase (and is required even if filler). */
      return phrase == OCCURS_SUBSTRING ? MUSIC_RANK_SUBSTRING : 0;
   }

   bool all_words = true;
   for (int t = 0; t < n->ntok; t++) {
      if (!n->tok[t].sig) {
         continue; /* filler may be absent */
      }
      int o = occurs(f, flen, n->folded + n->tok[t].off, n->tok[t].len);
      if (o == OCCURS_NONE) {
         /* A missing required token means this field doesn't match — unless the
          * whole phrase is a mid-word run (a 1-token needle). */
         return phrase == OCCURS_SUBSTRING ? MUSIC_RANK_SUBSTRING : 0;
      }
      if (o != OCCURS_WORD) {
         all_words = false;
      }
   }
   if (all_words) {
      return MUSIC_RANK_ALL_WORDS;
   }
   return phrase == OCCURS_SUBSTRING ? MUSIC_RANK_SUBSTRING : MUSIC_RANK_ALL_PRESENT;
}

int music_rank_field_quality_prepared(const char *field, const music_rank_needle_t *n) {
   if (!field || !field[0] || !n || n->len == 0) {
      return 0;
   }
   char f[MUSIC_RANK_FOLD_MAX];
   size_t flen = music_rank_fold(field, f, sizeof(f));
   return quality_folded(f, flen, n);
}

int music_rank_field_quality(const char *field, const char *needle) {
   if (!field || !field[0] || !needle || !needle[0]) {
      return 0;
   }
   music_rank_needle_t n;
   music_rank_needle_prepare(needle, &n);
   return music_rank_field_quality_prepared(field, &n);
}

int music_rank_field_match(const char *field, const char *needle) {
   if (!field || !field[0] || !needle || !needle[0]) {
      return 0;
   }
   music_rank_needle_t n;
   music_rank_needle_prepare_ex(needle, true, &n);
   return music_rank_field_quality_prepared(field, &n);
}

static int weighted(int quality, int weight) {
   return quality * weight / 100;
}

int music_rank_score_fields(const char *artist,
                            const char *title,
                            const char *album,
                            const char *genre,
                            const music_rank_needle_t *n) {
   if (!n || n->len == 0) {
      return 0;
   }
   const char *raw[4] = { artist, title, album, genre };
   static const int weight[4] = { WEIGHT_ARTIST, WEIGHT_TITLE, WEIGHT_ALBUM, WEIGHT_GENRE };
   char f[4][MUSIC_RANK_FOLD_MAX];
   size_t flen[4];

   int best = 0;
   for (int k = 0; k < 4; k++) {
      flen[k] = music_rank_fold(raw[k], f[k], sizeof(f[k]));
      int s = weighted(quality_folded(f[k], flen[k], n), weight[k]);
      if (s > best) {
         best = s;
      }
   }
   if (best > 0 || n->nsig < 2) {
      return best;
   }

   /* No single field holds every required token: accept the row if the fields
    * together do ("Ben Folds Rockin" → artist + title), below every single-field
    * match; failing that, a long query missing just one token still ranks last. */
   bool all_words = true;
   int covered = 0;
   int whole = 0;
   int missed = 0;
   const int max_missed = (n->nsig >= 3) ? 1 : 0; /* PARTIAL tolerates one miss */
   for (int t = 0; t < n->ntok; t++) {
      if (!n->tok[t].sig) {
         continue;
      }
      int o = OCCURS_NONE;
      for (int k = 0; k < 4 && o != OCCURS_WORD; k++) {
         int ok = occurs(f[k], flen[k], n->folded + n->tok[t].off, n->tok[t].len);
         if (ok > o) {
            o = ok;
         }
      }
      if (o != OCCURS_NONE) {
         covered++;
      } else if (++missed > max_missed) {
         return 0; /* can no longer reach any tier */
      }
      if (o == OCCURS_WORD) {
         whole++;
      } else {
         all_words = false;
      }
   }
   if (covered == n->nsig) {
      return all_words ? MUSIC_RANK_CROSS_WORDS : MUSIC_RANK_CROSS_PRESENT;
   }
   if (n->nsig >= 3 && covered >= n->nsig - 1 && whole >= 1) {
      return MUSIC_RANK_PARTIAL;
   }
   return 0;
}

int music_rank_score(const music_search_result_t *r, const char *text) {
   if (!r || !text || !text[0]) {
      return 0;
   }
   music_rank_needle_t n;
   music_rank_needle_prepare(text, &n);
   return music_rank_score_fields(r->artist, r->title, r->album, r->genre, &n);
}

int music_rank_pick_best(const music_search_result_t *results,
                         int count,
                         const char *title_query,
                         const char *artist) {
   if (!results || count <= 0) {
      return 0;
   }
   if (!title_query) {
      title_query = "";
   }

   music_rank_needle_t title_n;
   music_rank_needle_prepare(title_query, &title_n);
   music_rank_needle_t artist_n;
   const bool have_artist = artist && artist[0];
   if (have_artist) {
      music_rank_needle_prepare_ex(artist, true, &artist_n);
   }

   int best = 0;
   long best_score = -1; /* so candidate 0 always initializes the winner */
   size_t best_len = 0;

   for (int i = 0; i < count; i++) {
      const char *t = results[i].title;
      /* Title closeness via the shared tiers (max 1000), kept below the artist
       * bonus so a matching artist still dominates. */
      long score = music_rank_field_quality_prepared(t, &title_n);

      /* A matching artist dominates (disambiguates "Artist - Title"), using the
       * same punctuation-insensitive whole-word rule as artist: search. */
      if (have_artist &&
          music_rank_field_quality_prepared(results[i].artist, &artist_n) >= MUSIC_RANK_FIELD_MIN) {
         score += MUSIC_RANK_PICK_ARTIST_BONUS;
      }

      size_t len = strlen(t);
      /* Higher score wins; among equally-scored REAL matches prefer the shorter
       * (closer) title. When nothing matches, keep candidate 0 (the DB's first). */
      bool better = (score > best_score) || (score == best_score && score > 0 && len < best_len);
      if (better) {
         best_score = score;
         best_len = len;
         best = i;
      }
   }
   return best;
}

/* ============================ album grouping key ============================ */

/* Qualifier words that mark an edition/format of the same album rather than
 * different content. Matched as whole folded tokens (so "ep" never hits "deep");
 * "remaster" also matches as a prefix (remastered, remasters). */
static const char *const EDITION_WORDS[] = {
   "edition", "editions", "deluxe", "expanded", "explicit", "clean", "bonus",   "anniversary",
   "reissue", "ep",       "single", "disc",     "disk",     "cd",    "booklet",
};
#define EDITION_PREFIX "remaster"

/* Words that mean the qualifier names different CONTENT, so the group is kept even
 * if it also says "edition" ("(Live Edition)", "(Acoustic Version)"). */
static const char *const CONTENT_WORDS[] = {
   "live",     "acoustic",     "remix",         "remixes",   "demo",
   "demos",    "instrumental", "instrumentals", "unplugged", "session",
   "sessions", "version",      "mix",           "karaoke",   "orchestral",
};

static bool token_in(const char *t, size_t tl, const char *const *list, size_t n) {
   for (size_t w = 0; w < n; w++) {
      if (strlen(list[w]) == tl && memcmp(t, list[w], tl) == 0) {
         return true;
      }
   }
   return false;
}

static bool names_edition(const char *s, size_t n) {
   char raw[AUDIO_METADATA_STRING_MAX];
   char f[MUSIC_RANK_FOLD_MAX];
   if (n >= sizeof(raw)) {
      n = sizeof(raw) - 1;
   }
   memcpy(raw, s, n);
   raw[n] = '\0';
   size_t flen = music_rank_fold(raw, f, sizeof(f));
   bool edition = false;
   size_t i = 0;
   while (i < flen) {
      size_t start = i;
      while (i < flen && f[i] != ' ') {
         i++;
      }
      size_t tl = i - start;
      if (token_in(f + start, tl, CONTENT_WORDS,
                   sizeof(CONTENT_WORDS) / sizeof(CONTENT_WORDS[0]))) {
         return false; /* different content: keep the qualifier */
      }
      if ((tl >= strlen(EDITION_PREFIX) &&
           memcmp(f + start, EDITION_PREFIX, strlen(EDITION_PREFIX)) == 0) ||
          token_in(f + start, tl, EDITION_WORDS,
                   sizeof(EDITION_WORDS) / sizeof(EDITION_WORDS[0]))) {
         edition = true;
      }
      i++;
   }
   return edition;
}

/* True when raw text s[0..n) folds to the same string as @p artist. */
static bool names_artist(const char *s, size_t n, const char *artist) {
   if (!artist || !artist[0]) {
      return false;
   }
   char raw[AUDIO_METADATA_STRING_MAX];
   char a[MUSIC_RANK_FOLD_MAX];
   char b[MUSIC_RANK_FOLD_MAX];
   if (n >= sizeof(raw)) {
      n = sizeof(raw) - 1;
   }
   memcpy(raw, s, n);
   raw[n] = '\0';
   size_t al = music_rank_fold(raw, a, sizeof(a));
   size_t bl = music_rank_fold(artist, b, sizeof(b));
   return al > 0 && al == bl && memcmp(a, b, al) == 0;
}

void music_rank_album_key(const char *album, const char *artist, char *out, size_t cap) {
   if (!out || cap == 0) {
      return;
   }
   char buf[AUDIO_METADATA_STRING_MAX];
   size_t len = 0;
   if (album) {
      len = strnlen(album, sizeof(buf) - 1);
      memcpy(buf, album, len);
   }
   buf[len] = '\0';

   /* Treat " – " / " — " (en/em dash) like " - " so the suffix rule sees them. */
   for (size_t i = 0; i + 4 < len; i++) {
      if (buf[i] == ' ' && (unsigned char)buf[i + 1] == 0xE2 && (unsigned char)buf[i + 2] == 0x80 &&
          ((unsigned char)buf[i + 3] == 0x93 || (unsigned char)buf[i + 3] == 0x94) &&
          buf[i + 4] == ' ') {
         buf[i + 1] = '-';
         memmove(buf + i + 2, buf + i + 4, len - (i + 4) + 1);
         len -= 2;
      }
   }

   /* Peel trailing qualifiers one at a time: "X (Deluxe) [Explicit]", "X - Disc 1". */
   for (;;) {
      while (len > 0 && buf[len - 1] == ' ') {
         len--;
      }
      size_t cut = len;
      char close = len > 0 ? buf[len - 1] : '\0';
      if (close == ')' || close == ']') {
         char open = (close == ')') ? '(' : '[';
         size_t i = len - 1;
         while (i > 0 && buf[i - 1] != open) {
            i--;
         }
         if (i > 0 && names_edition(buf + i, len - 1 - i)) {
            cut = i - 1; /* drop the opener too */
         }
      } else {
         /* " - Deluxe Edition" / " - Single" / " - <artist>" style suffix. */
         for (size_t i = len; i >= 3; i--) {
            if (buf[i - 3] == ' ' && buf[i - 2] == '-' && buf[i - 1] == ' ') {
               if (names_edition(buf + i, len - i) || names_artist(buf + i, len - i, artist)) {
                  cut = i - 3;
               }
               break;
            }
         }
      }
      if (cut == len || cut == 0) {
         break; /* nothing to strip, or stripping would leave nothing */
      }
      len = cut;
   }
   buf[len] = '\0';
   music_rank_fold(buf, out, cap);
}
