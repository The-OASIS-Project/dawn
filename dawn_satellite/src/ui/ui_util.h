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
 * Shared SDL2 UI Utilities
 */

#ifndef UI_UTIL_H
#define UI_UTIL_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/**
 * @brief Render text as a white texture for later tinting via SDL_SetTextureColorMod.
 *
 * Pattern: build_white_tex() at init/state-change time, then at render time:
 *   SDL_SetTextureColorMod(tex, r, g, b) to apply theme or state color.
 * This avoids per-frame TTF_RenderUTF8_Blended calls.
 */
static inline SDL_Texture *ui_build_white_tex(SDL_Renderer *r,
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

#endif /* UI_UTIL_H */
