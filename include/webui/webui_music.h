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
 * WebUI Music Streaming - Stream music from server to WebUI clients
 *
 * Provides per-session music playback streaming using Opus encoding.
 * Each WebUI client has independent playback state - multiple clients
 * can stream different tracks simultaneously.
 *
 * Thread Safety:
 * - init/cleanup must be called from main thread
 * - Per-session functions are thread-safe (use internal mutex)
 * - Streaming thread is per-subscriber
 */

#ifndef WEBUI_MUSIC_H
#define WEBUI_MUSIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Include webui_internal.h for ws_connection_t definition */
#include "webui/webui_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct json_object;

/* =============================================================================
 * Constants
 * ============================================================================= */

/** Audio buffer size for streaming (20ms at 48kHz stereo) */
#define WEBUI_MUSIC_FRAME_SAMPLES 960
#define WEBUI_MUSIC_FRAME_MS 20

/** Maximum playlist size per session */
#define WEBUI_MUSIC_MAX_QUEUE 250

/**
 * Maximum path length for music files (matches MUSIC_DB_PATH_MAX for consistency).
 * Queue memory: 250 entries × ~1.4KB each ≈ 342KB per session — negligible vs model memory.
 */
#define WEBUI_MUSIC_PATH_MAX 1024

/** Maximum string length for title/artist/album (truncation acceptable for display) */
#define WEBUI_MUSIC_STRING_MAX 128

/* =============================================================================
 * Quality Tiers
 * ============================================================================= */

/**
 * @brief Music streaming quality tiers
 *
 * Different bitrate/complexity settings for various use cases.
 */
typedef enum {
   MUSIC_QUALITY_VOICE = 0, /**< 48 kbps - same as TTS, lowest bandwidth */
   MUSIC_QUALITY_STANDARD,  /**< 96 kbps - default music quality */
   MUSIC_QUALITY_HIGH,      /**< 128 kbps - high quality */
   MUSIC_QUALITY_HIFI,      /**< 256 kbps - maximum quality */
   MUSIC_QUALITY_COUNT
} music_quality_t;

/**
 * @brief Bitrate mode for Opus encoding
 */
typedef enum {
   MUSIC_BITRATE_VBR = 0, /**< Variable bitrate (default) */
   MUSIC_BITRATE_CBR,     /**< Constant bitrate */
} music_bitrate_mode_t;

/* =============================================================================
 * Track Information
 * ============================================================================= */

/**
 * @brief Music track metadata
 */
typedef struct {
   char path[WEBUI_MUSIC_PATH_MAX];
   char title[WEBUI_MUSIC_STRING_MAX];
   char artist[WEBUI_MUSIC_STRING_MAX];
   char album[WEBUI_MUSIC_STRING_MAX];
   uint32_t duration_sec;
   uint32_t sample_rate;
   uint8_t channels;
   uint8_t bits_per_sample;
} music_track_info_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the music streaming subsystem
 *
 * Must be called before any other webui_music_* functions.
 * Sets up shared resources and configuration.
 *
 * @return 0 on success, non-zero on error
 */
int webui_music_init(void);

/**
 * @brief Clean up the music streaming subsystem
 *
 * Stops all active streams and releases resources.
 * Must be called during shutdown.
 */
void webui_music_cleanup(void);

/**
 * @brief Check if music streaming is available
 *
 * @return true if initialized and ready
 */
bool webui_music_is_available(void);

/* =============================================================================
 * Session Music State Management
 * ============================================================================= */

/**
 * @brief Initialize music state for a connection
 *
 * Called when a WebSocket connection is established.
 * Allocates per-connection music streaming state.
 *
 * @param conn WebSocket connection
 * @return 0 on success, non-zero on error
 */
int webui_music_session_init(ws_connection_t *conn);

/**
 * @brief Clean up music state for a connection
 *
 * Called when a WebSocket connection is closed.
 * Stops streaming and frees resources.
 *
 * @param conn WebSocket connection
 */
void webui_music_session_cleanup(ws_connection_t *conn);

/* =============================================================================
 * Message Handlers (called from webui_server.c)
 * ============================================================================= */

/**
 * @brief Handle music_subscribe message
 *
 * Client requests to start receiving music stream.
 * Sets quality tier and begins streaming if music is playing.
 *
 * @param conn WebSocket connection
 * @param payload JSON payload with quality settings
 */
void handle_music_subscribe(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_unsubscribe message
 *
 * Client requests to stop receiving music stream.
 *
 * @param conn WebSocket connection
 */
void handle_music_unsubscribe(ws_connection_t *conn);

/**
 * @brief Handle music_control message
 *
 * Client sends playback control (play, pause, seek, next, prev, volume).
 *
 * @param conn WebSocket connection
 * @param payload JSON payload with action and optional value
 */
void handle_music_control(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_search message
 *
 * Client searches the music library.
 *
 * @param conn WebSocket connection
 * @param payload JSON payload with query
 */
void handle_music_search(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_library message
 *
 * Client browses the music library (artists, albums, stats).
 *
 * @param conn WebSocket connection
 * @param payload JSON payload with browse type
 */
void handle_music_library(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_queue message
 *
 * Client manages playback queue (add, remove, clear, reorder).
 *
 * @param conn WebSocket connection
 * @param payload JSON payload with queue operation
 */
void handle_music_queue(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * State Query Functions
 * ============================================================================= */

/**
 * @brief Get current playback state for a connection
 *
 * Returns JSON object with:
 * - playing: bool
 * - paused: bool
 * - track: current track info or null
 * - position_sec: float
 * - duration_sec: int
 * - queue_length: int
 * - quality: string
 *
 * @param conn WebSocket connection
 * @return JSON object (caller must free), or NULL on error
 */
struct json_object *webui_music_get_state(ws_connection_t *conn);

/**
 * @brief Get number of active music streams
 *
 * @return Number of connections currently streaming music
 */
int webui_music_get_stream_count(void);

/* =============================================================================
 * Configuration
 * ============================================================================= */

/**
 * @brief Music streaming configuration
 */
typedef struct {
   bool enabled;                      /**< Music streaming enabled */
   music_quality_t default_quality;   /**< Default quality tier */
   music_bitrate_mode_t bitrate_mode; /**< VBR or CBR */
} webui_music_config_t;

/**
 * @brief Get current music streaming configuration
 *
 * @param config Output configuration structure
 */
void webui_music_get_config(webui_music_config_t *config);

/**
 * @brief Set music streaming configuration
 *
 * Changes take effect for new streams only.
 *
 * @param config New configuration
 * @return 0 on success, non-zero on error
 */
int webui_music_set_config(const webui_music_config_t *config);

/* =============================================================================
 * Dedicated Music Server Integration
 * ============================================================================= */

/* Forward declaration */
struct lws;

/**
 * @brief Set the dedicated music WebSocket for a session
 *
 * Called by the music server when a client authenticates.
 * The streaming thread will write directly to this wsi.
 *
 * @param session Main session
 * @param wsi Music WebSocket instance (NULL to clear)
 */
void webui_music_set_stream_wsi(session_t *session, struct lws *wsi);

/** webui_music_write_pending() return: the write failed fatally (lws_write error
 *  or short write). A truncated WebSocket frame desyncs the client's WS parser, so
 *  the writable callback must CLOSE the socket (client reconnects cleanly) rather
 *  than leave it desynced. Distinct from SUCCESS(0) and the benign "nothing
 *  pending" case so the callback only closes on a genuine fatal write. */
#define WEBUI_MUSIC_WRITE_CLOSE 2

/**
 * @brief Write pending audio data to the music WebSocket
 *
 * Called from the music server's LWS_CALLBACK_SERVER_WRITEABLE.
 * Writes one frame from the pending buffer.
 *
 * @param session Main session
 * @param wsi Music WebSocket instance
 * @return SUCCESS(0) on success; WEBUI_MUSIC_WRITE_CLOSE on a fatal write (caller
 *         must close the socket); non-zero otherwise (nothing pending / no state)
 */
int webui_music_write_pending(session_t *session, struct lws *wsi);

/**
 * @brief Record a client buffer-depth report (closed-loop flow control)
 *
 * Called from the music-server lws thread when a client sends a
 * {"type":"music_buffer","buffered_ms":N} message on the dedicated music socket.
 * Resolves the session's music state and stores the value plus a receipt
 * timestamp (plain atomics), consumed by the streaming thread's pacer. No-op if
 * the session has no music state. Delegated here (rather than poked directly from
 * the server file) so music-state internals stay in this module.
 *
 * @param session Authenticated session the report belongs to
 * @param buffered_ms Client worklet buffered depth in ms (caller pre-clamps)
 */
void webui_music_report_buffer(session_t *session, uint32_t buffered_ms);

/** Upper clamp for a client-reported buffer depth (ms) = the client worklet ring
 *  capacity (music-worklet-processor.js: 48000*10 samples = 10s). The music server
 *  clamps `buffered_ms` to [0, this] before calling webui_music_report_buffer(),
 *  so a garbage/hostile report can't drive the pacer into a multi-second sleep. */
#define WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS 10000

/* =============================================================================
 * LLM Tool Integration
 * ============================================================================= */

/**
 * @brief Execute music action from LLM tool callback
 *
 * Called by music_tool when the request originated from a WebUI session.
 * Routes music commands to the WebUI's per-session streaming instead of
 * local speaker playback.
 *
 * @param conn WebSocket connection (from session->client_data)
 * @param action Music action (play, stop, pause, resume, next, previous)
 * @param query Search query for play/search actions, or NULL
 * @param result_out Output: allocated result string (caller must free)
 * @return 0 on success, >0 on error, MUSIC_NOT_HANDLED if action should
 *         fall through to the default (local) handler
 */

/**
 * @brief Return value from webui_music_execute_tool() meaning "not handled,
 *        fall through to the default handler".
 *
 * Distinct from SUCCESS (0 = handled OK) and positive error codes (>0 = error).
 */
#define MUSIC_NOT_HANDLED (-1)

int webui_music_execute_tool(ws_connection_t *conn,
                             const char *action,
                             const char *query,
                             char **result_out);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MUSIC_H */
