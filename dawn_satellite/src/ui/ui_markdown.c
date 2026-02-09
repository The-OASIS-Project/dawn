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
 * Inline Markdown Rendering for SDL2 Transcript
 *
 * Single-pass parser splits text into styled words, then composites them
 * onto a scratch SDL_Surface via SDL_BlitSurface (CPU, NEON-accelerated).
 * One SDL_TEXTUREACCESS_STATIC texture is created at the end.
 */

#include "ui/ui_markdown.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MD_MAX_WORDS 256
#define MD_SCRATCH_SIZE 2048
#define CODE_BG_PAD_H 4  /* Horizontal padding for code span background */
#define CODE_BG_PAD_V 2  /* Vertical padding for code span background */
#define BULLET_INDENT 18 /* Pixel indent for bullet items */
#define CODE_BG_R 0x36
#define CODE_BG_G 0x40
#define CODE_BG_B 0x50
#define CODE_BORDER_R 0x4A
#define CODE_BORDER_G 0x55
#define CODE_BORDER_B 0x60
#define CODE_BORDER_W 2       /* Border width in pixels */
#define MD_MAX_SURFACE_H 4096 /* Cap scratch surface height */

/* Fallback font paths */
#define FALLBACK_MONO_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FALLBACK_BODY_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* =============================================================================
 * Internal Types (render-thread only, no threading concern)
 * ============================================================================= */

typedef struct {
   const char *text; /* Pointer into scratch buffer (null-terminated) */
   uint16_t len;
   uint16_t pixel_w;
   uint8_t style;      /* md_style_t */
   uint8_t line_break; /* Forced newline before this word */
   uint16_t indent;    /* Extra x-offset (bullet indent) */
} md_word_t;

/* Static module-level buffers — only accessed from SDL render thread */
static md_word_t s_word_pool[MD_MAX_WORDS];
static char s_scratch[MD_SCRATCH_SIZE];

/* =============================================================================
 * Font Loading Helper
 * ============================================================================= */

static TTF_Font *try_load_font(const char *font_dir, const char *filename, int size) {
   TTF_Font *font = NULL;
   char path[512];

   if (font_dir && font_dir[0]) {
      snprintf(path, sizeof(path), "%s/%s", font_dir, filename);
      font = TTF_OpenFont(path, size);
      if (font)
         return font;
   }

   snprintf(path, sizeof(path), "assets/fonts/%s", filename);
   font = TTF_OpenFont(path, size);
   return font;
}

/* =============================================================================
 * md_fonts_init / md_fonts_cleanup
 * ============================================================================= */

int md_fonts_init(md_fonts_t *fonts, const char *font_dir, int body_size) {
   if (!fonts)
      return 1;

   memset(fonts, 0, sizeof(*fonts));

   /* Regular — must succeed */
   fonts->fonts[MD_STYLE_NORMAL] = try_load_font(font_dir, "SourceSans3-Regular.ttf", body_size);
   if (!fonts->fonts[MD_STYLE_NORMAL]) {
      fonts->fonts[MD_STYLE_NORMAL] = TTF_OpenFont(FALLBACK_BODY_FONT, body_size);
   }
   if (!fonts->fonts[MD_STYLE_NORMAL]) {
      LOG_WARNING("md_fonts: no regular font found");
      return 1;
   }

   /* Bold — try real file, fallback to synthesized */
   fonts->fonts[MD_STYLE_BOLD] = try_load_font(font_dir, "SourceSans3-Bold.ttf", body_size);
   if (fonts->fonts[MD_STYLE_BOLD]) {
      LOG_INFO("md_fonts: Bold font loaded (real)");
   } else {
      fonts->fonts[MD_STYLE_BOLD] = try_load_font(font_dir, "SourceSans3-Regular.ttf", body_size);
      if (!fonts->fonts[MD_STYLE_BOLD])
         fonts->fonts[MD_STYLE_BOLD] = TTF_OpenFont(FALLBACK_BODY_FONT, body_size);
      if (fonts->fonts[MD_STYLE_BOLD]) {
         TTF_SetFontStyle(fonts->fonts[MD_STYLE_BOLD], TTF_STYLE_BOLD);
         LOG_INFO("md_fonts: Bold font (synthesized)");
      }
   }

   /* Italic — try real file, fallback to synthesized */
   fonts->fonts[MD_STYLE_ITALIC] = try_load_font(font_dir, "SourceSans3-Italic.ttf", body_size);
   if (fonts->fonts[MD_STYLE_ITALIC]) {
      LOG_INFO("md_fonts: Italic font loaded (real)");
   } else {
      fonts->fonts[MD_STYLE_ITALIC] = try_load_font(font_dir, "SourceSans3-Regular.ttf", body_size);
      if (!fonts->fonts[MD_STYLE_ITALIC])
         fonts->fonts[MD_STYLE_ITALIC] = TTF_OpenFont(FALLBACK_BODY_FONT, body_size);
      if (fonts->fonts[MD_STYLE_ITALIC]) {
         TTF_SetFontStyle(fonts->fonts[MD_STYLE_ITALIC], TTF_STYLE_ITALIC);
         LOG_INFO("md_fonts: Italic font (synthesized)");
      }
   }

   /* Bold+Italic — try real file, fallback to synthesized */
   fonts->fonts[MD_STYLE_BOLD_ITALIC] = try_load_font(font_dir, "SourceSans3-BoldItalic.ttf",
                                                      body_size);
   if (fonts->fonts[MD_STYLE_BOLD_ITALIC]) {
      LOG_INFO("md_fonts: BoldItalic font loaded (real)");
   } else {
      fonts->fonts[MD_STYLE_BOLD_ITALIC] = try_load_font(font_dir, "SourceSans3-Regular.ttf",
                                                         body_size);
      if (!fonts->fonts[MD_STYLE_BOLD_ITALIC])
         fonts->fonts[MD_STYLE_BOLD_ITALIC] = TTF_OpenFont(FALLBACK_BODY_FONT, body_size);
      if (fonts->fonts[MD_STYLE_BOLD_ITALIC]) {
         TTF_SetFontStyle(fonts->fonts[MD_STYLE_BOLD_ITALIC], TTF_STYLE_BOLD | TTF_STYLE_ITALIC);
         LOG_INFO("md_fonts: BoldItalic font (synthesized)");
      }
   }

   /* Code — IBM Plex Mono at slightly smaller size */
   fonts->fonts[MD_STYLE_CODE] = try_load_font(font_dir, "IBMPlexMono-Regular.ttf", body_size - 2);
   if (!fonts->fonts[MD_STYLE_CODE]) {
      fonts->fonts[MD_STYLE_CODE] = TTF_OpenFont(FALLBACK_MONO_FONT, body_size - 2);
   }
   if (!fonts->fonts[MD_STYLE_CODE]) {
      /* Last resort: reuse regular font for code */
      fonts->fonts[MD_STYLE_CODE] = fonts->fonts[MD_STYLE_NORMAL];
      LOG_WARNING("md_fonts: no mono font, using regular for code spans");
   }

   /* Compute line_height as max across all fonts */
   fonts->line_height = 0;
   for (int i = 0; i < MD_STYLE_COUNT; i++) {
      if (fonts->fonts[i]) {
         int skip = TTF_FontLineSkip(fonts->fonts[i]);
         if (skip > fonts->line_height)
            fonts->line_height = skip;
      }
   }

   /* Compute space width from regular font */
   TTF_SizeUTF8(fonts->fonts[MD_STYLE_NORMAL], " ", &fonts->space_w, NULL);

   return 0;
}

void md_fonts_cleanup(md_fonts_t *fonts) {
   if (!fonts)
      return;

   /* Close each unique font handle (code font may alias regular) */
   for (int i = 0; i < MD_STYLE_COUNT; i++) {
      if (!fonts->fonts[i])
         continue;
      /* Check if this handle was already closed (alias check) */
      bool duplicate = false;
      for (int j = 0; j < i; j++) {
         if (fonts->fonts[j] == fonts->fonts[i]) {
            duplicate = true;
            break;
         }
      }
      if (!duplicate) {
         TTF_CloseFont(fonts->fonts[i]);
      }
      fonts->fonts[i] = NULL;
   }
}

/* =============================================================================
 * Markdown Parser + Word Splitter (single pass)
 * ============================================================================= */

/**
 * @brief Check if position is at a bullet pattern at start of a line
 * @return Number of chars consumed (including trailing space), or 0
 */
static int check_bullet(const char *p) {
   /* "- " or "* " */
   if ((*p == '-' || *p == '*') && p[1] == ' ')
      return 2;
   /* "N. " (single digit) */
   if (isdigit((unsigned char)*p) && p[1] == '.' && p[2] == ' ')
      return 3;
   /* "NN. " (double digit) */
   if (isdigit((unsigned char)*p) && isdigit((unsigned char)p[1]) && p[2] == '.' && p[3] == ' ')
      return 4;
   return 0;
}

/**
 * @brief Emit a word into the word pool
 */
static int emit_word(const char *start,
                     int len,
                     md_style_t style,
                     const md_fonts_t *fonts,
                     int *scratch_off,
                     int word_count,
                     bool line_break,
                     uint16_t indent) {
   if (word_count >= MD_MAX_WORDS || len <= 0)
      return word_count;

   /* Bounds-check scratch arena */
   if (*scratch_off + len + 1 > MD_SCRATCH_SIZE)
      return word_count;

   md_word_t *w = &s_word_pool[word_count];
   w->text = &s_scratch[*scratch_off];
   memcpy(&s_scratch[*scratch_off], start, len);
   s_scratch[*scratch_off + len] = '\0';
   *scratch_off += len + 1;

   w->len = (uint16_t)len;
   w->style = (uint8_t)style;
   w->line_break = line_break ? 1 : 0;
   w->indent = indent;

   /* Measure pixel width */
   TTF_Font *font = fonts->fonts[style];
   if (!font)
      font = fonts->fonts[MD_STYLE_NORMAL];
   int pw = 0;
   TTF_SizeUTF8(font, w->text, &pw, NULL);
   w->pixel_w = (uint16_t)pw;

   return word_count + 1;
}

/**
 * @brief Parse markdown and split into styled words
 * @return Number of words emitted
 */
static int md_parse_and_split(const char *text, const md_fonts_t *fonts) {
   int wc = 0;   /* word count */
   int soff = 0; /* scratch offset */
   md_style_t style = MD_STYLE_NORMAL;
   const char *p = text;
   bool at_line_start = true;
   bool next_line_break = false;
   uint16_t next_indent = 0;

   while (*p && wc < MD_MAX_WORDS - 1) {
      /* Handle newlines */
      if (*p == '\n') {
         p++;
         at_line_start = true;
         next_line_break = true;
         next_indent = 0;
         continue;
      }

      /* At line start, check for bullet patterns */
      if (at_line_start) {
         /* Skip leading spaces */
         while (*p == ' ')
            p++;
         int bullet_len = check_bullet(p);
         if (bullet_len > 0) {
            next_line_break = true;
            next_indent = BULLET_INDENT;
            /* Emit bullet character */
            wc = emit_word("\xe2\x80\xa2", 3, MD_STYLE_NORMAL, fonts, &soff, wc, next_line_break,
                           next_indent);
            next_line_break = false;
            next_indent = BULLET_INDENT;
            p += bullet_len;
            at_line_start = false;
            continue;
         }
         at_line_start = false;
      }

      /* Skip spaces between words */
      if (*p == ' ') {
         while (*p == ' ')
            p++;
         continue;
      }

      /* Check for markdown delimiters */
      if (*p == '`') {
         /* Toggle code style */
         if (style == MD_STYLE_CODE) {
            style = MD_STYLE_NORMAL;
            p++;
            continue;
         } else {
            style = MD_STYLE_CODE;
            p++;
            continue;
         }
      }

      if (*p == '*') {
         /* Count consecutive asterisks */
         int stars = 0;
         const char *star_start = p;
         while (*p == '*')
            stars++, p++;

         if (stars >= 3) {
            /* Toggle bold+italic */
            if (style == MD_STYLE_BOLD_ITALIC)
               style = MD_STYLE_NORMAL;
            else
               style = MD_STYLE_BOLD_ITALIC;
            continue;
         } else if (stars == 2) {
            /* Toggle bold */
            if (style == MD_STYLE_BOLD)
               style = MD_STYLE_NORMAL;
            else
               style = MD_STYLE_BOLD;
            continue;
         } else if (stars == 1) {
            /* Single * — check if this looks like italic (not standalone) */
            if (*p && *p != ' ') {
               if (style == MD_STYLE_ITALIC)
                  style = MD_STYLE_NORMAL;
               else
                  style = MD_STYLE_ITALIC;
               continue;
            }
            /* Standalone * — treat as literal text */
            p = star_start; /* Rewind */
         }
      }

      /* Collect word characters until space, newline, or delimiter */
      const char *word_start = p;
      while (*p && *p != ' ' && *p != '\n') {
         /* Stop at markdown delimiters that start a new span */
         if (*p == '`')
            break;
         if (*p == '*') {
            /* Check if this closes or opens a style (not mid-word punctuation) */
            if (p > word_start)
               break;
         }
         p++;
      }

      int word_len = (int)(p - word_start);
      if (word_len > 0) {
         wc = emit_word(word_start, word_len, style, fonts, &soff, wc, next_line_break,
                        next_indent);
         next_line_break = false;
         next_indent = (next_indent == BULLET_INDENT) ? BULLET_INDENT : 0;
      }
   }

   if (*p && wc >= MD_MAX_WORDS - 1) {
      LOG_WARNING("md_parse: text truncated at %d words", wc);
   }

   return wc;
}

/* =============================================================================
 * md_render_text — Scratch-surface composite
 * ============================================================================= */

SDL_Texture *md_render_text(SDL_Renderer *renderer,
                            const md_fonts_t *fonts,
                            const char *text,
                            SDL_Color color,
                            SDL_Color bold_color,
                            int wrap_width,
                            int *out_w,
                            int *out_h) {
   if (!renderer || !fonts || !text || !text[0])
      return NULL;

   /* Parse and split into styled words */
   int wc = md_parse_and_split(text, fonts);
   if (wc <= 0)
      return NULL;

   int lh = fonts->line_height;
   int sw = fonts->space_w;

   /* Measure pass: compute line count and max width */
   int x = 0;
   int lines = 1;
   int max_w = 0;

   for (int i = 0; i < wc; i++) {
      md_word_t *w = &s_word_pool[i];

      if (w->line_break) {
         if (x > max_w)
            max_w = x;
         x = w->indent;
         lines++;
      }

      int word_total = w->pixel_w;
      if (w->style == MD_STYLE_CODE)
         word_total += CODE_BG_PAD_H * 2;

      /* Wrap if this word exceeds line width (unless it's the first word on line) */
      if (x > 0 && x + sw + word_total > wrap_width) {
         if (x > max_w)
            max_w = x;
         x = w->indent;
         lines++;
      }

      if (x > 0 && !w->line_break)
         x += sw;

      x += word_total;
   }
   if (x > max_w)
      max_w = x;

   int total_w = (max_w < wrap_width) ? max_w : wrap_width;
   int total_h = lines * lh;
   if (total_h > MD_MAX_SURFACE_H) {
      LOG_WARNING("md_render: clamping surface height from %d to %d", total_h, MD_MAX_SURFACE_H);
      total_h = MD_MAX_SURFACE_H;
   }

   if (total_w <= 0 || total_h <= 0)
      return NULL;

   /* Create scratch surface (ARGB8888, transparent) */
   SDL_Surface *scratch = SDL_CreateRGBSurfaceWithFormat(0, total_w, total_h, 32,
                                                         SDL_PIXELFORMAT_ARGB8888);
   if (!scratch)
      return NULL;

   /* Clear to fully transparent */
   SDL_FillRect(scratch, NULL, SDL_MapRGBA(scratch->format, 0, 0, 0, 0));

   /* Render pass */
   x = 0;
   int y = 0;

   for (int i = 0; i < wc; i++) {
      md_word_t *w = &s_word_pool[i];

      if (w->line_break) {
         x = w->indent;
         y += lh;
      }

      int word_total = w->pixel_w;
      if (w->style == MD_STYLE_CODE)
         word_total += CODE_BG_PAD_H * 2;

      /* Word wrap */
      if (x > 0 && x + sw + word_total > wrap_width) {
         x = w->indent;
         y += lh;
      }

      if (x > 0 && !w->line_break)
         x += sw;

      /* Select font and color */
      TTF_Font *font = fonts->fonts[w->style];
      if (!font)
         font = fonts->fonts[MD_STYLE_NORMAL];

      SDL_Color word_color = color;
      if (w->style == MD_STYLE_BOLD || w->style == MD_STYLE_BOLD_ITALIC)
         word_color = bold_color;

      int draw_x = x;

      /* Code span: draw background rect */
      if (w->style == MD_STYLE_CODE) {
         SDL_Rect bg_rect = { x, y + (lh - TTF_FontHeight(font)) / 2 - CODE_BG_PAD_V,
                              w->pixel_w + CODE_BG_PAD_H * 2,
                              TTF_FontHeight(font) + CODE_BG_PAD_V * 2 };
         SDL_FillRect(scratch, &bg_rect,
                      SDL_MapRGBA(scratch->format, CODE_BG_R, CODE_BG_G, CODE_BG_B, 255));

         /* Border */
         uint32_t border_col = SDL_MapRGBA(scratch->format, CODE_BORDER_R, CODE_BORDER_G,
                                           CODE_BORDER_B, 255);
         SDL_Rect top = { bg_rect.x, bg_rect.y, bg_rect.w, CODE_BORDER_W };
         SDL_Rect bot = { bg_rect.x, bg_rect.y + bg_rect.h - CODE_BORDER_W, bg_rect.w,
                          CODE_BORDER_W };
         SDL_Rect lft = { bg_rect.x, bg_rect.y, CODE_BORDER_W, bg_rect.h };
         SDL_Rect rgt = { bg_rect.x + bg_rect.w - CODE_BORDER_W, bg_rect.y, CODE_BORDER_W,
                          bg_rect.h };
         SDL_FillRect(scratch, &top, border_col);
         SDL_FillRect(scratch, &bot, border_col);
         SDL_FillRect(scratch, &lft, border_col);
         SDL_FillRect(scratch, &rgt, border_col);

         draw_x += CODE_BG_PAD_H;
      }

      /* Render word surface and blit onto scratch */
      SDL_Surface *word_surf = TTF_RenderUTF8_Blended(font, w->text, word_color);
      if (word_surf) {
         int text_y = y + (lh - word_surf->h) / 2;
         SDL_Rect dst = { draw_x, text_y, word_surf->w, word_surf->h };
         SDL_BlitSurface(word_surf, NULL, scratch, &dst);
         SDL_FreeSurface(word_surf);
      }

      x += word_total;
   }

   /* Create single static texture from the composited surface */
   SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, scratch);
   SDL_FreeSurface(scratch);

   if (texture) {
      if (out_w)
         *out_w = total_w;
      if (out_h)
         *out_h = total_h;
   }

   return texture;
}
