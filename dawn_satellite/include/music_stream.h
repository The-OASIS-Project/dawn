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
 * Music WebSocket Stream Client - Receives Opus frames from daemon port+1
 */

#ifndef MUSIC_STREAM_H
#define MUSIC_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "music_playback.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct music_stream music_stream_t;

/**
 * Create music stream context.
 *
 * @param host Daemon hostname
 * @param port Main WebUI port (will connect to port+1)
 * @param use_ssl Use secure WebSocket
 * @param ssl_verify Verify SSL certificates
 * @param session_token 32-char hex token from register_ack
 * @param playback Music playback engine to push decoded frames into
 * @return New context, or NULL on failure
 */
music_stream_t *music_stream_create(const char *host,
                                    uint16_t port,
                                    bool use_ssl,
                                    bool ssl_verify,
                                    const char *session_token,
                                    music_playback_t *playback);

/**
 * Destroy music stream. Disconnects and frees all resources.
 */
void music_stream_destroy(music_stream_t *stream);

/**
 * Connect to the music WebSocket (port+1) and authenticate.
 * Starts a service thread for receiving binary frames.
 *
 * @return 0 on success, -1 on failure
 */
int music_stream_connect(music_stream_t *stream);

/**
 * Disconnect from the music WebSocket.
 */
void music_stream_disconnect(music_stream_t *stream);

/**
 * Check if connected to the music WebSocket.
 */
bool music_stream_is_connected(music_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_STREAM_H */
