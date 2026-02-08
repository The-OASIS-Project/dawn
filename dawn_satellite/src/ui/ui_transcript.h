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
 * Transcript Panel Rendering for SDL2 UI
 */

#ifndef UI_TRANSCRIPT_H
#define UI_TRANSCRIPT_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pthread.h>
#include <stdbool.h>

#include "voice_processing.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRANSCRIPT_MAX_ENTRIES 20
#define TRANSCRIPT_MAX_TEXT 1024

/**
 * @brief Single conversation entry
 */
typedef struct {
   char role[32];                  /* "You" or AI name */
   char text[TRANSCRIPT_MAX_TEXT]; /* Message content */
   bool is_user;                   /* true = user, false = AI */
   SDL_Texture *cached_texture;    /* Cached rendered text (NULL = needs re-render) */
   int cached_w;                   /* Width of cached texture */
   int cached_h;                   /* Height of cached texture */
   SDL_Texture *cached_role_tex;   /* Cached rendered role label */
   int cached_role_w;              /* Width of cached role texture */
   int cached_role_h;              /* Height of cached role texture */
} transcript_entry_t;

/**
 * @brief Transcript panel context
 */
typedef struct {
   transcript_entry_t entries[TRANSCRIPT_MAX_ENTRIES];
   int entry_count; /* Total entries in buffer */
   int write_index; /* Next write position (circular) */
   pthread_mutex_t mutex;

   /* Fonts */
   TTF_Font *label_font; /* State label (IBM Plex Mono, 14px) */
   TTF_Font *body_font;  /* Transcript body (Source Sans 3, 18px) */

   /* Layout */
   int panel_x;    /* Left edge of transcript panel */
   int panel_y;    /* Top edge */
   int panel_w;    /* Width */
   int panel_h;    /* Height */
   int padding;    /* Inner padding */
   int wrap_width; /* Text wrap width (panel_w - 2*padding) */

   /* Renderer for texture caching */
   SDL_Renderer *renderer;

   /* AI name for display */
   char ai_name[32];

   /* WiFi signal quality (polled once per second) */
   time_t last_wifi_poll;
   int wifi_quality; /* 0-100, or -1 if no wireless */

   /* Status detail from daemon (tool calls, thinking info) */
   char status_detail[128];
} ui_transcript_t;

/**
 * @brief Initialize transcript panel
 */
int ui_transcript_init(ui_transcript_t *t,
                       SDL_Renderer *renderer,
                       int panel_x,
                       int panel_y,
                       int panel_w,
                       int panel_h,
                       const char *font_dir,
                       const char *ai_name);

/**
 * @brief Cleanup transcript resources
 */
void ui_transcript_cleanup(ui_transcript_t *t);

/**
 * @brief Add an entry to the transcript (thread-safe)
 */
void ui_transcript_add(ui_transcript_t *t, const char *role, const char *text, bool is_user);

/**
 * @brief Update the most recent non-user entry with new text (thread-safe)
 *
 * Used for streaming: updates the AI response text as it arrives,
 * invalidating the cached texture so it re-renders next frame.
 * If no AI entry exists yet, creates one.
 */
void ui_transcript_update_live(ui_transcript_t *t,
                               const char *role,
                               const char *text,
                               size_t text_len);

/**
 * @brief Render the transcript panel
 */
void ui_transcript_render(ui_transcript_t *t, SDL_Renderer *renderer, voice_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* UI_TRANSCRIPT_H */
