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

#ifndef UI_SLIDER_H
#define UI_SLIDER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
   int track_x, track_y; /* Top-left of track */
   int track_w, track_h; /* Track dimensions */
   float value;          /* 0.0-1.0 */
   float min_value;      /* Minimum value (0.0 default, 0.10 for brightness) */
   bool dragging;
   uint8_t fill_r, fill_g, fill_b;

   /* Cached textures */
   SDL_Texture *label_tex; /* "BRIGHTNESS" / "VOLUME" */
   int label_w, label_h;
   SDL_Texture *pct_tex; /* "80%" etc */
   int pct_w, pct_h;
   int cached_pct; /* -1 = dirty */
} ui_slider_t;

/**
 * @brief Initialize a slider with label text and color
 */
void ui_slider_init(ui_slider_t *s,
                    SDL_Renderer *renderer,
                    int track_x,
                    int track_y,
                    int track_w,
                    uint8_t r,
                    uint8_t g,
                    uint8_t b,
                    const char *label,
                    TTF_Font *font);

/**
 * @brief Render slider (track + fill + thumb + labels)
 */
void ui_slider_render(ui_slider_t *s, SDL_Renderer *renderer, TTF_Font *font);

/**
 * @brief Handle finger down; returns true if consumed
 */
bool ui_slider_finger_down(ui_slider_t *s, int x, int y);

/**
 * @brief Handle finger motion during drag; returns true if consumed
 */
bool ui_slider_finger_motion(ui_slider_t *s, int x);

/**
 * @brief Handle finger up
 */
void ui_slider_finger_up(ui_slider_t *s);

/**
 * @brief Destroy cached textures
 */
void ui_slider_cleanup(ui_slider_t *s);

#endif /* UI_SLIDER_H */
