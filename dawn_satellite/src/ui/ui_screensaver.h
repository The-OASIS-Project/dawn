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
 * Screensaver / Ambient Mode for SDL2 UI
 *
 * Two modes:
 * - Clock: time/date centered with Lissajous drift, "D.A.W.N." corner watermarks
 * - Visualizer: fullscreen rainbow FFT spectrum using all 64 Goertzel bins
 *
 * Activates after idle timeout (no touch/voice). Visualizer mode also
 * triggerable manually via tap on music panel visualizer.
 */

#ifndef UI_SCREENSAVER_H
#define UI_SCREENSAVER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <time.h>

#include "spectrum_defs.h"
#include "ui/ui_colors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   SCREENSAVER_OFF,
   SCREENSAVER_FADING_IN,
   SCREENSAVER_ACTIVE,
   SCREENSAVER_FADING_OUT,
} screensaver_state_t;

typedef struct {
   screensaver_state_t state;
   bool visualizer_mode; /* true = rainbow viz, false = clock */

   /* Timing */
   double fade_start;
   double idle_start; /* Last user activity (touch or voice) */
   bool enabled;
   float timeout_sec; /* Config: idle timeout (default 120s) */

   /* Clock mode */
   TTF_Font *clock_font; /* ~80pt for time */
   TTF_Font *date_font;  /* ~24pt for date + watermark */
   SDL_Texture *time_tex, *date_tex;
   int time_w, time_h, date_w, date_h;
   SDL_Texture *watermark_tex; /* "D.A.W.N." corner watermark (bold, 32pt) */
   int watermark_w, watermark_h;
   int watermark_corner;     /* 0=TL, 1=TR, 2=BL, 3=BR — randomized each cycle */
   int watermark_last_cycle; /* Last completed pulse cycle (for corner switching) */
   char cached_time[8];      /* "HH:MM" - re-render on change */
   char cached_date[32];     /* "Tuesday, Feb 11" */
   time_t cached_epoch;      /* Gate time()/localtime() to once per second */
   char ai_name[32];
   float drift_x, drift_y; /* Lissajous burn-in prevention */

   /* Visualizer mode */
   float viz_bars[SPECTRUM_BINS];  /* Smoothed display values */
   float peak_hold[SPECTRUM_BINS]; /* Peak position per bar */
   float peak_age[SPECTRUM_BINS];  /* Seconds since peak was set */
   float hue_offset;               /* Slowly rotating rainbow offset */
   ui_color_t hsv_lut[360];        /* Precomputed rainbow palette */
   uint32_t viz_last_render;       /* SDL_GetTicks() for frame-rate independent smoothing */

   /* Track info (two-line: title bold, album/artist below, lower-left) */
   TTF_Font *track_font; /* ~36pt bold for track title */
   char track_artist[128];
   char track_title[128];
   char track_album[128];
   SDL_Texture *track_title_tex; /* Large title line */
   SDL_Texture *track_sub_tex;   /* Smaller "Album - Artist" line */
   int track_title_w, track_title_h;
   int track_sub_w, track_sub_h;
   double track_change_time; /* When track info last changed */
   bool track_dirty;

   /* Transport controls (visualizer mode, lower-right) */
   SDL_Texture *transport_tex[4]; /* 0=prev, 1=play, 2=pause, 3=next */
   int transport_sz;              /* Icon size (built once) */
   bool music_playing;            /* Cached: play vs pause icon selection */

   /* Manual trigger (independent of idle timer) */
   bool manual;

   SDL_Renderer *renderer; /* Cached for internal texture rebuilds */
   int screen_w, screen_h;
} ui_screensaver_t;

/**
 * @brief Initialize screensaver state and load fonts
 */
int ui_screensaver_init(ui_screensaver_t *ss,
                        SDL_Renderer *renderer,
                        int w,
                        int h,
                        const char *font_dir,
                        const char *ai_name,
                        bool enabled,
                        float timeout_sec);

/**
 * @brief Free fonts and textures
 */
void ui_screensaver_cleanup(ui_screensaver_t *ss);

/**
 * @brief Reset idle timer on user activity; fade out if active
 */
void ui_screensaver_activity(ui_screensaver_t *ss, double time_sec);

/**
 * @brief State machine tick - handles idle timeout and mode transitions
 */
void ui_screensaver_tick(ui_screensaver_t *ss,
                         double time_sec,
                         bool music_playing,
                         bool panels_open);

/**
 * @brief Render screensaver (clock or visualizer) with fade overlay
 */
void ui_screensaver_render(ui_screensaver_t *ss, SDL_Renderer *renderer, double time_sec);

/**
 * @brief Feed FFT spectrum data for visualizer mode
 */
void ui_screensaver_update_spectrum(ui_screensaver_t *ss, const float *spectrum, int count);

/**
 * @brief Update track info for visualizer overlay pill
 */
void ui_screensaver_update_track(ui_screensaver_t *ss,
                                 const char *artist,
                                 const char *title,
                                 const char *album,
                                 double time_sec);

/**
 * @brief Check if screensaver is active (FADING_IN, ACTIVE, or FADING_OUT)
 */
bool ui_screensaver_is_active(const ui_screensaver_t *ss);

/**
 * @brief Toggle manual fullscreen visualizer (long-press music icon)
 */
void ui_screensaver_toggle_manual(ui_screensaver_t *ss, double time_sec);

/**
 * @brief Handle tap during active visualizer screensaver
 *
 * Checks if tap hits a transport control button (prev/play-pause/next).
 * Returns the action string for ws_client_send_music_control, or NULL
 * if the tap didn't hit any button (caller should dismiss screensaver).
 *
 * @param ss Screensaver context
 * @param x Tap X coordinate
 * @param y Tap Y coordinate
 * @param playing True if music is currently playing (for play/pause toggle)
 * @return "previous", "play", "pause", "next", or NULL
 */
const char *ui_screensaver_handle_tap(const ui_screensaver_t *ss, int x, int y, bool playing);

/**
 * @brief Get target frame interval based on screensaver state
 * @return 33 for visualizer, 100 for clock, 0 if screensaver off
 */
int ui_screensaver_frame_ms(const ui_screensaver_t *ss);

#ifdef __cplusplus
}
#endif

#endif /* UI_SCREENSAVER_H */
