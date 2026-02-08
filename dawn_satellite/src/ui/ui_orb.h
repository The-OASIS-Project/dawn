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
 * Orb Visualization - Core, glow, ring segments, and animations
 */

#ifndef UI_ORB_H
#define UI_ORB_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "ui/ui_colors.h"
#include "voice_processing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pre-rendered glow textures - one per state color */
#define NUM_GLOW_TEXTURES 5

/**
 * @brief Orb rendering context (all state is instance-local)
 */
typedef struct {
   SDL_Texture *glow_textures[NUM_GLOW_TEXTURES];
   ui_color_t glow_colors[NUM_GLOW_TEXTURES];
   ui_color_t current_color;
   ui_color_t target_color;
   double color_transition_start;
   bool color_transitioning;
} ui_orb_ctx_t;

/**
 * @brief Initialize orb rendering context
 *
 * Pre-generates glow textures and trig lookup tables.
 *
 * @param ctx Orb context to initialize
 * @param renderer SDL renderer for texture creation
 */
void ui_orb_init(ui_orb_ctx_t *ctx, SDL_Renderer *renderer);

/**
 * @brief Cleanup orb resources (destroy textures)
 *
 * @param ctx Orb context
 */
void ui_orb_cleanup(ui_orb_ctx_t *ctx);

/**
 * @brief Render the orb visualization
 *
 * @param ctx Orb context
 * @param renderer SDL renderer
 * @param cx Center X of orb
 * @param cy Center Y of orb
 * @param state Current voice processing state
 * @param vad_prob Current VAD speech probability (0.0-1.0)
 * @param audio_amp Current audio playback amplitude (0.0-1.0, for SPEAKING state)
 * @param time_sec Monotonic time for animation
 */
void ui_orb_render(ui_orb_ctx_t *ctx,
                   SDL_Renderer *renderer,
                   int cx,
                   int cy,
                   voice_state_t state,
                   float vad_prob,
                   float audio_amp,
                   double time_sec);

#ifdef __cplusplus
}
#endif

#endif /* UI_ORB_H */
