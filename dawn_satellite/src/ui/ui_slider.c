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
 * Reusable horizontal slider component for SDL2 UI
 */

#include "ui/ui_slider.h"

#include <stdio.h>
#include <string.h>

#include "ui/ui_colors.h"

#define SLIDER_TRACK_H 8
#define SLIDER_THUMB_W 18
#define SLIDER_THUMB_H 36
#define SLIDER_TOUCH_PAD 20 /* Vertical touch padding above/below track */
/* SLIDER_LABEL_COL defined in ui_slider.h */

void ui_slider_init(ui_slider_t *s,
                    SDL_Renderer *renderer,
                    int track_x,
                    int track_y,
                    int track_w,
                    uint8_t r,
                    uint8_t g,
                    uint8_t b,
                    const char *label,
                    TTF_Font *font) {
   memset(s, 0, sizeof(*s));
   s->track_x = track_x;
   s->track_y = track_y;
   s->track_w = track_w;
   s->track_h = SLIDER_TRACK_H;
   s->fill_r = r;
   s->fill_g = g;
   s->fill_b = b;
   s->value = 0.8f;
   s->cached_pct = -1;

   /* Pre-render label texture */
   if (font && label && renderer) {
      SDL_Color clr = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                        255 };
      SDL_Surface *surf = TTF_RenderText_Blended(font, label, clr);
      if (surf) {
         s->label_tex = SDL_CreateTextureFromSurface(renderer, surf);
         s->label_w = surf->w;
         s->label_h = surf->h;
         SDL_FreeSurface(surf);
      }
   }
}

void ui_slider_render(ui_slider_t *s, SDL_Renderer *renderer, TTF_Font *font) {
   int tx = s->track_x;
   int ty = s->track_y;
   int tw = s->track_w;
   int th = s->track_h;

   /* Label (left-aligned to fixed column before track) */
   if (s->label_tex) {
      int label_x = tx - SLIDER_LABEL_COL;
      int ly = ty + (th - s->label_h) / 2;
      SDL_Rect dst = { label_x, ly, s->label_w, s->label_h };
      SDL_RenderCopy(renderer, s->label_tex, NULL, &dst);
   }

   /* Track background (tertiary for visibility against panel bg) */
   SDL_SetRenderDrawColor(renderer, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B,
                          255);
   SDL_Rect track_bg = { tx, ty, tw, th };
   SDL_RenderFillRect(renderer, &track_bg);

   /* Fill (proportional to value) */
   int fill_w = (int)(s->value * (float)tw);
   if (fill_w > 0) {
      SDL_SetRenderDrawColor(renderer, s->fill_r, s->fill_g, s->fill_b, 255);
      SDL_Rect fill = { tx, ty, fill_w, th };
      SDL_RenderFillRect(renderer, &fill);
   }

   /* Thumb with lighter border for visual distinction from fill */
   int thumb_x = tx + fill_w - SLIDER_THUMB_W / 2;
   if (thumb_x < tx)
      thumb_x = tx;
   if (thumb_x + SLIDER_THUMB_W > tx + tw)
      thumb_x = tx + tw - SLIDER_THUMB_W;
   int thumb_y = ty + th / 2 - SLIDER_THUMB_H / 2;
   SDL_SetRenderDrawColor(renderer, s->fill_r, s->fill_g, s->fill_b, 255);
   SDL_Rect thumb = { thumb_x, thumb_y, SLIDER_THUMB_W, SLIDER_THUMB_H };
   SDL_RenderFillRect(renderer, &thumb);
   /* 1px lighter border so thumb is distinguishable from fill bar */
   uint8_t br = (uint8_t)(s->fill_r + (255 - s->fill_r) / 3);
   uint8_t bg = (uint8_t)(s->fill_g + (255 - s->fill_g) / 3);
   uint8_t bb = (uint8_t)(s->fill_b + (255 - s->fill_b) / 3);
   SDL_SetRenderDrawColor(renderer, br, bg, bb, 255);
   SDL_RenderDrawRect(renderer, &thumb);

   /* Percentage text (right of track, re-rendered only when integer % changes) */
   int pct = (int)(s->value * 100.0f + 0.5f);
   if (pct != s->cached_pct && font) {
      if (s->pct_tex) {
         SDL_DestroyTexture(s->pct_tex);
         s->pct_tex = NULL;
      }
      char buf[8];
      snprintf(buf, sizeof(buf), "%d%%", pct);
      SDL_Color clr = { s->fill_r, s->fill_g, s->fill_b, 255 };
      SDL_Surface *surf = TTF_RenderText_Blended(font, buf, clr);
      if (surf) {
         s->pct_tex = SDL_CreateTextureFromSurface(renderer, surf);
         s->pct_w = surf->w;
         s->pct_h = surf->h;
         SDL_FreeSurface(surf);
      }
      s->cached_pct = pct;
   }
   if (s->pct_tex) {
      int py = ty + (th - s->pct_h) / 2;
      SDL_Rect dst = { tx + tw + 12, py, s->pct_w, s->pct_h };
      SDL_RenderCopy(renderer, s->pct_tex, NULL, &dst);
   }
}

bool ui_slider_finger_down(ui_slider_t *s, int x, int y) {
   int tx = s->track_x;
   int ty = s->track_y;
   int tw = s->track_w;
   int th = s->track_h;

   /* Expanded touch zone */
   if (x >= tx && x <= tx + tw && y >= ty - SLIDER_TOUCH_PAD && y <= ty + th + SLIDER_TOUCH_PAD) {
      s->dragging = true;
      float v = (float)(x - tx) / (float)tw;
      if (v < s->min_value)
         v = s->min_value;
      if (v > 1.0f)
         v = 1.0f;
      s->value = v;
      return true;
   }
   return false;
}

bool ui_slider_finger_motion(ui_slider_t *s, int x) {
   if (!s->dragging)
      return false;
   float v = (float)(x - s->track_x) / (float)s->track_w;
   if (v < s->min_value)
      v = s->min_value;
   if (v > 1.0f)
      v = 1.0f;
   s->value = v;
   return true;
}

void ui_slider_finger_up(ui_slider_t *s) {
   s->dragging = false;
}

void ui_slider_cleanup(ui_slider_t *s) {
   if (s->label_tex) {
      SDL_DestroyTexture(s->label_tex);
      s->label_tex = NULL;
   }
   if (s->pct_tex) {
      SDL_DestroyTexture(s->pct_tex);
      s->pct_tex = NULL;
   }
}
