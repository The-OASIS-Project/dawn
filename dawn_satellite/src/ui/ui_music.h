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
 * Music Player Panel for SDL2 UI
 *
 * Struct definition exposed here so sdl_ui.c can embed by value.
 * Data types live in music_types.h (no SDL dependency) for ws_client.h.
 */

#ifndef UI_MUSIC_H
#define UI_MUSIC_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pthread.h>

#include "ui/music_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct ws_client;
struct music_playback;

/* Visualizer bar count (used in struct definition) */
#define MUSIC_VIZ_BAR_COUNT 32

struct ui_music {
   pthread_mutex_t mutex;
   SDL_Renderer *renderer;
   TTF_Font *label_font;
   TTF_Font *body_font;
   int panel_x, panel_y, panel_w, panel_h;

   /* Current tab */
   music_tab_t active_tab;

   /* Now Playing state */
   bool playing;
   bool paused;
   music_track_t current_track;
   float position_sec;
   float duration_sec;
   char source_format[16];
   int source_rate;
   int bitrate;
   char bitrate_mode[8];

   /* Queue */
   music_track_t queue[MUSIC_MAX_QUEUE];
   int queue_count;
   int queue_index;

   /* Library browse */
   music_browse_type_t browse_type;
   music_browse_item_t browse_items[MUSIC_MAX_RESULTS];
   int browse_count;
   music_track_t *browse_tracks; /* Dynamically allocated, capacity = browse_tracks_cap */
   int browse_track_count;
   int browse_tracks_cap;
   int browse_total_count;   /* Total tracks available on server */
   bool browse_loading_more; /* Prevent duplicate load-more requests */

   /* Library stats */
   int stat_tracks;
   int stat_artists;
   int stat_albums;

   /* Scroll state for queue/library lists */
   int scroll_offset;
   int total_list_height;

   /* Visualizer */
   float viz_bars[MUSIC_VIZ_BAR_COUNT];
   float viz_targets[MUSIC_VIZ_BAR_COUNT];
   uint32_t viz_last_update;

   /* Shuffle / Repeat */
   bool shuffle;
   int repeat_mode; /* 0=none, 1=all, 2=one */

   /* Texture cache (invalidated on state change) */
   SDL_Texture *title_tex;
   SDL_Texture *artist_tex;
   SDL_Texture *album_tex;
   int title_w, title_h;
   int artist_w, artist_h;
   int album_w, album_h;
   bool cache_dirty;
   char cached_title[MUSIC_MAX_TITLE];
   char cached_artist[MUSIC_MAX_ARTIST];
   char cached_album[MUSIC_MAX_ALBUM];

   /* Static text caches (rendered white, tinted via SDL_SetTextureColorMod) */
   SDL_Texture *tab_tex[3]; /* Playing, Queue, Library */
   int tab_tex_w[3], tab_tex_h[3];
   SDL_Texture *transport_tex[4]; /* 0=prev, 1=play, 2=pause, 3=next */
   int transport_tex_w[4], transport_tex_h[4];
   SDL_Texture *shuffle_icon_tex;    /* White shuffle arrows (SDL primitives) */
   SDL_Texture *repeat_icon_tex;     /* White repeat-all loop (SDL primitives) */
   SDL_Texture *repeat_one_icon_tex; /* White repeat-one loop + "1" */

   /* Static label caches (white text, tinted at render time) */
#define MUSIC_SLABEL_COUNT 5
   SDL_Texture *slabel_tex[MUSIC_SLABEL_COUNT];
   int slabel_w[MUSIC_SLABEL_COUNT], slabel_h[MUSIC_SLABEL_COUNT];

   bool static_cache_ready;

   /* Transport button positions (set during render, used by touch handler) */
   int transport_btn_y; /* Y position of transport buttons */
   int progress_bar_y;  /* Y position of progress bar */
   int progress_bar_x;  /* X position of progress bar left edge */
   int progress_bar_w;  /* Width of progress bar */
   int toggle_btn_y;    /* Y position of shuffle/repeat row */

   /* Transport button press state */
   bool btn_pressed;
   int btn_pressed_id; /* 0=prev, 1=play, 2=next, 3=shuffle, 4=repeat */

   /* Seek drag state */
   bool seeking; /* True while finger is dragging the progress bar */

   /* Scroll indicator fade */
   uint32_t last_scroll_ms; /* SDL_GetTicks() of last scroll event */

   /* Tap debounce */
   uint32_t last_tap_ms;

   /* Add-to-queue flash feedback */
   int add_flash_row;     /* Row index that was just added (-1 = none) */
   uint32_t add_flash_ms; /* SDL_GetTicks() when flash started */

   /* WS client for sending commands */
   struct ws_client *ws;

   /* Music playback engine (for volume, flush, visualizer) */
   struct music_playback *music_pb;
   int volume;
};

typedef struct ui_music ui_music_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int ui_music_init(ui_music_t *m,
                  SDL_Renderer *renderer,
                  int x,
                  int y,
                  int w,
                  int h,
                  const char *font_dir);
void ui_music_cleanup(ui_music_t *m);

/* =============================================================================
 * Rendering
 * ============================================================================= */

void ui_music_render(ui_music_t *m, SDL_Renderer *renderer);

/* =============================================================================
 * Touch — returns true if tap was handled
 * ============================================================================= */

bool ui_music_handle_tap(ui_music_t *m, int x, int y);
void ui_music_handle_finger_down(ui_music_t *m, int x, int y);
void ui_music_handle_finger_up(ui_music_t *m);
void ui_music_handle_finger_motion(ui_music_t *m, int x, int y);
void ui_music_scroll(ui_music_t *m, int dy);

/* =============================================================================
 * State Updates (called from WS callback with pre-parsed typed structs)
 * ============================================================================= */

void ui_music_on_state(ui_music_t *m, const music_state_update_t *state);
void ui_music_on_position(ui_music_t *m, float position_sec);
void ui_music_on_queue(ui_music_t *m, const music_queue_update_t *queue);
void ui_music_on_library(ui_music_t *m, const music_library_update_t *lib);

/* =============================================================================
 * WS Client Connection (for sending commands)
 * ============================================================================= */

void ui_music_set_ws_client(ui_music_t *m, struct ws_client *client);

/**
 * @brief Check if music is currently playing (for icon color in status bar)
 */
bool ui_music_is_playing(ui_music_t *m);

/**
 * @brief Set music playback engine for volume, flush, and visualizer
 */
void ui_music_set_playback(ui_music_t *m, struct music_playback *pb);

/**
 * @brief Update visualizer spectrum from playback engine
 * Call from SDL render loop when music is playing.
 */
void ui_music_update_spectrum(ui_music_t *m, const volatile float *spectrum64);

#ifdef __cplusplus
}
#endif

#endif /* UI_MUSIC_H */
