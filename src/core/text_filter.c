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
 * Filters for the tags a model writes into its reply text.
 */

#include "core/text_filter.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Context for buffer output callback */
typedef struct {
   char *buf;
   size_t size;
   size_t len;
} buffer_ctx_t;

/* Callback that appends to a buffer */
static void buffer_output_fn(const char *text, size_t len, void *ctx) {
   buffer_ctx_t *bc = (buffer_ctx_t *)ctx;
   if (!bc || !bc->buf || bc->size == 0) {
      return;
   }

   size_t remaining = bc->size - bc->len - 1;
   size_t to_copy = len < remaining ? len : remaining;

   if (to_copy > 0) {
      memcpy(bc->buf + bc->len, text, to_copy);
      bc->len += to_copy;
      bc->buf[bc->len] = '\0';
   }
}

/* =============================================================================
 * Memory-citation tag filter — strips <cited>…</cited> (grammar in text_filter.h).
 *
 * One byte step reads the grammar for both the whole-string strip and the
 * streaming filter, so the two agree on what is a tag and where it counts.
 * ============================================================================= */

typedef enum {
   CITED_MORE, /* still a prefix of a tag, or a whole one and what follows it */
   CITED_DONE, /* the closer completed a run past the hold: the tag */
   CITED_TEXT, /* not a tag to remove: the candidate is text */
   CITED_END,  /* a run past the hold ended here: opener and run are the tag */
   CITED_NEXT, /* another opener followed a whole tag: the tag (and the space after) */
} cited_step_t;

static bool cited_is_space(char c) {
   return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* After a whole tag within the hold: whitespace, then another opener (or the end
 * of the text, which the caller sees). */
static cited_step_t cited_step_trail(cited_tag_scan_t *sc, char c) {
   if (sc->next > 0 || c == '<') {
      if (c != CITED_TAG_OPEN[sc->next]) {
         return CITED_TEXT;
      }
      sc->next++;
   } else if (cited_is_space(c) && sc->ws < CITED_TAG_TRAIL_WS_MAX) {
      sc->ws++;
   } else {
      return CITED_TEXT; /* more text: the tag was mid-reply */
   }
   sc->len++;
   return sc->next == CITED_TAG_OPEN_LEN ? CITED_NEXT : CITED_MORE;
}

/* Read @p c into the candidate @p sc (its '<' already read).  On MORE, DONE and
 * NEXT @p c is counted in; on TEXT and END it isn't (it is the next text). */
static cited_step_t cited_step(cited_tag_scan_t *sc, char c) {
   if (sc->trail) {
      return cited_step_trail(sc, c);
   }
   if (sc->len < CITED_TAG_OPEN_LEN) {
      if (c != CITED_TAG_OPEN[sc->len]) {
         return CITED_TEXT;
      }
   } else if (sc->close > 0 || c == '<') {
      /* Spaces after '<', after '/' and before '>', a few in all. */
      size_t pads = sc->len - CITED_TAG_OPEN_LEN - sc->content - sc->close;
      bool pad = (c == ' ' || c == '\t') && pads < CITED_TAG_CLOSE_PAD_MAX &&
                 (sc->close == 1 || sc->close == 2 || sc->close == CITED_TAG_CLOSE_LEN - 1);
      if (!pad) {
         if (c != CITED_TAG_CLOSE[sc->close]) {
            return sc->over ? CITED_END : CITED_TEXT;
         }
         sc->close++;
      }
   } else if (c != '\0' && strchr(CITED_TAG_CONTENT_CHARS, c) != NULL) {
      if (sc->content == CITED_TAG_HOLD_MAX) {
         sc->over = true;
      }
      sc->content++;
   } else {
      return sc->over ? CITED_END : CITED_TEXT;
   }
   sc->len++;
   if (sc->close < CITED_TAG_CLOSE_LEN) {
      return CITED_MORE;
   }
   if (sc->over) {
      return CITED_DONE;
   }
   sc->trail = true; /* a whole tag: it counts only if the text ends here */
   return CITED_MORE;
}

void text_filter_cited_reset(cited_tag_filter_state_t *state) {
   if (state) {
      memset(state, 0, sizeof(*state));
   }
}

const char *text_filter_cited_find(const char *text, size_t *content_len, size_t *tag_len) {
   if (!text) {
      return NULL;
   }
   for (const char *s = strstr(text, CITED_TAG_OPEN); s; s = strstr(s + 1, CITED_TAG_OPEN)) {
      cited_tag_scan_t sc = { .len = 1 }; /* its '<' */
      cited_step_t step = CITED_MORE;
      for (const char *p = s + 1; *p && step == CITED_MORE; p++) {
         step = cited_step(&sc, *p);
      }
      if (step == CITED_TEXT || (step == CITED_MORE && sc.next > 0)) {
         continue; /* an opener in prose, a tag mid-text, or one before "<cit" */
      }
      if (content_len) {
         *content_len = sc.content;
      }
      if (tag_len) {
         if (step == CITED_END) {
            *tag_len = CITED_TAG_OPEN_LEN + sc.content;
         } else if (step == CITED_NEXT) {
            *tag_len = sc.len - CITED_TAG_OPEN_LEN; /* the next opener stays */
         } else if (step == CITED_DONE || sc.trail) {
            *tag_len = sc.len; /* a trailing tag goes with the whitespace after it */
         } else {
            *tag_len = 0; /* the text ends inside the tag */
         }
      }
      return s;
   }
   return NULL;
}

void text_filter_cited_strip(char *text) {
   if (!text)
      return;

   /* Each search goes on from where the last tag was, as the stream reads. */
   char *from = text;
   char *s;
   size_t tag_len;
   while ((s = (char *)text_filter_cited_find(from, NULL, &tag_len)) != NULL) {
      if (tag_len == 0) {
         *s = '\0'; /* the text ends inside the tag (truncated stream): drop it */
         break;
      }
      memmove(s, s + tag_len, strlen(s + tag_len) + 1);
      from = s;
   }
}

void text_filter_cited_normalize(char *text) {
   if (!text)
      return;

   char *p = text;
   while ((p = strchr(p, '<')) != NULL) {
      const char *q = p + 1;
      while (*q == ' ' || *q == '\t')
         q++; /* whitespace after '<' */
      bool closing = false;
      if (*q == '/') {
         closing = true;
         q++;
         while (*q == ' ' || *q == '\t')
            q++; /* whitespace after '/' */
      }
      if (strncmp(q, "cited", 5) != 0) {
         p++; /* not a citation tag — step past this '<' */
         continue;
      }
      q += 5;
      while (*q == ' ' || *q == '\t')
         q++; /* whitespace before '>' */
      if (*q != '>') {
         p++; /* "< cited" with no closing '>' on this run — not a tag */
         continue;
      }
      q++; /* consume '>' — [p, q) is the malformed tag */

      /* Rewrite the malformed span to the canonical tag.  The canonical form is
       * never longer than the matched span (we only removed interior whitespace),
       * so this compacts in place: shift the tail left, then write the tag. */
      const char *canon = closing ? CITED_TAG_CLOSE : CITED_TAG_OPEN;
      size_t clen = closing ? CITED_TAG_CLOSE_LEN : CITED_TAG_OPEN_LEN;
      memmove(p + clen, q, strlen(q) + 1);
      memcpy(p, canon, clen);
      p += clen;
   }
}

void text_filter_end_of_turn_strip(char *text) {
   if (!text)
      return;

   char *eot = strstr(text, END_OF_TURN_TAG);
   if (eot != NULL) {
      *eot = '\0';
   }
}

static void cited_push(cited_tag_filter_state_t *state,
                       char c,
                       text_filter_output_fn output_fn,
                       void *ctx);

/* The candidate is over and its held bytes are text: they are read again from
 * @p from (0, or 1 when the first is a '<' that already failed as an opener, so
 * it goes straight out), since a '<' among them may start a tag. */
static void cited_reread(cited_tag_filter_state_t *state,
                         size_t from,
                         text_filter_output_fn output_fn,
                         void *ctx) {
   char held[CITED_TAG_BUF_SIZE];
   size_t n = state->held;
   memcpy(held, state->buffer, n);
   text_filter_cited_reset(state);
   if (from > 0 && n > 0) {
      output_fn(held, 1, ctx);
   }
   for (size_t i = from; i < n; i++) {
      cited_push(state, held[i], output_fn, ctx);
   }
}

/* One byte through the filter. */
static void cited_push(cited_tag_filter_state_t *state,
                       char c,
                       text_filter_output_fn output_fn,
                       void *ctx) {
   if (state->scan.len == 0) {
      if (c == '<') {
         state->buffer[0] = c;
         state->held = 1;
         state->scan.len = 1;
      } else {
         output_fn(&c, 1, ctx);
      }
      return;
   }
   switch (cited_step(&state->scan, c)) {
      case CITED_MORE:
         if (state->scan.over && state->scan.close == 0) {
            state->held = 0; /* content past the hold: a tag's, dropped as it comes */
         } else {
            state->buffer[state->held++] = c; /* fits: see CITED_TAG_BUF_SIZE */
         }
         break;
      case CITED_DONE:
         text_filter_cited_reset(state); /* a whole tag: dropped */
         break;
      case CITED_NEXT:
         /* The held tag was followed by another opener: dropped, and the opener
          * begins the next candidate. */
         text_filter_cited_reset(state);
         memcpy(state->buffer, CITED_TAG_OPEN, CITED_TAG_OPEN_LEN);
         state->held = CITED_TAG_OPEN_LEN;
         state->scan.len = CITED_TAG_OPEN_LEN;
         break;
      case CITED_TEXT:
         cited_reread(state, 1, output_fn, ctx);
         cited_push(state, c, output_fn, ctx);
         break;
      case CITED_END:
         cited_reread(state, 0, output_fn, ctx); /* the closer it began, if any */
         cited_push(state, c, output_fn, ctx);
         break;
   }
}

void text_filter_cited_tags(cited_tag_filter_state_t *state,
                            const char *text,
                            text_filter_output_fn output_fn,
                            void *ctx) {
   if (!state || !text || !output_fn) {
      return;
   }

   /* Text outside a held tag goes out in runs; only a '<' and what follows it
    * while it can still be a tag go byte by byte. */
   const char *run = NULL;
   const char *p = text;
   for (; *p; p++) {
      if (state->scan.len == 0 && *p != '<') {
         if (!run) {
            run = p;
         }
         continue;
      }
      if (run) {
         output_fn(run, (size_t)(p - run), ctx);
         run = NULL;
      }
      cited_push(state, *p, output_fn, ctx);
   }
   if (run) {
      output_fn(run, (size_t)(p - run), ctx);
   }
}

int text_filter_cited_tags_to_buffer(cited_tag_filter_state_t *state,
                                     const char *text,
                                     char *out_buf,
                                     size_t out_size) {
   if (!state || !text || !out_buf || out_size == 0) {
      return 0;
   }

   buffer_ctx_t ctx = { .buf = out_buf, .size = out_size, .len = 0 };
   out_buf[0] = '\0';

   text_filter_cited_tags(state, text, buffer_output_fn, &ctx);

   return (int)ctx.len;
}

int text_filter_cited_flush_to_buffer(cited_tag_filter_state_t *state,
                                      char *out_buf,
                                      size_t out_size) {
   if (!state || !out_buf || out_size == 0) {
      return 0;
   }
   out_buf[0] = '\0';

   buffer_ctx_t ctx = { .buf = out_buf, .size = out_size, .len = 0 };
   /* The end of the text.  A held partial OPENER is real text, and so is a whole
    * tag followed by one: emit them.  A whole tag with only whitespace after it
    * ended the text, and past a whole opener the held bytes are a tag whose
    * closer never arrived (truncated stream): drop them. */
   const cited_tag_scan_t *sc = &state->scan;
   if ((sc->len > 0 && sc->len < CITED_TAG_OPEN_LEN) || (sc->trail && sc->next > 0)) {
      buffer_output_fn(state->buffer, state->held, &ctx);
   }
   text_filter_cited_reset(state);
   return (int)ctx.len;
}

/* =============================================================================
 * Expression tags — `[emote:<name>]`, stripped from complete text.
 * ============================================================================= */

#define EMOTE_NAME_AS_STRING(name) #name,
static const char *const s_emote_names[] = { EMOTE_NAMES(EMOTE_NAME_AS_STRING) };

static bool emote_name_known(const char *name) {
   for (size_t i = 0; i < sizeof(s_emote_names) / sizeof(s_emote_names[0]); i++) {
      if (strcmp(name, s_emote_names[i]) == 0) {
         return true;
      }
   }
   return false;
}

/* The next opener at or after @p s, in any case ("[Emote:" is a slip worth
 * catching), or NULL. */
static char *emote_find_opener(char *s) {
   for (char *p = strchr(s, '['); p != NULL; p = strchr(p + 1, '[')) {
      if (strncasecmp(p, EMOTE_TAG_OPEN, EMOTE_TAG_OPEN_LEN) == 0) {
         return p;
      }
   }
   return NULL;
}

/* The tag at @p s (which starts with the opener): its end, past one trailing
 * space, with its name lower-cased into @p name ("" when what's inside isn't
 * one name, e.g. "happy, curious": still a tag, never spoken).  NULL when no
 * "]" closes it on the same line within EMOTE_TAG_SPAN: then it's text. */
static char *emote_tag_end(char *s, char name[EMOTE_NAME_MAX]) {
   char *p = s + EMOTE_TAG_OPEN_LEN;
   while (*p == ' ') {
      p++;
   }
   size_t n = 0;
   while ((isalpha((unsigned char)*p) || *p == '_' || *p == '-') && n + 1 < EMOTE_NAME_MAX) {
      name[n++] = (char)tolower((unsigned char)*p++);
   }
   name[n] = '\0';
   while (*p == ' ') {
      p++;
   }
   if (*p != ']') {
      name[0] = '\0';
      const size_t limit = EMOTE_TAG_OPEN_LEN +
                           EMOTE_TAG_SPAN; /* offsets: no pointer past the end */
      while ((size_t)(p - s) < limit && *p != '\0' && *p != ']' && *p != '\n' && *p != '[') {
         p++;
      }
      if ((size_t)(p - s) >= limit || *p != ']') {
         return NULL;
      }
   }
   p++;
   return *p == ' ' ? p + 1 : p;
}

bool text_filter_emote_strip(char *text, char *name_out, size_t name_size) {
   if (name_out && name_size > 0) {
      name_out[0] = '\0';
   }
   if (!text) {
      return false;
   }
   bool found = false;
   char *s = text;
   while ((s = emote_find_opener(s)) != NULL) {
      char name[EMOTE_NAME_MAX];
      char *end = emote_tag_end(s, name);
      if (end == NULL) {
         s += EMOTE_TAG_OPEN_LEN; /* not a tag: leave it as text */
         continue;
      }
      if (!found && emote_name_known(name)) {
         found = true;
         if (name_out && name_size > 0) {
            snprintf(name_out, name_size, "%s", name);
         }
      }
      /* Its trailing space goes too, unless the tag sat against a word:
       * "Sure,[emote:happy] glad" keeps the words apart. */
      if (end[-1] == ' ' && s > text && !isspace((unsigned char)s[-1])) {
         end--;
      }
      memmove(s, end, strlen(end) + 1);
   }
   return found;
}

void text_split_pieces(const char *text, size_t max_bytes, text_piece_fn fn, void *ctx) {
   if (!text || !text[0] || !fn) {
      return;
   }
   if (max_bytes == 0 || max_bytes > TEXT_PIECE_MAX) {
      max_bytes = TEXT_PIECE_MAX;
   } else if (max_bytes < TEXT_PIECE_MIN) {
      max_bytes = TEXT_PIECE_MIN; /* a shorter limit could cut inside a character */
   }

   size_t len = strlen(text);
   if (len <= max_bytes) {
      fn(text, ctx);
      return;
   }

   char piece[TEXT_PIECE_MAX + 1];
   while (len > 0) {
      size_t n = len;
      if (n > max_bytes) {
         /* Back up off continuation bytes (10xxxxxx) to the start of the
          * character the cut would split. */
         n = max_bytes;
         while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
         }
         if (n == 0) {
            n = max_bytes; /* malformed: no character start in reach */
         }
      }
      memcpy(piece, text, n);
      piece[n] = '\0';
      fn(piece, ctx);
      text += n;
      len -= n;
   }
}
