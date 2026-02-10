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
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ui/ui_colors.h"
#include "ui/ui_music.h"
#include "ui/ui_orb.h"
#include "ui/ui_touch.h"
#include "ui/ui_transcript.h"
#include "ws_client.h"

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
#define PANEL_HEIGHT 300
#define PANEL_ANIM_SEC 0.25
#define INFO_ROW_COUNT 5      /* Server, Device, IP, Uptime, Session */
#define ORB_HIT_RADIUS 180    /* Tap/long-press detection radius around orb center */
#define SWIPE_ZONE_FRAC 0.20f /* Top/bottom 20% of screen for swipe triggers */
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
      SDL_Texture *quick_actions_title; /* "QUICK ACTIONS" */
      SDL_Texture *box_labels[5];       /* "Music", "Lights", ... */
      int box_label_w[5];
      int box_label_h[5];
      int title_w, title_h;
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
   } panel_actions, panel_settings, panel_music;

   /* Music panel */
   ui_music_t music;
   ws_client_t *ws_client; /* WS client for music commands */
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

static float ease_out_cubic(float t) {
   float f = t - 1.0f;
   return f * f * f + 1.0f;
}

/** @brief Get panel slide offset (0.0 = hidden, 1.0 = fully visible) */
static float panel_offset(double anim_start, bool closing, double time_sec) {
   float t = (float)((time_sec - anim_start) / PANEL_ANIM_SEC);
   if (t < 0.0f)
      t = 0.0f;
   if (t > 1.0f)
      t = 1.0f;
   float eased = ease_out_cubic(t);
   return closing ? 1.0f - eased : eased;
}

static bool panel_any_open(const sdl_ui_t *ui) {
   return ui->panel_actions.visible || ui->panel_settings.visible || ui->panel_music.visible;
}

static void panel_open(sdl_ui_t *ui, bool is_actions, double time_sec) {
   /* Close the other panel if open */
   if (is_actions && ui->panel_settings.visible) {
      ui->panel_settings.closing = true;
      ui->panel_settings.anim_start = time_sec;
   } else if (!is_actions && ui->panel_actions.visible) {
      ui->panel_actions.closing = true;
      ui->panel_actions.anim_start = time_sec;
   }

   /* Close music panel when opening other panels */
   if (ui->panel_music.visible && !ui->panel_music.closing) {
      ui->panel_music.closing = true;
      ui->panel_music.anim_start = time_sec;
   }

   if (is_actions) {
      ui->panel_actions.visible = true;
      ui->panel_actions.closing = false;
      ui->panel_actions.anim_start = time_sec;
   } else {
      ui->panel_settings.visible = true;
      ui->panel_settings.closing = false;
      ui->panel_settings.anim_start = time_sec;
   }
}

static void panel_open_music(sdl_ui_t *ui, double time_sec) {
   /* Close other panels */
   if (ui->panel_actions.visible && !ui->panel_actions.closing) {
      ui->panel_actions.closing = true;
      ui->panel_actions.anim_start = time_sec;
   }
   if (ui->panel_settings.visible && !ui->panel_settings.closing) {
      ui->panel_settings.closing = true;
      ui->panel_settings.anim_start = time_sec;
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

static void panel_close(sdl_ui_t *ui, bool is_actions, double time_sec) {
   if (is_actions) {
      if (ui->panel_actions.visible && !ui->panel_actions.closing) {
         ui->panel_actions.closing = true;
         ui->panel_actions.anim_start = time_sec;
      }
   } else {
      if (ui->panel_settings.visible && !ui->panel_settings.closing) {
         ui->panel_settings.closing = true;
         ui->panel_settings.anim_start = time_sec;
      }
   }
}

/** @brief Finalize panels whose close animation is done */
static void panel_tick(sdl_ui_t *ui, double time_sec) {
   if (ui->panel_actions.closing) {
      float t = (float)((time_sec - ui->panel_actions.anim_start) / PANEL_ANIM_SEC);
      if (t >= 1.0f) {
         ui->panel_actions.visible = false;
         ui->panel_actions.closing = false;
      }
   }
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
 * Panel Rendering
 * ============================================================================= */

/** @brief Lazy-init cached panel label textures (called on render thread) */
static void panel_cache_init(sdl_ui_t *ui) {
   if (ui->panel_cache.initialized || !ui->transcript.label_font)
      return;

   SDL_Renderer *r = ui->renderer;
   TTF_Font *font = ui->transcript.label_font;
   SDL_Color primary = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B, 255 };
   SDL_Color secondary = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                           255 };

   /* "QUICK ACTIONS" title */
   SDL_Surface *surf = TTF_RenderText_Blended(font, "QUICK ACTIONS", primary);
   if (surf) {
      ui->panel_cache.quick_actions_title = SDL_CreateTextureFromSurface(r, surf);
      ui->panel_cache.title_w = surf->w;
      ui->panel_cache.title_h = surf->h;
      SDL_FreeSurface(surf);
   }

   /* Box labels */
   static const char *labels[] = { "Music", "Lights", "Thermostat", "Timer", "Settings" };
   for (int i = 0; i < 5; i++) {
      surf = TTF_RenderText_Blended(font, labels[i], secondary);
      if (surf) {
         ui->panel_cache.box_labels[i] = SDL_CreateTextureFromSurface(r, surf);
         ui->panel_cache.box_label_w[i] = surf->w;
         ui->panel_cache.box_label_h[i] = surf->h;
         SDL_FreeSurface(surf);
      }
   }

   /* AI name (upper case) — use body_font for visual hierarchy */
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
   if (ui->panel_cache.quick_actions_title)
      SDL_DestroyTexture(ui->panel_cache.quick_actions_title);
   for (int i = 0; i < 5; i++) {
      if (ui->panel_cache.box_labels[i])
         SDL_DestroyTexture(ui->panel_cache.box_labels[i]);
   }
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
   SDL_SetRenderDrawColor(r, COLOR_BG_PRIMARY_R, COLOR_BG_PRIMARY_G, COLOR_BG_PRIMARY_B, alpha);
   SDL_Rect full = { 0, 0, ui->width, ui->height };
   SDL_RenderFillRect(r, &full);
}

/** @brief Render Quick Actions panel (slides up from bottom) */
static void render_panel_actions(sdl_ui_t *ui, SDL_Renderer *r, float offset) {
   panel_cache_init(ui);
   int panel_y = ui->height - (int)(offset * PANEL_HEIGHT);

   /* Panel background */
   SDL_SetRenderDrawColor(r, COLOR_BG_SECONDARY_R, COLOR_BG_SECONDARY_G, COLOR_BG_SECONDARY_B, 240);
   SDL_Rect bg = { 0, panel_y, ui->width, PANEL_HEIGHT };
   SDL_RenderFillRect(r, &bg);

   /* Top edge highlight */
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x20, COLOR_BG_TERTIARY_G + 0x20,
                          COLOR_BG_TERTIARY_B + 0x20, 255);
   SDL_RenderDrawLine(r, 0, panel_y, ui->width, panel_y);

   /* Title (cached) + "Coming Soon" subtitle */
   if (ui->panel_cache.quick_actions_title) {
      SDL_Rect dst = { 20, panel_y + 15, ui->panel_cache.title_w, ui->panel_cache.title_h };
      SDL_RenderCopy(r, ui->panel_cache.quick_actions_title, NULL, &dst);

      /* Dim subtitle next to title */
      if (ui->transcript.label_font) {
         SDL_Color dim = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                           140 };
         SDL_Surface *sub = TTF_RenderText_Blended(ui->transcript.label_font, "Coming Soon", dim);
         if (sub) {
            SDL_Texture *stx = SDL_CreateTextureFromSurface(r, sub);
            SDL_SetTextureAlphaMod(stx, 140);
            SDL_Rect sdst = { 20 + ui->panel_cache.title_w + 12, panel_y + 15, sub->w, sub->h };
            SDL_RenderCopy(r, stx, NULL, &sdst);
            SDL_DestroyTexture(stx);
            SDL_FreeSurface(sub);
         }
      }
   }

   /* Action boxes (dimmed — not yet functional) */
   int box_w = 180, box_h = 120;
   int total_w = 5 * box_w + 4 * 15;
   int start_x = (ui->width - total_w) / 2;
   int box_y = panel_y + 55;

   for (int i = 0; i < 5; i++) {
      int bx = start_x + i * (box_w + 15);
      SDL_Rect box = { bx, box_y, box_w, box_h };

      /* Box fill (dimmed) */
      SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B, 120);
      SDL_RenderFillRect(r, &box);

      /* Box border */
      SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x15, COLOR_BG_TERTIARY_G + 0x15,
                             COLOR_BG_TERTIARY_B + 0x15, 180);
      SDL_RenderDrawRect(r, &box);

      /* Label text (cached, centered in box, dimmed) */
      if (ui->panel_cache.box_labels[i]) {
         SDL_SetTextureAlphaMod(ui->panel_cache.box_labels[i], 140);
         int lw = ui->panel_cache.box_label_w[i];
         int lh = ui->panel_cache.box_label_h[i];
         SDL_Rect dst = { bx + (box_w - lw) / 2, box_y + (box_h - lh) / 2, lw, lh };
         SDL_RenderCopy(r, ui->panel_cache.box_labels[i], NULL, &dst);
         SDL_SetTextureAlphaMod(ui->panel_cache.box_labels[i], 255);
      }
   }
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
   SDL_Color primary_clr = { COLOR_TEXT_PRIMARY_R, COLOR_TEXT_PRIMARY_G, COLOR_TEXT_PRIMARY_B,
                             255 };

   /* Draw cached label */
   if (ui->panel_cache.info_labels[label_idx]) {
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
   SDL_SetRenderDrawColor(r, COLOR_BG_SECONDARY_R, COLOR_BG_SECONDARY_G, COLOR_BG_SECONDARY_B, 240);
   SDL_Rect bg = { 0, panel_y, ui->width, PANEL_HEIGHT };
   SDL_RenderFillRect(r, &bg);

   /* Bottom edge highlight */
   int edge_y = panel_y + PANEL_HEIGHT - 1;
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x20, COLOR_BG_TERTIARY_G + 0x20,
                          COLOR_BG_TERTIARY_B + 0x20, 255);
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
   if (connected) {
      SDL_SetRenderDrawColor(r, COLOR_LISTENING_R, COLOR_LISTENING_G, COLOR_LISTENING_B, 255);
   } else {
      SDL_SetRenderDrawColor(r, COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B, 255);
   }
   for (int y = -dot_r; y <= dot_r; y++) {
      int dx = (int)sqrtf((float)(dot_r * dot_r - y * y));
      SDL_RenderDrawLine(r, dot_cx - dx, dot_cy + y, dot_cx + dx, dot_cy + y);
   }

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

   /* Dismiss pill indicator (swipe-up-to-close affordance) */
   int pill_w = 40, pill_h = 4;
   int pill_x = ui->width / 2 - pill_w / 2;
   int pill_y = panel_y + PANEL_HEIGHT - 14;
   SDL_SetRenderDrawColor(r, 0x55, 0x55, 0x55, 180);
   SDL_Rect pill = { pill_x, pill_y, pill_w, pill_h };
   SDL_RenderFillRect(r, &pill);
}

/** @brief Draw subtle swipe indicators at screen edges */
static void render_swipe_indicators(sdl_ui_t *ui, SDL_Renderer *r) {
   /* Bottom center: upward chevron (^) */
   int cx = ui->width / 2;
   int by = ui->height - 12;
   SDL_SetRenderDrawColor(r, COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B,
                          60);
   SDL_RenderDrawLine(r, cx - 12, by, cx, by - 8);
   SDL_RenderDrawLine(r, cx, by - 8, cx + 12, by);

   /* Top center: three horizontal lines (hamburger) */
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
         /* Music button tap (check first, works even when no panel open).
          * Skip if tap is inside the open music panel — let the panel's
          * own tab handler process it instead. */
         if (!ui->panel_actions.visible && !ui->panel_settings.visible) {
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
                  break;
               }
               /* Tap outside music panel - close it */
               panel_close_music(ui, time_sec);
               break;
            }

            /* Tap outside panels dismisses them */
            bool in_actions = (ui->panel_actions.visible && gesture.y > ui->height - PANEL_HEIGHT);
            bool in_settings = (ui->panel_settings.visible && gesture.y < PANEL_HEIGHT);
            if (!in_actions && !in_settings) {
               panel_close(ui, true, time_sec);
               panel_close(ui, false, time_sec);
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
            panel_close(ui, false, time_sec);
         } else if ((float)gesture.y > (float)ui->height * (1.0f - SWIPE_ZONE_FRAC)) {
            panel_open(ui, true, time_sec);
         }
         break;

      case TOUCH_GESTURE_SWIPE_DOWN:
         if (ui->panel_music.visible && !ui->panel_music.closing) {
            break; /* Swipe consumed by music panel scrolling */
         }
         if (ui->panel_actions.visible && !ui->panel_actions.closing) {
            panel_close(ui, true, time_sec);
         } else if ((float)gesture.y < (float)ui->height * SWIPE_ZONE_FRAC) {
            panel_open(ui, false, time_sec);
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
   memset(&ui->panel_actions, 0, sizeof(ui->panel_actions));
   memset(&ui->panel_settings, 0, sizeof(ui->panel_settings));
   memset(&ui->panel_music, 0, sizeof(ui->panel_music));
   LOG_INFO("SDL UI initialized (%dx%d, driver=%s)", ui->width, ui->height,
            SDL_GetCurrentVideoDriver());

   return 0;
}

/* =============================================================================
 * SDL Cleanup (called on render thread)
 * ============================================================================= */

static void sdl_cleanup_on_thread(sdl_ui_t *ui) {
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

   /* Poll spectrum data only during SPEAKING (avoid unnecessary copies) */
   if (state == VOICE_STATE_SPEAKING) {
      voice_processing_get_playback_spectrum(ui->voice_ctx, ui->spectrum, SPECTRUM_BINS);
      ui_orb_set_spectrum(&ui->orb, ui->spectrum, SPECTRUM_BINS);
   }

   /* Track state changes for idle timeout and transcript management */
   if (state != ui->last_state) {
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
   SDL_SetRenderDrawColor(r, COLOR_BG_PRIMARY_R, COLOR_BG_PRIMARY_G, COLOR_BG_PRIMARY_B, 255);
   SDL_RenderClear(r);

   /* Draw divider between panels (2px with gradient) */
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R + 0x10, COLOR_BG_TERTIARY_G + 0x10,
                          COLOR_BG_TERTIARY_B + 0x10, 255);
   SDL_RenderDrawLine(r, ORB_PANEL_WIDTH, 0, ORB_PANEL_WIDTH, ui->height);
   SDL_SetRenderDrawColor(r, COLOR_BG_TERTIARY_R, COLOR_BG_TERTIARY_G, COLOR_BG_TERTIARY_B, 180);
   SDL_RenderDrawLine(r, ORB_PANEL_WIDTH + 1, 0, ORB_PANEL_WIDTH + 1, ui->height);

   /* Render orb in left panel */
   int orb_cx = ORB_PANEL_WIDTH / 2;
   int orb_cy = ui->height / 2;
   ui_orb_render(&ui->orb, r, orb_cx, orb_cy, state, vad_prob, audio_amp, time_sec);

   /* Poll status detail for transcript display */
   voice_processing_get_status_detail(ui->voice_ctx, ui->transcript.status_detail,
                                      sizeof(ui->transcript.status_detail));

   /* Render transcript in right panel */
   ui_transcript_render(&ui->transcript, r, state);

   /* Update music playing state for transcript icon color */
   ui->transcript.music_playing = ui_music_is_playing(&ui->music);

   /* Slide-in panels: update animation, render scrim + panels */
   panel_tick(ui, time_sec);
   float act_off = ui->panel_actions.visible ? panel_offset(ui->panel_actions.anim_start,
                                                            ui->panel_actions.closing, time_sec)
                                             : 0.0f;
   float set_off = ui->panel_settings.visible ? panel_offset(ui->panel_settings.anim_start,
                                                             ui->panel_settings.closing, time_sec)
                                              : 0.0f;
   float mus_off = ui->panel_music.visible
                       ? panel_offset(ui->panel_music.anim_start, ui->panel_music.closing, time_sec)
                       : 0.0f;

   float max_off = act_off > set_off ? act_off : set_off;
   if (mus_off > max_off)
      max_off = mus_off;
   if (max_off > 0.001f) {
      render_scrim(ui, r, max_off);
   }
   if (act_off > 0.001f) {
      render_panel_actions(ui, r, act_off);
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
   if (act_off < 0.001f && set_off < 0.001f && mus_off < 0.001f) {
      render_swipe_indicators(ui, r);
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

         /* Transcript scroll via manual finger position tracking.
          * Using absolute positions instead of tfinger.dy for reliability
          * across different touch drivers (KMSDRM, evdev, etc.). */
         if (event.type == SDL_FINGERDOWN) {
            int fx = (int)(event.tfinger.x * ui->width);
            int fy = (int)(event.tfinger.y * ui->height);

            if (ui->panel_music.visible && !ui->panel_music.closing && fx >= ui->music.panel_x) {
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

      render_frame(ui, time_sec);

      /* Adaptive frame rate */
      voice_state_t state = voice_processing_get_state(ui->voice_ctx);
      double since_change = frame_start - ui->last_state_change_time;
      int target_ms = (state == VOICE_STATE_SILENCE && since_change > IDLE_TIMEOUT_SEC)
                          ? FRAME_MS_IDLE
                          : FRAME_MS_ACTIVE;

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

#ifdef HAVE_OPUS
void sdl_ui_set_music_playback(sdl_ui_t *ui, struct music_playback *pb) {
   if (!ui)
      return;
   ui->transcript.show_music_btn = true;
   ui_music_set_playback(&ui->music, pb);
}
#endif
