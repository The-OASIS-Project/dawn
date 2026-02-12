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
 * SDL2 UI Theme System - runtime-switchable accent and background colors
 */

#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdbool.h>

#include "ui/ui_colors.h"

typedef enum {
   THEME_CYAN = 0,
   THEME_PURPLE,
   THEME_GREEN,
   THEME_BLUE,
   THEME_TERMINAL,
   THEME_COUNT
} ui_theme_id_t;

typedef struct {
   const char *name;
   ui_color_t accent;
   ui_color_t accent_dim;
   ui_color_t bg_primary;
   ui_color_t bg_secondary;
   ui_color_t bg_tertiary;
   ui_color_t text_primary;
   ui_color_t text_secondary;
   ui_color_t text_tertiary;
} ui_theme_def_t;

/** @brief Set initial theme without transition (call once at startup) */
void ui_theme_init(ui_theme_id_t id);

/** @brief Switch to a new theme with 200ms crossfade */
void ui_theme_set(ui_theme_id_t id);

/** @brief Advance transition; call once per frame before rendering
 *  @param now Monotonic time in seconds (avoids redundant clock_gettime) */
void ui_theme_tick(double now);

/** @brief Current (possibly mid-lerp) accent color */
ui_color_t ui_theme_accent(void);

/** @brief Dimmed accent (~30% for fills, inactive toggles) */
ui_color_t ui_theme_accent_dim(void);

/** @brief Background color: level 0=primary, 1=secondary, 2=tertiary */
ui_color_t ui_theme_bg(int level);

/** @brief Text color: level 0=primary, 1=secondary, 2=tertiary */
ui_color_t ui_theme_text(int level);

/** @brief Current target theme ID */
ui_theme_id_t ui_theme_current_id(void);

/** @brief True during 200ms crossfade */
bool ui_theme_is_transitioning(void);

/** @brief Convert name string to enum (returns THEME_CYAN on unknown) */
ui_theme_id_t ui_theme_id_from_name(const char *name);

/** @brief Convert enum to name string */
const char *ui_theme_name(ui_theme_id_t id);

/** @brief Get static theme definition (for dot picker colors) */
const ui_theme_def_t *ui_theme_get_def(ui_theme_id_t id);

#endif /* UI_THEME_H */
