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

#ifndef UI_TOUCH_H
#define UI_TOUCH_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   TOUCH_GESTURE_NONE,
   TOUCH_GESTURE_TAP,
   TOUCH_GESTURE_LONG_PRESS,
   TOUCH_GESTURE_SWIPE_UP,
   TOUCH_GESTURE_SWIPE_DOWN,
   TOUCH_GESTURE_SWIPE_LEFT,
   TOUCH_GESTURE_SWIPE_RIGHT
} touch_gesture_type_t;

typedef struct {
   touch_gesture_type_t type;
   int x, y; /* Pixel coordinates where gesture started */
} touch_gesture_t;

typedef struct {
   bool finger_down;
   double down_time;
   int down_x, down_y;
   int cur_x, cur_y;
   int window_w, window_h;
   bool long_press_fired;
} ui_touch_state_t;

/**
 * @brief Initialize touch state
 */
void ui_touch_init(ui_touch_state_t *state, int window_w, int window_h);

/**
 * @brief Process an SDL event for gesture detection
 *
 * Returns a gesture on finger-up (tap or swipe). Returns NONE otherwise.
 */
touch_gesture_t ui_touch_process_event(ui_touch_state_t *state,
                                       const SDL_Event *event,
                                       double time_sec);

/**
 * @brief Check for long press (call once per frame while finger is down)
 *
 * Returns LONG_PRESS if finger held > threshold with minimal movement.
 * Only fires once per press (resets on finger-up).
 */
touch_gesture_t ui_touch_check_long_press(ui_touch_state_t *state, double time_sec);

#ifdef __cplusplus
}
#endif

#endif /* UI_TOUCH_H */
