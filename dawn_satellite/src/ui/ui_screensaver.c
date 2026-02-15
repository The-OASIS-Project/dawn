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
 * Screensaver / Ambient Mode Implementation
 *
 * Clock mode: time/date with Lissajous drift, "D.A.W.N." corner watermarks.
 * Visualizer mode: fullscreen 64-bar rainbow FFT spectrum with peak hold.
 * Fade transitions through black overlay (matches software dimming pattern).
 */

#include "ui/ui_screensaver.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define FADE_DURATION_SEC 0.5f

/* Clock mode */
#define CLOCK_FONT_SIZE 80
#define DATE_FONT_SIZE 24
#define WATERMARK_FONT_SIZE 32  /* Bold, 35% larger than date font */
#define TRACK_FONT_SIZE 36      /* Track title in visualizer pill */
#define DRIFT_RANGE_X 40.0f     /* +/- horizontal drift in pixels */
#define DRIFT_RANGE_Y 25.0f     /* +/- vertical drift in pixels */
#define DRIFT_PERIOD_X 297.0    /* Lissajous X period (seconds, ~5 min) */
#define DRIFT_PERIOD_Y 371.0    /* Lissajous Y period (seconds, ~6 min, coprime) */
#define CLOCK_ALPHA 180         /* Dimmed text (0-255) */
#define WATERMARK_PADDING 20    /* Corner padding for "D.A.W.N." */
#define WATERMARK_PERIOD 8.0    /* Seconds for one full fade cycle */
#define WATERMARK_MAX_ALPHA 200 /* Peak alpha at full fade-in */

/* Visualizer mode */
#define VIZ_BAR_GAP 2
#define VIZ_MARGIN 8
#define VIZ_MAX_HEIGHT 500
#define VIZ_HUE_SPEED 15.0f         /* Degrees per second */
#define VIZ_REFLECTION_ALPHA 0.35f  /* Reflection base opacity */
#define VIZ_REFLECTION_HEIGHT 0.30f /* Reflection height ratio */
#define VIZ_REFLECTION_STRIPS 4     /* Gradient fade strips */
#define VIZ_PEAK_HOLD_SEC 0.3f      /* Hold time before decay */
#define VIZ_PEAK_DECAY_RATE 2.0f

/* Smoothing (asymmetric rise/fall, frame-rate independent via powf) */
#define SMOOTH_RISE_BASE 0.4f  /* Match ui_music.c: powf(0.4, 60*dt) */
#define SMOOTH_FALL_BASE 0.65f /* Match ui_music.c: powf(0.65, 60*dt) */

/* Track info (lower-left, always visible) */
#define TRACK_INFO_MARGIN 24 /* Padding from screen edges */

/* Transport controls (lower-right) */
#define TRANSPORT_ICON_SZ 40 /* Icon texture size */
#define TRANSPORT_HIT_SZ 56  /* Touch target (>44px minimum) */
#define TRANSPORT_GAP 20     /* Gap between buttons */
#define TRANSPORT_MARGIN 24  /* Padding from screen edges */

/* Fallback fonts */
#define FALLBACK_MONO_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FALLBACK_BODY_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* =============================================================================
 * Helpers
 * ============================================================================= */

/** @brief Load a font with fallback to system fonts */
static TTF_Font *load_font(const char *font_dir,
                           const char *filename,
                           const char *fallback,
                           int size) {
   char path[512];
   TTF_Font *font = NULL;

   if (font_dir && filename) {
      snprintf(path, sizeof(path), "%s/%s", font_dir, filename);
      font = TTF_OpenFont(path, size);
   }
   if (!font && fallback) {
      font = TTF_OpenFont(fallback, size);
   }
   return font;
}

/** @brief Build a white texture from text (tint later with SDL_SetTextureColorMod) */
static SDL_Texture *build_white_tex(SDL_Renderer *r,
                                    TTF_Font *font,
                                    const char *text,
                                    int *w,
                                    int *h) {
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, white);
   if (!surf)
      return NULL;
   SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
   if (tex) {
      *w = surf->w;
      *h = surf->h;
   }
   SDL_FreeSurface(surf);
   return tex;
}

/* =============================================================================
 * Clock Rendering
 * ============================================================================= */

static void update_clock_textures(ui_screensaver_t *ss, SDL_Renderer *r) {
   time_t now = time(NULL);
   if (now == ss->cached_epoch)
      return; /* Already checked this second */
   ss->cached_epoch = now;
   struct tm *tm = localtime(&now);

   /* Time: re-render only on minute change */
   char time_str[8];
   if (ss->time_24h)
      snprintf(time_str, sizeof(time_str), "%02d:%02d", tm->tm_hour, tm->tm_min);
   else
      snprintf(time_str, sizeof(time_str), "%d:%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12,
               tm->tm_min);
   if (strcmp(time_str, ss->cached_time) != 0 && ss->clock_font) {
      snprintf(ss->cached_time, sizeof(ss->cached_time), "%s", time_str);
      if (ss->time_tex)
         SDL_DestroyTexture(ss->time_tex);
      ss->time_tex = build_white_tex(r, ss->clock_font, time_str, &ss->time_w, &ss->time_h);
   }

   /* Date: "Tuesday, Feb 11" — re-render only on day change */
   char date_str[32];
   strftime(date_str, sizeof(date_str), "%A, %b %d", tm);
   if (strcmp(date_str, ss->cached_date) != 0 && ss->date_font) {
      snprintf(ss->cached_date, sizeof(ss->cached_date), "%s", date_str);
      if (ss->date_tex)
         SDL_DestroyTexture(ss->date_tex);
      ss->date_tex = build_white_tex(r, ss->date_font, date_str, &ss->date_w, &ss->date_h);
   }
}

static void render_clock(ui_screensaver_t *ss, SDL_Renderer *r, double time_sec, uint8_t alpha) {
   update_clock_textures(ss, r);

   /* Lissajous drift for burn-in prevention */
   ss->drift_x = DRIFT_RANGE_X * sinf((float)(time_sec / DRIFT_PERIOD_X) * 2.0f * (float)M_PI);
   ss->drift_y = DRIFT_RANGE_Y * sinf((float)(time_sec / DRIFT_PERIOD_Y) * 2.0f * (float)M_PI);

   int cx = ss->screen_w / 2 + (int)ss->drift_x;
   int cy = ss->screen_h / 2 + (int)ss->drift_y;

   /* Vertical stack: time + date */
   int spacing = 12;
   int total_h = ss->time_h + spacing + ss->date_h;
   int top_y = cy - total_h / 2;

   uint8_t dim_alpha = (uint8_t)((int)alpha * CLOCK_ALPHA / 255);

   /* Time */
   if (ss->time_tex) {
      SDL_SetTextureAlphaMod(ss->time_tex, dim_alpha);
      ui_color_t txt0 = ui_theme_text(0);
      SDL_SetTextureColorMod(ss->time_tex, txt0.r, txt0.g, txt0.b);
      SDL_Rect dst = { cx - ss->time_w / 2, top_y, ss->time_w, ss->time_h };
      SDL_RenderCopy(r, ss->time_tex, NULL, &dst);
      top_y += ss->time_h + spacing;
   }

   /* Date — accent color for brightness and visual identity */
   if (ss->date_tex) {
      ui_color_t ac = ui_theme_accent();
      SDL_SetTextureAlphaMod(ss->date_tex, alpha);
      SDL_SetTextureColorMod(ss->date_tex, ac.r, ac.g, ac.b);
      SDL_Rect dst = { cx - ss->date_w / 2, top_y, ss->date_w, ss->date_h };
      SDL_RenderCopy(r, ss->date_tex, NULL, &dst);
   }

   /* "D.A.W.N." watermark — fades fully in/out, switches corner when invisible */
   if (ss->watermark_tex) {
      /* Sine pulse 0 → max → 0 (half-wave rectified for clean fade to zero) */
      float phase = fmodf((float)(time_sec / WATERMARK_PERIOD), 1.0f);
      float pulse = sinf(phase * (float)M_PI); /* 0→1→0 over one period */
      float wm_alpha_f = (float)WATERMARK_MAX_ALPHA * pulse;
      uint8_t wm_alpha = (uint8_t)(wm_alpha_f * (float)alpha / 255.0f);

      /* Switch to a new random corner when faded out (start of each cycle) */
      int cycle = (int)(time_sec / WATERMARK_PERIOD);
      if (cycle != ss->watermark_last_cycle) {
         ss->watermark_last_cycle = cycle;
         int prev = ss->watermark_corner;
         do {
            ss->watermark_corner = rand() % 4;
         } while (ss->watermark_corner == prev);
      }

      if (wm_alpha > 0) {
         SDL_SetTextureAlphaMod(ss->watermark_tex, wm_alpha);
         ui_color_t txt1 = ui_theme_text(1);
         SDL_SetTextureColorMod(ss->watermark_tex, txt1.r, txt1.g, txt1.b);

         int pad = WATERMARK_PADDING;
         int ww = ss->watermark_w;
         int wh = ss->watermark_h;
         int wx = (ss->watermark_corner & 1) ? (ss->screen_w - ww - pad) : pad;
         int wy = (ss->watermark_corner & 2) ? (ss->screen_h - wh - pad) : pad;
         SDL_Rect dst = { wx, wy, ww, wh };
         SDL_RenderCopy(r, ss->watermark_tex, NULL, &dst);
      }
   }
}

/* =============================================================================
 * Transport Icon Building (white textures, tinted at render time)
 * ============================================================================= */

/** @brief Build all 4 transport textures: prev, play, pause, next */
static void build_transport_icons(ui_screensaver_t *ss, SDL_Renderer *r) {
   int sz = TRANSPORT_ICON_SZ;
   ss->transport_sz = sz;

   /* Previous: vertical bar + two left-pointing triangles */
   {
      SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                           sz, sz);
      if (tex) {
         SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
         SDL_SetRenderTarget(r, tex);
         SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
         SDL_RenderClear(r);
         SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
         int cy = sz / 2;
         int bar_w = 2, bar_h = sz * 2 / 3;
         SDL_Rect bar = { 2, cy - bar_h / 2, bar_w, bar_h };
         SDL_RenderFillRect(r, &bar);
         int tri_h = sz / 2, tri_w = (sz - 6) / 2;
         int t1 = 2 + bar_w + 1, t2 = t1 + tri_w;
         for (int col = 0; col < tri_w; col++) {
            int h = tri_h * col / tri_w;
            SDL_RenderDrawLine(r, t1 + col, cy - h, t1 + col, cy + h);
         }
         for (int col = 0; col < tri_w; col++) {
            int h = tri_h * col / tri_w;
            SDL_RenderDrawLine(r, t2 + col, cy - h, t2 + col, cy + h);
         }
         SDL_SetRenderTarget(r, NULL);
      }
      ss->transport_tex[0] = tex;
   }

   /* Play: right-pointing filled triangle */
   {
      SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                           sz, sz);
      if (tex) {
         SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
         SDL_SetRenderTarget(r, tex);
         SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
         SDL_RenderClear(r);
         SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
         int cy = sz / 2, tri_h = sz * 2 / 5;
         int left = sz / 4, right = sz - sz / 4, tw = right - left;
         for (int col = 0; col < tw; col++) {
            int h = tri_h * (tw - col) / tw;
            SDL_RenderDrawLine(r, left + col, cy - h, left + col, cy + h);
         }
         SDL_SetRenderTarget(r, NULL);
      }
      ss->transport_tex[1] = tex;
   }

   /* Pause: two vertical bars */
   {
      SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                           sz, sz);
      if (tex) {
         SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
         SDL_SetRenderTarget(r, tex);
         SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
         SDL_RenderClear(r);
         SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
         int bw = sz / 5, bh = sz * 7 / 10, gap = sz / 5;
         int total = bw * 2 + gap;
         int x0 = (sz - total) / 2, y0 = (sz - bh) / 2;
         SDL_Rect b1 = { x0, y0, bw, bh };
         SDL_Rect b2 = { x0 + bw + gap, y0, bw, bh };
         SDL_RenderFillRect(r, &b1);
         SDL_RenderFillRect(r, &b2);
         SDL_SetRenderTarget(r, NULL);
      }
      ss->transport_tex[2] = tex;
   }

   /* Next: two right-pointing triangles + vertical bar */
   {
      SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                           sz, sz);
      if (tex) {
         SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
         SDL_SetRenderTarget(r, tex);
         SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
         SDL_RenderClear(r);
         SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
         int cy = sz / 2;
         int bar_w = 2, bar_h = sz * 2 / 3;
         int bar_x = sz - 2 - bar_w;
         SDL_Rect bar = { bar_x, cy - bar_h / 2, bar_w, bar_h };
         SDL_RenderFillRect(r, &bar);
         int tri_h = sz / 2, tri_w = (sz - 6) / 2;
         int t1 = 1, t2 = t1 + tri_w;
         for (int col = 0; col < tri_w; col++) {
            int h = tri_h * (tri_w - col) / tri_w;
            SDL_RenderDrawLine(r, t1 + col, cy - h, t1 + col, cy + h);
         }
         for (int col = 0; col < tri_w; col++) {
            int h = tri_h * (tri_w - col) / tri_w;
            SDL_RenderDrawLine(r, t2 + col, cy - h, t2 + col, cy + h);
         }
         SDL_SetRenderTarget(r, NULL);
      }
      ss->transport_tex[3] = tex;
   }
}

/* =============================================================================
 * Visualizer Rendering
 * ============================================================================= */

static void render_rainbow_viz(ui_screensaver_t *ss,
                               SDL_Renderer *r,
                               double time_sec,
                               uint8_t alpha) {
   /* Rotate hue offset for living rainbow (time-based, frame-rate independent) */
   ss->hue_offset = fmodf((float)time_sec * VIZ_HUE_SPEED, 360.0f);

   int total_bar_area = ss->screen_w - 2 * VIZ_MARGIN;
   int bar_w = (total_bar_area - (SPECTRUM_BINS - 1) * VIZ_BAR_GAP) / SPECTRUM_BINS;
   if (bar_w < 2)
      bar_w = 2;

   /* Recalculate margins to center bars */
   int actual_width = bar_w * SPECTRUM_BINS + VIZ_BAR_GAP * (SPECTRUM_BINS - 1);
   int left_margin = (ss->screen_w - actual_width) / 2;

   int baseline_y = ss->screen_h - 80; /* Leave room for reflections and track pill */
   float alpha_f = (float)alpha / 255.0f;

   for (int i = 0; i < SPECTRUM_BINS; i++) {
      float val = ss->viz_bars[i];
      if (val < 0.0f)
         val = 0.0f;
      if (val > 1.0f)
         val = 1.0f;

      int bar_h = (int)(val * VIZ_MAX_HEIGHT);
      if (bar_h < 1 && val > 0.01f)
         bar_h = 1;

      int x = left_margin + i * (bar_w + VIZ_BAR_GAP);
      int y = baseline_y - bar_h;

      /* Rainbow color via precomputed LUT (hue cycles across bars + rotating offset) */
      int hue_idx = ((i * 360 / SPECTRUM_BINS) + (int)ss->hue_offset) % 360;
      if (hue_idx < 0)
         hue_idx += 360;
      ui_color_t color = ss->hsv_lut[hue_idx];

      /* Main bar */
      uint8_t bar_alpha = (uint8_t)(alpha_f * 255.0f);
      SDL_SetRenderDrawColor(r, color.r, color.g, color.b, bar_alpha);
      SDL_Rect bar_rect = { x, y, bar_w, bar_h };
      SDL_RenderFillRect(r, &bar_rect);

      /* Reflection below baseline — gradient fade (4 strips, decreasing alpha) */
      int ref_h = (int)((float)bar_h * VIZ_REFLECTION_HEIGHT);
      if (ref_h > VIZ_REFLECTION_STRIPS) {
         int strip_h = ref_h / VIZ_REFLECTION_STRIPS;
         for (int s = 0; s < VIZ_REFLECTION_STRIPS; s++) {
            float strip_frac = 1.0f - (float)s / (float)VIZ_REFLECTION_STRIPS;
            uint8_t ref_alpha = (uint8_t)(alpha_f * VIZ_REFLECTION_ALPHA * strip_frac * 255.0f);
            SDL_SetRenderDrawColor(r, color.r, color.g, color.b, ref_alpha);
            SDL_Rect strip_rect = { x, baseline_y + s * strip_h, bar_w, strip_h };
            SDL_RenderFillRect(r, &strip_rect);
         }
      }

      /* Peak hold indicator — color-matched to bar hue, brighter */
      float peak = ss->peak_hold[i];
      float age = ss->peak_age[i];
      if (peak > 0.01f) {
         int peak_y = baseline_y - (int)(peak * VIZ_MAX_HEIGHT);
         /* Fade peak after hold time */
         float peak_alpha_f = 1.0f;
         if (age > VIZ_PEAK_HOLD_SEC) {
            peak_alpha_f = 1.0f - (age - VIZ_PEAK_HOLD_SEC) * VIZ_PEAK_DECAY_RATE;
            if (peak_alpha_f < 0.0f)
               peak_alpha_f = 0.0f;
         }
         if (peak_alpha_f > 0.0f) {
            uint8_t pk_alpha = (uint8_t)(alpha_f * peak_alpha_f * 255.0f);
            /* Brighten bar color for peak: blend 50% toward white */
            uint8_t pk_r = color.r + (255 - color.r) / 2;
            uint8_t pk_g = color.g + (255 - color.g) / 2;
            uint8_t pk_b = color.b + (255 - color.b) / 2;
            SDL_SetRenderDrawColor(r, pk_r, pk_g, pk_b, pk_alpha);
            SDL_Rect pk_rect = { x, peak_y, bar_w, 2 };
            SDL_RenderFillRect(r, &pk_rect);
         }
      }
   }

   /* Track info — lower-left, always visible, with shadow backdrop */
   if (ss->track_title_tex) {
      int line_gap = 4;
      int pad_x = 14, pad_y = 10;
      int content_w = ss->track_title_w;
      int content_h = ss->track_title_h;
      bool has_sub = (ss->track_sub_tex != NULL);
      if (has_sub) {
         if (ss->track_sub_w > content_w)
            content_w = ss->track_sub_w;
         content_h += line_gap + ss->track_sub_h;
      }

      int box_w = content_w + 2 * pad_x;
      int box_h = content_h + 2 * pad_y;
      int box_x = TRACK_INFO_MARGIN - pad_x;
      int box_y = ss->screen_h - TRACK_INFO_MARGIN - content_h - pad_y;

      /* Semi-transparent shadow backdrop */
      uint8_t bg_alpha = (uint8_t)((float)alpha * 0.45f);
      SDL_SetRenderDrawColor(r, 0, 0, 0, bg_alpha);
      SDL_Rect bg_rect = { box_x, box_y, box_w, box_h };
      SDL_RenderFillRect(r, &bg_rect);

      int tx = TRACK_INFO_MARGIN;
      int ty = box_y + pad_y;

      /* Title (large, bold, white) */
      SDL_SetTextureAlphaMod(ss->track_title_tex, alpha);
      SDL_Rect title_dst = { tx, ty, ss->track_title_w, ss->track_title_h };
      SDL_RenderCopy(r, ss->track_title_tex, NULL, &title_dst);

      /* Album / Artist subtitle (brighter) */
      if (has_sub) {
         int sy = ty + ss->track_title_h + line_gap;
         SDL_SetTextureAlphaMod(ss->track_sub_tex, alpha);
         SDL_SetTextureColorMod(ss->track_sub_tex, 230, 230, 230);
         SDL_Rect sub_dst = { tx, sy, ss->track_sub_w, ss->track_sub_h };
         SDL_RenderCopy(r, ss->track_sub_tex, NULL, &sub_dst);
      }
   }

   /* Transport controls — lower-right: [prev] [play/pause] [next] */
   {
      int btn_count = 3;
      int total_w = btn_count * TRANSPORT_HIT_SZ + (btn_count - 1) * TRANSPORT_GAP;
      int base_x = ss->screen_w - TRANSPORT_MARGIN - total_w;
      int base_y = ss->screen_h - TRANSPORT_MARGIN - TRANSPORT_HIT_SZ;
      int icon_off = (TRANSPORT_HIT_SZ - ss->transport_sz) / 2;

      /* Indices: 0=prev, 1=play, 2=pause, 3=next */
      int icons[3] = { 0, ss->music_playing ? 2 : 1, 3 };
      for (int b = 0; b < btn_count; b++) {
         int bx = base_x + b * (TRANSPORT_HIT_SZ + TRANSPORT_GAP);
         SDL_Texture *tex = ss->transport_tex[icons[b]];
         if (!tex)
            continue;
         SDL_SetTextureAlphaMod(tex, alpha);
         SDL_SetTextureColorMod(tex, 220, 220, 220);
         SDL_Rect dst = { bx + icon_off, base_y + icon_off, ss->transport_sz, ss->transport_sz };
         SDL_RenderCopy(r, tex, NULL, &dst);
      }
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int ui_screensaver_init(ui_screensaver_t *ss,
                        SDL_Renderer *renderer,
                        int w,
                        int h,
                        const char *font_dir,
                        const char *ai_name,
                        bool enabled,
                        float timeout_sec) {
   if (!ss)
      return 1;

   memset(ss, 0, sizeof(*ss));
   ss->state = SCREENSAVER_OFF;
   ss->enabled = enabled;
   ss->timeout_sec = timeout_sec;
   ss->screen_w = w;
   ss->screen_h = h;
   ss->renderer = renderer;

   /* Seed random for watermark corner selection */
   srand((unsigned int)time(NULL));

   /* Precompute 360-entry HSV rainbow lookup table */
   for (int i = 0; i < 360; i++) {
      ss->hsv_lut[i] = ui_color_from_hsv((float)i, 0.85f, 0.95f);
   }

   if (ai_name) {
      snprintf(ss->ai_name, sizeof(ss->ai_name), "%s", ai_name);
   }

   /* Load fonts for clock mode — mono font for time creates visual hierarchy */
   ss->clock_font = load_font(font_dir, "IBMPlexMono-Regular.ttf", FALLBACK_MONO_FONT,
                              CLOCK_FONT_SIZE);
   ss->date_font = load_font(font_dir, "SourceSans3-Regular.ttf", FALLBACK_BODY_FONT,
                             DATE_FONT_SIZE);
   ss->track_font = load_font(font_dir, "SourceSans3-Bold.ttf", FALLBACK_BODY_FONT,
                              TRACK_FONT_SIZE);

   if (!ss->clock_font) {
      LOG_WARNING("Screensaver: Failed to load clock font");
   }

   /* Pre-render "D.A.W.N." watermark texture (bold, 32pt, static) */
   {
      TTF_Font *wm_font = load_font(font_dir, "SourceSans3-Bold.ttf", FALLBACK_BODY_FONT,
                                    WATERMARK_FONT_SIZE);
      if (wm_font) {
         ss->watermark_tex = build_white_tex(renderer, wm_font, "D.A.W.N.", &ss->watermark_w,
                                             &ss->watermark_h);
         TTF_CloseFont(wm_font);
      }
      ss->watermark_corner = rand() % 4;
      ss->watermark_last_cycle = -1;
   }

   /* Build transport control icons for visualizer mode */
   build_transport_icons(ss, renderer);

   /* Initialize cached strings to force first render */
   ss->cached_time[0] = '\0';
   ss->cached_date[0] = '\0';

   LOG_INFO("Screensaver: initialized (enabled=%s, timeout=%.0fs)", enabled ? "yes" : "no",
            timeout_sec);
   return 0;
}

void ui_screensaver_cleanup(ui_screensaver_t *ss) {
   if (!ss)
      return;

   if (ss->clock_font)
      TTF_CloseFont(ss->clock_font);
   if (ss->date_font)
      TTF_CloseFont(ss->date_font);
   if (ss->track_font)
      TTF_CloseFont(ss->track_font);
   if (ss->time_tex)
      SDL_DestroyTexture(ss->time_tex);
   if (ss->date_tex)
      SDL_DestroyTexture(ss->date_tex);
   if (ss->watermark_tex)
      SDL_DestroyTexture(ss->watermark_tex);
   if (ss->track_title_tex)
      SDL_DestroyTexture(ss->track_title_tex);
   if (ss->track_sub_tex)
      SDL_DestroyTexture(ss->track_sub_tex);
   for (int i = 0; i < 4; i++) {
      if (ss->transport_tex[i])
         SDL_DestroyTexture(ss->transport_tex[i]);
   }

   ss->clock_font = NULL;
   ss->date_font = NULL;
   ss->track_font = NULL;
   ss->time_tex = NULL;
   ss->date_tex = NULL;
   ss->watermark_tex = NULL;
   ss->track_title_tex = NULL;
   ss->track_sub_tex = NULL;
   memset(ss->transport_tex, 0, sizeof(ss->transport_tex));
}

void ui_screensaver_activity(ui_screensaver_t *ss, double time_sec) {
   if (!ss)
      return;

   ss->idle_start = time_sec;

   /* If screensaver is active, begin fade out */
   if (ss->state == SCREENSAVER_ACTIVE || ss->state == SCREENSAVER_FADING_IN) {
      ss->state = SCREENSAVER_FADING_OUT;
      ss->fade_start = time_sec;
      ss->manual = false;
   }
}

void ui_screensaver_tick(ui_screensaver_t *ss,
                         double time_sec,
                         bool music_playing,
                         bool panels_open) {
   if (!ss)
      return;

   switch (ss->state) {
      case SCREENSAVER_OFF: {
         /* Manual trigger takes effect immediately regardless of enabled/panels */
         if (ss->manual && music_playing) {
            ss->state = SCREENSAVER_FADING_IN;
            ss->fade_start = time_sec;
            ss->visualizer_mode = true;
            return;
         }

         /* Don't activate while panels are open */
         if (!ss->enabled || panels_open)
            return;

         /* Check idle timeout */
         double idle_time = time_sec - ss->idle_start;
         if (idle_time >= (double)ss->timeout_sec && ss->idle_start > 0.0) {
            ss->state = SCREENSAVER_FADING_IN;
            ss->fade_start = time_sec;
            ss->visualizer_mode = music_playing;
         }
         break;
      }

      case SCREENSAVER_FADING_IN: {
         float t = (float)((time_sec - ss->fade_start) / FADE_DURATION_SEC);
         if (t >= 1.0f) {
            ss->state = SCREENSAVER_ACTIVE;
         }

         /* Handle mode switch during fade */
         if (!ss->manual) {
            if (music_playing && !ss->visualizer_mode) {
               ss->visualizer_mode = true;
            } else if (!music_playing && ss->visualizer_mode) {
               ss->visualizer_mode = false;
            }
         }
         break;
      }

      case SCREENSAVER_ACTIVE: {
         /* Manual visualizer: exit if music stops */
         if (ss->manual && !music_playing) {
            ss->state = SCREENSAVER_FADING_OUT;
            ss->fade_start = time_sec;
            ss->manual = false;
            return;
         }

         /* Auto mode: switch between clock/visualizer based on music */
         if (!ss->manual) {
            if (music_playing && !ss->visualizer_mode) {
               ss->visualizer_mode = true;
            } else if (!music_playing && ss->visualizer_mode) {
               ss->visualizer_mode = false;
            }
         }
         break;
      }

      case SCREENSAVER_FADING_OUT: {
         float t = (float)((time_sec - ss->fade_start) / FADE_DURATION_SEC);
         if (t >= 1.0f) {
            ss->state = SCREENSAVER_OFF;
            ss->manual = false;
            ss->idle_start = time_sec;
         }
         break;
      }
   }
}

/** @brief Rebuild track textures if dirty (must be called on render thread) */
static void rebuild_track_texture(ui_screensaver_t *ss) {
   if (!ss->track_dirty || !ss->renderer)
      return;

   /* Title line (large font) */
   if (ss->track_title_tex)
      SDL_DestroyTexture(ss->track_title_tex);
   ss->track_title_tex = NULL;
   if (ss->track_title[0] && ss->track_font) {
      ss->track_title_tex = build_white_tex(ss->renderer, ss->track_font, ss->track_title,
                                            &ss->track_title_w, &ss->track_title_h);
   }

   /* Subtitle line: "Album - Artist" or just one (small font) */
   if (ss->track_sub_tex)
      SDL_DestroyTexture(ss->track_sub_tex);
   ss->track_sub_tex = NULL;
   if (ss->date_font) {
      char sub_str[280];
      if (ss->track_album[0] && ss->track_artist[0]) {
         snprintf(sub_str, sizeof(sub_str), "%s  \xE2\x80\xA2  %s", ss->track_album,
                  ss->track_artist);
      } else if (ss->track_album[0]) {
         snprintf(sub_str, sizeof(sub_str), "%s", ss->track_album);
      } else if (ss->track_artist[0]) {
         snprintf(sub_str, sizeof(sub_str), "%s", ss->track_artist);
      } else {
         sub_str[0] = '\0';
      }
      if (sub_str[0]) {
         ss->track_sub_tex = build_white_tex(ss->renderer, ss->date_font, sub_str, &ss->track_sub_w,
                                             &ss->track_sub_h);
      }
   }

   ss->track_dirty = false;
}

void ui_screensaver_render(ui_screensaver_t *ss, SDL_Renderer *r, double time_sec) {
   if (!ss || ss->state == SCREENSAVER_OFF)
      return;

   /* Calculate fade alpha */
   float fade_t = (float)((time_sec - ss->fade_start) / FADE_DURATION_SEC);
   if (fade_t < 0.0f)
      fade_t = 0.0f;
   if (fade_t > 1.0f)
      fade_t = 1.0f;

   float eased = ui_ease_out_cubic(fade_t);
   uint8_t content_alpha;
   uint8_t bg_alpha;

   switch (ss->state) {
      case SCREENSAVER_FADING_IN:
         content_alpha = (uint8_t)(eased * 255.0f);
         bg_alpha = (uint8_t)(eased * 255.0f);
         break;
      case SCREENSAVER_ACTIVE:
         content_alpha = 255;
         bg_alpha = 255;
         break;
      case SCREENSAVER_FADING_OUT:
         content_alpha = (uint8_t)((1.0f - eased) * 255.0f);
         bg_alpha = (uint8_t)((1.0f - eased) * 255.0f);
         break;
      default:
         return;
   }

   /* Rebuild track texture if needed (render thread only) */
   rebuild_track_texture(ss);

   /* Black background overlay */
   SDL_SetRenderDrawColor(r, 0, 0, 0, bg_alpha);
   SDL_Rect full = { 0, 0, ss->screen_w, ss->screen_h };
   SDL_RenderFillRect(r, &full);

   /* Render content */
   if (ss->visualizer_mode) {
      render_rainbow_viz(ss, r, time_sec, content_alpha);
   } else {
      render_clock(ss, r, time_sec, content_alpha);
   }
}

void ui_screensaver_update_spectrum(ui_screensaver_t *ss, const float *spectrum, int count) {
   if (!ss || !spectrum)
      return;

   int n = count < SPECTRUM_BINS ? count : SPECTRUM_BINS;

   /* Frame-rate independent dt (match ui_music.c approach) */
   uint32_t now = SDL_GetTicks();
   float dt = (now > ss->viz_last_render && ss->viz_last_render > 0)
                  ? (float)(now - ss->viz_last_render) / 1000.0f
                  : 1.0f / 30.0f;
   ss->viz_last_render = now;

   /* Frame-rate independent smoothing: alpha = 1 - base^(60*dt)
    * At 30fps: rise ~0.84, fall ~0.58. Scales naturally to other rates. */
   /* powf(base, x) = expf(x * logf(base)). Precompute log constants to avoid powf. */
   static const float LOG_RISE = -0.916291f; /* logf(SMOOTH_RISE_BASE=0.4) */
   static const float LOG_FALL = -0.431364f; /* logf(SMOOTH_FALL_BASE=0.65) */
   float rise_alpha = 1.0f - expf(60.0f * dt * LOG_RISE);
   float fall_alpha = 1.0f - expf(60.0f * dt * LOG_FALL);

   for (int i = 0; i < n; i++) {
      /* Goertzel bins are already log-spaced (30Hz-16kHz), use directly.
       * 3-bin neighbor average for smoothness. */
      float sum = spectrum[i];
      int avg_count = 1;
      if (i > 0) {
         sum += spectrum[i - 1];
         avg_count++;
      }
      if (i < n - 1) {
         sum += spectrum[i + 1];
         avg_count++;
      }
      float target = sum / (float)avg_count;
      if (target > 1.0f)
         target = 1.0f;

      /* Asymmetric rise/fall smoothing */
      float current = ss->viz_bars[i];
      float alpha = (target > current) ? rise_alpha : fall_alpha;
      ss->viz_bars[i] = current + (target - current) * alpha;

      /* Peak hold tracking */
      ss->peak_age[i] += dt;
      if (ss->viz_bars[i] > ss->peak_hold[i]) {
         ss->peak_hold[i] = ss->viz_bars[i];
         ss->peak_age[i] = 0.0f;
      } else if (ss->peak_age[i] > VIZ_PEAK_HOLD_SEC + 1.0f / VIZ_PEAK_DECAY_RATE) {
         /* Peak has fully faded, reset */
         ss->peak_hold[i] = ss->viz_bars[i];
         ss->peak_age[i] = 0.0f;
      }
   }
}

void ui_screensaver_update_track(ui_screensaver_t *ss,
                                 const char *artist,
                                 const char *title,
                                 const char *album,
                                 double time_sec) {
   if (!ss || !title)
      return;

   /* Check if track actually changed */
   if (strcmp(ss->track_title, title) == 0 && (!artist || strcmp(ss->track_artist, artist) == 0) &&
       (!album || strcmp(ss->track_album, album) == 0)) {
      return;
   }

   if (artist)
      snprintf(ss->track_artist, sizeof(ss->track_artist), "%s", artist);
   else
      ss->track_artist[0] = '\0';
   snprintf(ss->track_title, sizeof(ss->track_title), "%s", title);
   if (album)
      snprintf(ss->track_album, sizeof(ss->track_album), "%s", album);
   else
      ss->track_album[0] = '\0';

   ss->track_dirty = true;
   ss->track_change_time = time_sec;
}

bool ui_screensaver_is_active(const ui_screensaver_t *ss) {
   if (!ss)
      return false;
   return ss->state == SCREENSAVER_FADING_IN || ss->state == SCREENSAVER_ACTIVE ||
          ss->state == SCREENSAVER_FADING_OUT;
}

void ui_screensaver_toggle_manual(ui_screensaver_t *ss, double time_sec) {
   if (!ss)
      return;

   if (ss->state == SCREENSAVER_OFF) {
      /* Activate manual visualizer */
      ss->manual = true;
      ss->state = SCREENSAVER_FADING_IN;
      ss->fade_start = time_sec;
      ss->visualizer_mode = true;
      LOG_INFO("Screensaver: manual visualizer activated");
   } else {
      /* Deactivate */
      ss->state = SCREENSAVER_FADING_OUT;
      ss->fade_start = time_sec;
      ss->manual = false;
      LOG_INFO("Screensaver: manual visualizer deactivated");
   }
}

const char *ui_screensaver_handle_tap(const ui_screensaver_t *ss, int x, int y, bool playing) {
   if (!ss || !ss->visualizer_mode)
      return NULL;

   /* Transport button layout mirrors render_rainbow_viz transport section */
   int btn_count = 3;
   int total_w = btn_count * TRANSPORT_HIT_SZ + (btn_count - 1) * TRANSPORT_GAP;
   int base_x = ss->screen_w - TRANSPORT_MARGIN - total_w;
   int base_y = ss->screen_h - TRANSPORT_MARGIN - TRANSPORT_HIT_SZ;

   /* Expand vertical hit area by 12px each side for fat-finger tolerance */
   if (y < base_y - 12 || y > base_y + TRANSPORT_HIT_SZ + 12)
      return NULL;
   if (x < base_x || x > base_x + total_w)
      return NULL;

   /* Divide row into equal zones (no dead gaps between buttons) */
   int zone_w = total_w / btn_count;
   int b = (x - base_x) / zone_w;
   if (b >= btn_count)
      b = btn_count - 1;

   if (b == 0)
      return "previous";
   if (b == 1)
      return playing ? "pause" : "play";
   return "next";
}

int ui_screensaver_frame_ms(const ui_screensaver_t *ss) {
   if (!ss || ss->state == SCREENSAVER_OFF)
      return 0;

   return ss->visualizer_mode ? 33 : 100;
}
