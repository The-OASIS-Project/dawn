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

   LOG_INFO("SDL UI initialized (%dx%d, driver=%s)", ui->width, ui->height,
            SDL_GetCurrentVideoDriver());

   return 0;
}

/* =============================================================================
 * SDL Cleanup (called on render thread)
 * ============================================================================= */

static void sdl_cleanup_on_thread(sdl_ui_t *ui) {
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

   /* Render transcript in right panel */
   ui_transcript_render(&ui->transcript, r, state);

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

      /* Process SDL events (required even without input handling) */
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
         if (event.type == SDL_QUIT) {
            ui->running = false;
            break;
         }
      }

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
