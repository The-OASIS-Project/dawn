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
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ui/ui_colors.h"
#include "ui/ui_orb.h"
#include "ui/ui_touch.h"
#include "ui/ui_transcript.h"

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
#define PANEL_HEIGHT 240
#define PANEL_ANIM_SEC 0.25
#define ORB_HIT_RADIUS 180    /* Tap/long-press detection radius around orb center */
#define SWIPE_ZONE_FRAC 0.20f /* Top/bottom 20% of screen for swipe triggers */

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

   /* Cached panel label textures (lazy-initialized on first render) */
   struct {
      SDL_Texture *quick_actions_title; /* "QUICK ACTIONS" */
      SDL_Texture *box_labels[5];       /* "Music", "Lights", ... */
      int box_label_w[5];
      int box_label_h[5];
      int title_w, title_h;
      SDL_Texture *ai_name; /* AI name (upper case) */
      int ai_name_w, ai_name_h;
      bool initialized;
   } panel_cache;

   /* Slide-in panels (each tracks own animation independently) */
   struct {
      bool visible;
      bool closing;
      double anim_start;
   } panel_actions, panel_settings;
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
   return ui->panel_actions.visible || ui->panel_settings.visible;
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

   /* AI name (upper case) */
   char name_upper[32];
   int ni = 0;
   for (const char *p = ui->ai_name; *p && ni < 31; p++, ni++) {
      name_upper[ni] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
   }
   name_upper[ni] = '\0';
   surf = TTF_RenderText_Blended(font, name_upper, primary);
   if (surf) {
      ui->panel_cache.ai_name = SDL_CreateTextureFromSurface(r, surf);
      ui->panel_cache.ai_name_w = surf->w;
      ui->panel_cache.ai_name_h = surf->h;
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
   if (ui->panel_cache.ai_name)
      SDL_DestroyTexture(ui->panel_cache.ai_name);
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

   SDL_Color secondary_clr = { COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G,
                               COLOR_TEXT_SECONDARY_B, 255 };

   int text_x = 30, text_y = panel_y + 30;

   /* AI Name (cached) */
   if (ui->panel_cache.ai_name) {
      SDL_Rect dst = { text_x, text_y, ui->panel_cache.ai_name_w, ui->panel_cache.ai_name_h };
      SDL_RenderCopy(r, ui->panel_cache.ai_name, NULL, &dst);
      text_y += ui->panel_cache.ai_name_h + 20;
   }

   /* Connection status with colored dot */
   bool connected = ui->voice_ctx
                        ? (voice_processing_get_state(ui->voice_ctx) != VOICE_STATE_SILENCE)
                        : false;
   /* Draw status dot */
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

   SDL_Surface *surf = TTF_RenderText_Blended(ui->transcript.label_font,
                                              connected ? "Connected" : "Disconnected",
                                              secondary_clr);
   if (surf) {
      SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
      SDL_Rect dst = { text_x + 20, text_y, surf->w, surf->h };
      SDL_RenderCopy(r, tex, NULL, &dst);
      text_y += surf->h + 15;
      SDL_DestroyTexture(tex);
      SDL_FreeSurface(surf);
   }

   /* Date/time */
   time_t now = time(NULL);
   struct tm *tm = localtime(&now);
   char timebuf[64];
   strftime(timebuf, sizeof(timebuf), "%Y-%m-%d  %H:%M", tm);
   surf = TTF_RenderText_Blended(ui->transcript.label_font, timebuf, secondary_clr);
   if (surf) {
      SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
      SDL_Rect dst = { text_x, text_y, surf->w, surf->h };
      SDL_RenderCopy(r, tex, NULL, &dst);
      SDL_DestroyTexture(tex);
      SDL_FreeSurface(surf);
   }
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
         if (panel_any_open(ui)) {
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
         if (ui->panel_settings.visible && !ui->panel_settings.closing) {
            panel_close(ui, false, time_sec);
         } else if ((float)gesture.y > (float)ui->height * (1.0f - SWIPE_ZONE_FRAC)) {
            panel_open(ui, true, time_sec);
         }
         break;

      case TOUCH_GESTURE_SWIPE_DOWN:
         if (ui->panel_actions.visible && !ui->panel_actions.closing) {
            panel_close(ui, true, time_sec);
         } else if ((float)gesture.y < (float)ui->height * SWIPE_ZONE_FRAC) {
            panel_open(ui, false, time_sec);
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

   /* Initialize touch gesture detection */
   ui_touch_init(&ui->touch, ui->width, ui->height);
   memset(&ui->panel_actions, 0, sizeof(ui->panel_actions));
   memset(&ui->panel_settings, 0, sizeof(ui->panel_settings));
   LOG_INFO("SDL UI initialized (%dx%d, driver=%s)", ui->width, ui->height,
            SDL_GetCurrentVideoDriver());

   return 0;
}

/* =============================================================================
 * SDL Cleanup (called on render thread)
 * ============================================================================= */

static void sdl_cleanup_on_thread(sdl_ui_t *ui) {
   panel_cache_cleanup(ui);
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

   /* Slide-in panels: update animation, render scrim + panels */
   panel_tick(ui, time_sec);
   float act_off = ui->panel_actions.visible ? panel_offset(ui->panel_actions.anim_start,
                                                            ui->panel_actions.closing, time_sec)
                                             : 0.0f;
   float set_off = ui->panel_settings.visible ? panel_offset(ui->panel_settings.anim_start,
                                                             ui->panel_settings.closing, time_sec)
                                              : 0.0f;

   float max_off = act_off > set_off ? act_off : set_off;
   if (max_off > 0.001f) {
      render_scrim(ui, r, max_off);
   }
   if (act_off > 0.001f) {
      render_panel_actions(ui, r, act_off);
   }
   if (set_off > 0.001f) {
      render_panel_settings(ui, r, set_off);
   }

   /* Swipe indicators (only when no panel visible) */
   if (act_off < 0.001f && set_off < 0.001f) {
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
