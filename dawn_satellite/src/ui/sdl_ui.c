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
 * SDL2 UI - Main render thread and lifecycle management
 *
 * All SDL operations (init, window, renderer, events, rendering, cleanup)
 * happen on the render thread. KMSDRM ties the DRM master and EGL context
 * to the initializing thread, so cross-thread rendering silently fails.
 */

#include "sdl_ui.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#ifdef HAVE_SDL2_GFX
#include <SDL2/SDL2_gfxPrimitives.h>
#endif
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio_playback.h"
#include "logging.h"
#include "satellite_config.h"
#include "ui/backlight.h"
#include "ui/ui_colors.h"
#include "ui/ui_music.h"
#include "ui/ui_orb.h"
#include "ui/ui_screensaver.h"
#include "ui/ui_slider.h"
#include "ui/ui_theme.h"
#include "ui/ui_touch.h"
#include "ui/ui_transcript.h"
#include "ws_client.h"

#ifdef HAVE_OPUS
#include "music_playback.h"
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define FPS_ACTIVE 30
#define FPS_IDLE 10
#define FRAME_MS_ACTIVE (1000 / FPS_ACTIVE)
#define FRAME_MS_IDLE (1000 / FPS_IDLE)
#define IDLE_TIMEOUT_SEC 5.0 /* Drop to idle FPS after this long in SILENCE */
#define RESPONSE_POLL_MS 100 /* How often to poll response text */
#define ORB_PANEL_WIDTH 400  /* Left panel for orb */

/* Touch / panel constants */
#define PANEL_HEIGHT 350
#define PANEL_ANIM_SEC 0.25

/* Theme dot picker constants (shared between render and touch) */
#define THEME_DOT_RADIUS 14
#define THEME_DOT_GAP 16
#define THEME_DOTS_CX 770 /* Center X in slider track area */
#define THEME_DOT_HIT 24  /* Touch hit half-width (48px meets Material Design 48dp) */

/* Fallback scanline half-widths for radius=14 dot fill (used without SDL2_gfx) */
#ifndef HAVE_SDL2_GFX
static const int DOT_DX[15] = { 14, 13, 13, 13, 13, 13, 12, 12, 11, 10, 9, 8, 7, 5, 0 };
#endif
#define INFO_ROW_COUNT 5      /* Server, Device, IP, Uptime, Session */
#define ORB_HIT_RADIUS 180    /* Tap/long-press detection radius around orb center */
#define SWIPE_ZONE_FRAC 0.20f /* Top 20% of screen for swipe-down trigger */
/* Music panel width = screen width (1024) minus orb area (401) = 623px.
 * Matches transcript panel width so the music overlay covers the same region. */
#define MUSIC_PANEL_WIDTH 623

/* =============================================================================
 * Internal Structure
 * ============================================================================= */

struct sdl_ui {
   /* SDL resources (created/destroyed on render thread only) */
   SDL_Window *window;
   SDL_Renderer *renderer;

   /* Render thread */
   pthread_t render_thread;
   atomic_bool running;
   atomic_int init_result; /* 0=pending, 1=success, -1=failure */
   bool thread_started;

   /* Voice context for state polling */
   voice_ctx_t *voice_ctx;

   /* Orb visualization */
   ui_orb_ctx_t orb;

   /* Transcript panel */
   ui_transcript_t transcript;

   /* Configuration */
   int width;
   int height;
   char ai_name[32];
   char font_dir[256];
   char satellite_name[64];
   char satellite_location[64];

   /* Timing */
   voice_state_t last_state;
   double last_state_change_time;

   /* Response text tracking for transcript */
   char last_response[8192];
   size_t last_response_len;
   bool response_added; /* True after adding completed response to transcript */
   double last_poll_time;

   /* Spectrum data buffer for orb visualization */
   float spectrum[SPECTRUM_BINS];

   /* Touch gesture state */
   ui_touch_state_t touch;

   /* Manual finger tracking for transcript scroll (more reliable than tfinger.dy) */
   bool finger_scrolling;
   int finger_last_y;

   /* Cached panel label textures (lazy-initialized on first render) */
   struct {
      SDL_Texture *ai_name; /* AI name (upper case, body_font) */
      int ai_name_w, ai_name_h;
      /* Settings panel info labels (label_font, WCAG AA color) */
      SDL_Texture *info_labels[INFO_ROW_COUNT];
      int info_label_w[INFO_ROW_COUNT];
      int info_label_h[INFO_ROW_COUNT];
      /* Settings panel cached value textures (invalidated on string change) */
      SDL_Texture *info_values[INFO_ROW_COUNT];
      int info_value_w[INFO_ROW_COUNT];
      int info_value_h[INFO_ROW_COUNT];
      char info_value_str[INFO_ROW_COUNT][128];
      /* Pre-rendered connection status texts */
      SDL_Texture *connected_tex;
      SDL_Texture *disconnected_tex;
      int connected_w, connected_h;
      int disconnected_w, disconnected_h;
      bool initialized;
   } panel_cache;

   /* Cached local IP address (refreshed every 60s) */
   char local_ip[46]; /* INET6_ADDRSTRLEN */
   time_t local_ip_last_poll;

   /* Cached system uptime (refreshed every 5s) */
   time_t cached_uptime;
   time_t uptime_last_poll;

   /* Slide-in panels (each tracks own animation independently) */
   struct {
      bool visible;
      bool closing;
      double anim_start;
   } panel_settings, panel_music;

   /* Music panel */
   ui_music_t music;
   ws_client_t *ws_client; /* WS client for music commands */

   /* Settings panel sliders */
   ui_slider_t brightness_slider;
   ui_slider_t volume_slider;
   bool sliders_initialized;
   audio_playback_t *audio_pb;     /* For master volume control */
   satellite_config_t *sat_config; /* For persisting UI prefs */

   /* 12/24h time format toggle */
   bool time_24h;
   float knob_anim;             /* 0.0=12h, 1.0=24h, animated toward target */
   SDL_Texture *time_label_tex; /* "TIME" — white, colormod */
   SDL_Texture *t12h_tex;       /* "12H" — white, colormod */
   SDL_Texture *t24h_tex;       /* "24H" — white, colormod */
   int time_label_w, time_label_h;
   int t12h_w, t12h_h, t24h_w, t24h_h;
   int time_toggle_row_y; /* Updated each frame for hit testing */

   /* Theme picker */
   SDL_Texture *theme_label_tex; /* "THEME" — white, colormod */
   int theme_label_w, theme_label_h;
   int theme_dots_row_y; /* Updated each frame for hit testing */

   /* Screensaver / ambient mode */
   ui_screensaver_t screensaver;

   /* Mute button (lower-center of orb panel) */
   struct {
      SDL_Texture *mic_on_tex;  /* Unmuted icon (white, 28x28) */
      SDL_Texture *mic_off_tex; /* Muted icon with slash (white, 28x28) */
      int icon_w, icon_h;
      int hit_x, hit_y, hit_w, hit_h; /* Touch target */
      double tap_time;                /* For flash feedback */
   } mute_btn;
};

/* =============================================================================
 * Helper: Get monotonic time in seconds
 * ============================================================================= */

static double get_time_sec(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* =============================================================================
 * Panel Animation Helpers
 * ============================================================================= */

/** @brief Get panel slide offset (0.0 = hidden, 1.0 = fully visible) */
static float panel_offset(double anim_start, bool closing, double time_sec) {
   float t = (float)((time_sec - anim_start) / PANEL_ANIM_SEC);
   if (t < 0.0f)
      t = 0.0f;
   if (t > 1.0f)
      t = 1.0f;
   float eased = ui_ease_out_cubic(t);
   return closing ? 1.0f - eased : eased;
}

static bool panel_any_open(const sdl_ui_t *ui) {
   return ui->panel_settings.visible || ui->panel_music.visible;
}

static void panel_open_settings(sdl_ui_t *ui, double time_sec) {
   /* Close music panel when opening settings */
   if (ui->panel_music.visible && !ui->panel_music.closing) {
      ui->panel_music.closing = true;
      ui->panel_music.anim_start = time_sec;
   }

   ui->panel_settings.visible = true;
   ui->panel_settings.closing = false;
   ui->panel_settings.anim_start = time_sec;
   backlight_open();
}

static void panel_open_music(sdl_ui_t *ui, double time_sec) {
   /* Close settings panel */
   if (ui->panel_settings.visible && !ui->panel_settings.closing) {
      ui->panel_settings.closing = true;
      ui->panel_settings.anim_start = time_sec;
      backlight_close();
   }

   ui->panel_music.visible = true;
   ui->panel_music.closing = false;
   ui->panel_music.anim_start = time_sec;
}

static void panel_close_music(sdl_ui_t *ui, double time_sec) {
   if (ui->panel_music.visible && !ui->panel_music.closing) {
      ui->panel_music.closing = true;
      ui->panel_music.anim_start = time_sec;
   }
}

static void panel_close_settings(sdl_ui_t *ui, double time_sec) {
   if (ui->panel_settings.visible && !ui->panel_settings.closing) {
      ui->panel_settings.closing = true;
      ui->panel_settings.anim_start = time_sec;
      backlight_close();
   }
}

/** @brief Finalize panels whose close animation is done */
static void panel_tick(sdl_ui_t *ui, double time_sec) {
   if (ui->panel_settings.closing) {
      float t = (float)((time_sec - ui->panel_settings.anim_start) / PANEL_ANIM_SEC);
      if (t >= 1.0f) {
         ui->panel_settings.visible = false;
         ui->panel_settings.closing = false;
      }
   }
   if (ui->panel_music.closing) {
      float t = (float)((time_sec - ui->panel_music.anim_start) / PANEL_ANIM_SEC);
      if (t >= 1.0f) {
         ui->panel_music.visible = false;
         ui->panel_music.closing = false;
      }
   }
}

/* =============================================================================
 * Volume Helper
 * ============================================================================= */

/** @brief Set master volume on both TTS and music playback (if available) */
static void set_master_volume(sdl_ui_t *ui, int pct) {
   if (ui->audio_pb)
      audio_playback_set_volume(ui->audio_pb, pct);
#ifdef HAVE_OPUS
   if (ui->music.music_pb)
      music_playback_set_volume(ui->music.music_pb, pct);
#endif
}

/* =============================================================================
 * Panel Rendering
 * ============================================================================= */

/** @brief Build a white text texture for later tinting via SDL_SetTextureColorMod */
static SDL_Texture *build_white_label(SDL_Renderer *r,
                                      TTF_Font *font,
                                      const char *text,
                                      int *w,
                                      int *h) {
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *surf = TTF_RenderText_Blended(font, text, white);
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

/** @brief Lazy-init cached panel label textures (called on render thread) */
static void panel_cache_init(sdl_ui_t *ui) {
   if (ui->panel_cache.initialized || !ui->transcript.label_font)
      return;

   SDL_Renderer *r = ui->renderer;
   TTF_Font *font = ui->transcript.label_font;
   SDL_Color primary = { 255, 255, 255, 255 }; /* White — tinted at render time for theme */

   /* AI name (upper case) — use body_font for visual hierarchy */
   SDL_Surface *surf;
   char name_upper[32];
   int ni = 0;
   for (const char *p = ui->ai_name; *p && ni < 31; p++, ni++) {
      name_upper[ni] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
   }
   name_upper[ni] = '\0';
   TTF_Font *name_font = ui->transcript.body_font ? ui->transcript.body_font : font;
   surf = TTF_RenderText_Blended(name_font, name_upper, primary);
   if (surf) {
      ui->panel_cache.ai_name = SDL_CreateTextureFromSurface(r, surf);
      ui->panel_cache.ai_name_w = surf->w;
      ui->panel_cache.ai_name_h = surf->h;
      SDL_FreeSurface(surf);
   }

   /* Settings panel info labels — brighter than secondary for WCAG AA on #1B1F24 */
   SDL_Color info_dim = { 0x8E, 0x99, 0xA4, 255 };
   static const char *info_labels[] = { "Server", "Device", "IP", "Uptime", "Session" };
   for (int i = 0; i < INFO_ROW_COUNT; i++) {
      surf = TTF_RenderText_Blended(font, info_labels[i], info_dim);
      if (surf) {
         ui->panel_cache.info_labels[i] = SDL_CreateTextureFromSurface(r, surf);
         ui->panel_cache.info_label_w[i] = surf->w;
         ui->panel_cache.info_label_h[i] = surf->h;
         SDL_FreeSurface(surf);
      }
   }

   /* Pre-render connection status texts */
   surf = TTF_RenderText_Blended(font, "Connected", info_dim);
   if (surf) {
      ui->panel_cache.connected_tex = SDL_CreateTextureFromSurface(r, surf);
      ui->panel_cache.connected_w = surf->w;
      ui->panel_cache.connected_h = surf->h;
      SDL_FreeSurface(surf);
   }
   surf = TTF_RenderText_Blended(font, "Disconnected", info_dim);
   if (surf) {
      ui->panel_cache.disconnected_tex = SDL_CreateTextureFromSurface(r, surf);
      ui->panel_cache.disconnected_w = surf->w;
      ui->panel_cache.disconnected_h = surf->h;
      SDL_FreeSurface(surf);
   }

   ui->panel_cache.initialized = true;
}

/** @brief Cleanup cached panel label textures */
static void panel_cache_cleanup(sdl_ui_t *ui) {
   for (int i = 0; i < INFO_ROW_COUNT; i++) {
      if (ui->panel_cache.info_labels[i])
         SDL_DestroyTexture(ui->panel_cache.info_labels[i]);
      if (ui->panel_cache.info_values[i])
         SDL_DestroyTexture(ui->panel_cache.info_values[i]);
   }
   if (ui->panel_cache.ai_name)
      SDL_DestroyTexture(ui->panel_cache.ai_name);
   if (ui->panel_cache.connected_tex)
      SDL_DestroyTexture(ui->panel_cache.connected_tex);
   if (ui->panel_cache.disconnected_tex)
      SDL_DestroyTexture(ui->panel_cache.disconnected_tex);
   memset(&ui->panel_cache, 0, sizeof(ui->panel_cache));
}

/** @brief Draw semi-transparent scrim overlay behind panels */
static void render_scrim(sdl_ui_t *ui, SDL_Renderer *r, float max_offset) {
   uint8_t alpha = (uint8_t)(max_offset * 150); /* 59% at full */
   ui_color_t bg0 = ui_theme_bg(0);
   SDL_SetRenderDrawColor(r, bg0.r, bg0.g, bg0.b, alpha);
   SDL_Rect full = { 0, 0, ui->width, ui->height };
   SDL_RenderFillRect(r, &full);
}

/** @brief Get system uptime from /proc/uptime (cached with 5s TTL) */
static time_t get_system_uptime(sdl_ui_t *ui) {
   time_t now = time(NULL);
   if (ui->cached_uptime > 0 && now >= ui->uptime_last_poll && (now - ui->uptime_last_poll) < 5) {
      return ui->cached_uptime;
   }

   double uptime_sec = 0.0;
   FILE *fp = fopen("/proc/uptime", "r");
   if (fp) {
      if (fscanf(fp, "%lf", &uptime_sec) != 1)
         uptime_sec = 0.0;
      fclose(fp);
   }

   ui->cached_uptime = (time_t)uptime_sec;
   ui->uptime_last_poll = now;
   return ui->cached_uptime;
}

/** @brief Format a duration in seconds into human-readable "2d 5h 14m" string */
static void format_duration(time_t seconds, char *buf, size_t size) {
   if (seconds <= 0 || size == 0) {
      if (size > 0)
         buf[0] = '\0';
      return;
   }
   int days = (int)(seconds / 86400);
   int hours = (int)((seconds % 86400) / 3600);
   int mins = (int)((seconds % 3600) / 60);

   if (days > 0) {
      snprintf(buf, size, "%dd %dh %dm", days, hours, mins);
   } else if (hours > 0) {
      snprintf(buf, size, "%dh %dm", hours, mins);
   } else if (mins > 0) {
      snprintf(buf, size, "%dm", mins);
   } else {
      snprintf(buf, size, "%ds", (int)seconds);
   }
}

/** @brief Get first non-loopback IPv4 address (cached with 60s TTL) */
static const char *get_local_ip(sdl_ui_t *ui) {
   time_t now = time(NULL);
   if (ui->local_ip[0] && now >= ui->local_ip_last_poll && (now - ui->local_ip_last_poll) < 60) {
      return ui->local_ip;
   }

   ui->local_ip[0] = '\0';
   ui->local_ip_last_poll = now;

   struct ifaddrs *ifaddr = NULL;
   if (getifaddrs(&ifaddr) == -1) {
      return "unknown";
   }

   for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
         continue;
      if (ifa->ifa_flags & IFF_LOOPBACK)
         continue;
      struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
      inet_ntop(AF_INET, &addr->sin_addr, ui->local_ip, sizeof(ui->local_ip));
      break;
   }
   freeifaddrs(ifaddr);

   return ui->local_ip[0] ? ui->local_ip : "unknown";
}

/** @brief Render a single label/value row; returns the y advance.
 *  Value textures are cached and only re-rendered when the string changes. */
static int render_info_row(sdl_ui_t *ui,
                           SDL_Renderer *r,
                           int label_idx,
                           const char *value,
                           int x,
                           int y,
                           int value_x_offset) {
   TTF_Font *font = ui->transcript.label_font;
   SDL_Color primary_clr = { 255, 255, 255, 255 }; /* White — tinted via colormod for theme */

   /* Draw cached label (white texture, tinted for theme) */
   ui_color_t txt1_clr = ui_theme_text(1);
   if (ui->panel_cache.info_labels[label_idx]) {
      SDL_SetTextureColorMod(ui->panel_cache.info_labels[label_idx], txt1_clr.r, txt1_clr.g,
                             txt1_clr.b);
      SDL_Rect dst = { x, y, ui->panel_cache.info_label_w[label_idx],
                       ui->panel_cache.info_label_h[label_idx] };
      SDL_RenderCopy(r, ui->panel_cache.info_labels[label_idx], NULL, &dst);
   }

   /* Draw value — cached texture, invalidated on string change */
   if (value && value[0] && font) {
      if (strcmp(ui->panel_cache.info_value_str[label_idx], value) != 0) {
         if (ui->panel_cache.info_values[label_idx])
            SDL_DestroyTexture(ui->panel_cache.info_values[label_idx]);
         snprintf(ui->panel_cache.info_value_str[label_idx],
                  sizeof(ui->panel_cache.info_value_str[label_idx]), "%s", value);
         SDL_Surface *surf = TTF_RenderText_Blended(font, value, primary_clr);
         if (surf) {
            ui->panel_cache.info_values[label_idx] = SDL_CreateTextureFromSurface(r, surf);
            ui->panel_cache.info_value_w[label_idx] = surf->w;
            ui->panel_cache.info_value_h[label_idx] = surf->h;
            SDL_FreeSurface(surf);
         }
      }
      if (ui->panel_cache.info_values[label_idx]) {
         ui_color_t txt0_clr = ui_theme_text(0);
         SDL_SetTextureColorMod(ui->panel_cache.info_values[label_idx], txt0_clr.r, txt0_clr.g,
                                txt0_clr.b);
         SDL_Rect dst = { x + value_x_offset, y, ui->panel_cache.info_value_w[label_idx],
                          ui->panel_cache.info_value_h[label_idx] };
         SDL_RenderCopy(r, ui->panel_cache.info_values[label_idx], NULL, &dst);
      }
   }

   return ui->panel_cache.info_label_h[label_idx] > 0 ? ui->panel_cache.info_label_h[label_idx]
                                                      : 18;
}

/** @brief Render Settings/Info panel (slides down from top) */
static void render_panel_settings(sdl_ui_t *ui, SDL_Renderer *r, float offset) {
   panel_cache_init(ui);
   int panel_y = -PANEL_HEIGHT + (int)(offset * PANEL_HEIGHT);

   /* Panel background */
   ui_color_t bg1 = ui_theme_bg(1);
   SDL_SetRenderDrawColor(r, bg1.r, bg1.g, bg1.b, 240);
   SDL_Rect bg = { 0, panel_y, ui->width, PANEL_HEIGHT };
   SDL_RenderFillRect(r, &bg);

   /* Bottom edge highlight */
   int edge_y = panel_y + PANEL_HEIGHT - 1;
   ui_color_t bg2 = ui_theme_bg(2);
   uint8_t edge_r = (uint8_t)(bg2.r + 0x20 < 255 ? bg2.r + 0x20 : 255);
   uint8_t edge_g = (uint8_t)(bg2.g + 0x20 < 255 ? bg2.g + 0x20 : 255);
   uint8_t edge_b = (uint8_t)(bg2.b + 0x20 < 255 ? bg2.b + 0x20 : 255);
   SDL_SetRenderDrawColor(r, edge_r, edge_g, edge_b, 255);
   SDL_RenderDrawLine(r, 0, edge_y, ui->width, edge_y);

   if (!ui->transcript.label_font)
      return;

   int text_x = 30;
   int text_y = panel_y + 24;
   int value_x_offset = 90; /* Aligns all values to same column */
   int row_spacing = 10;

   /* AI Name (cached, uses body_font for hierarchy) */
   if (ui->panel_cache.ai_name) {
      SDL_Rect dst = { text_x, text_y, ui->panel_cache.ai_name_w, ui->panel_cache.ai_name_h };
      SDL_RenderCopy(r, ui->panel_cache.ai_name, NULL, &dst);
      text_y += ui->panel_cache.ai_name_h + 20;
   }

   /* Connection status with colored dot — uses real WS connectivity */
   bool connected = ui->voice_ctx ? voice_processing_is_ws_connected(ui->voice_ctx) : false;

   int dot_r = 5;
   int dot_cx = text_x + dot_r;
   int dot_cy = text_y + 8;
   uint8_t dot_cr, dot_cg, dot_cb;
   if (connected) {
      dot_cr = COLOR_LISTENING_R;
      dot_cg = COLOR_LISTENING_G;
      dot_cb = COLOR_LISTENING_B;
   } else {
      dot_cr = COLOR_ERROR_R;
      dot_cg = COLOR_ERROR_G;
      dot_cb = COLOR_ERROR_B;
   }
#ifdef HAVE_SDL2_GFX
   filledCircleRGBA(r, dot_cx, dot_cy, dot_r, dot_cr, dot_cg, dot_cb, 255);
#else
   SDL_SetRenderDrawColor(r, dot_cr, dot_cg, dot_cb, 255);
   for (int y = -dot_r; y <= dot_r; y++) {
      int dx = (int)sqrtf((float)(dot_r * dot_r - y * y));
      SDL_RenderDrawLine(r, dot_cx - dx, dot_cy + y, dot_cx + dx, dot_cy + y);
   }
#endif

   /* Connection status text (pre-rendered, no per-frame texture churn) */
   SDL_Texture *status_tex = connected ? ui->panel_cache.connected_tex
                                       : ui->panel_cache.disconnected_tex;
   int status_w = connected ? ui->panel_cache.connected_w : ui->panel_cache.disconnected_w;
   int status_h = connected ? ui->panel_cache.connected_h : ui->panel_cache.disconnected_h;
   if (status_tex) {
      SDL_Rect dst = { text_x + 20, text_y, status_w, status_h };
      SDL_RenderCopy(r, status_tex, NULL, &dst);
      text_y += status_h + 18; /* Group break after connection status */
   }

   /* Info rows: Server, Device, IP, Uptime, Session */
   char buf[192];
   time_t now = time(NULL);

   /* Server */
   if (voice_processing_get_server_info(ui->voice_ctx, buf, sizeof(buf)) == 0) {
      snprintf(buf, sizeof(buf), "\xe2\x80\x94"); /* em-dash UTF-8 */
   }
   text_y += render_info_row(ui, r, 0, buf, text_x, text_y, value_x_offset) + row_spacing;

   /* Device (satellite name + location) */
   if (ui->satellite_location[0]) {
      snprintf(buf, sizeof(buf), "%s (%s)", ui->satellite_name, ui->satellite_location);
   } else {
      snprintf(buf, sizeof(buf), "%s", ui->satellite_name);
   }
   text_y += render_info_row(ui, r, 1, buf, text_x, text_y, value_x_offset) + row_spacing;

   /* IP */
   text_y += render_info_row(ui, r, 2, get_local_ip(ui), text_x, text_y, value_x_offset) +
             row_spacing;

   /* Uptime (system/OS uptime from /proc/uptime, cached 5s TTL) */
   format_duration(get_system_uptime(ui), buf, sizeof(buf));
   text_y += render_info_row(ui, r, 3, buf, text_x, text_y, value_x_offset) + row_spacing;

   /* Session (since WS connect) */
   time_t connect_time = voice_processing_get_connect_time(ui->voice_ctx);
   if (connect_time > 0) {
      format_duration(now - connect_time, buf, sizeof(buf));
   } else {
      snprintf(buf, sizeof(buf), "\xe2\x80\x94"); /* em-dash UTF-8 */
   }
   render_info_row(ui, r, 4, buf, text_x, text_y, value_x_offset);

   /* ---- Right-side sliders (brightness + volume) ---- */
   if (ui->sliders_initialized) {
      /* Update track_y each frame for panel animation offset (72px rhythm) */
      ui->brightness_slider.track_y = panel_y + 94;
      ui->volume_slider.track_y = panel_y + 166;

      ui_slider_render(&ui->brightness_slider, r, ui->transcript.label_font);
      ui_slider_render(&ui->volume_slider, r, ui->transcript.label_font);
   }

   /* 12/24h time format toggle (72px below volume slider) */
   if (ui->time_label_tex) {
      int row_y = panel_y + 238;
      ui->time_toggle_row_y = row_y;
      int slider_track_x = 620;
      int slider_track_w = 300;

      /* "TIME" label — same column as BRIGHTNESS/VOLUME */
      ui_color_t txt1 = ui_theme_text(1);
      SDL_SetTextureColorMod(ui->time_label_tex, txt1.r, txt1.g, txt1.b);
      SDL_Rect lbl_dst = { slider_track_x - SLIDER_LABEL_COL, row_y - ui->time_label_h / 2,
                           ui->time_label_w, ui->time_label_h };
      SDL_RenderCopy(r, ui->time_label_tex, NULL, &lbl_dst);

      /* Animate knob position: ease-out toward target with epsilon snap */
      float target = ui->time_24h ? 1.0f : 0.0f;
      float delta = target - ui->knob_anim;
      if (fabsf(delta) < 0.01f)
         ui->knob_anim = target;
      else
         ui->knob_anim += delta * 0.3f;

      /* Toggle track (pill-shaped, 44x24) centered in slider area */
      int toggle_w = 44, toggle_h = 24;
      int toggle_cx = slider_track_x + slider_track_w / 2;
      int toggle_x = toggle_cx - toggle_w / 2;
      int toggle_y = row_y - toggle_h / 2;
      int radius = toggle_h / 2;

      /* Lerp track color between tertiary and accent */
      ui_color_t ac = ui_theme_accent();
      float t = ui->knob_anim;
      uint8_t tr_r = (uint8_t)(0x2F + t * (ac.r - 0x2F));
      uint8_t tr_g = (uint8_t)(0x32 + t * (ac.g - 0x32));
      uint8_t tr_b = (uint8_t)(0x3C + t * (ac.b - 0x3C));
      SDL_SetRenderDrawColor(r, tr_r, tr_g, tr_b, 255);

#ifdef HAVE_SDL2_GFX
      roundedBoxRGBA(r, toggle_x, toggle_y, toggle_x + toggle_w - 1, toggle_y + toggle_h - 1,
                     radius, tr_r, tr_g, tr_b, 255);
#else
      /* Center rectangle */
      SDL_Rect trk_center = { toggle_x + radius, toggle_y, toggle_w - 2 * radius, toggle_h };
      SDL_RenderFillRect(r, &trk_center);
      /* Left + right caps (half-circles, merged to halve sqrtf calls) */
      for (int dy = -radius; dy <= radius; dy++) {
         int dx = (int)sqrtf((float)(radius * radius - dy * dy));
         int cy = toggle_y + radius + dy;
         SDL_RenderDrawLine(r, toggle_x + radius - dx, cy, toggle_x + radius, cy);
         SDL_RenderDrawLine(r, toggle_x + toggle_w - radius, cy, toggle_x + toggle_w - radius + dx,
                            cy);
      }
#endif

      /* Knob (white filled circle, 20px diameter, 2px inset) */
      int knob_r = 10;
      int knob_x_min = toggle_x + 2 + knob_r;
      int knob_x_max = toggle_x + toggle_w - 2 - knob_r;
      int knob_cx = knob_x_min + (int)(ui->knob_anim * (float)(knob_x_max - knob_x_min));
      int knob_cy = toggle_y + toggle_h / 2;
#ifdef HAVE_SDL2_GFX
      filledCircleRGBA(r, knob_cx, knob_cy, knob_r, 255, 255, 255, 255);
#else
      SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
      for (int dy = -knob_r; dy <= knob_r; dy++) {
         int dx = (int)sqrtf((float)(knob_r * knob_r - dy * dy));
         SDL_RenderDrawLine(r, knob_cx - dx, knob_cy + dy, knob_cx + dx, knob_cy + dy);
      }
#endif

      /* "12H" label (left of toggle) — accent when active, secondary when inactive */
      if (ui->t12h_tex) {
         if (ui->time_24h)
            SDL_SetTextureColorMod(ui->t12h_tex, txt1.r, txt1.g, txt1.b);
         else
            SDL_SetTextureColorMod(ui->t12h_tex, ac.r, ac.g, ac.b);
         int t12_x = toggle_x - 10 - ui->t12h_w;
         SDL_Rect t12_dst = { t12_x, row_y - ui->t12h_h / 2, ui->t12h_w, ui->t12h_h };
         SDL_RenderCopy(r, ui->t12h_tex, NULL, &t12_dst);
      }

      /* "24H" label (right of toggle) */
      if (ui->t24h_tex) {
         if (ui->time_24h)
            SDL_SetTextureColorMod(ui->t24h_tex, ac.r, ac.g, ac.b);
         else
            SDL_SetTextureColorMod(ui->t24h_tex, txt1.r, txt1.g, txt1.b);
         int t24_x = toggle_x + toggle_w + 10;
         SDL_Rect t24_dst = { t24_x, row_y - ui->t24h_h / 2, ui->t24h_w, ui->t24h_h };
         SDL_RenderCopy(r, ui->t24h_tex, NULL, &t24_dst);
      }
   }

   /* Theme dot picker (72px below time toggle) */
   if (ui->theme_label_tex) {
      int tdot_row_y = panel_y + 310;
      ui->theme_dots_row_y = tdot_row_y;
      int slider_track_x = 620;

      /* "THEME" label */
      ui_color_t ttxt = ui_theme_text(1);
      SDL_SetTextureColorMod(ui->theme_label_tex, ttxt.r, ttxt.g, ttxt.b);
      SDL_Rect tlbl = { slider_track_x - SLIDER_LABEL_COL, tdot_row_y - ui->theme_label_h / 2,
                        ui->theme_label_w, ui->theme_label_h };
      SDL_RenderCopy(r, ui->theme_label_tex, NULL, &tlbl);

      /* 5 dots centered in slider track area */
      int dot_stride = THEME_DOT_RADIUS * 2 + THEME_DOT_GAP;
      int total_dots_w = THEME_COUNT * THEME_DOT_RADIUS * 2 + (THEME_COUNT - 1) * THEME_DOT_GAP;
      int dots_start_x = THEME_DOTS_CX - total_dots_w / 2 + THEME_DOT_RADIUS;
      ui_theme_id_t current_id = ui_theme_current_id();

      for (int d = 0; d < THEME_COUNT; d++) {
         int dcx = dots_start_x + d * dot_stride;
         int dcy = tdot_row_y;
         const ui_theme_def_t *def = ui_theme_get_def((ui_theme_id_t)d);

         /* Active dot: white ring (2px wider than fill) */
         if ((ui_theme_id_t)d == current_id) {
            int ring_r = THEME_DOT_RADIUS + 2;
#ifdef HAVE_SDL2_GFX
            filledCircleRGBA(r, dcx, dcy, ring_r, 255, 255, 255, 255);
#else
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            for (int dy = -ring_r; dy <= ring_r; dy++) {
               int dx = (int)sqrtf((float)(ring_r * ring_r - dy * dy));
               SDL_RenderDrawLine(r, dcx - dx, dcy + dy, dcx + dx, dcy + dy);
            }
#endif
         }

         /* Filled dot with theme's accent color */
#ifdef HAVE_SDL2_GFX
         filledCircleRGBA(r, dcx, dcy, THEME_DOT_RADIUS, def->accent.r, def->accent.g,
                          def->accent.b, 255);
#else
         SDL_SetRenderDrawColor(r, def->accent.r, def->accent.g, def->accent.b, 255);
         for (int dy = -THEME_DOT_RADIUS; dy <= THEME_DOT_RADIUS; dy++) {
            int dx = DOT_DX[dy < 0 ? -dy : dy];
            SDL_RenderDrawLine(r, dcx - dx, dcy + dy, dcx + dx, dcy + dy);
         }
#endif
      }
   }

   /* Dismiss pill indicator (swipe-up-to-close affordance) */
   int pill_w = 40, pill_h = 4;
   int pill_x = ui->width / 2 - pill_w / 2;
   int pill_y = panel_y + PANEL_HEIGHT - 14;
   SDL_SetRenderDrawColor(r, 0x55, 0x55, 0x55, 180);
   SDL_Rect pill = { pill_x, pill_y, pill_w, pill_h };
   SDL_RenderFillRect(r, &pill);
}

/** @brief Draw subtle swipe indicator at top edge */
static void render_swipe_indicators(sdl_ui_t *ui, SDL_Renderer *r) {
   int cx = ui->width / 2;
   ui_color_t swipe_clr = ui_theme_text(1);
   SDL_SetRenderDrawColor(r, swipe_clr.r, swipe_clr.g, swipe_clr.b, 60);

   /* Top center: three horizontal lines (hamburger) — settings pull-down */
   int ty = 10;
   for (int i = 0; i < 3; i++) {
      SDL_RenderDrawLine(r, cx - 10, ty + i * 5, cx + 10, ty + i * 5);
   }
}

/* =============================================================================
 * Gesture Dispatch
 * ============================================================================= */

static void handle_gesture(sdl_ui_t *ui, touch_gesture_t gesture, double time_sec) {
   if (gesture.type == TOUCH_GESTURE_NONE)
      return;

   int orb_cx = ORB_PANEL_WIDTH / 2;
   int orb_cy = ui->height / 2;
   int dx = gesture.x - orb_cx;
   int dy = gesture.y - orb_cy;
   int dist_sq = dx * dx + dy * dy;
   bool in_orb = (dist_sq < ORB_HIT_RADIUS * ORB_HIT_RADIUS);

   switch (gesture.type) {
      case TOUCH_GESTURE_TAP:
         /* Mute button (always accessible when no panel is open) */
         if (!panel_any_open(ui) && gesture.x >= ui->mute_btn.hit_x &&
             gesture.x < ui->mute_btn.hit_x + ui->mute_btn.hit_w &&
             gesture.y >= ui->mute_btn.hit_y &&
             gesture.y < ui->mute_btn.hit_y + ui->mute_btn.hit_h) {
            bool muted = !voice_processing_is_muted(ui->voice_ctx);
            voice_processing_set_mute(ui->voice_ctx, muted);
            ui->mute_btn.tap_time = time_sec;
            LOG_INFO("UI: Mic %s", muted ? "muted" : "unmuted");
            break;
         }

         /* Music button tap (check first, works even when no panel open).
          * Skip if tap is inside the open music panel — let the panel's
          * own tab handler process it instead. */
         if (!ui->panel_settings.visible) {
            int mx = gesture.x;
            int my = gesture.y;
            bool in_music_panel = (ui->panel_music.visible && !ui->panel_music.closing &&
                                   mx >= ui->width - MUSIC_PANEL_WIDTH);
            ui_transcript_t *t = &ui->transcript;
            if (!in_music_panel && t->show_music_btn && mx >= t->music_btn_x &&
                mx < t->music_btn_x + t->music_btn_w && my >= t->music_btn_y &&
                my < t->music_btn_y + t->music_btn_h) {
               if (ui->panel_music.visible && !ui->panel_music.closing) {
                  panel_close_music(ui, time_sec);
               } else {
                  panel_open_music(ui, time_sec);
                  /* Request queue data on open; library stats are fetched
                   * when the Library tab is tapped (avoids tx_buffer overwrite
                   * since ws_client supports only one pending message). */
                  if (ui->ws_client) {
                     ws_client_send_music_queue(ui->ws_client, "list", NULL, -1);
                  }
               }
               break;
            }
         }

         if (panel_any_open(ui)) {
            /* Music panel tap handling */
            if (ui->panel_music.visible && !ui->panel_music.closing) {
               int music_panel_x = ui->width - MUSIC_PANEL_WIDTH;
               if (gesture.x >= music_panel_x) {
                  ui_music_handle_tap(&ui->music, gesture.x, gesture.y);
                  /* Check if tap was on the visualizer → go fullscreen */
                  if (ui->music.fullscreen_viz_requested) {
                     ui->music.fullscreen_viz_requested = false;
                     panel_close_music(ui, time_sec);
                     ui_screensaver_toggle_manual(&ui->screensaver, time_sec);
                  }
                  break;
               }
               /* Tap outside music panel - close it */
               panel_close_music(ui, time_sec);
               break;
            }

            /* Tap outside settings panel dismisses it */
            bool in_settings = (ui->panel_settings.visible && gesture.y < PANEL_HEIGHT);
            if (!in_settings) {
               panel_close_settings(ui, time_sec);
            }
         } else if (in_orb) {
            voice_state_t state = voice_processing_get_state(ui->voice_ctx);
            if (state == VOICE_STATE_SILENCE) {
               voice_processing_trigger_wake(ui->voice_ctx);
               ui->orb.tap_pulse_time = time_sec;
               LOG_INFO("UI: Orb tapped — manual wake");
            }
         }
         break;

      case TOUCH_GESTURE_LONG_PRESS:
         if (in_orb) {
            voice_state_t state = voice_processing_get_state(ui->voice_ctx);
            if (state == VOICE_STATE_WAITING || state == VOICE_STATE_SPEAKING ||
                state == VOICE_STATE_PROCESSING) {
               voice_processing_cancel(ui->voice_ctx);
               ui->orb.cancel_flash_time = time_sec;
               LOG_INFO("UI: Orb long-pressed — cancel");
            }
         }
         break;

      case TOUCH_GESTURE_SWIPE_UP:
         if (ui->panel_music.visible && !ui->panel_music.closing) {
            break; /* Swipe consumed by music panel scrolling */
         }
         if (ui->panel_settings.visible && !ui->panel_settings.closing) {
            panel_close_settings(ui, time_sec);
         }
         /* Intentionally unassigned when no panel open — reserved for future use */
         break;

      case TOUCH_GESTURE_SWIPE_DOWN:
         if (ui->panel_music.visible && !ui->panel_music.closing) {
            break; /* Swipe consumed by music panel scrolling */
         }
         if ((float)gesture.y < (float)ui->height * SWIPE_ZONE_FRAC) {
            panel_open_settings(ui, time_sec);
         }
         break;

      case TOUCH_GESTURE_SWIPE_RIGHT:
         if (ui->panel_music.visible && !ui->panel_music.closing) {
            panel_close_music(ui, time_sec);
         }
         break;

      default:
         break;
   }
}

/* =============================================================================
 * SDL Initialization (called on render thread)
 * ============================================================================= */

static int sdl_init_on_thread(sdl_ui_t *ui) {
   /* Hint KMSDRM backend for Pi OS Lite (no X11) */
   SDL_SetHint("SDL_VIDEO_DRIVER", "kmsdrm,x11,wayland");

   /* Disable synthetic mouse events from touch — prevents double-processing scroll */
   SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

   /* Initialize SDL */
   if (SDL_Init(SDL_INIT_VIDEO) < 0) {
      LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
      return -1;
   }

   /* Initialize SDL_ttf */
   if (TTF_Init() < 0) {
      LOG_ERROR("TTF_Init failed: %s", TTF_GetError());
      SDL_Quit();
      return -1;
   }

   /* KMSDRM needs SDL_WINDOW_FULLSCREEN (sets video mode to requested resolution).
    * FULLSCREEN_DESKTOP doesn't work reliably with KMSDRM since there's no desktop. */
   const char *driver = SDL_GetCurrentVideoDriver();
   Uint32 fs_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
   if (driver && strcmp(driver, "KMSDRM") == 0) {
      fs_flag = SDL_WINDOW_FULLSCREEN;
      LOG_INFO("SDL UI: KMSDRM detected, using SDL_WINDOW_FULLSCREEN (%dx%d)", ui->width,
               ui->height);
   }

   ui->window = SDL_CreateWindow("DAWN Satellite", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 ui->width, ui->height, fs_flag);
   if (!ui->window) {
      LOG_WARNING("Fullscreen failed, trying windowed: %s", SDL_GetError());
      ui->window = SDL_CreateWindow("DAWN Satellite", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, ui->width, ui->height, 0);
   }
   if (!ui->window) {
      LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
      TTF_Quit();
      SDL_Quit();
      return -1;
   }

   /* Hide cursor for kiosk mode */
   SDL_ShowCursor(SDL_DISABLE);

   /* Create hardware-accelerated renderer */
   ui->renderer = SDL_CreateRenderer(ui->window, -1, SDL_RENDERER_ACCELERATED);
   if (!ui->renderer) {
      LOG_WARNING("HW renderer failed, trying software: %s", SDL_GetError());
      ui->renderer = SDL_CreateRenderer(ui->window, -1, SDL_RENDERER_SOFTWARE);
   }
   if (!ui->renderer) {
      LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
      SDL_DestroyWindow(ui->window);
      TTF_Quit();
      SDL_Quit();
      return -1;
   }

   /* Enable alpha blending */
   SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);

   /* Initialize orb rendering (pre-generate glow textures) */
   ui_orb_init(&ui->orb, ui->renderer);

   /* Initialize transcript panel (right side) */
   int transcript_x = ORB_PANEL_WIDTH + 1;
   int transcript_w = ui->width - transcript_x;
   if (ui_transcript_init(&ui->transcript, ui->renderer, transcript_x, 0, transcript_w, ui->height,
                          ui->font_dir, ui->ai_name) != 0) {
      LOG_WARNING("Transcript init failed, continuing without text");
   }

   ui->last_state = VOICE_STATE_SILENCE;
   ui->last_state_change_time = get_time_sec();

   /* Initialize music panel (right-side overlay on transcript area) */
   int music_x = ui->width - MUSIC_PANEL_WIDTH;
   if (ui_music_init(&ui->music, ui->renderer, music_x, 0, MUSIC_PANEL_WIDTH, ui->height,
                     ui->font_dir) != 0) {
      LOG_WARNING("Music panel init failed, continuing without music UI");
   }
   if (ui->ws_client) {
      ui_music_set_ws_client(&ui->music, ui->ws_client);
   }

   /* Initialize touch gesture detection */
   ui_touch_init(&ui->touch, ui->width, ui->height);
   memset(&ui->panel_settings, 0, sizeof(ui->panel_settings));
   memset(&ui->panel_music, 0, sizeof(ui->panel_music));

   /* Probe sysfs backlight for brightness slider */
   if (backlight_init() == 0) {
      LOG_INFO("SDL UI: Backlight control available (sysfs)");
   } else {
      LOG_INFO("SDL UI: No sysfs backlight, using software dimming overlay");
   }

   /* Initialize settings panel sliders (renderer + fonts are ready) */
   if (ui->transcript.label_font) {
      int slider_track_x = 620;
      int slider_track_w = 300;

      /* Brightness uses fixed amber (#F0B429) — intentionally not theme-aware.
       * It is a system-level control, distinct from the media volume slider. */
      ui_slider_init(&ui->brightness_slider, ui->renderer, slider_track_x, 0, slider_track_w,
                     COLOR_THINKING_R, COLOR_THINKING_G, COLOR_THINKING_B, "BRIGHTNESS",
                     ui->transcript.label_font);
      ui->brightness_slider.min_value = 0.10f;
      if (backlight_available()) {
         ui->brightness_slider.value = (float)backlight_get() / 100.0f;
      } else if (ui->sat_config && ui->sat_config->sdl_ui.brightness_pct >= 10) {
         ui->brightness_slider.value = (float)ui->sat_config->sdl_ui.brightness_pct / 100.0f;
      } else {
         ui->brightness_slider.value = 1.0f;
      }

      ui_slider_init(&ui->volume_slider, ui->renderer, slider_track_x, 0, slider_track_w,
                     COLOR_SPEAKING_R, COLOR_SPEAKING_G, COLOR_SPEAKING_B, "VOLUME",
                     ui->transcript.label_font);
      if (ui->sat_config && ui->sat_config->sdl_ui.volume_pct >= 0) {
         ui->volume_slider.value = (float)ui->sat_config->sdl_ui.volume_pct / 100.0f;
      } else if (ui->audio_pb) {
         ui->volume_slider.value = (float)audio_playback_get_volume(ui->audio_pb) / 100.0f;
      } else {
         ui->volume_slider.value = 0.8f;
      }

      ui->sliders_initialized = true;
   }

   /* 12/24h time format toggle */
   if (ui->sat_config) {
      ui->time_24h = ui->sat_config->sdl_ui.time_24h;
   }
   ui->knob_anim = ui->time_24h ? 1.0f : 0.0f;
   ui->transcript.time_24h = ui->time_24h;
   if (ui->transcript.label_font) {
      ui->time_label_tex = build_white_label(ui->renderer, ui->transcript.label_font, "TIME",
                                             &ui->time_label_w, &ui->time_label_h);
      ui->t12h_tex = build_white_label(ui->renderer, ui->transcript.label_font, "12H", &ui->t12h_w,
                                       &ui->t12h_h);
      ui->t24h_tex = build_white_label(ui->renderer, ui->transcript.label_font, "24H", &ui->t24h_w,
                                       &ui->t24h_h);
   }

   /* Initialize theme system from config */
   if (ui->sat_config) {
      ui_theme_init(ui_theme_id_from_name(ui->sat_config->sdl_ui.theme));
   } else {
      ui_theme_init(THEME_CYAN);
   }

   /* Build "THEME" label for settings panel */
   if (ui->transcript.label_font) {
      ui->theme_label_tex = build_white_label(ui->renderer, ui->transcript.label_font, "THEME",
                                              &ui->theme_label_w, &ui->theme_label_h);
   }

   /* Sync volume slider fill color with initial theme accent */
   if (ui->sliders_initialized) {
      ui_color_t ac = ui_theme_accent();
      ui->volume_slider.fill_r = ac.r;
      ui->volume_slider.fill_g = ac.g;
      ui->volume_slider.fill_b = ac.b;
      ui->volume_slider.cached_pct = -1;
   }

   /* Initialize screensaver (after fonts/renderer ready) */
   {
      bool ss_enabled = true;
      float ss_timeout = 120.0f;
      if (ui->sat_config) {
         ss_enabled = ui->sat_config->screensaver.enabled;
         ss_timeout = (float)ui->sat_config->screensaver.timeout_sec;
      }
      ui_screensaver_init(&ui->screensaver, ui->renderer, ui->width, ui->height, ui->font_dir,
                          ui->ai_name, ss_enabled, ss_timeout);
      ui->screensaver.idle_start = get_time_sec();
      ui->screensaver.time_24h = ui->time_24h;
   }

   LOG_INFO("SDL UI initialized (%dx%d, driver=%s)", ui->width, ui->height,
            SDL_GetCurrentVideoDriver());

   return 0;
}

/* =============================================================================
 * Mute Button (lower-center of orb panel)
 * ============================================================================= */

#define MUTE_ICON_SIZE 32
#define MUTE_HIT_SIZE 56
#define MUTE_BTN_Y 548 /* Center Y of button (below orb, above screen edge) */
#define MUTE_FLASH_SEC 0.15

/** @brief Build a microphone icon as a white render-target texture */
static SDL_Texture *build_mic_icon(SDL_Renderer *r, int sz, bool muted) {
   SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sz,
                                        sz);
   if (!tex)
      return NULL;
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(r, tex);
   SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
   SDL_RenderClear(r);
   SDL_SetRenderDrawColor(r, 255, 255, 255, 255);

   int cx = sz / 2; /* 14 */

   /* Capsule body (rounded rect) */
   int cap_w = 8, cap_top = 2, cap_bot = 13;
   SDL_Rect cap = { cx - cap_w / 2, cap_top + 3, cap_w, cap_bot - cap_top - 3 };
   SDL_RenderFillRect(r, &cap);
   /* Round top */
   int cap_r = cap_w / 2;
   for (int dy = -cap_r; dy <= 0; dy++) {
      int dx = (int)sqrtf((float)(cap_r * cap_r - dy * dy));
      SDL_RenderDrawLine(r, cx - dx, cap_top + cap_r + dy, cx + dx, cap_top + cap_r + dy);
   }
   /* Round bottom */
   for (int dy = 0; dy <= cap_r; dy++) {
      int dx = (int)sqrtf((float)(cap_r * cap_r - dy * dy));
      SDL_RenderDrawLine(r, cx - dx, cap_bot - cap_r + dy, cx + dx, cap_bot - cap_r + dy);
   }

   /* Cradle (U-shape) */
   int cr = 7, cy = 13;
   SDL_RenderDrawLine(r, cx - cr, 9, cx - cr, cy);
   SDL_RenderDrawLine(r, cx - cr + 1, 9, cx - cr + 1, cy);
   SDL_RenderDrawLine(r, cx + cr, 9, cx + cr, cy);
   SDL_RenderDrawLine(r, cx + cr - 1, 9, cx + cr - 1, cy);
   for (int dy = 0; dy <= cr; dy++) {
      int dx = (int)sqrtf((float)(cr * cr - dy * dy));
      SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
   }

   /* Stem */
   SDL_Rect stem = { cx - 1, cy + cr, 2, 4 };
   SDL_RenderFillRect(r, &stem);

   /* Base */
   SDL_Rect base = { cx - 5, cy + cr + 4, 10, 2 };
   SDL_RenderFillRect(r, &base);

   /* Diagonal slash (muted only) */
   if (muted) {
      for (int off = -1; off <= 1; off++) {
         SDL_RenderDrawLine(r, sz - 4 + off, 1, 3 + off, sz - 2);
      }
   }

   SDL_SetRenderTarget(r, NULL);
   return tex;
}

static void render_mute_button(sdl_ui_t *ui, SDL_Renderer *r, double time_sec) {
   /* Lazy-init textures on first frame */
   if (!ui->mute_btn.mic_on_tex) {
      ui->mute_btn.mic_on_tex = build_mic_icon(r, MUTE_ICON_SIZE, false);
      ui->mute_btn.mic_off_tex = build_mic_icon(r, MUTE_ICON_SIZE, true);
      ui->mute_btn.icon_w = MUTE_ICON_SIZE;
      ui->mute_btn.icon_h = MUTE_ICON_SIZE;
      ui->mute_btn.hit_w = MUTE_HIT_SIZE;
      ui->mute_btn.hit_h = MUTE_HIT_SIZE;
      ui->mute_btn.hit_x = ORB_PANEL_WIDTH / 2 - MUTE_HIT_SIZE / 2;
      ui->mute_btn.hit_y = MUTE_BTN_Y - MUTE_HIT_SIZE / 2;
      ui->mute_btn.tap_time = -1.0;
   }

   bool muted = voice_processing_is_muted(ui->voice_ctx);
   SDL_Texture *tex = muted ? ui->mute_btn.mic_off_tex : ui->mute_btn.mic_on_tex;
   if (!tex)
      return;

   /* Color: white flash on tap, red when muted, secondary when unmuted */
   double since_tap = time_sec - ui->mute_btn.tap_time;
   if (since_tap >= 0.0 && since_tap < MUTE_FLASH_SEC) {
      SDL_SetTextureColorMod(tex, 0xEE, 0xEE, 0xEE);
   } else if (muted) {
      SDL_SetTextureColorMod(tex, COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B);
   } else {
      ui_color_t mute_clr = ui_theme_text(1);
      SDL_SetTextureColorMod(tex, mute_clr.r, mute_clr.g, mute_clr.b);
   }

   /* Center icon within hit area */
   int icon_x = ui->mute_btn.hit_x + (ui->mute_btn.hit_w - ui->mute_btn.icon_w) / 2;
   int icon_y = ui->mute_btn.hit_y + (ui->mute_btn.hit_h - ui->mute_btn.icon_h) / 2;
   SDL_Rect dst = { icon_x, icon_y, ui->mute_btn.icon_w, ui->mute_btn.icon_h };
   SDL_RenderCopy(r, tex, NULL, &dst);
}

/* =============================================================================
 * SDL Cleanup (called on render thread)
 * ============================================================================= */

static void sdl_cleanup_on_thread(sdl_ui_t *ui) {
   if (ui->mute_btn.mic_on_tex)
      SDL_DestroyTexture(ui->mute_btn.mic_on_tex);
   if (ui->mute_btn.mic_off_tex)
      SDL_DestroyTexture(ui->mute_btn.mic_off_tex);
   ui_screensaver_cleanup(&ui->screensaver);
   ui_slider_cleanup(&ui->brightness_slider);
   ui_slider_cleanup(&ui->volume_slider);
   if (ui->time_label_tex)
      SDL_DestroyTexture(ui->time_label_tex);
   if (ui->t12h_tex)
      SDL_DestroyTexture(ui->t12h_tex);
   if (ui->t24h_tex)
      SDL_DestroyTexture(ui->t24h_tex);
   if (ui->theme_label_tex)
      SDL_DestroyTexture(ui->theme_label_tex);
   panel_cache_cleanup(ui);
   ui_music_cleanup(&ui->music);
   ui_transcript_cleanup(&ui->transcript);
   ui_orb_cleanup(&ui->orb);

   if (ui->renderer) {
      SDL_DestroyRenderer(ui->renderer);
      ui->renderer = NULL;
   }
   if (ui->window) {
      SDL_DestroyWindow(ui->window);
      ui->window = NULL;
   }

   TTF_Quit();
   SDL_Quit();
   LOG_INFO("SDL UI cleaned up");
}

/* =============================================================================
 * Render Thread
 * ============================================================================= */

static void render_frame(sdl_ui_t *ui, double time_sec) {
   SDL_Renderer *r = ui->renderer;

   /* Poll voice state */
   voice_state_t state = voice_processing_get_state(ui->voice_ctx);
   float vad_prob = voice_processing_get_vad_probability(ui->voice_ctx);
   float audio_amp = voice_processing_get_playback_amplitude(ui->voice_ctx);

   /* Poll spectrum data only during SPEAKING and when screensaver isn't fully covering */
   bool ss_opaque = (ui->screensaver.state == SCREENSAVER_ACTIVE);
   if (state == VOICE_STATE_SPEAKING && !ss_opaque) {
      voice_processing_get_playback_spectrum(ui->voice_ctx, ui->spectrum, SPECTRUM_BINS);
      ui_orb_set_spectrum(&ui->orb, ui->spectrum, SPECTRUM_BINS);
   }

   /* Track state changes for idle timeout and transcript management */
   if (state != ui->last_state) {
      /* Only dismiss screensaver on wake word detection (leaving SILENCE),
       * not on intermediate state changes like PROCESSING→SPEAKING */
      if (ui->last_state == VOICE_STATE_SILENCE && state != VOICE_STATE_SILENCE) {
         ui_screensaver_activity(&ui->screensaver, time_sec);
      }

      /* Only reset response tracking on the initial transition into WAITING
       * (from PROCESSING), not on SPEAKING→WAITING which happens between
       * sentences during streaming TTS. */
      if (state == VOICE_STATE_WAITING && ui->last_state != VOICE_STATE_SPEAKING) {
         ui->response_added = false;
         ui->last_response_len = 0;
         ui->last_response[0] = '\0';
      }

      ui->last_state = state;
      ui->last_state_change_time = time_sec;
   }

   /* Check for new user transcription and add to transcript */
   {
      char user_buf[512];
      size_t ulen = voice_processing_get_user_text(ui->voice_ctx, user_buf, sizeof(user_buf));
      if (ulen > 0) {
         ui_transcript_add(&ui->transcript, "You", user_buf, true);
      }
   }

   /* Check response_complete flag — finalize the live transcript entry */
   if (!ui->response_added && voice_processing_is_response_complete(ui->voice_ctx)) {
      size_t len = voice_processing_get_response_text(ui->voice_ctx, ui->last_response,
                                                      sizeof(ui->last_response));
      if (len > 0) {
         /* Final update to live entry with complete text */
         ui_transcript_update_live(&ui->transcript, ui->ai_name, ui->last_response, len);
      }
      /* Mark streaming complete — triggers markdown re-render on next frame */
      ui_transcript_finalize_live(&ui->transcript);
      ui->response_added = true;
   }

   /* Poll response text and stream into transcript during WAITING/SPEAKING */
   if (!ui->response_added && (state == VOICE_STATE_WAITING || state == VOICE_STATE_SPEAKING) &&
       (time_sec - ui->last_poll_time) * 1000.0 >= RESPONSE_POLL_MS) {
      ui->last_poll_time = time_sec;
      size_t len = voice_processing_get_response_text(ui->voice_ctx, ui->last_response,
                                                      sizeof(ui->last_response));
      if (len > 0) {
         ui_transcript_update_live(&ui->transcript, ui->ai_name, ui->last_response, len);
      }
   }

   /* Clear screen with primary background */
   {
      ui_color_t bg0 = ui_theme_bg(0);
      SDL_SetRenderDrawColor(r, bg0.r, bg0.g, bg0.b, 255);
      SDL_RenderClear(r);
   }

   /* Skip main scene rendering when screensaver fully covers the screen */
   if (!ss_opaque) {
      /* Draw divider between panels (2px with gradient) */
      {
         ui_color_t bg2 = ui_theme_bg(2);
         uint8_t div_r = (uint8_t)(bg2.r + 0x10 < 255 ? bg2.r + 0x10 : 255);
         uint8_t div_g = (uint8_t)(bg2.g + 0x10 < 255 ? bg2.g + 0x10 : 255);
         uint8_t div_b = (uint8_t)(bg2.b + 0x10 < 255 ? bg2.b + 0x10 : 255);
         SDL_SetRenderDrawColor(r, div_r, div_g, div_b, 255);
         SDL_RenderDrawLine(r, ORB_PANEL_WIDTH, 0, ORB_PANEL_WIDTH, ui->height);
         SDL_SetRenderDrawColor(r, bg2.r, bg2.g, bg2.b, 180);
         SDL_RenderDrawLine(r, ORB_PANEL_WIDTH + 1, 0, ORB_PANEL_WIDTH + 1, ui->height);
      }

      /* Render orb in left panel */
      int orb_cx = ORB_PANEL_WIDTH / 2;
      int orb_cy = ui->height / 2;
      ui_orb_render(&ui->orb, r, orb_cx, orb_cy, state, vad_prob, audio_amp, time_sec);

      /* Mute button below orb */
      render_mute_button(ui, r, time_sec);

      /* Poll status detail for transcript display */
      voice_processing_get_status_detail(ui->voice_ctx, ui->transcript.status_detail,
                                         sizeof(ui->transcript.status_detail));

      /* Render transcript in right panel */
      ui_transcript_render(&ui->transcript, r, state);

      /* Update music playing state for transcript icon color */
      ui->transcript.music_playing = ui_music_is_playing(&ui->music);
      ui->transcript.mic_muted = voice_processing_is_muted(ui->voice_ctx);

      /* Slide-in panels: update animation, render scrim + panels */
      panel_tick(ui, time_sec);
      float set_off = ui->panel_settings.visible
                          ? panel_offset(ui->panel_settings.anim_start, ui->panel_settings.closing,
                                         time_sec)
                          : 0.0f;
      float mus_off = ui->panel_music.visible ? panel_offset(ui->panel_music.anim_start,
                                                             ui->panel_music.closing, time_sec)
                                              : 0.0f;

      float max_off = set_off > mus_off ? set_off : mus_off;
      if (max_off > 0.001f) {
         render_scrim(ui, r, max_off);
      }
      if (set_off > 0.001f) {
         render_panel_settings(ui, r, set_off);
      }
      if (mus_off > 0.001f) {
         /* Music panel slides in from right */
         int full_x = ui->width - MUSIC_PANEL_WIDTH;
         int anim_x = ui->width - (int)(mus_off * MUSIC_PANEL_WIDTH);
         ui->music.panel_x = anim_x > full_x ? anim_x : full_x;

         /* Feed spectrum from ALSA playback to music visualizer while music plays.
          * audio_playback_t::spectrum[] is updated per-chunk by play_stereo(). */
         if (ui_music_is_playing(&ui->music) && ui->voice_ctx) {
            float spectrum[SPECTRUM_BINS];
            voice_processing_get_playback_spectrum(ui->voice_ctx, spectrum, SPECTRUM_BINS);
            ui_music_update_spectrum(&ui->music, spectrum);
         }
         ui_music_render(&ui->music, r);
      }

      /* Swipe indicators (only when no panel visible) */
      if (set_off < 0.001f && mus_off < 0.001f) {
         render_swipe_indicators(ui, r);
      }

      /* Software dimming overlay for HDMI displays without sysfs backlight.
       * Draws a semi-transparent black rect over everything to simulate
       * brightness reduction. At 100% brightness the alpha is 0 (no-op). */
      if (!backlight_available() && ui->sliders_initialized &&
          ui->brightness_slider.value < 0.99f) {
         uint8_t alpha = (uint8_t)(255.0f * (1.0f - ui->brightness_slider.value));
         SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
         SDL_SetRenderDrawColor(r, 0, 0, 0, alpha);
         SDL_Rect dim = { 0, 0, ui->width, ui->height };
         SDL_RenderFillRect(r, &dim);
      }
   }

   /* Screensaver renders OVER everything including dimming overlay */
   {
      bool music_active = ui_music_is_playing(&ui->music);
      ui->screensaver.music_playing = music_active;
      ui_screensaver_tick(&ui->screensaver, time_sec, music_active, panel_any_open(ui));

      if (ui_screensaver_is_active(&ui->screensaver)) {
         /* Feed spectrum data to screensaver visualizer */
         if (ui->screensaver.visualizer_mode && music_active && ui->voice_ctx) {
            float spectrum[SPECTRUM_BINS];
            voice_processing_get_playback_spectrum(ui->voice_ctx, spectrum, SPECTRUM_BINS);
            ui_screensaver_update_spectrum(&ui->screensaver, spectrum, SPECTRUM_BINS);
         }

         /* Update track info from music panel state */
         if (ui->screensaver.visualizer_mode && music_active) {
            ui_screensaver_update_track(&ui->screensaver, ui->music.current_track.artist,
                                        ui->music.current_track.title,
                                        ui->music.current_track.album, time_sec);
         }

         ui_screensaver_render(&ui->screensaver, r, time_sec);
      }
   }

   SDL_RenderPresent(r);
}

static void *render_thread_func(void *arg) {
   sdl_ui_t *ui = (sdl_ui_t *)arg;

   /* Initialize all SDL resources on this thread */
   if (sdl_init_on_thread(ui) != 0) {
      atomic_store(&ui->init_result, -1);
      return NULL;
   }
   atomic_store(&ui->init_result, 1);

   LOG_INFO("SDL UI render thread started");
   double start_time = get_time_sec();

   while (ui->running) {
      double frame_start = get_time_sec();
      double time_sec = frame_start - start_time;

      /* Process SDL events including touch/mouse input */
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
         if (event.type == SDL_QUIT) {
            ui->running = false;
            break;
         }

         /* Screensaver touch handling: transport buttons pass through, others dismiss */
         if (event.type == SDL_FINGERDOWN || event.type == SDL_MOUSEBUTTONDOWN) {
            if (ui_screensaver_is_active(&ui->screensaver)) {
               /* Convert touch coordinates */
               int tx, ty;
               if (event.type == SDL_FINGERDOWN) {
                  tx = (int)(event.tfinger.x * ui->width);
                  ty = (int)(event.tfinger.y * ui->height);
               } else {
                  tx = event.button.x;
                  ty = event.button.y;
               }

               /* Check transport buttons in visualizer mode */
               bool music_active = ui_music_is_playing(&ui->music);
               const char *action = ui_screensaver_handle_tap(&ui->screensaver, tx, ty,
                                                              music_active);
               if (action && ui->ws_client) {
#ifdef HAVE_OPUS
                  if (ui->music.music_pb)
                     music_playback_flush(ui->music.music_pb);
#endif
                  ws_client_send_music_control(ui->ws_client, action, NULL);
               } else {
                  /* No transport hit — dismiss screensaver */
                  ui_screensaver_activity(&ui->screensaver, time_sec);
               }
               continue; /* Swallow all touches while screensaver active */
            }
            ui_screensaver_activity(&ui->screensaver, time_sec);
         }

         /* Transcript scroll via manual finger position tracking.
          * Using absolute positions instead of tfinger.dy for reliability
          * across different touch drivers (KMSDRM, evdev, etc.). */
         if (event.type == SDL_FINGERDOWN) {
            int fx = (int)(event.tfinger.x * ui->width);
            int fy = (int)(event.tfinger.y * ui->height);

            /* Settings panel sliders take priority when visible */
            if (ui->panel_settings.visible && !ui->panel_settings.closing && fy < PANEL_HEIGHT &&
                ui->sliders_initialized) {
               if (ui_slider_finger_down(&ui->brightness_slider, fx, fy)) {
                  backlight_set((int)(ui->brightness_slider.value * 100.0f + 0.5f));
                  ui->finger_scrolling = false;
               } else if (ui_slider_finger_down(&ui->volume_slider, fx, fy)) {
                  set_master_volume(ui, (int)(ui->volume_slider.value * 100.0f + 0.5f));
                  ui->finger_scrolling = false;
               } else if (fy >= ui->time_toggle_row_y - 22 && fy <= ui->time_toggle_row_y + 22 &&
                          fx >= 470 && fx <= 920) {
                  /* Time format toggle tap */
                  ui->time_24h = !ui->time_24h;
                  ui->transcript.time_24h = ui->time_24h;
                  ui->screensaver.time_24h = ui->time_24h;
                  ui->screensaver.cached_time[0] = '\0';
                  ui->screensaver.cached_epoch = 0;
                  if (ui->sat_config) {
                     ui->sat_config->sdl_ui.time_24h = ui->time_24h;
                     satellite_config_save_ui_prefs(ui->sat_config);
                  }
                  ui->finger_scrolling = false;
               } else if (fy >= ui->theme_dots_row_y - THEME_DOT_HIT &&
                          fy <= ui->theme_dots_row_y + THEME_DOT_HIT) {
                  /* Theme dot picker tap */
                  int dot_stride = THEME_DOT_RADIUS * 2 + THEME_DOT_GAP;
                  int total_dots_w = THEME_COUNT * THEME_DOT_RADIUS * 2 +
                                     (THEME_COUNT - 1) * THEME_DOT_GAP;
                  int dots_start_x = THEME_DOTS_CX - total_dots_w / 2 + THEME_DOT_RADIUS;
                  for (int d = 0; d < THEME_COUNT; d++) {
                     int dcx = dots_start_x + d * dot_stride;
                     if (fx >= dcx - THEME_DOT_HIT && fx <= dcx + THEME_DOT_HIT) {
                        ui_theme_set((ui_theme_id_t)d);
                        if (ui->sat_config) {
                           snprintf(ui->sat_config->sdl_ui.theme,
                                    sizeof(ui->sat_config->sdl_ui.theme), "%s",
                                    ui_theme_name((ui_theme_id_t)d));
                           satellite_config_save_ui_prefs(ui->sat_config);
                        }
                        break;
                     }
                  }
                  ui->finger_scrolling = false;
               }
            } else if (ui->panel_music.visible && !ui->panel_music.closing &&
                       fx >= ui->music.panel_x) {
               /* Finger in music panel — scroll music lists */
               ui->finger_scrolling = true;
               ui->finger_last_y = fy;
               ui_music_handle_finger_down(&ui->music, fx, fy);
            } else if (fx > ORB_PANEL_WIDTH && !panel_any_open(ui)) {
               ui->finger_scrolling = true;
               ui->finger_last_y = fy;
            } else {
               ui->finger_scrolling = false;
            }
         } else if (event.type == SDL_FINGERMOTION) {
            int new_x = (int)(event.tfinger.x * ui->width);
            int new_y = (int)(event.tfinger.y * ui->height);

            /* Settings panel slider drag */
            if (ui->sliders_initialized) {
               if (ui_slider_finger_motion(&ui->brightness_slider, new_x)) {
                  backlight_set((int)(ui->brightness_slider.value * 100.0f + 0.5f));
               } else if (ui_slider_finger_motion(&ui->volume_slider, new_x)) {
                  set_master_volume(ui, (int)(ui->volume_slider.value * 100.0f + 0.5f));
               }
            }

            /* Drag-to-seek takes priority over scroll */
            if (ui->panel_music.visible && !ui->panel_music.closing) {
               ui_music_handle_finger_motion(&ui->music, new_x, new_y);
            }

            if (ui->finger_scrolling) {
               int dy = new_y - ui->finger_last_y;
               ui->finger_last_y = new_y;
               if (dy != 0) {
                  if (ui->panel_music.visible && !ui->panel_music.closing) {
                     ui_music_scroll(&ui->music, dy);
                  } else {
                     ui_transcript_scroll(&ui->transcript, dy);
                  }
               }
            }
         } else if (event.type == SDL_FINGERUP) {
            ui->finger_scrolling = false;

            /* Persist slider values to config on release */
            if ((ui->brightness_slider.dragging || ui->volume_slider.dragging) && ui->sat_config) {
               ui->sat_config->sdl_ui.brightness_pct = (int)(ui->brightness_slider.value * 100.0f +
                                                             0.5f);
               ui->sat_config->sdl_ui.volume_pct = (int)(ui->volume_slider.value * 100.0f + 0.5f);
               satellite_config_save_ui_prefs(ui->sat_config);
            }

            ui_slider_finger_up(&ui->brightness_slider);
            ui_slider_finger_up(&ui->volume_slider);
            ui_music_handle_finger_up(&ui->music);
         } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
            /* Mouse fallback for desktop testing */
            if (ui->panel_music.visible && !ui->panel_music.closing &&
                event.motion.x >= ui->music.panel_x) {
               int dy = event.motion.yrel;
               if (dy != 0) {
                  ui_music_scroll(&ui->music, dy);
               }
            } else if (event.motion.x > ORB_PANEL_WIDTH && !panel_any_open(ui)) {
               int dy = event.motion.yrel;
               if (dy != 0) {
                  ui_transcript_scroll(&ui->transcript, dy);
               }
            }
         }

         touch_gesture_t gesture = ui_touch_process_event(&ui->touch, &event, time_sec);
         handle_gesture(ui, gesture, time_sec);
      }

      /* Per-frame long press check */
      touch_gesture_t lp = ui_touch_check_long_press(&ui->touch, time_sec);
      handle_gesture(ui, lp, time_sec);

      /* Advance theme transition and sync slider colors */
      ui_theme_tick(frame_start);
      if (ui->sliders_initialized) {
         ui_color_t ac = ui_theme_accent();
         if (ui->volume_slider.fill_r != ac.r || ui->volume_slider.fill_g != ac.g ||
             ui->volume_slider.fill_b != ac.b) {
            ui->volume_slider.fill_r = ac.r;
            ui->volume_slider.fill_g = ac.g;
            ui->volume_slider.fill_b = ac.b;
            ui->volume_slider.cached_pct = -1;
         }
      }

      render_frame(ui, time_sec);

      /* Adaptive frame rate — stay active when voice is active or music is playing */
      voice_state_t state = voice_processing_get_state(ui->voice_ctx);
      double since_change = frame_start - ui->last_state_change_time;
      bool music_active = ui_music_is_playing(&ui->music);
      int target_ms = (state == VOICE_STATE_SILENCE && since_change > IDLE_TIMEOUT_SEC &&
                       !music_active)
                          ? FRAME_MS_IDLE
                          : FRAME_MS_ACTIVE;

      /* Screensaver overrides frame rate when active */
      int ss_ms = ui_screensaver_frame_ms(&ui->screensaver);
      if (ss_ms > 0 && ss_ms < target_ms)
         target_ms = ss_ms;

      double elapsed_ms = (get_time_sec() - frame_start) * 1000.0;
      int delay = target_ms - (int)elapsed_ms;
      if (delay > 0) {
         SDL_Delay(delay);
      }
   }

   LOG_INFO("SDL UI render thread exiting");

   /* Cleanup all SDL resources on this thread */
   sdl_cleanup_on_thread(ui);

   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

sdl_ui_t *sdl_ui_init(const sdl_ui_config_t *config) {
   if (!config || !config->voice_ctx) {
      LOG_ERROR("SDL UI: NULL config or voice context");
      return NULL;
   }

   sdl_ui_t *ui = calloc(1, sizeof(sdl_ui_t));
   if (!ui) {
      LOG_ERROR("SDL UI: allocation failed");
      return NULL;
   }

   /* Store configuration (SDL init deferred to render thread) */
   ui->width = config->width > 0 ? config->width : 1024;
   ui->height = config->height > 0 ? config->height : 600;
   ui->voice_ctx = config->voice_ctx;
   snprintf(ui->ai_name, sizeof(ui->ai_name), "%s", config->ai_name ? config->ai_name : "DAWN");
   if (config->font_dir) {
      snprintf(ui->font_dir, sizeof(ui->font_dir), "%s", config->font_dir);
   }
   if (config->satellite_name) {
      snprintf(ui->satellite_name, sizeof(ui->satellite_name), "%s", config->satellite_name);
   }
   if (config->satellite_location) {
      snprintf(ui->satellite_location, sizeof(ui->satellite_location), "%s",
               config->satellite_location);
   }
   ui->sat_config = config->sat_config;

   return ui;
}

int sdl_ui_start(sdl_ui_t *ui) {
   if (!ui)
      return 1;

   ui->running = true;
   atomic_store(&ui->init_result, 0);

   if (pthread_create(&ui->render_thread, NULL, render_thread_func, ui) != 0) {
      LOG_ERROR("Failed to create render thread");
      ui->running = false;
      return 1;
   }
   ui->thread_started = true;

   /* Wait for SDL init to complete on render thread */
   while (atomic_load(&ui->init_result) == 0) {
      struct timespec ts = { 0, 10000000 }; /* 10ms */
      nanosleep(&ts, NULL);
   }

   if (atomic_load(&ui->init_result) < 0) {
      LOG_ERROR("SDL UI: init failed on render thread");
      ui->running = false;
      pthread_join(ui->render_thread, NULL);
      ui->thread_started = false;
      return 1;
   }

   return 0;
}

void sdl_ui_stop(sdl_ui_t *ui) {
   if (!ui)
      return;
   ui->running = false;
}

void sdl_ui_cleanup(sdl_ui_t *ui) {
   if (!ui)
      return;

   /* Deregister music callbacks before freeing (prevents use-after-free) */
   if (ui->ws_client) {
      ws_client_set_music_callbacks(ui->ws_client, NULL, NULL, NULL, NULL, NULL);
      ui_music_set_ws_client(&ui->music, NULL);
      ui->ws_client = NULL;
   }

   /* Stop and join render thread (SDL cleanup happens on that thread) */
   ui->running = false;
   if (ui->thread_started) {
      pthread_join(ui->render_thread, NULL);
      ui->thread_started = false;
   }

   free(ui);
}

void sdl_ui_add_transcript(sdl_ui_t *ui, const char *role, const char *text) {
   if (!ui || !role || !text)
      return;

   bool is_user = (strcmp(role, "You") == 0);
   ui_transcript_add(&ui->transcript, role, text, is_user);
}

/* =============================================================================
 * Music Callback Bridges (called from WS thread -> updates UI state)
 * ============================================================================= */

static void music_state_cb(const music_state_update_t *state, void *user_data) {
   sdl_ui_t *ui = (sdl_ui_t *)user_data;
   ui_music_on_state(&ui->music, state);
}

static void music_position_cb(float position_sec, void *user_data) {
   sdl_ui_t *ui = (sdl_ui_t *)user_data;
   ui_music_on_position(&ui->music, position_sec);
}

static void music_queue_cb(const music_queue_update_t *queue, void *user_data) {
   sdl_ui_t *ui = (sdl_ui_t *)user_data;
   ui_music_on_queue(&ui->music, queue);
}

static void music_library_cb(const music_library_update_t *lib, void *user_data) {
   sdl_ui_t *ui = (sdl_ui_t *)user_data;
   ui_music_on_library(&ui->music, lib);
}

void sdl_ui_set_ws_client(sdl_ui_t *ui, struct ws_client *client) {
   if (!ui || !client)
      return;

   ui->ws_client = client;
   ui_music_set_ws_client(&ui->music, client);

   /* Register music callbacks so ws_client routes parsed data to our UI */
   ws_client_set_music_callbacks(client, music_state_cb, music_position_cb, music_queue_cb,
                                 music_library_cb, ui);
}

void sdl_ui_set_audio_playback(sdl_ui_t *ui, struct audio_playback *pb) {
   if (!ui)
      return;
   ui->audio_pb = (audio_playback_t *)pb;

   /* Apply saved volume from config */
   if (pb && ui->sliders_initialized) {
      int vol = (int)(ui->volume_slider.value * 100.0f + 0.5f);
      set_master_volume(ui, vol);
   }
}

#ifdef HAVE_OPUS
void sdl_ui_set_music_playback(sdl_ui_t *ui, struct music_playback *pb) {
   if (!ui)
      return;
   ui->transcript.show_music_btn = true;
   ui_music_set_playback(&ui->music, pb);

   /* Apply saved volume from config */
   if (pb && ui->sliders_initialized)
      music_playback_set_volume(pb, (int)(ui->volume_slider.value * 100.0f + 0.5f));
}
#endif
