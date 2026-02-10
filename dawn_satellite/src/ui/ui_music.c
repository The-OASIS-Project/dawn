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
 * Music Player Panel - SDL2 UI
 *
 * Three-tab panel (Playing / Queue / Library) with visualizer,
 * transport controls, queue management, and library browsing.
 * Phase 1: control + UI only (no audio streaming).
 */

#include "ui/ui_music.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "ui/ui_colors.h"
#include "ws_client.h"

#ifdef HAVE_OPUS
#include "music_playback.h"
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TAB_HEIGHT 44
#define VIZ_HEIGHT 180
#define VIZ_BAR_COUNT MUSIC_VIZ_BAR_COUNT
#define VIZ_UPDATE_MS 50
#define TRANSPORT_BTN_SIZE 48
#define TRANSPORT_PLAY_SIZE 56
#define TOGGLE_BTN_SIZE 44
#define LIST_ROW_HEIGHT 48
#define ADD_BTN_SIZE 44
#define TAP_DEBOUNCE_MS 200
#define PROGRESS_BAR_HEIGHT 12
#define INSET_BG_R 0x0D
#define INSET_BG_G 0x0F
#define INSET_BG_B 0x12
#define ACCENT_R 0x2D
#define ACCENT_G 0xD4
#define ACCENT_B 0xBF

/* Font sizes */
#define LABEL_FONT_SIZE 18
#define BODY_FONT_SIZE 22

/* Fallback fonts */
#define FALLBACK_MONO_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FALLBACK_BODY_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* Struct definition is in ui_music.h (needed by sdl_ui.c for embedding) */

/* =============================================================================
 * Font Loading Helper
 * ============================================================================= */

static TTF_Font *load_font(const char *font_dir,
                           const char *filename,
                           const char *fallback,
                           int size) {
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
   if (font)
      return font;
   if (fallback) {
      font = TTF_OpenFont(fallback, size);
   }
   return font;
}

/* =============================================================================
 * Duration Formatting
 * ============================================================================= */

static void format_time(float seconds, char *buf, size_t size) {
   int total = (int)seconds;
   if (total < 0)
      total = 0;
   int mins = total / 60;
   int secs = total % 60;
   snprintf(buf, size, "%d:%02d", mins, secs);
}

/* =============================================================================
 * Texture Cache Helpers
 * ============================================================================= */

static void invalidate_track_cache(ui_music_t *m) {
   if (m->title_tex) {
      SDL_DestroyTexture(m->title_tex);
      m->title_tex = NULL;
   }
   if (m->artist_tex) {
      SDL_DestroyTexture(m->artist_tex);
      m->artist_tex = NULL;
   }
   if (m->album_tex) {
      SDL_DestroyTexture(m->album_tex);
      m->album_tex = NULL;
   }
   m->title_w = m->title_h = 0;
   m->artist_w = m->artist_h = 0;
   m->album_w = m->album_h = 0;
}

static void ensure_track_cached(ui_music_t *m) {
   if (!m->renderer)
      return;

   /* Title */
   if (!m->title_tex && m->current_track.title[0] && m->body_font) {
      if (strcmp(m->cached_title, m->current_track.title) != 0) {
         snprintf(m->cached_title, sizeof(m->cached_title), "%s", m->current_track.title);
      }
      SDL_Color c = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B, 255 };
      SDL_Surface *s = TTF_RenderUTF8_Blended(m->body_font, m->current_track.title, c);
      if (s) {
         m->title_tex = SDL_CreateTextureFromSurface(m->renderer, s);
         m->title_w = s->w;
         m->title_h = s->h;
         SDL_FreeSurface(s);
      }
   }

   /* Artist */
   if (!m->artist_tex && m->current_track.artist[0] && m->label_font) {
      SDL_Color c = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B, 255 };
      SDL_Surface *s = TTF_RenderUTF8_Blended(m->label_font, m->current_track.artist, c);
      if (s) {
         m->artist_tex = SDL_CreateTextureFromSurface(m->renderer, s);
         m->artist_w = s->w;
         m->artist_h = s->h;
         SDL_FreeSurface(s);
      }
   }

   /* Album */
   if (!m->album_tex && m->current_track.album[0] && m->label_font) {
      SDL_Color c = { COLOR_TEXT_TERTIARY_R, COLOR_TEXT_TERTIARY_G, COLOR_TEXT_TERTIARY_B, 255 };
      SDL_Surface *s = TTF_RenderUTF8_Blended(m->label_font, m->current_track.album, c);
      if (s) {
         m->album_tex = SDL_CreateTextureFromSurface(m->renderer, s);
         m->album_w = s->w;
         m->album_h = s->h;
         SDL_FreeSurface(s);
      }
   }
}

/* =============================================================================
 * Static Texture Caches (tab labels + transport/toggle icons)
 * ============================================================================= */

/* Size of shuffle/repeat icon drawing area (pixels) */
#define TOGGLE_ICON_DIM 22
/* Size of transport icon drawing area (pixels) */
#define TRANSPORT_ICON_DIM 24

static SDL_Texture *build_white_tex(SDL_Renderer *r,
                                    TTF_Font *font,
                                    const char *text,
                                    int *out_w,
                                    int *out_h) {
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, white);
   if (!s)
      return NULL;
   SDL_Texture *tex = SDL_CreateTextureFromSurface(r, s);
   if (out_w)
      *out_w = s->w;
   if (out_h)
      *out_h = s->h;
   SDL_FreeSurface(s);
   return tex;
}

/* --- SDL primitive drawing helpers for transport/toggle icons --- */

/* --- Transport icon builders (render-to-texture, white on transparent) --- */

/** Previous track: vertical bar + two left-pointing triangles */
static SDL_Texture *build_prev_icon(SDL_Renderer *r, int sz) {
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int cy = sz / 2;
   int bar_w = 2;
   int bar_h = sz * 2 / 3;
   int bar_x = 2;

   /* Vertical bar on left */
   SDL_Rect bar = { bar_x, cy - bar_h / 2, bar_w, bar_h };
   SDL_RenderFillRect(r, &bar);

   /* Two left-pointing filled triangles (tip on left, base on right) */
   int tri_h = sz / 2;       /* Half-height of each triangle */
   int tri_w = (sz - 6) / 2; /* Width of each triangle */
   int tri1_left = bar_x + bar_w + 1;
   int tri2_left = tri1_left + tri_w;

   /* First triangle */
   for (int col = 0; col < tri_w; col++) {
      int h = tri_h * col / tri_w;
      SDL_RenderDrawLine(r, tri1_left + col, cy - h, tri1_left + col, cy + h);
   }

   /* Second triangle */
   for (int col = 0; col < tri_w; col++) {
      int h = tri_h * col / tri_w;
      SDL_RenderDrawLine(r, tri2_left + col, cy - h, tri2_left + col, cy + h);
   }

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

/** Play: right-pointing filled triangle, centered */
static SDL_Texture *build_play_icon(SDL_Renderer *r, int sz) {
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int cy = sz / 2;
   int tri_h = sz * 2 / 5; /* Half-height */
   int left = sz / 4;
   int right = sz - sz / 4;
   int tri_w = right - left;

   /* Right-pointing triangle via vertical scanlines */
   for (int col = 0; col < tri_w; col++) {
      int h = tri_h * (tri_w - col) / tri_w;
      SDL_RenderDrawLine(r, left + col, cy - h, left + col, cy + h);
   }

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

/** Pause: two vertical bars */
static SDL_Texture *build_pause_icon(SDL_Renderer *r, int sz) {
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int bar_w = sz / 5;
   int bar_h = sz * 7 / 10;
   int gap = sz / 5;
   int total_w = bar_w * 2 + gap;
   int x0 = (sz - total_w) / 2;
   int y0 = (sz - bar_h) / 2;

   SDL_Rect bar1 = { x0, y0, bar_w, bar_h };
   SDL_Rect bar2 = { x0 + bar_w + gap, y0, bar_w, bar_h };
   SDL_RenderFillRect(r, &bar1);
   SDL_RenderFillRect(r, &bar2);

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

/** Next track: two right-pointing triangles + vertical bar on right */
static SDL_Texture *build_next_icon(SDL_Renderer *r, int sz) {
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int cy = sz / 2;
   int bar_w = 2;
   int bar_h = sz * 2 / 3;
   int bar_x = sz - 2 - bar_w;

   /* Vertical bar on right */
   SDL_Rect bar = { bar_x, cy - bar_h / 2, bar_w, bar_h };
   SDL_RenderFillRect(r, &bar);

   /* Two right-pointing filled triangles (base on left, tip on right) */
   int tri_h = sz / 2;
   int tri_w = (sz - 6) / 2;
   int tri1_left = 1;
   int tri2_left = tri1_left + tri_w;

   /* First triangle */
   for (int col = 0; col < tri_w; col++) {
      int h = tri_h * (tri_w - col) / tri_w;
      SDL_RenderDrawLine(r, tri1_left + col, cy - h, tri1_left + col, cy + h);
   }

   /* Second triangle */
   for (int col = 0; col < tri_w; col++) {
      int h = tri_h * (tri_w - col) / tri_w;
      SDL_RenderDrawLine(r, tri2_left + col, cy - h, tri2_left + col, cy + h);
   }

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

/** Draw a 2px thick line (offsets perpendicular to the line direction) */
static void draw_thick_line(SDL_Renderer *r, int x1, int y1, int x2, int y2) {
   SDL_RenderDrawLine(r, x1, y1, x2, y2);
   if (abs(x2 - x1) >= abs(y2 - y1)) {
      SDL_RenderDrawLine(r, x1, y1 + 1, x2, y2 + 1);
   } else {
      SDL_RenderDrawLine(r, x1 + 1, y1, x2 + 1, y2);
   }
}

/** Filled right-pointing triangle (arrowhead) */
static void fill_arrow_right(SDL_Renderer *r, int tip_x, int tip_y, int sz) {
   for (int i = 0; i <= sz; i++) {
      SDL_RenderDrawLine(r, tip_x - i, tip_y - i, tip_x - i, tip_y + i);
   }
}

/** Filled left-pointing triangle (arrowhead) */
static void fill_arrow_left(SDL_Renderer *r, int tip_x, int tip_y, int sz) {
   for (int i = 0; i <= sz; i++) {
      SDL_RenderDrawLine(r, tip_x + i, tip_y - i, tip_x + i, tip_y + i);
   }
}

/**
 * Build shuffle icon: two crossing arrows (SVG-style X-pattern).
 * Matches web UI: two diagonal lines crossing in center with arrowheads
 * at top-right and bottom-right corners.
 * Rendered white on transparent RGBA texture for later color-modding.
 */
static SDL_Texture *build_shuffle_icon(SDL_Renderer *r) {
   const int sz = TOGGLE_ICON_DIM;
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;

   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int mg = 2;            /* margin */
   int ah = 3;            /* arrowhead size */
   int top = mg + 2;      /* top path y */
   int bot = sz - mg - 3; /* bottom path y */
   int left = mg;
   int right = sz - mg - 1;

   /* Path 1: bottom-left → top-right (full diagonal) */
   draw_thick_line(r, left, bot, right - ah, top);

   /* Path 2: top-left → bottom-right, split into two halves with gap at cross */
   int mid_x = sz / 2;
   int mid_y = (top + bot) / 2;
   draw_thick_line(r, left, top, mid_x - 2, mid_y - 1);
   draw_thick_line(r, mid_x + 2, mid_y + 1, right - ah, bot);

   /* Top-right arrowhead (L-shaped polyline: down-left to tip, tip to left) */
   fill_arrow_right(r, right, top, ah);

   /* Bottom-right arrowhead */
   fill_arrow_right(r, right, bot, ah);

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

/**
 * Build repeat icon: rounded loop with two opposing arrowheads (SVG-style).
 * Top path goes left-to-right with rounded left corner, arrow at right.
 * Bottom path goes right-to-left with rounded right corner, arrow at left.
 * If one_mode is true, a "1" is rendered in the center.
 */
static SDL_Texture *build_repeat_icon(SDL_Renderer *r, TTF_Font *font, bool one_mode) {
   const int sz = TOGGLE_ICON_DIM;
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;

   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int mg = 2;
   int ah = 3; /* arrowhead size */
   int cr = 3; /* corner radius (approx with diagonals) */

   int top = mg + ah;      /* top horizontal line y */
   int bot = sz - mg - ah; /* bottom horizontal line y */
   int lt = mg;            /* left edge */
   int rt = sz - mg - 1;   /* right edge */

   /* Top path: rounded left corner going up, then horizontal to right arrow */
   /* Left vertical segment (going up from bottom-ish to near top) */
   draw_thick_line(r, lt, bot - cr, lt, top + cr);
   /* Rounded top-left corner (3 short diagonal segments) */
   draw_thick_line(r, lt, top + cr, lt + 1, top + 1);
   draw_thick_line(r, lt + 1, top + 1, lt + cr, top);
   /* Top horizontal line */
   draw_thick_line(r, lt + cr, top, rt - ah, top);
   /* Right arrow */
   fill_arrow_right(r, rt, top, ah);

   /* Bottom path: rounded right corner going down, then horizontal to left arrow */
   /* Right vertical segment */
   draw_thick_line(r, rt, top + cr, rt, bot - cr);
   /* Rounded bottom-right corner */
   draw_thick_line(r, rt, bot - cr, rt - 1, bot - 1);
   draw_thick_line(r, rt - 1, bot - 1, rt - cr, bot);
   /* Bottom horizontal line */
   draw_thick_line(r, rt - cr, bot, lt + ah, bot);
   /* Left arrow */
   fill_arrow_left(r, lt, bot, ah);

   /* Repeat-one: draw "1" in center */
   if (one_mode && font) {
      SDL_Color white = { 255, 255, 255, 255 };
      SDL_Surface *s = TTF_RenderUTF8_Blended(font, "1", white);
      if (s) {
         SDL_Texture *one = SDL_CreateTextureFromSurface(r, s);
         if (one) {
            SDL_Rect dst = { (sz - s->w) / 2, (sz - s->h) / 2, s->w, s->h };
            SDL_RenderCopy(r, one, NULL, &dst);
            SDL_DestroyTexture(one);
         }
         SDL_FreeSurface(s);
      }
   }

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

/* Static label indices (match MUSIC_SLABEL_COUNT in header) */
enum {
   SLABEL_NO_TRACK = 0, /* "No track selected" (body_font) */
   SLABEL_CLEAR_ALL,    /* "Clear All" */
   SLABEL_BROWSE_HINT,  /* "Tap a category to browse" */
   SLABEL_BACK,         /* "← Back" */
   SLABEL_PLUS,         /* "+" */
};

static void build_static_caches(ui_music_t *m) {
   if (m->static_cache_ready || !m->renderer || !m->label_font)
      return;

   static const char *tab_names[] = { "Playing", "Queue", "Library" };
   for (int i = 0; i < 3; i++) {
      m->tab_tex[i] = build_white_tex(m->renderer, m->label_font, tab_names[i], &m->tab_tex_w[i],
                                      &m->tab_tex_h[i]);
   }

   /* Transport icons: 0=prev, 1=play, 2=pause, 3=next (SDL primitives) */
   m->transport_tex[0] = build_prev_icon(m->renderer, TRANSPORT_ICON_DIM);
   m->transport_tex_w[0] = m->transport_tex_h[0] = TRANSPORT_ICON_DIM;
   m->transport_tex[1] = build_play_icon(m->renderer, TRANSPORT_ICON_DIM);
   m->transport_tex_w[1] = m->transport_tex_h[1] = TRANSPORT_ICON_DIM;
   m->transport_tex[2] = build_pause_icon(m->renderer, TRANSPORT_ICON_DIM);
   m->transport_tex_w[2] = m->transport_tex_h[2] = TRANSPORT_ICON_DIM;
   m->transport_tex[3] = build_next_icon(m->renderer, TRANSPORT_ICON_DIM);
   m->transport_tex_w[3] = m->transport_tex_h[3] = TRANSPORT_ICON_DIM;

   /* Toggle icons: shuffle + repeat (SDL primitives, not emoji) */
   m->shuffle_icon_tex = build_shuffle_icon(m->renderer);
   m->repeat_icon_tex = build_repeat_icon(m->renderer, m->label_font, false);
   m->repeat_one_icon_tex = build_repeat_icon(m->renderer, m->label_font, true);

   /* Static labels (white text, tinted via SDL_SetTextureColorMod at render time) */
   m->slabel_tex[SLABEL_NO_TRACK] = build_white_tex(
       m->renderer, m->body_font ? m->body_font : m->label_font, "No track selected",
       &m->slabel_w[SLABEL_NO_TRACK], &m->slabel_h[SLABEL_NO_TRACK]);
   m->slabel_tex[SLABEL_CLEAR_ALL] = build_white_tex(m->renderer, m->label_font, "Clear All",
                                                     &m->slabel_w[SLABEL_CLEAR_ALL],
                                                     &m->slabel_h[SLABEL_CLEAR_ALL]);
   m->slabel_tex[SLABEL_BROWSE_HINT] = build_white_tex(m->renderer, m->label_font,
                                                       "Tap a category to browse",
                                                       &m->slabel_w[SLABEL_BROWSE_HINT],
                                                       &m->slabel_h[SLABEL_BROWSE_HINT]);
   m->slabel_tex[SLABEL_BACK] = build_white_tex(m->renderer, m->label_font, "\xE2\x86\x90 Back",
                                                &m->slabel_w[SLABEL_BACK],
                                                &m->slabel_h[SLABEL_BACK]);
   m->slabel_tex[SLABEL_PLUS] = build_white_tex(m->renderer, m->label_font, "+",
                                                &m->slabel_w[SLABEL_PLUS],
                                                &m->slabel_h[SLABEL_PLUS]);

   m->static_cache_ready = true;
}

/* =============================================================================
 * Rendering: Tabs
 * ============================================================================= */

static void render_tabs(ui_music_t *m, SDL_Renderer *r) {
   build_static_caches(m);

   int tab_w = m->panel_w / 3;
   int y = m->panel_y;

   for (int i = 0; i < 3; i++) {
      int tx = m->panel_x + i * tab_w;

      /* Tab background */
      if (i == (int)m->active_tab) {
         SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                                255);
      } else {
         SDL_SetRenderDrawColor(r, COLOR_BG_SECONDARY_R, COLOR_BG_SECONDARY_G, COLOR_BG_SECONDARY_B,
                                255);
      }
      SDL_Rect tab_rect = { tx, y, tab_w, TAB_HEIGHT };
      SDL_RenderFillRect(r, &tab_rect);

      /* Tab label from cache (white texture, color-modulated) */
      if (m->tab_tex[i]) {
         if (i == (int)m->active_tab) {
            SDL_SetTextureColorMod(m->tab_tex[i], ACCENT_R, ACCENT_G, ACCENT_B);
         } else {
            SDL_SetTextureColorMod(m->tab_tex[i], COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                                   COLOR_TEXT_SECONDARY_B);
         }
         SDL_Rect dst = { tx + (tab_w - m->tab_tex_w[i]) / 2,
                          y + (TAB_HEIGHT - m->tab_tex_h[i]) / 2, m->tab_tex_w[i],
                          m->tab_tex_h[i] };
         SDL_RenderCopy(r, m->tab_tex[i], NULL, &dst);
      }

      /* Active tab underline */
      if (i == (int)m->active_tab) {
         SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 255);
         SDL_Rect underline = { tx + 8, y + TAB_HEIGHT - 3, tab_w - 16, 3 };
         SDL_RenderFillRect(r, &underline);
      }
   }
}

/* =============================================================================
 * Rendering: Visualizer (simulated in Phase 1)
 * ============================================================================= */

static void update_visualizer(ui_music_t *m) {
   /* Remove 50ms gate - read spectrum every frame for lower latency.
    * Audio computes spectrum every ~53ms, but UI should poll as fast as possible. */

#ifdef HAVE_OPUS
   if (m->music_pb) {
      /* Real spectrum data arrives via ui_music_update_spectrum().
       * When paused/stopped, decay bars to near-zero. */
      if (!m->playing || m->paused) {
         for (int i = 0; i < VIZ_BAR_COUNT; i++) {
            m->viz_targets[i] = 0.03f;
         }
      }
      return;
   }
#endif

   /* Fallback: random visualizer when no playback engine (Phase 1 mode).
    * Gate random updates to 50ms to avoid CPU waste. */
   uint32_t now = SDL_GetTicks();
   if (now - m->viz_last_update < VIZ_UPDATE_MS)
      return;
   m->viz_last_update = now;

   if (m->playing && !m->paused) {
      for (int i = 0; i < VIZ_BAR_COUNT; i++) {
         m->viz_targets[i] = 0.15f + ((float)(rand() % 85)) / 100.0f;
      }
   } else {
      for (int i = 0; i < VIZ_BAR_COUNT; i++) {
         m->viz_targets[i] = 0.03f;
      }
   }
}

static void render_visualizer(ui_music_t *m, SDL_Renderer *r, int y) {
   update_visualizer(m);

   /* Smooth transitions (frame-rate independent via delta time).
    * Use viz_last_render (previous frame) for dt — NOT viz_last_update
    * which resets every VIZ_UPDATE_MS and causes sawtooth jitter. */
   uint32_t now = SDL_GetTicks();
   float dt = (now > m->viz_last_render && m->viz_last_render > 0)
                  ? (float)(now - m->viz_last_render) / 1000.0f
                  : 1.0f / 30.0f;
   m->viz_last_render = now;
   float alpha = 1.0f - powf(0.05f, dt); /* ~0.22 at 30fps — smooth rise/fall */
   for (int i = 0; i < VIZ_BAR_COUNT; i++) {
      float diff = m->viz_targets[i] - m->viz_bars[i];
      m->viz_bars[i] += diff * alpha;
   }

   int viz_x = m->panel_x + 16;
   int viz_w = m->panel_w - 32;

   /* Inset background */
   SDL_SetRenderDrawColor(r, INSET_BG_R, INSET_BG_G, INSET_BG_B, 255);
   SDL_Rect bg = { viz_x, y, viz_w, VIZ_HEIGHT };
   SDL_RenderFillRect(r, &bg);

   /* Bars */
   int bar_gap = 2;
   int total_gaps = (VIZ_BAR_COUNT - 1) * bar_gap;
   int bar_w = (viz_w - 16 - total_gaps) / VIZ_BAR_COUNT;
   if (bar_w < 2)
      bar_w = 2;
   int bar_start_x = viz_x + 8;

   for (int i = 0; i < VIZ_BAR_COUNT; i++) {
      int bx = bar_start_x + i * (bar_w + bar_gap);
      int bar_h = (int)(m->viz_bars[i] * (VIZ_HEIGHT - 12));
      if (bar_h < 2)
         bar_h = 2;
      int by = y + VIZ_HEIGHT - 6 - bar_h;

      /* Gradient: accent at top, darker at bottom */
      float t = (float)i / (float)VIZ_BAR_COUNT;
      uint8_t gr = (uint8_t)(ACCENT_R * (0.5f + 0.5f * t));
      uint8_t gg = (uint8_t)(ACCENT_G * (0.5f + 0.5f * t));
      uint8_t gb = (uint8_t)(ACCENT_B * (0.5f + 0.5f * t));

      SDL_SetRenderDrawColor(r, gr, gg, gb, 220);
      SDL_Rect bar = { bx, by, bar_w, bar_h };
      SDL_RenderFillRect(r, &bar);
   }
}

/* =============================================================================
 * Rendering: Now Playing Tab
 * ============================================================================= */

static void render_now_playing(ui_music_t *m, SDL_Renderer *r) {
   int y = m->panel_y + TAB_HEIGHT + 12;

   /* Visualizer */
   render_visualizer(m, r, y);
   y += VIZ_HEIGHT + 16;

   /* Track info (centered) */
   ensure_track_cached(m);

   if (m->title_tex) {
      int max_w = m->panel_w - 32; /* 16px padding each side */
      int draw_w = m->title_w < max_w ? m->title_w : max_w;
      int tx = m->panel_x + (m->panel_w - draw_w) / 2;
      SDL_Rect src = { 0, 0, draw_w, m->title_h };
      SDL_Rect dst = { tx, y, draw_w, m->title_h };
      SDL_RenderCopy(r, m->title_tex, &src, &dst);
      y += m->title_h + 4;
   } else {
      /* "No track" placeholder */
      if (m->slabel_tex[SLABEL_NO_TRACK]) {
         SDL_SetTextureColorMod(m->slabel_tex[SLABEL_NO_TRACK], COLOR_TEXT_SECONDARY_R,
                                COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B);
         int tw = m->slabel_w[SLABEL_NO_TRACK];
         int th = m->slabel_h[SLABEL_NO_TRACK];
         int tx = m->panel_x + (m->panel_w - tw) / 2;
         SDL_Rect dst = { tx, y, tw, th };
         SDL_RenderCopy(r, m->slabel_tex[SLABEL_NO_TRACK], NULL, &dst);
      }
      y += 26;
   }

   if (m->artist_tex) {
      int max_w = m->panel_w - 32;
      int draw_w = m->artist_w < max_w ? m->artist_w : max_w;
      int tx = m->panel_x + (m->panel_w - draw_w) / 2;
      SDL_Rect src = { 0, 0, draw_w, m->artist_h };
      SDL_Rect dst = { tx, y, draw_w, m->artist_h };
      SDL_RenderCopy(r, m->artist_tex, &src, &dst);
      y += m->artist_h + 2;
   }

   if (m->album_tex) {
      int max_w = m->panel_w - 32;
      int draw_w = m->album_w < max_w ? m->album_w : max_w;
      int tx = m->panel_x + (m->panel_w - draw_w) / 2;
      SDL_Rect src = { 0, 0, draw_w, m->album_h };
      SDL_Rect dst = { tx, y, draw_w, m->album_h };
      SDL_RenderCopy(r, m->album_tex, &src, &dst);
      y += m->album_h;
   }

   y += 16;

   /* Progress bar */
   if (m->label_font) {
      char time_cur[16], time_dur[16];
      format_time(m->position_sec, time_cur, sizeof(time_cur));
      format_time(m->duration_sec, time_dur, sizeof(time_dur));

      SDL_Color tc = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                       255 };

      /* Current time label */
      SDL_Surface *cur_s = TTF_RenderUTF8_Blended(m->label_font, time_cur, tc);
      if (cur_s) {
         SDL_Texture *tex = SDL_CreateTextureFromSurface(r, cur_s);
         SDL_Rect dst = { m->panel_x + 16, y, cur_s->w, cur_s->h };
         SDL_RenderCopy(r, tex, NULL, &dst);
         SDL_DestroyTexture(tex);

         /* Duration label */
         SDL_Surface *dur_s = TTF_RenderUTF8_Blended(m->label_font, time_dur, tc);
         if (dur_s) {
            SDL_Texture *dtex = SDL_CreateTextureFromSurface(r, dur_s);
            SDL_Rect ddst = { m->panel_x + m->panel_w - 16 - dur_s->w, y, dur_s->w, dur_s->h };
            SDL_RenderCopy(r, dtex, NULL, &ddst);
            SDL_DestroyTexture(dtex);
            SDL_FreeSurface(dur_s);
         }

         int bar_x = m->panel_x + 16 + cur_s->w + 8;
         int bar_w = m->panel_w - 32 - cur_s->w * 2 - 16;
         int bar_y = y + cur_s->h / 2 - PROGRESS_BAR_HEIGHT / 2;

         /* Store for touch handler */
         m->progress_bar_y = bar_y;
         m->progress_bar_x = bar_x;
         m->progress_bar_w = bar_w;

         /* Track background */
         SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                                255);
         SDL_Rect track_rect = { bar_x, bar_y, bar_w, PROGRESS_BAR_HEIGHT };
         SDL_RenderFillRect(r, &track_rect);

         /* Fill */
         float progress = (m->duration_sec > 0.0f) ? (m->position_sec / m->duration_sec) : 0.0f;
         if (progress > 1.0f)
            progress = 1.0f;
         if (progress < 0.0f)
            progress = 0.0f;
         int fill_w = (int)(bar_w * progress);
         SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 255);
         SDL_Rect fill_rect = { bar_x, bar_y, fill_w, PROGRESS_BAR_HEIGHT };
         SDL_RenderFillRect(r, &fill_rect);

         /* Thumb circle */
         int thumb_x = bar_x + fill_w;
         int thumb_y = bar_y + PROGRESS_BAR_HEIGHT / 2;
         int thumb_r = 6;
         for (int dy = -thumb_r; dy <= thumb_r; dy++) {
            int dx = (int)sqrtf((float)(thumb_r * thumb_r - dy * dy));
            SDL_RenderDrawLine(r, thumb_x - dx, thumb_y + dy, thumb_x + dx, thumb_y + dy);
         }

         y += cur_s->h;
         SDL_FreeSurface(cur_s);
      }
   }

   y += 20;

   /* Transport buttons */
   int center_x = m->panel_x + m->panel_w / 2;

   /* Prev */
   int prev_x = center_x - TRANSPORT_PLAY_SIZE / 2 - 20 - TRANSPORT_BTN_SIZE;
   int btn_y = y;
   m->transport_btn_y = btn_y; /* Store for touch handler */
   {
      bool pressed = (m->btn_pressed && m->btn_pressed_id == 0);
      uint8_t alpha = pressed ? 180 : 255;
      SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                             alpha);
      SDL_Rect br = { prev_x, btn_y, TRANSPORT_BTN_SIZE, TRANSPORT_BTN_SIZE };
      SDL_RenderFillRect(r, &br);

      if (m->transport_tex[0]) {
         SDL_SetTextureColorMod(m->transport_tex[0], COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G,
                                COLOR_TEXT_PRIMARY_B);
         SDL_SetTextureAlphaMod(m->transport_tex[0], alpha);
         SDL_Rect dst = { prev_x + (TRANSPORT_BTN_SIZE - m->transport_tex_w[0]) / 2,
                          btn_y + (TRANSPORT_BTN_SIZE - m->transport_tex_h[0]) / 2,
                          m->transport_tex_w[0], m->transport_tex_h[0] };
         SDL_RenderCopy(r, m->transport_tex[0], NULL, &dst);
      }
   }

   /* Play/Pause (accent circle) */
   int play_x = center_x - TRANSPORT_PLAY_SIZE / 2;
   {
      bool pressed = (m->btn_pressed && m->btn_pressed_id == 1);
      uint8_t ar = pressed ? (uint8_t)(ACCENT_R * 0.8f) : ACCENT_R;
      uint8_t ag = pressed ? (uint8_t)(ACCENT_G * 0.8f) : ACCENT_G;
      uint8_t ab = pressed ? (uint8_t)(ACCENT_B * 0.8f) : ACCENT_B;

      /* Filled circle */
      int cx = play_x + TRANSPORT_PLAY_SIZE / 2;
      int cy = btn_y + TRANSPORT_PLAY_SIZE / 2;
      int radius = TRANSPORT_PLAY_SIZE / 2;
      SDL_SetRenderDrawColor(r, ar, ag, ab, 255);
      for (int dy = -radius; dy <= radius; dy++) {
         int dx = (int)sqrtf((float)(radius * radius - dy * dy));
         SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
      }

      int idx = (m->playing && !m->paused) ? 2 : 1; /* 2=pause, 1=play */
      if (m->transport_tex[idx]) {
         SDL_SetTextureColorMod(m->transport_tex[idx], COLOR_BG_PRIMARY_R, COLOR_BG_PRIMARY_G,
                                COLOR_BG_PRIMARY_B);
         SDL_Rect dst = { cx - m->transport_tex_w[idx] / 2, cy - m->transport_tex_h[idx] / 2,
                          m->transport_tex_w[idx], m->transport_tex_h[idx] };
         SDL_RenderCopy(r, m->transport_tex[idx], NULL, &dst);
      }
   }

   /* Next */
   int next_x = center_x + TRANSPORT_PLAY_SIZE / 2 + 20;
   {
      bool pressed = (m->btn_pressed && m->btn_pressed_id == 2);
      uint8_t alpha = pressed ? 180 : 255;
      SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                             alpha);
      SDL_Rect br = { next_x, btn_y, TRANSPORT_BTN_SIZE, TRANSPORT_BTN_SIZE };
      SDL_RenderFillRect(r, &br);

      if (m->transport_tex[3]) {
         SDL_SetTextureColorMod(m->transport_tex[3], COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G,
                                COLOR_TEXT_PRIMARY_B);
         SDL_SetTextureAlphaMod(m->transport_tex[3], alpha);
         SDL_Rect dst = { next_x + (TRANSPORT_BTN_SIZE - m->transport_tex_w[3]) / 2,
                          btn_y + (TRANSPORT_BTN_SIZE - m->transport_tex_h[3]) / 2,
                          m->transport_tex_w[3], m->transport_tex_h[3] };
         SDL_RenderCopy(r, m->transport_tex[3], NULL, &dst);
      }
   }

   y += TRANSPORT_PLAY_SIZE + 16;

   /* Shuffle / Repeat toggles */
   int shuf_x = center_x - 80 - TOGGLE_BTN_SIZE / 2;
   int rep_x = center_x + 80 - TOGGLE_BTN_SIZE / 2;
   int tog_y = y;
   m->toggle_btn_y = tog_y; /* Store for touch handler */

   /* Shuffle button */
   {
      if (m->shuffle) {
         SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 200);
      } else {
         SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                                255);
      }
      SDL_Rect br = { shuf_x, tog_y, TOGGLE_BTN_SIZE, TOGGLE_BTN_SIZE };
      SDL_RenderFillRect(r, &br);

      if (m->shuffle_icon_tex) {
         if (m->shuffle) {
            SDL_SetTextureColorMod(m->shuffle_icon_tex, COLOR_BG_PRIMARY_R, COLOR_BG_PRIMARY_G,
                                   COLOR_BG_PRIMARY_B);
         } else {
            SDL_SetTextureColorMod(m->shuffle_icon_tex, COLOR_TEXT_SECONDARY_R,
                                   COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B);
         }
         SDL_Rect dst = { shuf_x + (TOGGLE_BTN_SIZE - TOGGLE_ICON_DIM) / 2,
                          tog_y + (TOGGLE_BTN_SIZE - TOGGLE_ICON_DIM) / 2, TOGGLE_ICON_DIM,
                          TOGGLE_ICON_DIM };
         SDL_RenderCopy(r, m->shuffle_icon_tex, NULL, &dst);
      }
   }

   /* Repeat button */
   {
      bool active = (m->repeat_mode > 0);
      if (active) {
         SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 200);
      } else {
         SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                                255);
      }
      SDL_Rect br = { rep_x, tog_y, TOGGLE_BTN_SIZE, TOGGLE_BTN_SIZE };
      SDL_RenderFillRect(r, &br);

      SDL_Texture *rep_tex = (m->repeat_mode == 2) ? m->repeat_one_icon_tex : m->repeat_icon_tex;
      if (rep_tex) {
         if (active) {
            SDL_SetTextureColorMod(rep_tex, COLOR_BG_PRIMARY_R, COLOR_BG_PRIMARY_G,
                                   COLOR_BG_PRIMARY_B);
         } else {
            SDL_SetTextureColorMod(rep_tex, COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                                   COLOR_TEXT_SECONDARY_B);
         }
         SDL_Rect dst = { rep_x + (TOGGLE_BTN_SIZE - TOGGLE_ICON_DIM) / 2,
                          tog_y + (TOGGLE_BTN_SIZE - TOGGLE_ICON_DIM) / 2, TOGGLE_ICON_DIM,
                          TOGGLE_ICON_DIM };
         SDL_RenderCopy(r, rep_tex, NULL, &dst);
      }
   }

   y += TOGGLE_BTN_SIZE + 12;

   /* Status line */
   if (m->label_font && m->source_format[0]) {
      char status[128];
      if (m->bitrate > 0) {
         snprintf(status, sizeof(status), "%s %dk \xC2\xB7 %dkbps %s", m->source_format,
                  m->source_rate / 1000, m->bitrate / 1000,
                  strcmp(m->bitrate_mode, "vbr") == 0 ? "VBR" : "CBR");
      } else {
         snprintf(status, sizeof(status), "%s %dk", m->source_format, m->source_rate / 1000);
      }

      SDL_Color c = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B, 180 };
      SDL_Surface *s = TTF_RenderUTF8_Blended(m->label_font, status, c);
      if (s) {
         SDL_Texture *tex = SDL_CreateTextureFromSurface(r, s);
         int tx = m->panel_x + (m->panel_w - s->w) / 2;
         SDL_Rect dst = { tx, y, s->w, s->h };
         SDL_RenderCopy(r, tex, NULL, &dst);
         SDL_DestroyTexture(tex);
         SDL_FreeSurface(s);
      }
   }
}

/* =============================================================================
 * Scroll Indicator
 * ============================================================================= */

/** @brief Render a scrollbar thumb that fades out 1.5s after last scroll */
static void render_scroll_indicator(SDL_Renderer *r,
                                    int x,
                                    int y,
                                    int w,
                                    int h,
                                    int scroll_offset,
                                    int total_height,
                                    uint32_t last_scroll_ms) {
   if (total_height <= h)
      return; /* No scrollbar needed */

   /* Fade: full opacity during scroll, fade out over 500ms after 1s idle */
   uint32_t elapsed = SDL_GetTicks() - last_scroll_ms;
   uint8_t alpha;
   if (elapsed < 1000) {
      alpha = 140;
   } else if (elapsed < 1500) {
      alpha = (uint8_t)(140 - 140 * (elapsed - 1000) / 500);
   } else {
      return; /* Fully faded */
   }

   int bar_x = x + w - 4;
   int bar_w = 4;

   /* Thumb proportional to viewport/total */
   int thumb_h = (h * h) / total_height;
   if (thumb_h < 20)
      thumb_h = 20;

   int max_scroll = total_height - h;
   float frac = (max_scroll > 0) ? (float)scroll_offset / (float)max_scroll : 0.0f;
   int thumb_y = y + (int)(frac * (h - thumb_h));

   SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
   SDL_SetRenderDrawColor(r, COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                          alpha);
   SDL_Rect thumb = { bar_x, thumb_y, bar_w, thumb_h };
   SDL_RenderFillRect(r, &thumb);
}

/* =============================================================================
 * Rendering: Queue Tab
 * ============================================================================= */

static void render_queue(ui_music_t *m, SDL_Renderer *r) {
   int y = m->panel_y + TAB_HEIGHT;
   int content_h = m->panel_h - TAB_HEIGHT;

   /* Header */
   int header_h = 44;
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B, 200);
   SDL_Rect hdr = { m->panel_x, y, m->panel_w, header_h };
   SDL_RenderFillRect(r, &hdr);

   if (m->label_font) {
      /* Title */
      char title[64];
      snprintf(title, sizeof(title), "PLAYBACK QUEUE (%d)", m->queue_count);
      SDL_Color tc = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B, 255 };
      SDL_Surface *s = TTF_RenderUTF8_Blended(m->label_font, title, tc);
      if (s) {
         SDL_Texture *tex = SDL_CreateTextureFromSurface(r, s);
         SDL_Rect dst = { m->panel_x + 16, y + (header_h - s->h) / 2, s->w, s->h };
         SDL_RenderCopy(r, tex, NULL, &dst);
         SDL_DestroyTexture(tex);
         SDL_FreeSurface(s);
      }

      /* Clear button */
      if (m->queue_count > 0 && m->slabel_tex[SLABEL_CLEAR_ALL]) {
         SDL_SetTextureColorMod(m->slabel_tex[SLABEL_CLEAR_ALL], COLOR_ERROR_R, COLOR_ERROR_G,
                                COLOR_ERROR_B);
         int cw = m->slabel_w[SLABEL_CLEAR_ALL];
         int ch = m->slabel_h[SLABEL_CLEAR_ALL];
         SDL_Rect cdst = { m->panel_x + m->panel_w - 16 - cw, y + (header_h - ch) / 2, cw, ch };
         SDL_RenderCopy(r, m->slabel_tex[SLABEL_CLEAR_ALL], NULL, &cdst);
      }
   }

   y += header_h;
   int list_h = content_h - header_h;

   /* Clip to list area */
   SDL_Rect clip = { m->panel_x, y, m->panel_w, list_h };
   SDL_RenderSetClipRect(r, &clip);

   m->total_list_height = m->queue_count * LIST_ROW_HEIGHT;
   int start_y = y - m->scroll_offset;

   for (int i = 0; i < m->queue_count; i++) {
      int row_y = start_y + i * LIST_ROW_HEIGHT;
      if (row_y + LIST_ROW_HEIGHT < y)
         continue;
      if (row_y > y + list_h)
         break;

      music_track_t *track = &m->queue[i];
      bool is_current = (i == m->queue_index);

      /* Row background */
      if (is_current) {
         SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 30);
         SDL_Rect row = { m->panel_x, row_y, m->panel_w, LIST_ROW_HEIGHT };
         SDL_RenderFillRect(r, &row);

         /* Left accent border */
         SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 255);
         SDL_Rect border = { m->panel_x, row_y, 3, LIST_ROW_HEIGHT };
         SDL_RenderFillRect(r, &border);
      }

      /* Row separator */
      SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B, 100);
      SDL_RenderDrawLine(r, m->panel_x + 16, row_y + LIST_ROW_HEIGHT - 1,
                         m->panel_x + m->panel_w - 16, row_y + LIST_ROW_HEIGHT - 1);

      if (m->label_font) {
         /* Index number */
         char idx_str[16];
         snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
         SDL_Color ic = { COLOR_TEXT_TERTIARY_R, COLOR_TEXT_TERTIARY_G, COLOR_TEXT_TERTIARY_B,
                          255 };
         SDL_Surface *is = TTF_RenderUTF8_Blended(m->label_font, idx_str, ic);
         if (is) {
            SDL_Texture *itex = SDL_CreateTextureFromSurface(r, is);
            SDL_Rect idst = { m->panel_x + 12, row_y + (LIST_ROW_HEIGHT - is->h) / 2, is->w,
                              is->h };
            SDL_RenderCopy(r, itex, NULL, &idst);
            SDL_DestroyTexture(itex);
            SDL_FreeSurface(is);
         }

         /* Duration (render first to compute text budget) */
         char dur[16];
         format_time((float)track->duration_sec, dur, sizeof(dur));
         SDL_Color dc = { COLOR_TEXT_TERTIARY_R, COLOR_TEXT_TERTIARY_G, COLOR_TEXT_TERTIARY_B,
                          255 };
         SDL_Surface *ds = TTF_RenderUTF8_Blended(m->label_font, dur, dc);
         int dur_w = ds ? ds->w : 50;
         int dur_right = m->panel_x + m->panel_w - 16;
         if (ds) {
            SDL_Texture *dtex = SDL_CreateTextureFromSurface(r, ds);
            SDL_Rect ddst = { dur_right - ds->w, row_y + (LIST_ROW_HEIGHT - ds->h) / 2, ds->w,
                              ds->h };
            SDL_RenderCopy(r, dtex, NULL, &ddst);
            SDL_DestroyTexture(dtex);
            SDL_FreeSurface(ds);
         }

         /* Title + Artist (vertically centered, truncated before duration) */
         int text_left = m->panel_x + 40;
         int max_w = dur_right - dur_w - 12 - text_left;
         if (max_w < 40)
            max_w = 40;

         SDL_Color tc = is_current ? (SDL_Color){ ACCENT_R, ACCENT_G, ACCENT_B, 255 }
                                   : (SDL_Color){ COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G,
                                                  COLOR_TEXT_PRIMARY_B, 255 };
         SDL_Surface *ts = TTF_RenderUTF8_Blended(m->label_font, track->title, tc);
         if (ts) {
            SDL_Texture *ttex = SDL_CreateTextureFromSurface(r, ts);
            int tw = ts->w < max_w ? ts->w : max_w;

            int block_h = ts->h + ts->h;
            int block_y = row_y + (LIST_ROW_HEIGHT - block_h) / 2;

            SDL_Rect src_r = { 0, 0, tw, ts->h };
            SDL_Rect tdst = { text_left, block_y, tw, ts->h };
            SDL_RenderCopy(r, ttex, &src_r, &tdst);
            SDL_DestroyTexture(ttex);

            /* Artist (below title) */
            SDL_Color ac = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                             255 };
            SDL_Surface *as = TTF_RenderUTF8_Blended(m->label_font, track->artist, ac);
            if (as) {
               SDL_Texture *atex = SDL_CreateTextureFromSurface(r, as);
               int aw = as->w < max_w ? as->w : max_w;
               SDL_Rect asrc = { 0, 0, aw, as->h };
               SDL_Rect adst = { text_left, block_y + ts->h, aw, as->h };
               SDL_RenderCopy(r, atex, &asrc, &adst);
               SDL_DestroyTexture(atex);
               SDL_FreeSurface(as);
            }

            SDL_FreeSurface(ts);
         }
      }
   }

   render_scroll_indicator(r, m->panel_x, clip.y, m->panel_w, list_h, m->scroll_offset,
                           m->total_list_height, m->last_scroll_ms);
   SDL_RenderSetClipRect(r, NULL);
}

/* =============================================================================
 * Rendering: Library Tab
 * ============================================================================= */

static void render_stat_box(ui_music_t *m,
                            SDL_Renderer *r,
                            int x,
                            int y,
                            int w,
                            int h,
                            int count,
                            const char *label) {
   /* Box background */
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B, 200);
   SDL_Rect box = { x, y, w, h };
   SDL_RenderFillRect(r, &box);

   /* Border */
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x15, COLOR_BG_TERTIARY_G + 0x15,
                          COLOR_BG_TERTIARY_B + 0x15, 255);
   SDL_RenderDrawRect(r, &box);

   if (!m->body_font || !m->label_font)
      return;

   /* Count number */
   char num[32];
   snprintf(num, sizeof(num), "%d", count);
   SDL_Color nc = { ACCENT_R, ACCENT_G, ACCENT_B, 255 };
   SDL_Surface *ns = TTF_RenderUTF8_Blended(m->body_font, num, nc);
   if (ns) {
      SDL_Texture *ntex = SDL_CreateTextureFromSurface(r, ns);
      SDL_Rect ndst = { x + (w - ns->w) / 2, y + h / 2 - ns->h - 2, ns->w, ns->h };
      SDL_RenderCopy(r, ntex, NULL, &ndst);
      SDL_DestroyTexture(ntex);
      SDL_FreeSurface(ns);
   }

   /* Label */
   SDL_Color lc = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B, 255 };
   SDL_Surface *ls = TTF_RenderUTF8_Blended(m->label_font, label, lc);
   if (ls) {
      SDL_Texture *ltex = SDL_CreateTextureFromSurface(r, ls);
      SDL_Rect ldst = { x + (w - ls->w) / 2, y + h / 2 + 4, ls->w, ls->h };
      SDL_RenderCopy(r, ltex, NULL, &ldst);
      SDL_DestroyTexture(ltex);
      SDL_FreeSurface(ls);
   }
}

static void render_library(ui_music_t *m, SDL_Renderer *r) {
   int y = m->panel_y + TAB_HEIGHT + 12;

   /* Stats grid (when browse_type == NONE) */
   if (m->browse_type == MUSIC_BROWSE_NONE) {
      int box_w = (m->panel_w - 48 - 16) / 3;
      int box_h = 80;
      int box_x = m->panel_x + 16;

      render_stat_box(m, r, box_x, y, box_w, box_h, m->stat_tracks, "Tracks");
      render_stat_box(m, r, box_x + box_w + 8, y, box_w, box_h, m->stat_artists, "Artists");
      render_stat_box(m, r, box_x + 2 * (box_w + 8), y, box_w, box_h, m->stat_albums, "Albums");

      y += box_h + 16;

      /* Hint text */
      if (m->slabel_tex[SLABEL_BROWSE_HINT]) {
         SDL_SetTextureColorMod(m->slabel_tex[SLABEL_BROWSE_HINT], COLOR_TEXT_TERTIARY_R,
                                COLOR_TEXT_TERTIARY_G, COLOR_TEXT_TERTIARY_B);
         int tw = m->slabel_w[SLABEL_BROWSE_HINT];
         int th = m->slabel_h[SLABEL_BROWSE_HINT];
         int tx = m->panel_x + (m->panel_w - tw) / 2;
         SDL_Rect dst = { tx, y, tw, th };
         SDL_RenderCopy(r, m->slabel_tex[SLABEL_BROWSE_HINT], NULL, &dst);
      }
      return;
   }

   /* Browse header with back button */
   int header_h = 44;
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B, 200);
   SDL_Rect hdr = { m->panel_x, y, m->panel_w, header_h };
   SDL_RenderFillRect(r, &hdr);

   if (m->label_font) {
      /* Back arrow */
      if (m->slabel_tex[SLABEL_BACK]) {
         SDL_SetTextureColorMod(m->slabel_tex[SLABEL_BACK], ACCENT_R, ACCENT_G, ACCENT_B);
         int bw = m->slabel_w[SLABEL_BACK];
         int bh = m->slabel_h[SLABEL_BACK];
         SDL_Rect bdst = { m->panel_x + 12, y + (header_h - bh) / 2, bw, bh };
         SDL_RenderCopy(r, m->slabel_tex[SLABEL_BACK], NULL, &bdst);
      }

      /* Browse type label */
      const char *type_label = "";
      switch (m->browse_type) {
         case MUSIC_BROWSE_TRACKS:
            type_label = "All Tracks";
            break;
         case MUSIC_BROWSE_ARTISTS:
            type_label = "Artists";
            break;
         case MUSIC_BROWSE_ALBUMS:
            type_label = "Albums";
            break;
         case MUSIC_BROWSE_BY_ARTIST:
            type_label = "By Artist";
            break;
         case MUSIC_BROWSE_BY_ALBUM:
            type_label = "By Album";
            break;
         default:
            break;
      }
      SDL_Color tc = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B, 255 };
      SDL_Surface *ts = TTF_RenderUTF8_Blended(m->label_font, type_label, tc);
      if (ts) {
         SDL_Texture *ttex = SDL_CreateTextureFromSurface(r, ts);
         SDL_Rect tdst = { m->panel_x + m->panel_w - 16 - ts->w, y + (header_h - ts->h) / 2, ts->w,
                           ts->h };
         SDL_RenderCopy(r, ttex, NULL, &tdst);
         SDL_DestroyTexture(ttex);
         SDL_FreeSurface(ts);
      }
   }

   y += header_h;
   int list_h = m->panel_h - TAB_HEIGHT - 12 - header_h;

   SDL_Rect clip = { m->panel_x, y, m->panel_w, list_h };
   SDL_RenderSetClipRect(r, &clip);

   /* Browse items (artists/albums) */
   if (m->browse_type == MUSIC_BROWSE_ARTISTS || m->browse_type == MUSIC_BROWSE_ALBUMS) {
      m->total_list_height = m->browse_count * LIST_ROW_HEIGHT;
      int start_y = y - m->scroll_offset;

      for (int i = 0; i < m->browse_count; i++) {
         int row_y = start_y + i * LIST_ROW_HEIGHT;
         if (row_y + LIST_ROW_HEIGHT < y || row_y > y + list_h)
            continue;

         music_browse_item_t *item = &m->browse_items[i];

         /* Separator */
         SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                                100);
         SDL_RenderDrawLine(r, m->panel_x + 16, row_y + LIST_ROW_HEIGHT - 1,
                            m->panel_x + m->panel_w - 16, row_y + LIST_ROW_HEIGHT - 1);

         if (m->label_font) {
            /* "+" button (add all tracks by this artist/album) */
            int add_x = m->panel_x + m->panel_w - 16 - ADD_BTN_SIZE;
            int add_y = row_y + (LIST_ROW_HEIGHT - ADD_BTN_SIZE) / 2;

            bool flash = (m->add_flash_row == i && SDL_GetTicks() - m->add_flash_ms < 300);
            if (flash) {
               SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 200);
            } else {
               SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x10, COLOR_BG_TERTIARY_G + 0x10,
                                      COLOR_BG_TERTIARY_B + 0x10, 255);
            }
            SDL_Rect abtn = { add_x, add_y, ADD_BTN_SIZE, ADD_BTN_SIZE };
            SDL_RenderFillRect(r, &abtn);

            if (m->slabel_tex[SLABEL_PLUS]) {
               if (flash) {
                  SDL_SetTextureColorMod(m->slabel_tex[SLABEL_PLUS], COLOR_BG_PRIMARY_R,
                                         COLOR_BG_PRIMARY_G, COLOR_BG_PRIMARY_B);
               } else {
                  SDL_SetTextureColorMod(m->slabel_tex[SLABEL_PLUS], ACCENT_R, ACCENT_G, ACCENT_B);
               }
               int pw = m->slabel_w[SLABEL_PLUS];
               int ph = m->slabel_h[SLABEL_PLUS];
               SDL_Rect pdst = { add_x + (ADD_BTN_SIZE - pw) / 2, add_y + (ADD_BTN_SIZE - ph) / 2,
                                 pw, ph };
               SDL_RenderCopy(r, m->slabel_tex[SLABEL_PLUS], NULL, &pdst);
            }

            /* Name + track count (vertically centered, truncated before "+" btn) */
            int text_left = m->panel_x + 16;
            int max_w = add_x - 8 - text_left;
            if (max_w < 40)
               max_w = 40;

            SDL_Color nc = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B,
                             255 };
            SDL_Surface *ns = TTF_RenderUTF8_Blended(m->label_font, item->name, nc);
            if (ns) {
               SDL_Texture *ntex = SDL_CreateTextureFromSurface(r, ns);
               int nw = ns->w < max_w ? ns->w : max_w;

               int block_h = ns->h + ns->h;
               int block_y = row_y + (LIST_ROW_HEIGHT - block_h) / 2;

               SDL_Rect nsrc = { 0, 0, nw, ns->h };
               SDL_Rect ndst = { text_left, block_y, nw, ns->h };
               SDL_RenderCopy(r, ntex, &nsrc, &ndst);
               SDL_DestroyTexture(ntex);

               /* Track count */
               char sub[32];
               snprintf(sub, sizeof(sub), "%d tracks", item->track_count);
               SDL_Color sc = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                                COLOR_TEXT_SECONDARY_B, 255 };
               SDL_Surface *ss = TTF_RenderUTF8_Blended(m->label_font, sub, sc);
               if (ss) {
                  SDL_Texture *stex = SDL_CreateTextureFromSurface(r, ss);
                  int sw = ss->w < max_w ? ss->w : max_w;
                  SDL_Rect ssrc = { 0, 0, sw, ss->h };
                  SDL_Rect sdst = { text_left, block_y + ns->h, sw, ss->h };
                  SDL_RenderCopy(r, stex, &ssrc, &sdst);
                  SDL_DestroyTexture(stex);
                  SDL_FreeSurface(ss);
               }

               SDL_FreeSurface(ns);
            }
         }
      }
   }

   /* Track lists (all tracks, by artist, by album) */
   if (m->browse_type == MUSIC_BROWSE_TRACKS || m->browse_type == MUSIC_BROWSE_BY_ARTIST ||
       m->browse_type == MUSIC_BROWSE_BY_ALBUM) {
      m->total_list_height = m->browse_track_count * LIST_ROW_HEIGHT;
      int start_y = y - m->scroll_offset;

      for (int i = 0; i < m->browse_track_count; i++) {
         int row_y = start_y + i * LIST_ROW_HEIGHT;
         if (row_y + LIST_ROW_HEIGHT < y || row_y > y + list_h)
            continue;

         music_track_t *track = &m->browse_tracks[i];

         /* Separator */
         SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                                100);
         SDL_RenderDrawLine(r, m->panel_x + 16, row_y + LIST_ROW_HEIGHT - 1,
                            m->panel_x + m->panel_w - 16, row_y + LIST_ROW_HEIGHT - 1);

         if (m->label_font) {
            /* "+" button (draw first so we know right-edge budget) */
            int add_x = m->panel_x + m->panel_w - 16 - ADD_BTN_SIZE;
            int add_y = row_y + (LIST_ROW_HEIGHT - ADD_BTN_SIZE) / 2;

            /* Flash feedback: accent bg briefly after add */
            bool flash = (m->add_flash_row == i && SDL_GetTicks() - m->add_flash_ms < 300);
            if (flash) {
               SDL_SetRenderDrawColor(r, ACCENT_R, ACCENT_G, ACCENT_B, 200);
            } else {
               SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x10, COLOR_BG_TERTIARY_G + 0x10,
                                      COLOR_BG_TERTIARY_B + 0x10, 255);
            }
            SDL_Rect abtn = { add_x, add_y, ADD_BTN_SIZE, ADD_BTN_SIZE };
            SDL_RenderFillRect(r, &abtn);

            if (m->slabel_tex[SLABEL_PLUS]) {
               if (flash) {
                  SDL_SetTextureColorMod(m->slabel_tex[SLABEL_PLUS], COLOR_BG_PRIMARY_R,
                                         COLOR_BG_PRIMARY_G, COLOR_BG_PRIMARY_B);
               } else {
                  SDL_SetTextureColorMod(m->slabel_tex[SLABEL_PLUS], ACCENT_R, ACCENT_G, ACCENT_B);
               }
               int pw = m->slabel_w[SLABEL_PLUS];
               int ph = m->slabel_h[SLABEL_PLUS];
               SDL_Rect pdst = { add_x + (ADD_BTN_SIZE - pw) / 2, add_y + (ADD_BTN_SIZE - ph) / 2,
                                 pw, ph };
               SDL_RenderCopy(r, m->slabel_tex[SLABEL_PLUS], NULL, &pdst);
            }

            /* Duration (right-aligned, left of "+" button) */
            char dur[16];
            format_time((float)track->duration_sec, dur, sizeof(dur));
            SDL_Color dc = { COLOR_TEXT_TERTIARY_R, COLOR_TEXT_TERTIARY_G, COLOR_TEXT_TERTIARY_B,
                             255 };
            int dur_right = add_x - 8; /* right edge of duration text */
            SDL_Surface *ds = TTF_RenderUTF8_Blended(m->label_font, dur, dc);
            int dur_w = ds ? ds->w : 50; /* estimate if render fails */
            if (ds) {
               SDL_Texture *dtex = SDL_CreateTextureFromSurface(r, ds);
               SDL_Rect ddst = { dur_right - ds->w, row_y + (LIST_ROW_HEIGHT - ds->h) / 2, ds->w,
                                 ds->h };
               SDL_RenderCopy(r, dtex, NULL, &ddst);
               SDL_DestroyTexture(dtex);
               SDL_FreeSurface(ds);
            }

            /* Title + Artist (vertically centered, truncated before duration) */
            int text_left = m->panel_x + 16;
            int max_w = dur_right - dur_w - 12 - text_left;
            if (max_w < 40)
               max_w = 40;

            SDL_Color tc = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B,
                             255 };
            SDL_Surface *ts = TTF_RenderUTF8_Blended(m->label_font, track->title, tc);
            if (ts) {
               SDL_Texture *ttex = SDL_CreateTextureFromSurface(r, ts);
               int tw = ts->w < max_w ? ts->w : max_w;

               /* Vertically center the two-line block (title + artist) in the row */
               int block_h = ts->h + ts->h; /* title + artist (same font) */
               int block_y = row_y + (LIST_ROW_HEIGHT - block_h) / 2;

               SDL_Rect tsrc = { 0, 0, tw, ts->h };
               SDL_Rect tdst = { text_left, block_y, tw, ts->h };
               SDL_RenderCopy(r, ttex, &tsrc, &tdst);
               SDL_DestroyTexture(ttex);

               /* Artist - Album subtitle line */
               char subtitle[540];
               bool has_artist = track->artist[0] != '\0';
               bool has_album = track->album[0] != '\0';
               if (has_artist && has_album)
                  snprintf(subtitle, sizeof(subtitle), "%s - %s", track->artist, track->album);
               else if (has_artist)
                  snprintf(subtitle, sizeof(subtitle), "%s", track->artist);
               else if (has_album)
                  snprintf(subtitle, sizeof(subtitle), "%s", track->album);
               else
                  snprintf(subtitle, sizeof(subtitle), "Unknown");

               SDL_Color ac = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                                COLOR_TEXT_SECONDARY_B, 255 };
               SDL_Surface *as = TTF_RenderUTF8_Blended(m->label_font, subtitle, ac);
               if (as) {
                  SDL_Texture *atex = SDL_CreateTextureFromSurface(r, as);
                  int aw = as->w < max_w ? as->w : max_w;
                  SDL_Rect asrc = { 0, 0, aw, as->h };
                  SDL_Rect adst = { text_left, block_y + ts->h, aw, as->h };
                  SDL_RenderCopy(r, atex, &asrc, &adst);
                  SDL_DestroyTexture(atex);
                  SDL_FreeSurface(as);
               }

               SDL_FreeSurface(ts);
            }
         }
      }
   }

   render_scroll_indicator(r, m->panel_x, clip.y, m->panel_w, list_h, m->scroll_offset,
                           m->total_list_height, m->last_scroll_ms);
   SDL_RenderSetClipRect(r, NULL);
}

/* =============================================================================
 * Public API: Lifecycle
 * ============================================================================= */

int ui_music_init(ui_music_t *m,
                  SDL_Renderer *renderer,
                  int x,
                  int y,
                  int w,
                  int h,
                  const char *font_dir) {
   if (!m)
      return 1;

   memset(m, 0, sizeof(*m));
   pthread_mutex_init(&m->mutex, NULL);

   m->renderer = renderer;
   m->panel_x = x;
   m->panel_y = y;
   m->panel_w = w;
   m->panel_h = h;
   m->active_tab = MUSIC_TAB_PLAYING;
   m->add_flash_row = -1;

   /* Allocate browse buffers (supports pagination) */
   m->browse_tracks_cap = 500;
   m->browse_tracks = calloc(m->browse_tracks_cap, sizeof(music_track_t));
   if (!m->browse_tracks) {
      LOG_ERROR("Music panel: failed to allocate browse tracks buffer");
      return 1;
   }

   m->browse_items_cap = 500;
   m->browse_items = calloc(m->browse_items_cap, sizeof(music_browse_item_t));
   if (!m->browse_items) {
      LOG_ERROR("Music panel: failed to allocate browse items buffer");
      free(m->browse_tracks);
      m->browse_tracks = NULL;
      return 1;
   }

   m->label_font = load_font(font_dir, "IBMPlexMono-Regular.ttf", FALLBACK_MONO_FONT,
                             LABEL_FONT_SIZE);
   m->body_font = load_font(font_dir, "SourceSans3-Medium.ttf", FALLBACK_BODY_FONT, BODY_FONT_SIZE);

   if (!m->label_font)
      LOG_WARNING("Music panel: failed to load label font");
   if (!m->body_font)
      LOG_WARNING("Music panel: failed to load body font");

   return 0;
}

void ui_music_cleanup(ui_music_t *m) {
   if (!m)
      return;

   invalidate_track_cache(m);

   /* Destroy static texture caches */
   for (int i = 0; i < 3; i++) {
      if (m->tab_tex[i]) {
         SDL_DestroyTexture(m->tab_tex[i]);
         m->tab_tex[i] = NULL;
      }
   }
   for (int i = 0; i < 4; i++) {
      if (m->transport_tex[i]) {
         SDL_DestroyTexture(m->transport_tex[i]);
         m->transport_tex[i] = NULL;
      }
   }
   if (m->shuffle_icon_tex) {
      SDL_DestroyTexture(m->shuffle_icon_tex);
      m->shuffle_icon_tex = NULL;
   }
   if (m->repeat_icon_tex) {
      SDL_DestroyTexture(m->repeat_icon_tex);
      m->repeat_icon_tex = NULL;
   }
   if (m->repeat_one_icon_tex) {
      SDL_DestroyTexture(m->repeat_one_icon_tex);
      m->repeat_one_icon_tex = NULL;
   }
   for (int i = 0; i < MUSIC_SLABEL_COUNT; i++) {
      if (m->slabel_tex[i]) {
         SDL_DestroyTexture(m->slabel_tex[i]);
         m->slabel_tex[i] = NULL;
      }
   }
   m->static_cache_ready = false;

   if (m->label_font) {
      TTF_CloseFont(m->label_font);
      m->label_font = NULL;
   }
   if (m->body_font) {
      TTF_CloseFont(m->body_font);
      m->body_font = NULL;
   }

   free(m->browse_tracks);
   m->browse_tracks = NULL;
   m->browse_tracks_cap = 0;

   free(m->browse_items);
   m->browse_items = NULL;
   m->browse_items_cap = 0;

   pthread_mutex_destroy(&m->mutex);
}

/* =============================================================================
 * Public API: Rendering
 * ============================================================================= */

void ui_music_render(ui_music_t *m, SDL_Renderer *renderer) {
   if (!m || !renderer)
      return;

   /* Panel background */
   SDL_SetRenderDrawColor(renderer, COLOR_BG_SECONDARY_R, COLOR_BG_SECONDARY_G,
                          COLOR_BG_SECONDARY_B, 255);
   SDL_Rect bg = { m->panel_x, m->panel_y, m->panel_w, m->panel_h };
   SDL_RenderFillRect(renderer, &bg);

   /* Left edge highlight */
   SDL_SetRenderDrawColor(renderer, COLOR_BG_TERTIARY_R + 0x20, COLOR_BG_TERTIARY_G + 0x20,
                          COLOR_BG_TERTIARY_B + 0x20, 255);
   SDL_RenderDrawLine(renderer, m->panel_x, m->panel_y, m->panel_x, m->panel_y + m->panel_h);

   /* Lock for entire render pass — WS callbacks modify state from another thread */
   pthread_mutex_lock(&m->mutex);

   /* Tabs */
   render_tabs(m, renderer);

   switch (m->active_tab) {
      case MUSIC_TAB_PLAYING:
         render_now_playing(m, renderer);
         break;
      case MUSIC_TAB_QUEUE:
         render_queue(m, renderer);
         break;
      case MUSIC_TAB_LIBRARY:
         render_library(m, renderer);
         break;
   }

   pthread_mutex_unlock(&m->mutex);
}

/* =============================================================================
 * Public API: Touch Handling
 * ============================================================================= */

void ui_music_handle_finger_down(ui_music_t *m, int x, int y) {
   if (!m)
      return;

   /* Check transport button press states for visual feedback */
   if (m->active_tab == MUSIC_TAB_PLAYING) {
      int center_x = m->panel_x + m->panel_w / 2;
      int btn_y = m->transport_btn_y; /* Set during render */

      int prev_x = center_x - TRANSPORT_PLAY_SIZE / 2 - 20 - TRANSPORT_BTN_SIZE;
      int play_x = center_x - TRANSPORT_PLAY_SIZE / 2;
      int next_x = center_x + TRANSPORT_PLAY_SIZE / 2 + 20;

      if (x >= prev_x && x < prev_x + TRANSPORT_BTN_SIZE && y >= btn_y &&
          y < btn_y + TRANSPORT_BTN_SIZE) {
         m->btn_pressed = true;
         m->btn_pressed_id = 0;
      } else if (x >= play_x && x < play_x + TRANSPORT_PLAY_SIZE && y >= btn_y &&
                 y < btn_y + TRANSPORT_PLAY_SIZE) {
         m->btn_pressed = true;
         m->btn_pressed_id = 1;
      } else if (x >= next_x && x < next_x + TRANSPORT_BTN_SIZE && y >= btn_y &&
                 y < btn_y + TRANSPORT_BTN_SIZE) {
         m->btn_pressed = true;
         m->btn_pressed_id = 2;
      }

      /* Check if finger landed on progress bar (start drag-to-seek) */
      if (!m->btn_pressed && m->progress_bar_w > 0 && m->duration_sec > 0.1f) {
         int pb_y = m->progress_bar_y;
         if (y >= pb_y - 20 && y <= pb_y + PROGRESS_BAR_HEIGHT + 20 && x >= m->progress_bar_x &&
             x <= m->progress_bar_x + m->progress_bar_w) {
            m->seeking = true;
         }
      }
   }
}

void ui_music_handle_finger_up(ui_music_t *m) {
   if (!m)
      return;
   m->btn_pressed = false;
   if (m->seeking) {
      m->seeking = false;
      /* Final seek position was already sent during motion */
   }
}

void ui_music_handle_finger_motion(ui_music_t *m, int x, int y) {
   if (!m || !m->seeking)
      return;

   (void)y; /* Only horizontal position matters for seek */
   if (m->progress_bar_w > 0 && m->duration_sec > 0.1f) {
      float frac = (float)(x - m->progress_bar_x) / (float)m->progress_bar_w;
      if (frac < 0.0f)
         frac = 0.0f;
      if (frac > 1.0f)
         frac = 1.0f;
      float seek_pos = frac * m->duration_sec;
      /* Update local position for immediate visual feedback */
      m->position_sec = seek_pos;
      /* Send seek to daemon */
      if (m->ws)
         ws_client_send_music_seek(m->ws, seek_pos);
   }
}

bool ui_music_handle_tap(ui_music_t *m, int x, int y) {
   if (!m)
      return false;

   /* Debounce */
   uint32_t now = SDL_GetTicks();
   if (now - m->last_tap_ms < TAP_DEBOUNCE_MS)
      return true; /* Consumed but ignored */
   m->last_tap_ms = now;

   /* Check if tap is within panel bounds */
   if (x < m->panel_x || x >= m->panel_x + m->panel_w || y < m->panel_y ||
       y >= m->panel_y + m->panel_h) {
      return false;
   }

   /* Tab selection */
   if (y < m->panel_y + TAB_HEIGHT) {
      int tab_w = m->panel_w / 3;
      int tab_idx = (x - m->panel_x) / tab_w;
      if (tab_idx >= 0 && tab_idx < 3) {
         m->active_tab = (music_tab_t)tab_idx;
         m->scroll_offset = 0;

         /* Request data for the selected tab */
         if (m->active_tab == MUSIC_TAB_QUEUE && m->ws) {
            ws_client_send_music_queue(m->ws, "list", NULL, -1);
         } else if (m->active_tab == MUSIC_TAB_LIBRARY && m->ws) {
            if (m->browse_type == MUSIC_BROWSE_NONE) {
               ws_client_send_music_library(m->ws, "stats", NULL);
            }
         }
      }
      return true;
   }

   /* Tab-specific handling */
   pthread_mutex_lock(&m->mutex);

   bool handled = false;

   if (m->active_tab == MUSIC_TAB_PLAYING) {
      int center_x = m->panel_x + m->panel_w / 2;
      int btn_y = m->transport_btn_y; /* Set during render */

      int prev_x = center_x - TRANSPORT_PLAY_SIZE / 2 - 20 - TRANSPORT_BTN_SIZE;
      int next_x = center_x + TRANSPORT_PLAY_SIZE / 2 + 20;
      /* Expand hit targets into 20px gaps to eliminate dead zones.
       * prev: left edge to midpoint of prev-play gap
       * play: midpoint of prev-play gap to midpoint of play-next gap
       * next: midpoint of play-next gap to right edge */
      int prev_right = center_x - TRANSPORT_PLAY_SIZE / 2 - 10;
      int next_left = center_x + TRANSPORT_PLAY_SIZE / 2 + 10;

      /* Prev button (expanded) */
      if (x >= prev_x && x < prev_right && y >= btn_y && y < btn_y + TRANSPORT_PLAY_SIZE) {
#ifdef HAVE_OPUS
         if (m->music_pb)
            music_playback_flush(m->music_pb);
#endif
         if (m->ws) {
            if (m->shuffle && m->queue_count > 1) {
               /* Shuffle: pick random track (excluding current) */
               int new_index;
               do {
                  new_index = rand() % m->queue_count;
               } while (new_index == m->queue_index && m->queue_count > 1);
               char idx_str[16];
               snprintf(idx_str, sizeof(idx_str), "%d", new_index);
               ws_client_send_music_control(m->ws, "play_index", idx_str);
            } else {
               ws_client_send_music_control(m->ws, "previous", NULL);
            }
         }
         handled = true;
      }
      /* Play/Pause button (expanded) */
      else if (x >= prev_right && x < next_left && y >= btn_y && y < btn_y + TRANSPORT_PLAY_SIZE) {
         if (m->ws) {
            if (m->playing && !m->paused) {
               ws_client_send_music_control(m->ws, "pause", NULL);
               /* Optimistic update: prevent rapid tap from sending conflicting command */
               m->paused = true;
            } else if (m->paused) {
               ws_client_send_music_control(m->ws, "play", NULL);
               m->paused = false;
            } else if (m->queue_count > 0) {
               /* Not playing, not paused, but queue has items — start from current index */
               char idx_str[16];
               snprintf(idx_str, sizeof(idx_str), "%d", m->queue_index);
               ws_client_send_music_control(m->ws, "play_index", idx_str);
               m->playing = true;
            }
         }
         handled = true;
      }
      /* Next button (expanded) */
      else if (x >= next_left && x < next_x + TRANSPORT_BTN_SIZE && y >= btn_y &&
               y < btn_y + TRANSPORT_PLAY_SIZE) {
#ifdef HAVE_OPUS
         if (m->music_pb)
            music_playback_flush(m->music_pb);
#endif
         if (m->ws) {
            if (m->shuffle && m->queue_count > 1) {
               /* Shuffle: pick random track (excluding current) */
               int new_index;
               do {
                  new_index = rand() % m->queue_count;
               } while (new_index == m->queue_index && m->queue_count > 1);
               char idx_str[16];
               snprintf(idx_str, sizeof(idx_str), "%d", new_index);
               ws_client_send_music_control(m->ws, "play_index", idx_str);
            } else {
               ws_client_send_music_control(m->ws, "next", NULL);
            }
         }
         handled = true;
      }

      /* Shuffle / Repeat */
      int tog_y = m->toggle_btn_y; /* Set during render */
      int shuf_x = center_x - 80 - TOGGLE_BTN_SIZE / 2;
      int rep_x = center_x + 80 - TOGGLE_BTN_SIZE / 2;

      if (x >= shuf_x && x < shuf_x + TOGGLE_BTN_SIZE && y >= tog_y &&
          y < tog_y + TOGGLE_BTN_SIZE) {
         m->shuffle = !m->shuffle;
         handled = true;
      } else if (x >= rep_x && x < rep_x + TOGGLE_BTN_SIZE && y >= tog_y &&
                 y < tog_y + TOGGLE_BTN_SIZE) {
         m->repeat_mode = (m->repeat_mode + 1) % 3;
         handled = true;
      }

      /* Progress bar seek (20px vertical padding for fat-finger tolerance) */
      if (!handled && m->progress_bar_w > 0 && m->duration_sec > 0.1f) {
         int pb_y = m->progress_bar_y;
         if (y >= pb_y - 20 && y <= pb_y + PROGRESS_BAR_HEIGHT + 20 && x >= m->progress_bar_x &&
             x <= m->progress_bar_x + m->progress_bar_w) {
            float frac = (float)(x - m->progress_bar_x) / (float)m->progress_bar_w;
            if (frac < 0.0f)
               frac = 0.0f;
            if (frac > 1.0f)
               frac = 1.0f;
            float seek_pos = frac * m->duration_sec;
#ifdef HAVE_OPUS
            if (m->music_pb)
               music_playback_flush(m->music_pb);
#endif
            if (m->ws)
               ws_client_send_music_seek(m->ws, seek_pos);
            handled = true;
         }
      }

      if (!handled)
         handled = true; /* Consume all taps in Playing tab */

   } else if (m->active_tab == MUSIC_TAB_QUEUE) {
      int header_h = 44;
      int list_y = m->panel_y + TAB_HEIGHT + header_h;

      /* Clear All button */
      if (y >= m->panel_y + TAB_HEIGHT && y < list_y && x > m->panel_x + m->panel_w / 2) {
         if (m->ws && m->queue_count > 0) {
            ws_client_send_music_queue(m->ws, "clear", NULL, -1);
         }
         handled = true;
      }

      /* Queue item tap - play that track */
      if (y >= list_y && !handled) {
         int row_idx = (y - list_y + m->scroll_offset) / LIST_ROW_HEIGHT;
         if (row_idx >= 0 && row_idx < m->queue_count && m->ws) {
#ifdef HAVE_OPUS
            if (m->music_pb)
               music_playback_flush(m->music_pb);
#endif
            /* Send play_index command */
            char idx_str[16];
            snprintf(idx_str, sizeof(idx_str), "%d", row_idx);
            ws_client_send_music_control(m->ws, "play_index", idx_str);
         }
         handled = true;
      }

   } else if (m->active_tab == MUSIC_TAB_LIBRARY) {
      int stats_y = m->panel_y + TAB_HEIGHT + 12;

      if (m->browse_type == MUSIC_BROWSE_NONE) {
         /* Stats grid tap */
         int box_w = (m->panel_w - 48 - 16) / 3;
         int box_h = 80;
         int box_x = m->panel_x + 16;

         if (y >= stats_y && y < stats_y + box_h && m->ws) {
            if (x >= box_x && x < box_x + box_w) {
               ws_client_send_music_library_paged(m->ws, "tracks", NULL, 0, MUSIC_MAX_RESULTS);
            } else if (x >= box_x + box_w + 8 && x < box_x + 2 * box_w + 8) {
               ws_client_send_music_library_paged(m->ws, "artists", NULL, 0, MUSIC_MAX_RESULTS);
            } else if (x >= box_x + 2 * (box_w + 8) && x < box_x + 3 * box_w + 16) {
               ws_client_send_music_library_paged(m->ws, "albums", NULL, 0, MUSIC_MAX_RESULTS);
            }
         }
         handled = true;
      } else {
         /* Back button */
         int header_y = stats_y;
         int header_h = 44;
         if (y >= header_y && y < header_y + header_h && x < m->panel_x + 100) {
            m->browse_type = MUSIC_BROWSE_NONE;
            m->scroll_offset = 0;
            if (m->ws)
               ws_client_send_music_library(m->ws, "stats", NULL);
            handled = true;
         }

         /* Browse list items */
         int list_y = header_y + header_h;
         if (y >= list_y && !handled) {
            int row_idx = (y - list_y + m->scroll_offset) / LIST_ROW_HEIGHT;

            if (m->browse_type == MUSIC_BROWSE_ARTISTS && row_idx >= 0 &&
                row_idx < m->browse_count && m->ws) {
               int add_x = m->panel_x + m->panel_w - 16 - ADD_BTN_SIZE;
               if (x >= add_x) {
                  /* "+" tapped — add all tracks by this artist */
                  ws_client_send_music_queue_bulk(m->ws, "add_artist",
                                                  m->browse_items[row_idx].name);
                  m->add_flash_row = row_idx;
                  m->add_flash_ms = SDL_GetTicks();
               } else {
                  /* Row tapped — drill into artist tracks */
                  ws_client_send_music_library(m->ws, "tracks_by_artist",
                                               m->browse_items[row_idx].name);
                  m->scroll_offset = 0;
               }
            } else if (m->browse_type == MUSIC_BROWSE_ALBUMS && row_idx >= 0 &&
                       row_idx < m->browse_count && m->ws) {
               int add_x = m->panel_x + m->panel_w - 16 - ADD_BTN_SIZE;
               if (x >= add_x) {
                  /* "+" tapped — add all tracks from this album */
                  ws_client_send_music_queue_bulk(m->ws, "add_album",
                                                  m->browse_items[row_idx].name);
                  m->add_flash_row = row_idx;
                  m->add_flash_ms = SDL_GetTicks();
               } else {
                  /* Row tapped — drill into album tracks */
                  ws_client_send_music_library(m->ws, "tracks_by_album",
                                               m->browse_items[row_idx].name);
                  m->scroll_offset = 0;
               }
            } else if ((m->browse_type == MUSIC_BROWSE_TRACKS ||
                        m->browse_type == MUSIC_BROWSE_BY_ARTIST ||
                        m->browse_type == MUSIC_BROWSE_BY_ALBUM) &&
                       row_idx >= 0 && row_idx < m->browse_track_count) {
               /* Check if "+" button was tapped */
               int add_x = m->panel_x + m->panel_w - 16 - ADD_BTN_SIZE;
               if (x >= add_x && m->ws) {
                  ws_client_send_music_queue(m->ws, "add", m->browse_tracks[row_idx].path, -1);
                  m->add_flash_row = row_idx;
                  m->add_flash_ms = SDL_GetTicks();
               }
            }
            handled = true;
         }
      }
      if (!handled)
         handled = true;
   }

   pthread_mutex_unlock(&m->mutex);
   return handled;
}

void ui_music_scroll(ui_music_t *m, int dy) {
   if (!m)
      return;

   /* Only scroll in queue/library tabs */
   if (m->active_tab == MUSIC_TAB_PLAYING)
      return;

   m->scroll_offset -= dy;
   m->last_scroll_ms = SDL_GetTicks();
   if (m->scroll_offset < 0)
      m->scroll_offset = 0;

   int max_scroll = m->total_list_height - (m->panel_h - TAB_HEIGHT - 44);
   if (max_scroll < 0)
      max_scroll = 0;
   if (m->scroll_offset > max_scroll)
      m->scroll_offset = max_scroll;

   /* Load more when scrolling near the bottom (tracks, artists, albums) */
   if (m->active_tab == MUSIC_TAB_LIBRARY && m->ws && !m->browse_loading_more) {
      int visible_h = m->panel_h - TAB_HEIGHT - 44;
      int bottom_visible = m->scroll_offset + visible_h;
      int load_trigger = m->total_list_height - visible_h; /* One screen from bottom */

      if (bottom_visible >= load_trigger && load_trigger > 0) {
         if (m->browse_type == MUSIC_BROWSE_TRACKS &&
             m->browse_track_count < m->browse_total_count &&
             m->browse_track_count < m->browse_tracks_cap) {
            m->browse_loading_more = true;
            ws_client_send_music_library_paged(m->ws, "tracks", NULL, m->browse_track_count,
                                               MUSIC_MAX_RESULTS);
         } else if (m->browse_type == MUSIC_BROWSE_ARTISTS &&
                    m->browse_count < m->browse_items_total &&
                    m->browse_count < m->browse_items_cap) {
            m->browse_loading_more = true;
            ws_client_send_music_library_paged(m->ws, "artists", NULL, m->browse_count,
                                               MUSIC_MAX_RESULTS);
         } else if (m->browse_type == MUSIC_BROWSE_ALBUMS &&
                    m->browse_count < m->browse_items_total &&
                    m->browse_count < m->browse_items_cap) {
            m->browse_loading_more = true;
            ws_client_send_music_library_paged(m->ws, "albums", NULL, m->browse_count,
                                               MUSIC_MAX_RESULTS);
         }
      }
   }
}

/* =============================================================================
 * Public API: State Updates (thread-safe, called from WS callback)
 * ============================================================================= */

void ui_music_on_state(ui_music_t *m, const music_state_update_t *state) {
   if (!m || !state)
      return;

   pthread_mutex_lock(&m->mutex);

   bool track_changed = (strcmp(m->current_track.title, state->track.title) != 0 ||
                         strcmp(m->current_track.artist, state->track.artist) != 0);

   m->playing = state->playing;
   m->paused = state->paused;
   memcpy(&m->current_track, &state->track, sizeof(music_track_t));
   m->duration_sec = state->duration_sec;
   snprintf(m->source_format, sizeof(m->source_format), "%s", state->source_format);
   m->source_rate = state->source_rate;
   m->bitrate = state->bitrate;
   snprintf(m->bitrate_mode, sizeof(m->bitrate_mode), "%s", state->bitrate_mode);

   /* Sync queue metadata so play button works without fetching full queue */
   if (state->queue_length >= 0)
      m->queue_count = state->queue_length;
   if (state->queue_index >= 0)
      m->queue_index = state->queue_index;

   if (track_changed) {
      invalidate_track_cache(m);
   }

   /* Detect end-of-track for repeat handling (client-side like WebUI) */
   bool now_playing = state->playing && !state->paused;
   bool trigger_repeat = false;
   int repeat_index = -1;

   if (m->was_playing && !now_playing && m->queue_count > 0) {
      /* Track ended — check repeat mode */
      if (m->repeat_mode == 2) {
         /* Repeat one — replay current track */
         trigger_repeat = true;
         repeat_index = m->queue_index;
      } else if (m->repeat_mode == 1 && m->queue_index == 0 && !state->playing) {
         /* Repeat all — end of queue, loop back to start */
         trigger_repeat = true;
         if (m->shuffle && m->queue_count > 1) {
            repeat_index = rand() % m->queue_count;
         } else {
            repeat_index = 0;
         }
      }
   }
   m->was_playing = now_playing;

   pthread_mutex_unlock(&m->mutex);

   /* Sync local playback engine with daemon state.
    * Done after releasing UI mutex since pause() can block up to 200ms. */
#ifdef HAVE_OPUS
   if (m->music_pb) {
      if (!state->playing || (state->playing && state->paused)) {
         /* Daemon says stopped or paused — pause local engine if it's playing */
         music_pb_state_t pb_st = music_playback_get_state(m->music_pb);
         if (pb_st == MUSIC_PB_PLAYING || pb_st == MUSIC_PB_BUFFERING)
            music_playback_pause(m->music_pb);
      } else if (state->playing && !state->paused) {
         /* Daemon says playing — resume local engine if paused */
         music_pb_state_t pb_st = music_playback_get_state(m->music_pb);
         if (pb_st == MUSIC_PB_PAUSED)
            music_playback_resume(m->music_pb);
      }
   }
#endif

   /* Trigger repeat if end-of-track was detected and repeat mode is on */
   if (trigger_repeat && repeat_index >= 0 && m->ws) {
      char idx_str[16];
      snprintf(idx_str, sizeof(idx_str), "%d", repeat_index);
      ws_client_send_music_control(m->ws, "play_index", idx_str);
   }
}

void ui_music_on_position(ui_music_t *m, float position_sec) {
   if (!m)
      return;
   pthread_mutex_lock(&m->mutex);
   m->position_sec = position_sec;
   pthread_mutex_unlock(&m->mutex);
}

void ui_music_on_queue(ui_music_t *m, const music_queue_update_t *queue) {
   if (!m || !queue)
      return;

   pthread_mutex_lock(&m->mutex);
   m->queue_count = queue->count;
   m->queue_index = queue->current_index;
   if (queue->count > 0) {
      memcpy(m->queue, queue->tracks, queue->count * sizeof(music_track_t));
   }
   pthread_mutex_unlock(&m->mutex);
}

void ui_music_on_library(ui_music_t *m, const music_library_update_t *lib) {
   if (!m || !lib)
      return;

   pthread_mutex_lock(&m->mutex);

   m->browse_type = lib->browse_type;

   if (lib->browse_type == MUSIC_BROWSE_NONE) {
      m->stat_tracks = lib->stat_tracks;
      m->stat_artists = lib->stat_artists;
      m->stat_albums = lib->stat_albums;
   }

   /* Artist/Album pagination: append if offset > 0, replace if offset == 0 */
   if (lib->offset > 0 && lib->item_count > 0 &&
       (lib->browse_type == MUSIC_BROWSE_ARTISTS || lib->browse_type == MUSIC_BROWSE_ALBUMS)) {
      /* Append mode — add new items after existing ones */
      int space = m->browse_items_cap - m->browse_count;
      int to_copy = lib->item_count < space ? lib->item_count : space;
      if (to_copy > 0 && m->browse_items) {
         memcpy(m->browse_items + m->browse_count, lib->items,
                to_copy * sizeof(music_browse_item_t));
         m->browse_count += to_copy;
         m->total_list_height = m->browse_count * LIST_ROW_HEIGHT;
      }
      /* Don't reset scroll on append */
   } else {
      /* Replace mode — new browse or first page */
      int to_copy = lib->item_count;
      if (to_copy > m->browse_items_cap)
         to_copy = m->browse_items_cap;
      m->browse_count = to_copy;
      if (to_copy > 0 && m->browse_items) {
         memcpy(m->browse_items, lib->items, to_copy * sizeof(music_browse_item_t));
      }
      if (lib->browse_type == MUSIC_BROWSE_ARTISTS || lib->browse_type == MUSIC_BROWSE_ALBUMS) {
         m->scroll_offset = 0;
         m->total_list_height = m->browse_count * LIST_ROW_HEIGHT;
      }
   }

   /* Track artist/album total for pagination (separate from track total) */
   if (lib->browse_type == MUSIC_BROWSE_ARTISTS || lib->browse_type == MUSIC_BROWSE_ALBUMS) {
      m->browse_items_total = lib->total_count;
   }

   /* Track pagination: append if offset > 0, replace if offset == 0 */
   if (lib->offset > 0 && lib->track_count > 0 && m->browse_tracks) {
      /* Append mode — add new tracks after existing ones */
      int space = m->browse_tracks_cap - m->browse_track_count;
      int to_copy = lib->track_count < space ? lib->track_count : space;
      if (to_copy > 0) {
         memcpy(m->browse_tracks + m->browse_track_count, lib->tracks,
                to_copy * sizeof(music_track_t));
         m->browse_track_count += to_copy;
      }
      /* Don't reset scroll on append */
   } else {
      /* Replace mode — new browse or first page */
      int to_copy = lib->track_count;
      if (to_copy > m->browse_tracks_cap)
         to_copy = m->browse_tracks_cap;
      m->browse_track_count = to_copy;
      if (to_copy > 0 && m->browse_tracks) {
         memcpy(m->browse_tracks, lib->tracks, to_copy * sizeof(music_track_t));
      }
      m->scroll_offset = 0;
   }

   m->browse_total_count = lib->total_count;
   m->browse_loading_more = false;

   pthread_mutex_unlock(&m->mutex);
}

/* =============================================================================
 * Public API: WS Client Connection
 * ============================================================================= */

void ui_music_set_ws_client(ui_music_t *m, struct ws_client *client) {
   if (!m)
      return;
   m->ws = client;
}

bool ui_music_is_playing(ui_music_t *m) {
   if (!m)
      return false;
   return m->playing && !m->paused;
}

void ui_music_set_playback(ui_music_t *m, struct music_playback *pb) {
   if (!m)
      return;
   m->music_pb = pb;
   m->volume = 80;
}

void ui_music_update_spectrum(ui_music_t *m, const volatile float *spectrum64) {
   if (!m || !spectrum64)
      return;

   /* Apply log-frequency mapping like the WebUI does (processFFTDataForBars).
    * Raw Goertzel bins are linearly spaced 0..12kHz.  Most music energy is
    * below 4kHz, so linear mapping leaves bars 16-31 nearly empty.
    *
    * WebUI: logT = pow(t, 0.6) — lower frequencies get proportionally more bars.
    * We replicate this by sampling the 64-bin spectrum with log-spaced indices,
    * then re-normalize per-frame so bars span the full 0..1 range.
    */
   float bars[MUSIC_VIZ_BAR_COUNT];
   float peak = 0.0f;

   for (int i = 0; i < MUSIC_VIZ_BAR_COUNT; i++) {
      float t = (float)i / (float)MUSIC_VIZ_BAR_COUNT;
      /* Log-frequency mapping: concentrate lower bars on bass/mids */
      float log_t = powf(t, 0.6f);
      int bin = (int)(log_t * 63.0f); /* 0..63 */
      if (bin > 63)
         bin = 63;

      /* Average with neighbors for smoothness (like WebUI's 3-bin average) */
      float sum = spectrum64[bin];
      int count = 1;
      if (bin > 0) {
         sum += spectrum64[bin - 1];
         count++;
      }
      if (bin < 63) {
         sum += spectrum64[bin + 1];
         count++;
      }
      bars[i] = sum / (float)count;
      if (bars[i] > peak)
         peak = bars[i];
   }

   /* Per-frame peak normalization + noise floor + gamma (matches WebUI) */
   float noise_floor = 0.05f;
   pthread_mutex_lock(&m->mutex);
   if (peak > noise_floor) {
      float inv = 1.0f / (peak - noise_floor);
      for (int i = 0; i < MUSIC_VIZ_BAR_COUNT; i++) {
         float v = (bars[i] - noise_floor) * inv;
         if (v < 0.0f)
            v = 0.0f;
         /* Gamma 0.7 (matches WebUI) — less aggressive than sqrt (0.5) */
         m->viz_targets[i] = powf(v, 0.7f);
      }
   } else {
      for (int i = 0; i < MUSIC_VIZ_BAR_COUNT; i++)
         m->viz_targets[i] = 0.0f;
   }
   pthread_mutex_unlock(&m->mutex);
}
