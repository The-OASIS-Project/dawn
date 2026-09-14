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
 */

/**
 * @file tui.h
 * @brief Terminal User Interface for DAWN
 *
 * Provides ncurses-based dashboard for real-time DAWN monitoring.
 */

#ifndef TUI_H
#define TUI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Color scheme options for the TUI
 */
typedef enum {
   TUI_THEME_GREEN = 1, /**< Apple ][ Classic Green */
   TUI_THEME_BLUE = 2,  /**< JARVIS-inspired Blue */
   TUI_THEME_BW = 3     /**< High Contrast Black/White */
} tui_theme_t;

/**
 * @brief TUI display mode
 */
typedef enum {
   TUI_MODE_OFF = 0,  /**< TUI disabled, normal logging */
   TUI_MODE_DISPLAY,  /**< TUI active, showing dashboard */
   TUI_MODE_DEBUG_LOG /**< TUI active but showing debug logs */
} tui_mode_t;

/**
 * @brief Maximum characters for text input buffer
 */
#define TUI_INPUT_MAX_LEN 512

/**
 * @brief Minimum terminal dimensions for TUI
 */
#define TUI_MIN_COLS 80
#define TUI_MIN_ROWS 24

/**
 * @brief TUI refresh rate in milliseconds
 */
#define TUI_REFRESH_MS 100

/**
 * @brief Initialize the TUI system
 *
 * Sets up ncurses, initializes color pairs, and prepares the display.
 * Falls back gracefully if terminal is not suitable.
 *
 * @param theme Initial color theme to use
 * @return 0 on success, non-zero on failure (TUI not available)
 */
int tui_init(tui_theme_t theme);

/**
 * @brief Cleanup and restore terminal
 *
 * Must be called before program exit to restore terminal state.
 */
void tui_cleanup(void);

/**
 * @brief Check if TUI is currently active
 *
 * @return 1 if TUI is active, 0 otherwise
 */
int tui_is_active(void);

/**
 * @brief Get current TUI mode
 *
 * @return Current TUI mode
 */
tui_mode_t tui_get_mode(void);

/**
 * @brief Set TUI mode
 *
 * @param mode New mode to set
 */
void tui_set_mode(tui_mode_t mode);

/**
 * @brief Toggle between TUI display and debug log mode
 */
void tui_toggle_debug_mode(void);

/**
 * @brief Set the color theme
 *
 * @param theme Theme to switch to
 */
void tui_set_theme(tui_theme_t theme);

/**
 * @brief Get current theme
 *
 * @return Current theme
 */
tui_theme_t tui_get_theme(void);

/**
 * @brief Update and redraw the TUI display
 *
 * Called periodically (10 Hz) to refresh the display with current metrics.
 * This is the main render function that draws all panels.
 */
void tui_update(void);

/**
 * @brief Process keyboard input
 *
 * Non-blocking check for keyboard input. Handles:
 * - Q: Request quit
 * - D: Toggle debug mode
 * - R: Reset stats
 * - 1/2/3: Switch themes
 * - ?: Show help
 *
 * @return 1 if quit was requested, 0 otherwise
 */
int tui_handle_input(void);

/**
 * @brief Show help overlay
 */
void tui_show_help(void);

/**
 * @brief Hide help overlay
 */
void tui_hide_help(void);

/**
 * @brief Check if terminal size is adequate
 *
 * @return 1 if terminal is large enough, 0 otherwise
 */
int tui_check_terminal_size(void);

/**
 * @brief Handle terminal resize event
 *
 * Called when SIGWINCH is received to adapt to new terminal size.
 */
void tui_handle_resize(void);

/**
 * @brief Check if text input is pending
 *
 * Thread-safe check for pending text input from TUI.
 * Call this from the main loop to check for typed commands.
 *
 * @return 1 if text input is available, 0 otherwise
 */
int tui_has_text_input(void);

/**
 * @brief Get pending text input
 *
 * Thread-safe retrieval of text input. Clears the pending flag.
 * Buffer must be at least TUI_INPUT_MAX_LEN + 1 bytes.
 *
 * @param buffer Output buffer for the text (must be TUI_INPUT_MAX_LEN + 1 bytes)
 * @return 1 if text was retrieved, 0 if no text available
 */
int tui_get_text_input(char *buffer);

/**
 * @brief Check if TUI is in text input mode
 *
 * @return 1 if in input mode, 0 otherwise
 */
int tui_is_input_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* TUI_H */
