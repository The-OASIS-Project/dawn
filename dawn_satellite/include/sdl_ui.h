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
 * SDL2 Touchscreen UI for DAWN Satellite
 *
 * Renders an orb visualization and transcript panel on a 7" touchscreen
 * using SDL2 with KMSDRM backend (no X11 required).
 */

#ifndef SDL_UI_H
#define SDL_UI_H

#include "voice_processing.h"

/* Forward declarations */
struct ws_client;
struct audio_playback;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SDL UI context (opaque)
 */
typedef struct sdl_ui sdl_ui_t;

/**
 * @brief SDL UI configuration
 */
typedef struct {
   int width;                      /* Display width (default: 1024) */
   int height;                     /* Display height (default: 600) */
   const char *font_dir;           /* Path to fonts directory */
   const char *ai_name;            /* AI name for display (e.g., "DAWN") */
   voice_ctx_t *voice_ctx;         /* Voice processing context for state polling */
   const char *satellite_name;     /* Satellite identity name (e.g., "Kitchen Assistant") */
   const char *satellite_location; /* Satellite location (e.g., "kitchen") */
} sdl_ui_config_t;

/**
 * @brief Initialize SDL2 UI configuration
 *
 * Stores configuration only. SDL initialization is deferred to the render
 * thread (sdl_ui_start) because KMSDRM ties the DRM master and EGL context
 * to the initializing thread.
 *
 * @param config UI configuration
 * @return Allocated UI context, or NULL on failure
 */
sdl_ui_t *sdl_ui_init(const sdl_ui_config_t *config);

/**
 * @brief Start the render thread
 *
 * Spawns a dedicated thread that initializes SDL, creates the window and
 * renderer, and renders at 30 FPS (active) / 10 FPS (idle). Blocks until
 * SDL initialization completes on the render thread.
 *
 * @param ui UI context
 * @return 0 on success, non-zero on failure (continues headless)
 */
int sdl_ui_start(sdl_ui_t *ui);

/**
 * @brief Signal the render thread to stop
 *
 * Non-blocking. Thread will finish current frame and exit.
 *
 * @param ui UI context
 */
void sdl_ui_stop(sdl_ui_t *ui);

/**
 * @brief Cleanup SDL2 UI
 *
 * Stops render thread (if running), destroys SDL resources, frees memory.
 *
 * @param ui UI context
 */
void sdl_ui_cleanup(sdl_ui_t *ui);

/**
 * @brief Add a conversation entry to the transcript
 *
 * Thread-safe. Can be called from any thread (e.g., WebSocket callback).
 *
 * @param ui UI context
 * @param role "You" or AI name
 * @param text Message text
 */
void sdl_ui_add_transcript(sdl_ui_t *ui, const char *role, const char *text);

/**
 * @brief Set WebSocket client for music control
 *
 * Must be called before sdl_ui_start() so the music panel can send commands.
 * Also registers music callbacks on the ws_client.
 *
 * @param ui UI context
 * @param client WebSocket client
 */
void sdl_ui_set_ws_client(sdl_ui_t *ui, struct ws_client *client);

/**
 * @brief Set audio playback context for master volume control
 *
 * Links the TTS audio playback to the UI volume slider so both TTS
 * and music volumes can be controlled from the settings panel.
 *
 * @param ui UI context
 * @param pb Audio playback context (TTS path)
 */
void sdl_ui_set_audio_playback(sdl_ui_t *ui, struct audio_playback *pb);

#ifdef HAVE_OPUS
struct music_playback;

/**
 * @brief Set music playback engine on the SDL UI
 *
 * Wires music playback into the music panel for volume control,
 * buffer flush on transport actions, and visualizer spectrum.
 *
 * @param ui UI context
 * @param pb Music playback engine
 */
void sdl_ui_set_music_playback(sdl_ui_t *ui, struct music_playback *pb);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SDL_UI_H */
