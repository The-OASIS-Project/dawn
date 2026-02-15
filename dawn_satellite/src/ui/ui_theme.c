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
 * SDL2 UI Theme System - 5-theme table with 200ms crossfade transitions
 */

#include "ui/ui_theme.h"

#include <string.h>
#include <time.h>

/* =============================================================================
 * Theme Definitions
 * ============================================================================= */

static const ui_theme_def_t THEMES[THEME_COUNT] = {
   [THEME_CYAN] = {
      .name = "cyan",
      .accent      = { 0x2D, 0xD4, 0xBF },  /* Intentionally matches SPEAKING state color —
                                                  this is the original design palette */
      .accent_dim  = { 0x0D, 0x3F, 0x39 },
      .bg_primary  = { 0x12, 0x14, 0x17 },
      .bg_secondary = { 0x1B, 0x1F, 0x24 },
      .bg_tertiary = { 0x24, 0x2A, 0x31 },
      .text_primary = { 0xEE, 0xEE, 0xEE },
      .text_secondary = { 0x8C, 0x99, 0xA7 },
      .text_tertiary = { 0x6B, 0x77, 0x85 },
   },
   [THEME_PURPLE] = {
      .name = "purple",
      .accent      = { 0xA8, 0x55, 0xF7 },
      .accent_dim  = { 0x32, 0x19, 0x4A },
      .bg_primary  = { 0x12, 0x14, 0x17 },
      .bg_secondary = { 0x1B, 0x1F, 0x24 },
      .bg_tertiary = { 0x24, 0x2A, 0x31 },
      .text_primary = { 0xEE, 0xEE, 0xEE },
      .text_secondary = { 0x8C, 0x99, 0xA7 },
      .text_tertiary = { 0x6B, 0x77, 0x85 },
   },
   [THEME_GREEN] = {
      .name = "green",
      .accent      = { 0x4A, 0xDE, 0x80 },  /* #4ADE80 — shifted from #22C55E to avoid
                                                  collision with LISTENING state color */
      .accent_dim  = { 0x16, 0x42, 0x26 },
      .bg_primary  = { 0x12, 0x14, 0x17 },
      .bg_secondary = { 0x1B, 0x1F, 0x24 },
      .bg_tertiary = { 0x24, 0x2A, 0x31 },
      .text_primary = { 0xEE, 0xEE, 0xEE },
      .text_secondary = { 0x8C, 0x99, 0xA7 },
      .text_tertiary = { 0x6B, 0x77, 0x85 },
   },
   [THEME_BLUE] = {
      .name = "blue",
      .accent      = { 0x3B, 0x82, 0xF6 },
      .accent_dim  = { 0x11, 0x27, 0x4A },
      .bg_primary  = { 0x12, 0x14, 0x17 },
      .bg_secondary = { 0x1B, 0x1F, 0x24 },
      .bg_tertiary = { 0x24, 0x2A, 0x31 },
      .text_primary = { 0xEE, 0xEE, 0xEE },
      .text_secondary = { 0x8C, 0x99, 0xA7 },
      .text_tertiary = { 0x6B, 0x77, 0x85 },
   },
   [THEME_TERMINAL] = {
      .name = "terminal",
      .accent      = { 0x7F, 0xFF, 0x7F },
      .accent_dim  = { 0x19, 0x4C, 0x19 },
      .bg_primary  = { 0x0A, 0x0A, 0x0A },
      .bg_secondary = { 0x14, 0x14, 0x14 },
      .bg_tertiary = { 0x1E, 0x1E, 0x1E },
      .text_primary = { 0xB8, 0xB8, 0xB8 },
      .text_secondary = { 0x82, 0x82, 0x82 },
      .text_tertiary = { 0x58, 0x58, 0x58 },
   },
};

/* =============================================================================
 * Transition State
 * ============================================================================= */

#define TRANSITION_DURATION 0.2 /* 200ms crossfade */

static struct {
   ui_theme_id_t current;
   ui_theme_id_t target;
   double start_time;
   bool transitioning;
   /* Resolved (lerped) colors — returned by accessors */
   ui_color_t accent;
   ui_color_t accent_dim;
   ui_color_t bg[3];
   ui_color_t text[3];
} s_theme;

static double monotonic_sec(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

/** @brief Snap all resolved colors to a theme definition */
static void snap_to(const ui_theme_def_t *def) {
   s_theme.accent = def->accent;
   s_theme.accent_dim = def->accent_dim;
   s_theme.bg[0] = def->bg_primary;
   s_theme.bg[1] = def->bg_secondary;
   s_theme.bg[2] = def->bg_tertiary;
   s_theme.text[0] = def->text_primary;
   s_theme.text[1] = def->text_secondary;
   s_theme.text[2] = def->text_tertiary;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void ui_theme_init(ui_theme_id_t id) {
   if ((unsigned)id >= THEME_COUNT)
      id = THEME_CYAN;
   s_theme.current = id;
   s_theme.target = id;
   s_theme.transitioning = false;
   snap_to(&THEMES[id]);
}

void ui_theme_set(ui_theme_id_t id) {
   if ((unsigned)id >= THEME_COUNT)
      id = THEME_CYAN;
   if (id == s_theme.target)
      return;

   /* Start transition from current resolved state */
   s_theme.current = s_theme.target;
   s_theme.target = id;
   s_theme.start_time = monotonic_sec();
   s_theme.transitioning = true;
}

void ui_theme_tick(double now) {
   if (!s_theme.transitioning)
      return;

   float t_raw = (float)((now - s_theme.start_time) / TRANSITION_DURATION);
   if (t_raw >= 1.0f) {
      /* Transition complete — snap to target */
      s_theme.current = s_theme.target;
      s_theme.transitioning = false;
      snap_to(&THEMES[s_theme.target]);
      return;
   }

   /* Apply ease-out cubic for natural-feeling transition */
   float t = ui_ease_out_cubic(t_raw);

   /* Lerp all resolved colors from current to target */
   const ui_theme_def_t *from = &THEMES[s_theme.current];
   const ui_theme_def_t *to = &THEMES[s_theme.target];

   s_theme.accent = ui_color_lerp(from->accent, to->accent, t);
   s_theme.accent_dim = ui_color_lerp(from->accent_dim, to->accent_dim, t);
   s_theme.bg[0] = ui_color_lerp(from->bg_primary, to->bg_primary, t);
   s_theme.bg[1] = ui_color_lerp(from->bg_secondary, to->bg_secondary, t);
   s_theme.bg[2] = ui_color_lerp(from->bg_tertiary, to->bg_tertiary, t);
   s_theme.text[0] = ui_color_lerp(from->text_primary, to->text_primary, t);
   s_theme.text[1] = ui_color_lerp(from->text_secondary, to->text_secondary, t);
   s_theme.text[2] = ui_color_lerp(from->text_tertiary, to->text_tertiary, t);
}

ui_color_t ui_theme_accent(void) {
   return s_theme.accent;
}

ui_color_t ui_theme_accent_dim(void) {
   return s_theme.accent_dim;
}

ui_color_t ui_theme_bg(int level) {
   if (level < 0)
      level = 0;
   if (level > 2)
      level = 2;
   return s_theme.bg[level];
}

ui_color_t ui_theme_text(int level) {
   if (level < 0)
      level = 0;
   if (level > 2)
      level = 2;
   return s_theme.text[level];
}

ui_theme_id_t ui_theme_current_id(void) {
   return s_theme.target;
}

bool ui_theme_is_transitioning(void) {
   return s_theme.transitioning;
}

ui_theme_id_t ui_theme_id_from_name(const char *name) {
   if (!name)
      return THEME_CYAN;
   for (int i = 0; i < THEME_COUNT; i++) {
      if (strcmp(name, THEMES[i].name) == 0)
         return (ui_theme_id_t)i;
   }
   return THEME_CYAN;
}

const char *ui_theme_name(ui_theme_id_t id) {
   if ((unsigned)id >= THEME_COUNT)
      return "cyan";
   return THEMES[id].name;
}

const ui_theme_def_t *ui_theme_get_def(ui_theme_id_t id) {
   if ((unsigned)id >= THEME_COUNT)
      id = THEME_CYAN;
   return &THEMES[id];
}
