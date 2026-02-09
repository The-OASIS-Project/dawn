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
 * Touch/mouse gesture detection for SDL2 UI
 */

#include "ui/ui_touch.h"

#include <math.h>

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TAP_MAX_SEC 0.3      /* Max duration for a tap */
#define LONG_PRESS_SEC 0.6   /* Hold duration for long press */
#define MOVE_THRESHOLD_PX 25 /* Max movement for tap/long-press */
#define SWIPE_MIN_PX 60      /* Min displacement for a swipe */

/* =============================================================================
 * Public API
 * ============================================================================= */

void ui_touch_init(ui_touch_state_t *state, int window_w, int window_h) {
   if (!state)
      return;
   state->finger_down = false;
   state->down_time = 0.0;
   state->down_x = 0;
   state->down_y = 0;
   state->cur_x = 0;
   state->cur_y = 0;
   state->window_w = window_w;
   state->window_h = window_h;
   state->long_press_fired = false;
}

touch_gesture_t ui_touch_process_event(ui_touch_state_t *state,
                                       const SDL_Event *event,
                                       double time_sec) {
   touch_gesture_t none = { TOUCH_GESTURE_NONE, 0, 0 };
   if (!state || !event)
      return none;

   switch (event->type) {
      case SDL_FINGERDOWN: {
         int px = (int)(event->tfinger.x * state->window_w);
         int py = (int)(event->tfinger.y * state->window_h);
         state->finger_down = true;
         state->down_time = time_sec;
         state->down_x = px;
         state->down_y = py;
         state->cur_x = px;
         state->cur_y = py;
         state->long_press_fired = false;
         return none;
      }

      case SDL_MOUSEBUTTONDOWN:
         if (event->button.button == SDL_BUTTON_LEFT) {
            state->finger_down = true;
            state->down_time = time_sec;
            state->down_x = event->button.x;
            state->down_y = event->button.y;
            state->cur_x = event->button.x;
            state->cur_y = event->button.y;
            state->long_press_fired = false;
         }
         return none;

      case SDL_FINGERMOTION: {
         if (state->finger_down) {
            state->cur_x = (int)(event->tfinger.x * state->window_w);
            state->cur_y = (int)(event->tfinger.y * state->window_h);
         }
         return none;
      }

      case SDL_MOUSEMOTION:
         if (state->finger_down && (event->motion.state & SDL_BUTTON_LMASK)) {
            state->cur_x = event->motion.x;
            state->cur_y = event->motion.y;
         }
         return none;

      case SDL_FINGERUP:
      case SDL_MOUSEBUTTONUP: {
         if (!state->finger_down) {
            return none;
         }
         state->finger_down = false;

         /* If long press already fired, don't also emit tap/swipe */
         if (state->long_press_fired) {
            return none;
         }

         int dx = state->cur_x - state->down_x;
         int dy = state->cur_y - state->down_y;
         int dist_sq = dx * dx + dy * dy;
         double dt = time_sec - state->down_time;

         /* Check for tap: small movement, short duration */
         if (dist_sq < MOVE_THRESHOLD_PX * MOVE_THRESHOLD_PX && dt < TAP_MAX_SEC) {
            touch_gesture_t tap = { TOUCH_GESTURE_TAP, state->down_x, state->down_y };
            return tap;
         }

         /* Check for swipe: sufficient displacement */
         int abs_dx = dx < 0 ? -dx : dx;
         int abs_dy = dy < 0 ? -dy : dy;
         float dist = sqrtf((float)dist_sq);

         if (dist >= SWIPE_MIN_PX) {
            touch_gesture_t swipe = { TOUCH_GESTURE_NONE, state->down_x, state->down_y };
            if (abs_dy > abs_dx) {
               /* Vertical swipe dominant */
               swipe.type = (dy < 0) ? TOUCH_GESTURE_SWIPE_UP : TOUCH_GESTURE_SWIPE_DOWN;
            } else {
               /* Horizontal swipe dominant */
               swipe.type = (dx < 0) ? TOUCH_GESTURE_SWIPE_LEFT : TOUCH_GESTURE_SWIPE_RIGHT;
            }
            return swipe;
         }

         return none;
      }

      default:
         return none;
   }
}

touch_gesture_t ui_touch_check_long_press(ui_touch_state_t *state, double time_sec) {
   touch_gesture_t none = { TOUCH_GESTURE_NONE, 0, 0 };
   if (!state || !state->finger_down || state->long_press_fired)
      return none;

   double dt = time_sec - state->down_time;
   if (dt < LONG_PRESS_SEC)
      return none;

   int dx = state->cur_x - state->down_x;
   int dy = state->cur_y - state->down_y;
   if (dx * dx + dy * dy > MOVE_THRESHOLD_PX * MOVE_THRESHOLD_PX)
      return none;

   state->long_press_fired = true;
   touch_gesture_t lp = { TOUCH_GESTURE_LONG_PRESS, state->down_x, state->down_y };
   return lp;
}
