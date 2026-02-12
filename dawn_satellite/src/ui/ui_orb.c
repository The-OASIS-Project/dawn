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

#include "ui/ui_orb.h"

#include <SDL2/SDL.h>
#ifdef HAVE_SDL2_GFX
#include <SDL2/SDL2_gfxPrimitives.h>
#endif
#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* =============================================================================
 * Constants
 * ============================================================================= */

#define PI 3.14159265358979323846

/* Orb core */
#define ORB_CORE_RADIUS 50

/* Glow layers (concentric circles with decreasing alpha) */
#define GLOW_LAYERS 4
static const int glow_offsets[GLOW_LAYERS] = { 10, 25, 45, 70 };
static const float glow_alphas[GLOW_LAYERS] = { 0.3f, 0.2f, 0.12f, 0.05f };

/* Ring system */
#define RING_SEGMENTS 64
#define RING_GAP_DEG 1.0f       /* Gap between segments in degrees */
#define RING_ANGLE_STEPS 10     /* Points per segment arc */
#define RING_MAX_WIDTH_LINES 13 /* Max width lines for scaled segments */
#define RING_INNER_R 100
#define RING_INNER_W 6
#define RING_MIDDLE_R 145
#define RING_MIDDLE_W 4
#define RING_OUTER_R 178
#define RING_OUTER_W 5

/* Max points per ring: segments * steps * max_width */
#define RING_MAX_POINTS (RING_SEGMENTS * RING_ANGLE_STEPS * RING_MAX_WIDTH_LINES)

/* Glow texture dimensions */
#define GLOW_TEX_SIZE 400

/* Color transition */
#define COLOR_TRANSITION_MS 300.0

/* Spectrum bar visualization */
#define BAR_COUNT SPECTRUM_BINS
#define BAR_INNER_R 75
#define BAR_MAX_OUTER_R 135
#define BAR_MIN_EXTENSION 6                               /* Always-visible resting ring */
#define BAR_MAX_EXTENSION (BAR_MAX_OUTER_R - BAR_INNER_R) /* 60px */
#define BAR_WIDTH_CURRENT 3                               /* Current frame bar width */
#define BAR_WIDTH_TRAIL 1                                 /* Trail frame bar width */
#define TRAIL_SAMPLE_INTERVAL 5   /* Frames between trail samples (~167ms at 30 FPS) */
#define SPECTRUM_SMOOTH_NEW 0.45f /* Snappy response */
#define SPECTRUM_SMOOTH_OLD 0.55f /* Complement */

/* =============================================================================
 * Shared Read-Only Trig Table (computed once, safe to share)
 * ============================================================================= */

static float ring_cos_table[RING_SEGMENTS * RING_ANGLE_STEPS];
static float ring_sin_table[RING_SEGMENTS * RING_ANGLE_STEPS];
static _Atomic bool trig_table_initialized = false;

static void init_ring_trig_table(void) {
   if (atomic_exchange(&trig_table_initialized, true))
      return;

   float seg_deg = 360.0f / RING_SEGMENTS;
   float gap = RING_GAP_DEG;

   for (int seg = 0; seg < RING_SEGMENTS; seg++) {
      float start_deg = seg * seg_deg + gap / 2.0f;
      float end_deg = (seg + 1) * seg_deg - gap / 2.0f;
      float start_rad = start_deg * (float)PI / 180.0f;
      float end_rad = end_deg * (float)PI / 180.0f;

      for (int step = 0; step < RING_ANGLE_STEPS; step++) {
         float angle = start_rad + (end_rad - start_rad) * step / (float)(RING_ANGLE_STEPS - 1);
         int idx = seg * RING_ANGLE_STEPS + step;
         ring_cos_table[idx] = cosf(angle);
         ring_sin_table[idx] = sinf(angle);
      }
   }
}

/* Pre-computed bar angle trig tables (one cos/sin per bar, evenly spaced around circle) */
static float bar_cos[BAR_COUNT];
static float bar_sin[BAR_COUNT];
static _Atomic bool bar_trig_initialized = false;

static void init_bar_trig_table(void) {
   if (atomic_exchange(&bar_trig_initialized, true))
      return;

   for (int i = 0; i < BAR_COUNT; i++) {
      float angle = (float)(2.0 * PI * i) / (float)BAR_COUNT - (float)(PI / 2.0);
      bar_cos[i] = cosf(angle);
      bar_sin[i] = sinf(angle);
   }
}

/* =============================================================================
 * Glow Texture Generation
 * ============================================================================= */

static SDL_Texture *create_glow_texture(SDL_Renderer *renderer, ui_color_t color) {
   SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, GLOW_TEX_SIZE, GLOW_TEX_SIZE);
   if (!tex)
      return NULL;

   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   SDL_SetRenderTarget(renderer, tex);

   /* Clear to transparent */
   SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
   SDL_RenderClear(renderer);

   int cx = GLOW_TEX_SIZE / 2;
   int cy = GLOW_TEX_SIZE / 2;

   /* Draw glow layers from outermost to innermost */
   for (int layer = GLOW_LAYERS - 1; layer >= 0; layer--) {
      int radius = ORB_CORE_RADIUS + glow_offsets[layer];
      uint8_t alpha = (uint8_t)(glow_alphas[layer] * 255);

#ifdef HAVE_SDL2_GFX
      filledCircleRGBA(renderer, cx, cy, radius, color.r, color.g, color.b, alpha);
#else
      SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, alpha);
      for (int y = -radius; y <= radius; y++) {
         int dx = (int)sqrtf((float)(radius * radius - y * y));
         SDL_RenderDrawLine(renderer, cx - dx, cy + y, cx + dx, cy + y);
      }
#endif
   }

   /* Draw solid core */
#ifdef HAVE_SDL2_GFX
   filledCircleRGBA(renderer, cx, cy, ORB_CORE_RADIUS, color.r, color.g, color.b, 255);
#else
   SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
   for (int y = -ORB_CORE_RADIUS; y <= ORB_CORE_RADIUS; y++) {
      int dx = (int)sqrtf((float)(ORB_CORE_RADIUS * ORB_CORE_RADIUS - y * y));
      SDL_RenderDrawLine(renderer, cx - dx, cy + y, cx + dx, cy + y);
   }
#endif

   SDL_SetRenderTarget(renderer, NULL);
   return tex;
}

/* =============================================================================
 * Ring Segment Drawing (batched)
 * ============================================================================= */

static void draw_ring(SDL_Renderer *renderer,
                      int cx,
                      int cy,
                      int radius,
                      int width,
                      int active_segments,
                      ui_color_t active_color,
                      float segment_scale) {
   SDL_Point active_pts[RING_MAX_POINTS];
   SDL_Point inactive_pts[RING_MAX_POINTS];
   int active_count = 0;
   int inactive_count = 0;

   for (int seg = 0; seg < RING_SEGMENTS; seg++) {
      bool active = (seg < active_segments);

      int seg_width = width;
      if (segment_scale != 1.0f && active) {
         seg_width = (int)(width * segment_scale);
         if (seg_width < 1)
            seg_width = 1;
      }

      SDL_Point *pts = active ? active_pts : inactive_pts;
      int *count = active ? &active_count : &inactive_count;
      int half_w = seg_width / 2;

      for (int w = -half_w; w <= half_w; w++) {
         int r_offset = radius + w;
         for (int step = 0; step < RING_ANGLE_STEPS; step++) {
            int idx = seg * RING_ANGLE_STEPS + step;
            if (*count < RING_MAX_POINTS) {
               pts[*count].x = cx + (int)(r_offset * ring_cos_table[idx]);
               pts[*count].y = cy + (int)(r_offset * ring_sin_table[idx]);
               (*count)++;
            }
         }
      }
   }

   if (active_count > 0) {
      SDL_SetRenderDrawColor(renderer, active_color.r, active_color.g, active_color.b, 200);
      SDL_RenderDrawPoints(renderer, active_pts, active_count);
   }
   if (inactive_count > 0) {
      SDL_SetRenderDrawColor(renderer, COLOR_IDLE_R, COLOR_IDLE_G, COLOR_IDLE_B, 100);
      SDL_RenderDrawPoints(renderer, inactive_pts, inactive_count);
   }
}

/* =============================================================================
 * Animation Helpers
 * ============================================================================= */

static float breathing_scale(double time_sec) {
   /* Breathing: scale 0.95-1.05, 3-second period */
   return 1.0f + 0.05f * sinf((float)(time_sec * 2.0 * PI / 3.0));
}

static float thinking_scale(double time_sec) {
   /* Faster pulse: 1.5-second period */
   return 1.0f + 0.04f * sinf((float)(time_sec * 2.0 * PI / 1.5));
}

/* =============================================================================
 * Spectrum Bar Rendering (SPEAKING state only)
 * ============================================================================= */

/**
 * Compute per-bar color: cyan (#22d3ee) → amber (#f59e0b) based on magnitude.
 * Derived from WebUI visualization.js gradient.
 */
static void bar_color(float mag, uint8_t *r, uint8_t *g, uint8_t *b) {
   if (mag < 0.0f)
      mag = 0.0f;
   if (mag > 1.0f)
      mag = 1.0f;
   *r = (uint8_t)(34.0f + 211.0f * mag);  /* 34→245 */
   *g = (uint8_t)(211.0f - 53.0f * mag);  /* 211→158 */
   *b = (uint8_t)(238.0f - 227.0f * mag); /* 238→11 */
}

/**
 * Draw a single radial bar as a rotated filled rectangle using SDL_RenderFillRect.
 * Since SDL2 doesn't support rotated rects natively, we draw the bar as a series
 * of small rects along the radial direction.
 */
static void draw_radial_bar(SDL_Renderer *renderer,
                            int cx,
                            int cy,
                            int bar_idx,
                            int inner_r,
                            int length,
                            int width,
                            uint8_t r,
                            uint8_t g,
                            uint8_t b,
                            uint8_t a) {
   SDL_SetRenderDrawColor(renderer, r, g, b, a);

   float cos_a = bar_cos[bar_idx];
   float sin_a = bar_sin[bar_idx];
   int half_w = width / 2;

   /* Draw filled rect along the radial direction, stepping outward */
   for (int d = 0; d < length; d++) {
      int dist = inner_r + d;
      int px = cx + (int)(dist * cos_a);
      int py = cy + (int)(dist * sin_a);

      /* Small rect perpendicular to the radial direction */
      SDL_Rect rect = { px - half_w, py - half_w, width, width };
      SDL_RenderFillRect(renderer, &rect);
   }
}

static void draw_spectrum_bars(ui_orb_ctx_t *ctx, SDL_Renderer *renderer, int cx, int cy) {
   /* Trail opacities (oldest to newest, higher than WebUI for SDL2 visibility) */
   static const float trail_opacities[SPECTRUM_TRAIL_FRAMES] = { 0.25f, 0.35f, 0.50f, 0.70f };

   /* Smoothing already applied in ui_orb_set_spectrum() */

   /* Push to trail circular buffer every TRAIL_SAMPLE_INTERVAL frames */
   ctx->trail_frame_counter++;
   if (ctx->trail_frame_counter >= TRAIL_SAMPLE_INTERVAL) {
      ctx->trail_frame_counter = 0;
      memcpy(ctx->spectrum_trail[ctx->trail_write_idx], ctx->smoothed_spectrum,
             sizeof(float) * BAR_COUNT);
      ctx->trail_write_idx = (ctx->trail_write_idx + 1) % SPECTRUM_TRAIL_FRAMES;
   }

   /* Render trail frames (oldest first) */
   for (int t = 0; t < SPECTRUM_TRAIL_FRAMES; t++) {
      /* Read from oldest to newest in circular buffer */
      int trail_idx = (ctx->trail_write_idx + t) % SPECTRUM_TRAIL_FRAMES;
      float opacity = trail_opacities[t];
      uint8_t alpha = (uint8_t)(opacity * 255.0f);

      for (int k = 0; k < BAR_COUNT; k++) {
         float mag = ctx->spectrum_trail[trail_idx][k];
         int extension = (int)(mag * BAR_MAX_EXTENSION);
         if (extension < BAR_MIN_EXTENSION)
            extension = BAR_MIN_EXTENSION;

         uint8_t r, g, b;
         bar_color(mag, &r, &g, &b);
         draw_radial_bar(renderer, cx, cy, k, BAR_INNER_R, extension, BAR_WIDTH_TRAIL, r, g, b,
                         alpha);
      }
   }

   /* Render current frame (on top) */
   uint8_t current_alpha = (uint8_t)(0.90f * 255.0f);
   for (int k = 0; k < BAR_COUNT; k++) {
      float mag = ctx->smoothed_spectrum[k];
      int extension = (int)(mag * BAR_MAX_EXTENSION);
      if (extension < BAR_MIN_EXTENSION)
         extension = BAR_MIN_EXTENSION;

      uint8_t r, g, b;
      bar_color(mag, &r, &g, &b);
      draw_radial_bar(renderer, cx, cy, k, BAR_INNER_R, extension, BAR_WIDTH_CURRENT, r, g, b,
                      current_alpha);
   }
}

void ui_orb_set_spectrum(ui_orb_ctx_t *ctx, const float *spectrum, int count) {
   if (!ctx || !spectrum)
      return;
   int n = count < SPECTRUM_BINS ? count : SPECTRUM_BINS;

   /* Apply temporal smoothing: blend new data with previous smoothed values */
   for (int k = 0; k < n; k++) {
      ctx->smoothed_spectrum[k] = SPECTRUM_SMOOTH_NEW * spectrum[k] +
                                  SPECTRUM_SMOOTH_OLD * ctx->smoothed_spectrum[k];
   }
   for (int k = n; k < SPECTRUM_BINS; k++) {
      ctx->smoothed_spectrum[k] *= SPECTRUM_SMOOTH_OLD;
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void ui_orb_init(ui_orb_ctx_t *ctx, SDL_Renderer *renderer) {
   if (!ctx || !renderer)
      return;

   memset(ctx, 0, sizeof(*ctx));

   /* Pre-compute trig lookup tables (shared, computed once) */
   init_ring_trig_table();
   init_bar_trig_table();

   /* Pre-render glow textures for each state color */
   ctx->glow_colors[0] = UI_COLOR_IDLE;
   ctx->glow_colors[1] = UI_COLOR_LISTENING;
   ctx->glow_colors[2] = UI_COLOR_THINKING;
   ctx->glow_colors[3] = UI_COLOR_SPEAKING;
   ctx->glow_colors[4] = UI_COLOR_ERROR;

   for (int i = 0; i < NUM_GLOW_TEXTURES; i++) {
      ctx->glow_textures[i] = create_glow_texture(renderer, ctx->glow_colors[i]);
   }

   ctx->current_color = UI_COLOR_IDLE;
   ctx->target_color = UI_COLOR_IDLE;
   ctx->color_transitioning = false;

   ctx->tap_pulse_time = -1.0;
   ctx->cancel_flash_time = -1.0;
}

void ui_orb_cleanup(ui_orb_ctx_t *ctx) {
   if (!ctx)
      return;

   for (int i = 0; i < NUM_GLOW_TEXTURES; i++) {
      if (ctx->glow_textures[i]) {
         SDL_DestroyTexture(ctx->glow_textures[i]);
         ctx->glow_textures[i] = NULL;
      }
   }
}

void ui_orb_render(ui_orb_ctx_t *ctx,
                   SDL_Renderer *renderer,
                   int cx,
                   int cy,
                   voice_state_t state,
                   float vad_prob,
                   float audio_amp,
                   double time_sec) {
   if (!ctx || !renderer)
      return;

   /* Update color target */
   ui_color_t state_color = ui_color_for_state(state);
   if (state_color.r != ctx->target_color.r || state_color.g != ctx->target_color.g ||
       state_color.b != ctx->target_color.b) {
      ctx->target_color = state_color;
      ctx->color_transition_start = time_sec;
      ctx->color_transitioning = true;
   }

   /* Interpolate color during transition */
   if (ctx->color_transitioning) {
      float t = (float)((time_sec - ctx->color_transition_start) * 1000.0 / COLOR_TRANSITION_MS);
      if (t >= 1.0f) {
         ctx->current_color = ctx->target_color;
         ctx->color_transitioning = false;
      } else {
         ctx->current_color = ui_color_lerp(ctx->current_color, ctx->target_color, t);
      }
   }

   /* Calculate animation scale */
   float scale = 1.0f;
   float glow_alpha_mult = 1.0f;

   switch (state) {
      case VOICE_STATE_SILENCE:
         scale = breathing_scale(time_sec);
         glow_alpha_mult = 0.2f + 0.15f * sinf((float)(time_sec * 2.0 * PI / 3.0));
         break;
      case VOICE_STATE_WAKEWORD_LISTEN:
      case VOICE_STATE_COMMAND_RECORDING:
         /* VAD-driven glow */
         scale = 1.0f + vad_prob * 0.03f;
         glow_alpha_mult = 0.3f + vad_prob * 0.5f;
         break;
      case VOICE_STATE_PROCESSING:
      case VOICE_STATE_WAITING:
         scale = thinking_scale(time_sec);
         glow_alpha_mult = 0.4f + 0.3f * (0.5f + 0.5f * sinf((float)(time_sec * 2.0 * PI / 1.5)));
         break;
      case VOICE_STATE_SPEAKING:
         /* Audio amplitude drives core pulse and glow (mirrors WebUI EQ feel) */
         scale = 1.0f + audio_amp * 0.25f;          /* 1.0-1.25x scale like WebUI */
         glow_alpha_mult = 0.4f + audio_amp * 0.5f; /* 0.4-0.9 glow tracks volume */
         break;
      default:
         break;
   }

   /* Find closest pre-rendered glow texture for current state */
   int glow_idx = 0;
   switch (state) {
      case VOICE_STATE_SILENCE:
         glow_idx = 0;
         break;
      case VOICE_STATE_WAKEWORD_LISTEN:
      case VOICE_STATE_COMMAND_RECORDING:
         glow_idx = 1;
         break;
      case VOICE_STATE_PROCESSING:
      case VOICE_STATE_WAITING:
         glow_idx = 2;
         break;
      case VOICE_STATE_SPEAKING:
         glow_idx = 3;
         break;
      default:
         glow_idx = 0;
         break;
   }

   /* Render glow texture (scaled + alpha modulated) */
   if (ctx->glow_textures[glow_idx]) {
      int tex_size = (int)(GLOW_TEX_SIZE * scale);
      SDL_Rect dst = { cx - tex_size / 2, cy - tex_size / 2, tex_size, tex_size };
      int alpha_i = (int)(glow_alpha_mult * 255);
      if (alpha_i > 255)
         alpha_i = 255;
      uint8_t alpha = (uint8_t)alpha_i;
      SDL_SetTextureAlphaMod(ctx->glow_textures[glow_idx], alpha);
      SDL_RenderCopy(renderer, ctx->glow_textures[glow_idx], NULL, &dst);
   }

   /* Render ring segments */

   /* Outer ring: all segments lit in state color */
   draw_ring(renderer, cx, cy, RING_OUTER_R, RING_OUTER_W, RING_SEGMENTS, ctx->current_color, 1.0f);

   /* Middle ring: activity level (fill segments based on state) */
   int middle_active = 0;
   switch (state) {
      case VOICE_STATE_SILENCE:
         middle_active = 8; /* Minimal activity */
         break;
      case VOICE_STATE_WAKEWORD_LISTEN:
      case VOICE_STATE_COMMAND_RECORDING:
         middle_active = 16 + (int)(vad_prob * 48); /* VAD-proportional */
         break;
      case VOICE_STATE_PROCESSING:
      case VOICE_STATE_WAITING: {
         /* Rotating fill for processing animation */
         float cycle = (float)fmod(time_sec * 1.5, 1.0);
         middle_active = 16 + (int)(cycle * 48);
         break;
      }
      case VOICE_STATE_SPEAKING:
         /* Amplitude-proportional fill (like WebUI throughput ring) */
         middle_active = 16 + (int)(audio_amp * 48);
         break;
      default:
         break;
   }
   if (middle_active > RING_SEGMENTS)
      middle_active = RING_SEGMENTS;
   draw_ring(renderer, cx, cy, RING_MIDDLE_R, RING_MIDDLE_W, middle_active, ctx->current_color,
             1.0f);

   /* Inner ring: spectrum bars during SPEAKING, ring segments otherwise */
   if (state == VOICE_STATE_SPEAKING) {
      /* Draw radial spectrum bars instead of inner ring */
      draw_spectrum_bars(ctx, renderer, cx, cy);
   } else {
      int inner_active = 0;
      float inner_scale = 1.0f;
      switch (state) {
         case VOICE_STATE_SILENCE: {
            /* Gentle idle animation */
            inner_active = RING_SEGMENTS;
            inner_scale = 0.5f + 0.2f * sinf((float)(time_sec * 2.0 * PI / 4.0));
            break;
         }
         case VOICE_STATE_WAKEWORD_LISTEN:
         case VOICE_STATE_COMMAND_RECORDING:
            inner_active = RING_SEGMENTS;
            inner_scale = 0.5f + vad_prob * 1.5f; /* VAD modulates width */
            break;
         case VOICE_STATE_PROCESSING:
         case VOICE_STATE_WAITING:
            inner_active = RING_SEGMENTS;
            inner_scale = 0.7f;
            break;
         default:
            inner_active = RING_SEGMENTS;
            inner_scale = 0.5f;
            break;
      }
      draw_ring(renderer, cx, cy, RING_INNER_R, RING_INNER_W, inner_active, ctx->current_color,
                inner_scale);
   }

   /* Touch feedback: tap pulse (white flash expanding outward, 0.3s) */
   if (ctx->tap_pulse_time > 0.0) {
      double dt = time_sec - ctx->tap_pulse_time;
      if (dt >= 0.0 && dt < 0.3) {
         float t = (float)(dt / 0.3);
         uint8_t alpha = (uint8_t)((1.0f - t) * 120);
         int radius = ORB_CORE_RADIUS + (int)(t * 30);
#ifdef HAVE_SDL2_GFX
         filledCircleRGBA(renderer, cx, cy, radius, 255, 255, 255, alpha);
#else
         SDL_SetRenderDrawColor(renderer, 255, 255, 255, alpha);
         for (int y = -radius; y <= radius; y++) {
            int dx = (int)sqrtf((float)(radius * radius - y * y));
            SDL_RenderDrawLine(renderer, cx - dx, cy + y, cx + dx, cy + y);
         }
#endif
      }
   }

   /* Touch feedback: cancel red flash (0.4s) */
   if (ctx->cancel_flash_time > 0.0) {
      double dt = time_sec - ctx->cancel_flash_time;
      if (dt >= 0.0 && dt < 0.4) {
         float t = (float)(dt / 0.4);
         uint8_t alpha = (uint8_t)((1.0f - t) * 180);
#ifdef HAVE_SDL2_GFX
         filledCircleRGBA(renderer, cx, cy, ORB_CORE_RADIUS, COLOR_ERROR_R, COLOR_ERROR_G,
                          COLOR_ERROR_B, alpha);
#else
         SDL_SetRenderDrawColor(renderer, COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B, alpha);
         for (int y = -ORB_CORE_RADIUS; y <= ORB_CORE_RADIUS; y++) {
            int dx = (int)sqrtf((float)(ORB_CORE_RADIUS * ORB_CORE_RADIUS - y * y));
            SDL_RenderDrawLine(renderer, cx - dx, cy + y, cx + dx, cy + y);
         }
#endif
      }
   }
}
