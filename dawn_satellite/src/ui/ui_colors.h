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
 * SDL2 UI Color Palette - matches WebUI CSS variables
 */

#ifndef UI_COLORS_H
#define UI_COLORS_H

#include <SDL2/SDL.h>
#include <math.h>

#include "voice_processing.h"

/* NOTE: Accent colors are now runtime-switchable via ui_theme.h.
 * The macros below define fixed palette values used as defaults and for
 * state colors (listening/thinking/speaking/error) which never change. */

/* =============================================================================
 * Background Colors (from www/css/base/variables.css)
 * ============================================================================= */

#define COLOR_BG_PRIMARY_R 0x12
#define COLOR_BG_PRIMARY_G 0x14
#define COLOR_BG_PRIMARY_B 0x17

#define COLOR_BG_SECONDARY_R 0x1B
#define COLOR_BG_SECONDARY_G 0x1F
#define COLOR_BG_SECONDARY_B 0x24

#define COLOR_BG_TERTIARY_R 0x24
#define COLOR_BG_TERTIARY_G 0x2A
#define COLOR_BG_TERTIARY_B 0x31

/* =============================================================================
 * Text Colors
 * ============================================================================= */

#define COLOR_TEXT_PRIMARY_R 0xEE
#define COLOR_TEXT_PRIMARY_G 0xEE
#define COLOR_TEXT_PRIMARY_B 0xEE

#define COLOR_TEXT_SECONDARY_R 0x8C
#define COLOR_TEXT_SECONDARY_G 0x99
#define COLOR_TEXT_SECONDARY_B 0xA7

/* Tertiary text - dimmed labels, album names (#6B7785)
 * Lightened from #555F6B to meet WCAG AA (4.5:1 against #1B1F24) */
#define COLOR_TEXT_TERTIARY_R 0x6B
#define COLOR_TEXT_TERTIARY_G 0x77
#define COLOR_TEXT_TERTIARY_B 0x85

/* =============================================================================
 * Voice State Colors
 * ============================================================================= */

/* Idle/inactive ring (#2a323a) */
#define COLOR_IDLE_R 0x2A
#define COLOR_IDLE_G 0x32
#define COLOR_IDLE_B 0x3A

/* Listening/recording - green (#22c55e) */
#define COLOR_LISTENING_R 0x22
#define COLOR_LISTENING_G 0xC5
#define COLOR_LISTENING_B 0x5E

/* Thinking/processing - amber (#f0b429) */
#define COLOR_THINKING_R 0xF0
#define COLOR_THINKING_G 0xB4
#define COLOR_THINKING_B 0x29

/* Speaking - cyan (#2dd4bf) */
#define COLOR_SPEAKING_R 0x2D
#define COLOR_SPEAKING_G 0xD4
#define COLOR_SPEAKING_B 0xBF

/* Error - red (#ef4444) */
#define COLOR_ERROR_R 0xEF
#define COLOR_ERROR_G 0x44
#define COLOR_ERROR_B 0x44

/* =============================================================================
 * Color Structures
 * ============================================================================= */

typedef struct {
   uint8_t r, g, b;
} ui_color_t;

static const ui_color_t UI_COLOR_IDLE = { COLOR_IDLE_R, COLOR_IDLE_G, COLOR_IDLE_B };
static const ui_color_t UI_COLOR_LISTENING = { COLOR_LISTENING_R, COLOR_LISTENING_G,
                                               COLOR_LISTENING_B };
static const ui_color_t UI_COLOR_THINKING = { COLOR_THINKING_R, COLOR_THINKING_G,
                                              COLOR_THINKING_B };
static const ui_color_t UI_COLOR_SPEAKING = { COLOR_SPEAKING_R, COLOR_SPEAKING_G,
                                              COLOR_SPEAKING_B };
static const ui_color_t UI_COLOR_ERROR = { COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B };

/* =============================================================================
 * State-to-Color Mapping
 * ============================================================================= */

/**
 * @brief Get orb color for a voice state
 */
static inline ui_color_t ui_color_for_state(voice_state_t state) {
   switch (state) {
      case VOICE_STATE_SILENCE:
         return UI_COLOR_IDLE;
      case VOICE_STATE_WAKEWORD_LISTEN:
      case VOICE_STATE_COMMAND_RECORDING:
         return UI_COLOR_LISTENING;
      case VOICE_STATE_PROCESSING:
      case VOICE_STATE_WAITING:
         return UI_COLOR_THINKING;
      case VOICE_STATE_SPEAKING:
         return UI_COLOR_SPEAKING;
      default:
         return UI_COLOR_IDLE;
   }
}

/**
 * @brief Get state label string for display
 */
static inline const char *ui_state_label(voice_state_t state) {
   switch (state) {
      case VOICE_STATE_SILENCE:
         return "READY";
      case VOICE_STATE_WAKEWORD_LISTEN:
         return "LISTENING";
      case VOICE_STATE_COMMAND_RECORDING:
         return "RECORDING";
      case VOICE_STATE_PROCESSING:
         return "PROCESSING";
      case VOICE_STATE_WAITING:
         return "THINKING";
      case VOICE_STATE_SPEAKING:
         return "SPEAKING";
      default:
         return "READY";
   }
}

/* =============================================================================
 * Label Color Override (brighter for SILENCE so [READY] is visible)
 * ============================================================================= */

/**
 * @brief Get label color for a voice state
 *
 * Same as ui_color_for_state() except SILENCE returns a brighter color
 * so the [READY] label is readable on the dark background.
 */
static inline ui_color_t ui_label_color_for_state(voice_state_t state) {
   if (state == VOICE_STATE_SILENCE) {
      return (ui_color_t){ COLOR_TEXT_SECONDARY_R, COLOR_TEXT_SECONDARY_G, COLOR_TEXT_SECONDARY_B };
   }
   return ui_color_for_state(state);
}

/* =============================================================================
 * Color Interpolation
 * ============================================================================= */

/**
 * @brief Linearly interpolate between two colors
 *
 * @param a Start color
 * @param b End color
 * @param t Interpolation factor (0.0 = a, 1.0 = b)
 * @return Interpolated color
 */
static inline ui_color_t ui_color_lerp(ui_color_t a, ui_color_t b, float t) {
   if (t <= 0.0f)
      return a;
   if (t >= 1.0f)
      return b;
   ui_color_t result;
   result.r = (uint8_t)((1.0f - t) * a.r + t * b.r);
   result.g = (uint8_t)((1.0f - t) * a.g + t * b.g);
   result.b = (uint8_t)((1.0f - t) * a.b + t * b.b);
   return result;
}

/* =============================================================================
 * Animation Easing
 * ============================================================================= */

/**
 * @brief Cubic ease-out curve (decelerating)
 *
 * @param t Progress (0.0 to 1.0)
 * @return Eased value
 */
static inline float ui_ease_out_cubic(float t) {
   float f = t - 1.0f;
   return f * f * f + 1.0f;
}

/* =============================================================================
 * HSV to RGB Conversion
 * ============================================================================= */

/**
 * @brief Convert HSV color to RGB
 *
 * @param h Hue (0-360 degrees)
 * @param s Saturation (0.0-1.0)
 * @param v Value/brightness (0.0-1.0)
 * @return RGB color
 */
static inline ui_color_t ui_color_from_hsv(float h, float s, float v) {
   float c = v * s;
   float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
   float m = v - c;
   float r, g, b;
   if (h < 60) {
      r = c;
      g = x;
      b = 0;
   } else if (h < 120) {
      r = x;
      g = c;
      b = 0;
   } else if (h < 180) {
      r = 0;
      g = c;
      b = x;
   } else if (h < 240) {
      r = 0;
      g = x;
      b = c;
   } else if (h < 300) {
      r = x;
      g = 0;
      b = c;
   } else {
      r = c;
      g = 0;
      b = x;
   }
   return (ui_color_t){ (uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255),
                        (uint8_t)((b + m) * 255) };
}

#endif /* UI_COLORS_H */
