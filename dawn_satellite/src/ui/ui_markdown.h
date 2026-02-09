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
 * Supports: **bold**, *italic*, ***bold italic***, `code`, bullet lists.
 * Renders styled text spans onto a single SDL_Surface, then creates one
 * SDL_TEXTUREACCESS_STATIC texture. No GPU render-target churn.
 */

#ifndef UI_MARKDOWN_H
#define UI_MARKDOWN_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   MD_STYLE_NORMAL = 0,
   MD_STYLE_BOLD,
   MD_STYLE_ITALIC,
   MD_STYLE_BOLD_ITALIC,
   MD_STYLE_CODE
} md_style_t;

#define MD_STYLE_COUNT 5

typedef struct {
   TTF_Font *fonts[MD_STYLE_COUNT]; /* regular, bold, italic, bold_italic, code */
   int line_height;                 /* Max line height across all fonts */
   int space_w;                     /* Space width for regular font */
} md_fonts_t;

/**
 * @brief Initialize markdown font set
 *
 * Loads bold/italic/bold-italic from font_dir if available (SourceSans3-*.ttf),
 * falling back to synthesized styles via TTF_SetFontStyle. Code font uses
 * IBMPlexMono-Regular.ttf at body_size-2.
 *
 * @return 0 on success, 1 if no fonts could be loaded
 */
int md_fonts_init(md_fonts_t *fonts, const char *font_dir, int body_size);

/**
 * @brief Cleanup markdown font resources
 */
void md_fonts_cleanup(md_fonts_t *fonts);

/**
 * @brief Render markdown-styled text into a single texture
 *
 * Parses inline markdown (bold, italic, code, bullets), word-wraps to
 * wrap_width, composites all words onto a scratch SDL_Surface, and creates
 * one static texture.
 *
 * @param renderer   SDL renderer (for texture creation only)
 * @param fonts      Initialized markdown font set
 * @param text       Input text with markdown
 * @param color      Normal text color
 * @param bold_color Brighter color for bold/bold-italic text
 * @param wrap_width Maximum line width in pixels
 * @param out_w      Output: texture width
 * @param out_h      Output: texture height
 * @return SDL_Texture* or NULL on failure. Caller owns the texture.
 */
SDL_Texture *md_render_text(SDL_Renderer *renderer,
                            const md_fonts_t *fonts,
                            const char *text,
                            SDL_Color color,
                            SDL_Color bold_color,
                            int wrap_width,
                            int *out_w,
                            int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* UI_MARKDOWN_H */
