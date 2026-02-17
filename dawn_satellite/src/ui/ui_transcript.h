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

#include "ui/ui_markdown.h"
#include "voice_processing.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRANSCRIPT_MAX_ENTRIES 40
#define TRANSCRIPT_MAX_TEXT 4096

/**
 * @brief Single conversation entry
 */
typedef struct {
   char role[32];                  /* "You" or AI name */
   char text[TRANSCRIPT_MAX_TEXT]; /* Message content */
   bool is_user;                   /* true = user, false = AI */
   bool is_streaming;              /* true = still receiving streamed text */
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
   md_fonts_t md_fonts;  /* Markdown font set (regular/bold/italic/code) */

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

   /* Scroll state */
   int scroll_offset; /* Pixels scrolled back from bottom (0 = auto-scroll to newest) */
   int total_height;  /* Total rendered content height (for scroll bounds) */
   bool auto_scroll;  /* True when following newest content */

   /* Music button in status bar (hit area for sdl_ui.c tap detection) */
   int music_btn_x, music_btn_y, music_btn_w, music_btn_h;
   bool show_music_btn;         /* True when Opus is available and music playback initialized */
   bool music_playing;          /* Controls icon color: accent when playing, secondary otherwise */
   bool mic_muted;              /* True when mic is muted (shows red dot in status bar) */
   SDL_Texture *music_icon_tex; /* White "♪" glyph, tinted via SDL_SetTextureColorMod */
   int music_icon_w, music_icon_h;
   bool time_24h;  /* Use 24-hour time format */
   bool connected; /* WebSocket connection to daemon (false = show OFFLINE) */

   /* Cached header textures (white, tinted via SDL_SetTextureColorMod) */
   SDL_Texture *cached_state_tex; /* "[LISTENING]" etc. */
   int cached_state_w, cached_state_h;
   voice_state_t cached_state_val; /* Key: which state was rendered */
   bool cached_state_muted;        /* Key: muted flag at render time */
   bool cached_state_connected;    /* Key: connection status at render time */

   SDL_Texture *cached_muted_tex; /* "[MUTED]" label (static, rendered once) */
   int cached_muted_w, cached_muted_h;

   SDL_Texture *cached_time_tex; /* "Mon Feb 15  14:30" etc. */
   int cached_time_w, cached_time_h;
   int cached_time_min; /* Key: minute of hour (invalidate on change) */

   SDL_Texture *cached_detail_tex; /* Status detail text */
   int cached_detail_w, cached_detail_h;
   char cached_detail_str[128]; /* Key: previous detail string */
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
 * @brief Mark the most recent AI entry as finalized (streaming complete)
 *
 * Clears the is_streaming flag and invalidates the cache so the entry
 * re-renders with full markdown styling on the next frame.
 */
void ui_transcript_finalize_live(ui_transcript_t *t);

/**
 * @brief Scroll the transcript by a delta (positive = scroll up into history)
 */
void ui_transcript_scroll(ui_transcript_t *t, int delta_y);

/**
 * @brief Snap transcript back to auto-scroll mode (follow newest)
 */
void ui_transcript_scroll_to_bottom(ui_transcript_t *t);

/**
 * @brief Render the transcript panel
 */
void ui_transcript_render(ui_transcript_t *t, SDL_Renderer *renderer, voice_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* UI_TRANSCRIPT_H */
