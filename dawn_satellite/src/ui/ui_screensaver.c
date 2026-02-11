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
 * Clock mode: time/date/AI name with Lissajous drift for burn-in prevention.
 * Visualizer mode: fullscreen 64-bar rainbow FFT spectrum with peak hold.
 * Fade transitions through black overlay (matches software dimming pattern).
 */

#include "ui/ui_screensaver.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ui/ui_colors.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define FADE_DURATION_SEC 0.5f

/* Clock mode */
#define CLOCK_FONT_SIZE 80
#define DATE_FONT_SIZE 24
#define DRIFT_RANGE_X 40.0f  /* +/- horizontal drift in pixels */
#define DRIFT_RANGE_Y 25.0f  /* +/- vertical drift in pixels */
#define DRIFT_PERIOD_X 297.0 /* Lissajous X period (seconds, ~5 min) */
#define DRIFT_PERIOD_Y 371.0 /* Lissajous Y period (seconds, ~6 min, coprime) */
#define CLOCK_ALPHA 180      /* Dimmed text (0-255) */

/* Visualizer mode */
#define VIZ_BAR_GAP 2
#define VIZ_MARGIN 8
#define VIZ_MAX_HEIGHT 500
#define VIZ_HUE_SPEED 15.0f         /* Degrees per second */
#define VIZ_REFLECTION_ALPHA 0.18f  /* Reflection base opacity */
#define VIZ_REFLECTION_HEIGHT 0.30f /* Reflection height ratio */
#define VIZ_REFLECTION_STRIPS 4     /* Gradient fade strips */
#define VIZ_PEAK_HOLD_SEC 0.3f      /* Hold time before decay */
#define VIZ_PEAK_DECAY_RATE 2.0f

/* Smoothing (asymmetric rise/fall, delta-time independent) */
#define SMOOTH_RISE 0.84f /* At 30fps reference */
#define SMOOTH_FALL 0.58f

/* Track info pill */
#define TRACK_PILL_FADE_IN 0.5
#define TRACK_PILL_VISIBLE 5.0
#define TRACK_PILL_FADE_OUT 1.0
#define TRACK_PILL_Y_OFFSET 40 /* Pixels from bottom */
#define TRACK_PILL_RADIUS 8    /* Corner radius for rounded pill */

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

   /* Time: "HH:MM" — re-render only on minute change */
   char time_str[8];
   snprintf(time_str, sizeof(time_str), "%02d:%02d", tm->tm_hour, tm->tm_min);
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

   /* Vertical stack: time, date, AI name */
   int spacing = 12;
   int total_h = ss->time_h + spacing + ss->date_h + spacing + ss->name_h;
   int top_y = cy - total_h / 2;

   uint8_t dim_alpha = (uint8_t)((int)alpha * CLOCK_ALPHA / 255);

   /* Time */
   if (ss->time_tex) {
      SDL_SetTextureAlphaMod(ss->time_tex, dim_alpha);
      SDL_SetTextureColorMod(ss->time_tex, COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G,
                             COLOR_TEXT_PRIMARY_B);
      SDL_Rect dst = { cx - ss->time_w / 2, top_y, ss->time_w, ss->time_h };
      SDL_RenderCopy(r, ss->time_tex, NULL, &dst);
      top_y += ss->time_h + spacing;
   }

   /* Date */
   if (ss->date_tex) {
      SDL_SetTextureAlphaMod(ss->date_tex, dim_alpha);
      SDL_SetTextureColorMod(ss->date_tex, COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                             COLOR_TEXT_SECONDARY_B);
      SDL_Rect dst = { cx - ss->date_w / 2, top_y, ss->date_w, ss->date_h };
      SDL_RenderCopy(r, ss->date_tex, NULL, &dst);
      top_y += ss->date_h + spacing;
   }

   /* AI Name — secondary color for better visibility on dark background */
   if (ss->name_tex) {
      SDL_SetTextureAlphaMod(ss->name_tex, dim_alpha);
      SDL_SetTextureColorMod(ss->name_tex, COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                             COLOR_TEXT_SECONDARY_B);
      SDL_Rect dst = { cx - ss->name_w / 2, top_y, ss->name_w, ss->name_h };
      SDL_RenderCopy(r, ss->name_tex, NULL, &dst);
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

   /* Track info pill at bottom center */
   if (ss->track_tex) {
      double since_change = time_sec - ss->track_change_time;
      float pill_alpha = 0.0f;

      if (since_change < TRACK_PILL_FADE_IN) {
         /* Ease-out cubic for smooth fade-in */
         float t = (float)(since_change / TRACK_PILL_FADE_IN);
         pill_alpha = ui_ease_out_cubic(t);
      } else if (since_change < TRACK_PILL_FADE_IN + TRACK_PILL_VISIBLE) {
         pill_alpha = 1.0f;
      } else if (since_change < TRACK_PILL_FADE_IN + TRACK_PILL_VISIBLE + TRACK_PILL_FADE_OUT) {
         float t = (float)(since_change - TRACK_PILL_FADE_IN - TRACK_PILL_VISIBLE);
         pill_alpha = 1.0f - t / (float)TRACK_PILL_FADE_OUT;
      }

      if (pill_alpha > 0.01f) {
         uint8_t pa = (uint8_t)(alpha_f * pill_alpha * 220.0f);
         int px = ss->screen_w / 2 - ss->track_tex_w / 2;
         int py = ss->screen_h - TRACK_PILL_Y_OFFSET;

         /* Rounded pill background (circles at corners + rects for body) */
         int pad_x = 16, pad_y = 6;
         int bg_x = px - pad_x, bg_y = py - pad_y;
         int bg_w = ss->track_tex_w + 2 * pad_x, bg_h = ss->track_tex_h + 2 * pad_y;
         int rad = TRACK_PILL_RADIUS;
         if (rad > bg_h / 2)
            rad = bg_h / 2;
         uint8_t bg_alpha = (uint8_t)(pa * 0.6f);
         SDL_SetRenderDrawColor(r, 0, 0, 0, bg_alpha);

         /* Center rect (full width, reduced height) */
         SDL_Rect center = { bg_x, bg_y + rad, bg_w, bg_h - 2 * rad };
         SDL_RenderFillRect(r, &center);
         /* Top/bottom rects (reduced width) */
         SDL_Rect top = { bg_x + rad, bg_y, bg_w - 2 * rad, rad };
         SDL_RenderFillRect(r, &top);
         SDL_Rect bot = { bg_x + rad, bg_y + bg_h - rad, bg_w - 2 * rad, rad };
         SDL_RenderFillRect(r, &bot);
         /* Four corner circles */
         int corners[4][2] = {
            { bg_x + rad, bg_y + rad },
            { bg_x + bg_w - rad - 1, bg_y + rad },
            { bg_x + rad, bg_y + bg_h - rad - 1 },
            { bg_x + bg_w - rad - 1, bg_y + bg_h - rad - 1 },
         };
         for (int c = 0; c < 4; c++) {
            int ccx = corners[c][0], ccy = corners[c][1];
            for (int cy = -rad; cy <= rad; cy++) {
               int cdx = (int)sqrtf((float)(rad * rad - cy * cy));
               SDL_RenderDrawLine(r, ccx - cdx, ccy + cy, ccx + cdx, ccy + cy);
            }
         }

         /* Track text */
         SDL_SetTextureAlphaMod(ss->track_tex, pa);
         SDL_Rect dst = { px, py, ss->track_tex_w, ss->track_tex_h };
         SDL_RenderCopy(r, ss->track_tex, NULL, &dst);
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

   /* Precompute 360-entry HSV rainbow lookup table */
   for (int i = 0; i < 360; i++) {
      ss->hsv_lut[i] = ui_color_from_hsv((float)i, 0.85f, 0.95f);
   }

   if (ai_name) {
      /* Store upper-case AI name */
      int ni = 0;
      for (const char *p = ai_name; *p && ni < 31; p++, ni++) {
         ss->ai_name[ni] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
      }
      ss->ai_name[ni] = '\0';
   }

   /* Load fonts for clock mode — mono font for time creates visual hierarchy */
   ss->clock_font = load_font(font_dir, "IBMPlexMono-Regular.ttf", FALLBACK_MONO_FONT,
                              CLOCK_FONT_SIZE);
   ss->date_font = load_font(font_dir, "SourceSans3-Regular.ttf", FALLBACK_BODY_FONT,
                             DATE_FONT_SIZE);

   if (!ss->clock_font) {
      LOG_WARNING("Screensaver: Failed to load clock font");
   }

   /* Pre-render AI name texture (static, never changes) */
   if (ss->date_font && ss->ai_name[0]) {
      ss->name_tex = build_white_tex(renderer, ss->date_font, ss->ai_name, &ss->name_w,
                                     &ss->name_h);
   }

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
   if (ss->time_tex)
      SDL_DestroyTexture(ss->time_tex);
   if (ss->date_tex)
      SDL_DestroyTexture(ss->date_tex);
   if (ss->name_tex)
      SDL_DestroyTexture(ss->name_tex);
   if (ss->track_tex)
      SDL_DestroyTexture(ss->track_tex);

   ss->clock_font = NULL;
   ss->date_font = NULL;
   ss->time_tex = NULL;
   ss->date_tex = NULL;
   ss->name_tex = NULL;
   ss->track_tex = NULL;
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

/** @brief Rebuild track texture if dirty (must be called on render thread) */
static void rebuild_track_texture(ui_screensaver_t *ss) {
   if (!ss->track_dirty || !ss->date_font || !ss->renderer)
      return;

   char track_str[280];
   if (ss->track_artist[0]) {
      snprintf(track_str, sizeof(track_str), "%s - %s", ss->track_artist, ss->track_title);
   } else {
      snprintf(track_str, sizeof(track_str), "%s", ss->track_title);
   }
   if (ss->track_tex)
      SDL_DestroyTexture(ss->track_tex);
   ss->track_tex = build_white_tex(ss->renderer, ss->date_font, track_str, &ss->track_tex_w,
                                   &ss->track_tex_h);
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

   /* Asymmetric smoothing at ~30fps reference rate.
    * Called once per frame, so dt is implicitly 1/30s. */
   static const float dt = 1.0f / 30.0f;

   for (int i = 0; i < n; i++) {
      float target = spectrum[i];
      float current = ss->viz_bars[i];

      /* Asymmetric rise/fall */
      float factor = (target > current) ? SMOOTH_RISE : SMOOTH_FALL;
      ss->viz_bars[i] = current + (target - current) * factor;

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
                                 double time_sec) {
   if (!ss || !title)
      return;

   /* Check if track actually changed */
   if (strcmp(ss->track_title, title) == 0 && (!artist || strcmp(ss->track_artist, artist) == 0)) {
      return;
   }

   if (artist)
      snprintf(ss->track_artist, sizeof(ss->track_artist), "%s", artist);
   else
      ss->track_artist[0] = '\0';
   snprintf(ss->track_title, sizeof(ss->track_title), "%s", title);

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

int ui_screensaver_frame_ms(const ui_screensaver_t *ss) {
   if (!ss || ss->state == SCREENSAVER_OFF)
      return 0;

   return ss->visualizer_mode ? 33 : 100;
}
