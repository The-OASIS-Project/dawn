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
 * DAP2 WebSocket Client - Tier 1 Satellite Protocol Implementation
 *
 * This module implements the DAP2 protocol for Tier 1 satellites:
 * - WebSocket connection to DAWN daemon
 * - Satellite registration with identity and capabilities
 * - Text-based query/response protocol
 * - Streaming response handling
 */

#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define WS_CLIENT_UUID_SIZE 37   /* UUID string with null terminator */
#define WS_CLIENT_NAME_SIZE 64   /* Satellite name max length */
#define WS_CLIENT_LOC_SIZE 64    /* Location max length (matches CONFIG_LOCATION_SIZE) */
#define WS_CLIENT_TEXT_SIZE 4096 /* Max text response chunk */
#define WS_CLIENT_SECRET_SIZE 65 /* 32 bytes hex-encoded + null */

/* Connection states */
typedef enum {
   WS_STATE_DISCONNECTED = 0,
   WS_STATE_CONNECTING,
   WS_STATE_CONNECTED,
   WS_STATE_REGISTERED,
   WS_STATE_ERROR
} ws_state_t;

/* Response types */
typedef enum {
   WS_RESP_NONE = 0,
   WS_RESP_REGISTER_ACK, /* Registration acknowledged */
   WS_RESP_STATE,        /* State update (idle, thinking, etc.) */
   WS_RESP_STREAM_START, /* Start of streaming response */
   WS_RESP_STREAM_DELTA, /* Streaming response chunk */
   WS_RESP_STREAM_END,   /* End of streaming response */
   WS_RESP_ERROR,        /* Error message */
   WS_RESP_PONG,         /* Ping response */
   WS_RESP_TRANSCRIPT,   /* Complete transcript */
} ws_response_type_t;

/* =============================================================================
 * Structures
 * ============================================================================= */

/**
 * @brief Satellite identity sent during registration
 */
typedef struct {
   char uuid[WS_CLIENT_UUID_SIZE];
   char name[WS_CLIENT_NAME_SIZE];
   char location[WS_CLIENT_LOC_SIZE];
   char reconnect_secret[WS_CLIENT_SECRET_SIZE]; /* Set by server, send back for reconnection */
} ws_identity_t;

/**
 * @brief Satellite capabilities sent during registration
 */
typedef struct {
   bool local_asr; /* Has local speech recognition */
   bool local_tts; /* Has local text-to-speech */
   bool wake_word; /* Has local wake word detection */
} ws_capabilities_t;

/**
 * @brief Parsed response from daemon
 */
typedef struct {
   ws_response_type_t type;
   union {
      struct {
         bool success;
         uint32_t session_id;
         char message[256];
      } register_ack;

      struct {
         char state[32];
         char detail[128];
      } state;

      struct {
         uint32_t stream_id;
         char text[WS_CLIENT_TEXT_SIZE];
      } stream;

      struct {
         char code[32];
         char message[256];
      } error;

      struct {
         char role[32];
         char text[WS_CLIENT_TEXT_SIZE];
      } transcript;
   } data;
} ws_response_t;

/**
 * @brief Callback for streaming responses
 *
 * @param text Streamed text chunk
 * @param is_end True if this is the end of stream
 * @param user_data User-provided context
 */
typedef void (*ws_stream_callback_t)(const char *text, bool is_end, void *user_data);

/**
 * @brief Callback for state changes
 *
 * @param state New state string (e.g., "idle", "thinking")
 * @param user_data User-provided context
 */
typedef void (*ws_state_callback_t)(const char *state, void *user_data);

/**
 * @brief WebSocket client context
 */
typedef struct ws_client ws_client_t;

/* =============================================================================
 * Client Lifecycle
 * ============================================================================= */

/**
 * @brief Create WebSocket client
 *
 * @param host Daemon hostname or IP
 * @param port Daemon WebUI port (default 8080)
 * @param use_ssl Use wss:// instead of ws://
 * @param ssl_verify Verify SSL certificates (default: true for production)
 * @return Client context or NULL on failure
 */
ws_client_t *ws_client_create(const char *host, uint16_t port, bool use_ssl, bool ssl_verify);

/**
 * @brief Destroy WebSocket client
 *
 * Disconnects if connected and frees all resources.
 *
 * @param client Client context
 */
void ws_client_destroy(ws_client_t *client);

/* =============================================================================
 * Connection Management
 * ============================================================================= */

/**
 * @brief Connect to daemon WebSocket server
 *
 * @param client Client context
 * @return 0 on success, -1 on failure
 */
int ws_client_connect(ws_client_t *client);

/**
 * @brief Disconnect from daemon
 *
 * @param client Client context
 */
void ws_client_disconnect(ws_client_t *client);

/**
 * @brief Check if client is connected
 *
 * @param client Client context
 * @return true if connected
 */
bool ws_client_is_connected(ws_client_t *client);

/**
 * @brief Get current connection state
 *
 * @param client Client context
 * @return Connection state
 */
ws_state_t ws_client_get_state(ws_client_t *client);

/* =============================================================================
 * Registration
 * ============================================================================= */

/**
 * @brief Register satellite with daemon
 *
 * Must be called after connect and before sending queries.
 *
 * @param client Client context
 * @param identity Satellite identity (UUID, name, location)
 * @param caps Satellite capabilities
 * @return 0 on success, -1 on failure
 */
int ws_client_register(ws_client_t *client,
                       const ws_identity_t *identity,
                       const ws_capabilities_t *caps);

/**
 * @brief Check if satellite is registered
 *
 * @param client Client context
 * @return true if registered
 */
bool ws_client_is_registered(ws_client_t *client);

/* =============================================================================
 * Query/Response
 * ============================================================================= */

/**
 * @brief Send text query to daemon
 *
 * @param client Client context
 * @param text Query text (transcribed speech)
 * @return 0 on success, -1 on failure
 */
int ws_client_send_query(ws_client_t *client, const char *text);

/**
 * @brief Set callback for streaming responses
 *
 * @param client Client context
 * @param callback Callback function
 * @param user_data User context passed to callback
 */
void ws_client_set_stream_callback(ws_client_t *client,
                                   ws_stream_callback_t callback,
                                   void *user_data);

/**
 * @brief Set callback for state changes
 *
 * @param client Client context
 * @param callback Callback function
 * @param user_data User context passed to callback
 */
void ws_client_set_state_callback(ws_client_t *client,
                                  ws_state_callback_t callback,
                                  void *user_data);

/**
 * @brief Process pending messages (call in main loop)
 *
 * This function processes WebSocket events and dispatches callbacks.
 * Call this regularly in your main loop.
 *
 * @param client Client context
 * @param timeout_ms Max time to wait for events (0 = non-blocking)
 * @return Number of messages processed, or -1 on error
 */
int ws_client_service(ws_client_t *client, int timeout_ms);

/**
 * @brief Send keepalive ping
 *
 * @param client Client context
 * @return 0 on success, -1 on failure
 */
int ws_client_ping(ws_client_t *client);

/* =============================================================================
 * Utility
 * ============================================================================= */

/**
 * @brief Generate a new UUID for satellite identity
 *
 * @param uuid Buffer to receive UUID (must be WS_CLIENT_UUID_SIZE bytes)
 */
void ws_client_generate_uuid(char *uuid);

/**
 * @brief Get last error message
 *
 * @param client Client context
 * @return Error message or NULL
 */
const char *ws_client_get_error(ws_client_t *client);

/**
 * @brief Get the reconnect secret received from server
 *
 * @param client Client context
 * @return Secret string or NULL if not set
 */
const char *ws_client_get_reconnect_secret(ws_client_t *client);

/**
 * @brief Set the reconnect secret (load from saved config)
 *
 * @param client Client context
 * @param secret Secret string (64 hex chars)
 */
void ws_client_set_reconnect_secret(ws_client_t *client, const char *secret);

#ifdef __cplusplus
}
#endif

#endif /* WS_CLIENT_H */
