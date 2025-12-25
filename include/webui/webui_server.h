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
 * WebUI Server - HTTP and WebSocket server for browser-based interface
 *
 * This module provides a unified HTTP + WebSocket server using libwebsockets.
 * It serves static files (HTML/CSS/JS) and handles WebSocket connections for
 * real-time communication with browser clients.
 *
 * Thread Safety:
 * - webui_server_init/shutdown must be called from main thread
 * - The server runs in its own dedicated thread (lws event loop)
 * - Status query functions are thread-safe
 */

#ifndef WEBUI_SERVER_H
#define WEBUI_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define WEBUI_DEFAULT_PORT 3000 /* "I love you 3000" */
#define WEBUI_DEFAULT_WWW_PATH "/var/lib/dawn/www"
#define WEBUI_MAX_CLIENTS 4
#define WEBUI_SUBPROTOCOL "dawn-1.0"

/* =============================================================================
 * Return Codes
 * ============================================================================= */

#define WEBUI_SUCCESS 0
#define WEBUI_ERROR 1
#define WEBUI_ERROR_ALREADY_RUNNING 2
#define WEBUI_ERROR_SOCKET 3
#define WEBUI_ERROR_THREAD 4

/* =============================================================================
 * WebSocket Binary Message Types (match WEBUI_DESIGN.md protocol spec)
 * ============================================================================= */

#define WS_BIN_AUDIO_IN 0x01      /* Client -> Server: Opus audio chunk */
#define WS_BIN_AUDIO_IN_END 0x02  /* Client -> Server: End of utterance */
#define WS_BIN_AUDIO_OUT 0x11     /* Server -> Client: TTS Opus audio chunk */
#define WS_BIN_AUDIO_OUT_END 0x12 /* Server -> Client: End of response audio */

/* =============================================================================
 * Buffer Size Constants
 * ============================================================================= */

#define WEBUI_SESSION_TOKEN_LEN 33      /* 32 hex chars + null terminator */
#define WEBUI_AUDIO_BUFFER_SIZE 32768   /* 32KB initial buffer for audio input */
#define WEBUI_AUDIO_MAX_CAPACITY 960000 /* ~30s @ 16kHz mono 16-bit PCM */
#define WEBUI_RESPONSE_QUEUE_SIZE 256   /* Pending responses (handles ~30s audio @ 8KB chunks) */

/* Forward declarations */
struct lws;
struct session;

/* =============================================================================
 * Response Types (worker -> WebUI thread)
 * ============================================================================= */

typedef enum {
   WS_RESP_STATE,      /* State machine update */
   WS_RESP_TRANSCRIPT, /* ASR or LLM text */
   WS_RESP_ERROR,      /* Error notification */
   WS_RESP_SESSION,    /* Session token for client */
   WS_RESP_AUDIO,      /* Binary audio data (Opus encoded) */
   WS_RESP_AUDIO_END,  /* End of audio stream marker */
   WS_RESP_CONTEXT,    /* Context/token usage update */
} ws_response_type_t;

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Initialize and start the WebUI server
 *
 * Creates a dedicated thread running the libwebsockets event loop.
 * Serves static files via HTTP and handles WebSocket connections.
 *
 * @param port Port to listen on (0 = use config default)
 * @param www_path Path to static files directory (NULL = use config/default)
 * @return WEBUI_SUCCESS on success, error code on failure
 *
 * @note Must be called from main thread
 * @note Safe to call if already running (returns WEBUI_ERROR_ALREADY_RUNNING)
 */
int webui_server_init(int port, const char *www_path);

/**
 * @brief Shutdown the WebUI server
 *
 * Signals the server thread to stop, closes all connections, and joins
 * the thread. Blocks until shutdown is complete.
 *
 * @note Must be called from main thread
 * @note Safe to call if not running (no-op)
 */
void webui_server_shutdown(void);

/**
 * @brief Check if WebUI server is currently running
 *
 * @return true if server is running, false otherwise
 *
 * @note Thread-safe
 */
bool webui_server_is_running(void);

/**
 * @brief Get current number of connected WebSocket clients
 *
 * @return Number of active WebSocket connections
 *
 * @note Thread-safe
 */
int webui_server_client_count(void);

/**
 * @brief Get the port the server is listening on
 *
 * @return Port number, or 0 if not running
 *
 * @note Thread-safe
 */
int webui_server_get_port(void);

/* =============================================================================
 * Worker-Callable Response Functions (Thread-Safe)
 *
 * These functions queue responses for delivery via the WebUI thread.
 * They use lws_cancel_service() to wake the event loop for processing.
 * ============================================================================= */

/**
 * @brief Send transcript message to WebSocket client
 *
 * Queues a transcript response for the session's WebSocket client.
 * The message will be delivered as JSON: {"type":"transcript","payload":{...}}
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param role Message role ("user" or "assistant")
 * @param text Transcript text
 *
 * @note Thread-safe - can be called from any thread (typically worker threads)
 * @note Copies role and text; caller retains ownership
 */
void webui_send_transcript(struct session *session, const char *role, const char *text);

/**
 * @brief Send state update to WebSocket client
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param state State name ("idle", "thinking", "speaking", "error")
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_state(struct session *session, const char *state);

/**
 * @brief Send context/token usage update to WebSocket client
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET), or NULL for all
 * @param current_tokens Current tokens used
 * @param max_tokens Maximum context size
 * @param threshold Compaction threshold (0.0-1.0)
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_context(struct session *session,
                        int current_tokens,
                        int max_tokens,
                        float threshold);

/**
 * @brief Send error message to WebSocket client
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param code Error code (e.g., "LLM_TIMEOUT", "ASR_FAILED")
 * @param message Human-readable error message
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_error(struct session *session, const char *code, const char *message);

/**
 * @brief Process a text message from WebSocket client
 *
 * Handles a text input message from a WebSocket client. This function
 * spawns async processing via the worker infrastructure.
 *
 * @param session Session that sent the message
 * @param text User's text input
 * @return 0 on success, non-zero on error
 *
 * @note Called from WebUI thread when text message received
 */
int webui_process_text_input(struct session *session, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_SERVER_H */
