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
 * Dedicated Music Streaming WebSocket Server
 *
 * Provides a separate WebSocket server for music audio streaming.
 * This isolates high-bandwidth audio data from the main UI WebSocket,
 * preventing audio streaming from interfering with chat and controls.
 *
 * Architecture:
 *   - Dedicated libwebsockets context and thread
 *   - Handles only binary audio data (Opus frames)
 *   - Control messages stay on main WebSocket
 *   - Authenticates using main session tokens
 */

#ifndef WEBUI_MUSIC_SERVER_H
#define WEBUI_MUSIC_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Initialize and start the dedicated music streaming server
 *
 * Creates a separate libwebsockets context and thread for music streaming.
 * Uses the main WebUI port + 1 by default.
 *
 * @param port Port to listen on (0 = main WebUI port + 1)
 * @return 0 on success, non-zero on failure
 *
 * @note Must be called after webui_server_init()
 * @note Thread-safe
 */
int webui_music_server_init(int port);

/**
 * @brief Shutdown the music streaming server
 *
 * Stops the server thread and closes all connections.
 * Blocks until shutdown is complete.
 *
 * @note Thread-safe
 */
void webui_music_server_shutdown(void);

/**
 * @brief Check if music server is running
 *
 * @return true if server is running
 */
bool webui_music_server_is_running(void);

/**
 * @brief Get the port the music server is listening on
 *
 * @return Port number, or 0 if not running
 */
int webui_music_server_get_port(void);

/**
 * @brief Wake up the music server's event loop
 *
 * Call this after lws_callback_on_writable() from another thread
 * to ensure the service loop processes the writeable request.
 */
void webui_music_server_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MUSIC_SERVER_H */
