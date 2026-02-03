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

#include "webui/webui_audio.h" /* For WEBUI_MAX_RECORDING_SECONDS */

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

/* Vision image limits - conservative for embedded (Jetson) */
#define WEBUI_MAX_IMAGE_SIZE (4 * 1024 * 1024)                   /* 4MB decoded max */
#define WEBUI_MAX_BASE64_SIZE (WEBUI_MAX_IMAGE_SIZE * 4 / 3 + 4) /* ~5.3MB encoded */
#define WEBUI_MAX_IMAGE_DIMENSION 1024 /* 1024px max edge (optimized for LLM APIs) */
#define WEBUI_MAX_CONCURRENT_VISION 2  /* Limit concurrent */
#define WEBUI_VISION_MIME_MAX 24       /* MIME type buffer */
#define WEBUI_MAX_VISION_IMAGES 5      /* Max images per message */

/* Thumbnail limits for conversation history storage (security/DoS prevention) */
#define WEBUI_MAX_THUMBNAIL_SIZE (150 * 1024)   /* 150KB max per thumbnail */
#define WEBUI_MAX_THUMBNAIL_BASE64 (200 * 1024) /* ~200KB encoded (150KB * 4/3) */

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

#define WS_BIN_AUDIO_IN 0x01          /* Client -> Server: Opus audio chunk */
#define WS_BIN_AUDIO_IN_END 0x02      /* Client -> Server: End of utterance */
#define WS_BIN_AUDIO_OUT 0x11         /* Server -> Client: TTS audio chunk */
#define WS_BIN_AUDIO_SEGMENT_END 0x12 /* Server -> Client: Play this audio segment now */

/* Music streaming binary types (0x20-0x2F range) */
#define WS_BIN_MUSIC_DATA 0x20        /* Server -> Client: Opus music audio chunk */
#define WS_BIN_MUSIC_SEGMENT_END 0x21 /* Server -> Client: End of buffered segment */

/* =============================================================================
 * Buffer Size Constants
 * ============================================================================= */

#define WEBUI_SESSION_TOKEN_LEN 33    /* 32 hex chars + null terminator */
#define WEBUI_AUDIO_BUFFER_SIZE 32768 /* 32KB initial buffer for audio input */
/* Use WEBUI_MAX_RECORDING_SECONDS from webui_audio.h */
#define WEBUI_AUDIO_MAX_CAPACITY                                    \
   (WEBUI_MAX_RECORDING_SECONDS * 16000 * 2) /* @ 16kHz mono 16-bit \
                                              */
#define WEBUI_RESPONSE_QUEUE_SIZE 512        /* Pending responses for sentence streaming */

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
   WS_RESP_MUSIC_DATA, /* Binary music audio data (Opus encoded) */
   WS_RESP_CONTEXT,    /* Context/token usage update */

   /* LLM streaming types (ChatGPT-style real-time text) */
   WS_RESP_STREAM_START,        /* Start of LLM token stream */
   WS_RESP_STREAM_DELTA,        /* Incremental token chunk */
   WS_RESP_STREAM_END,          /* End of LLM token stream */
   WS_RESP_METRICS_UPDATE,      /* Real-time metrics for UI visualization */
   WS_RESP_COMPACTION_COMPLETE, /* Context compaction completed */

   /* Extended thinking types (reasoning/thinking content) */
   WS_RESP_THINKING_START,    /* Start of thinking block */
   WS_RESP_THINKING_DELTA,    /* Incremental thinking content */
   WS_RESP_THINKING_END,      /* End of thinking block */
   WS_RESP_REASONING_SUMMARY, /* OpenAI o-series reasoning token summary (no content) */

   /* Tool-initiated events */
   WS_RESP_CONVERSATION_RESET, /* Conversation was reset via tool */

   /* Music streaming */
   WS_RESP_MUSIC_POSITION /* Music playback position update */
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

/**
 * @brief Clear login rate limit for an IP address
 *
 * Clears the in-memory rate limit entries for the specified IP.
 * Used by admin tools to unblock rate-limited IPs.
 *
 * @param ip_address IP address to unblock (NULL to clear all)
 *
 * @note Thread-safe
 */
void webui_clear_login_rate_limit(const char *ip_address);

/**
 * @brief Get response queue fill level (0-100)
 *
 * Returns the current queue utilization as a percentage.
 * Used by high-frequency senders (e.g., music streaming) to implement
 * backpressure and avoid starving low-frequency control messages.
 *
 * @return Queue fill percentage (0 = empty, 100 = full)
 *
 * @note Thread-safe
 */
int webui_get_queue_fill_pct(void);

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
 * @brief Send state update with detail message to WebSocket client
 *
 * Allows showing additional context during long operations, e.g.,
 * "thinking" state with detail "Fetching URL..." or "Summarizing content...".
 * The detail is shown alongside the state in the UI.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param state State name ("idle", "thinking", "speaking", "error", "summarizing")
 * @param detail Optional detail message (NULL for no detail)
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_state_with_detail(struct session *session, const char *state, const char *detail);

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
 * @brief Send context compaction notification to WebSocket client
 *
 * Sent after auto-compaction completes. The client can use this to trigger
 * conversation continuation in the database (archive old, create new with summary).
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param tokens_before Token count before compaction
 * @param tokens_after Token count after compaction
 * @param messages_summarized Number of messages that were summarized
 * @param summary The generated summary text (for continuation)
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_compaction_complete(struct session *session,
                                    int tokens_before,
                                    int tokens_after,
                                    int messages_summarized,
                                    const char *summary);

/* =============================================================================
 * LLM Streaming Functions (ChatGPT-style real-time text)
 *
 * These functions provide real-time token streaming to WebUI clients.
 * Protocol:
 *   1. stream_start - Create new assistant entry, enter streaming state
 *   2. stream_delta - Append text to current entry (multiple calls)
 *   3. stream_end   - Finalize entry, exit streaming state
 *
 * Stream IDs prevent stale deltas from cancelled streams from being displayed.
 * ============================================================================= */

/**
 * @brief Start a new LLM token stream
 *
 * Signals the client to create a new assistant transcript entry and prepare
 * for incremental text updates. Increments session's stream_id.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 *
 * @note Thread-safe - can be called from any thread
 * @note Sets session->llm_streaming_active = true
 */
void webui_send_stream_start(struct session *session);

/**
 * @brief Send incremental text chunk during LLM streaming
 *
 * Appends text to the current streaming entry on the client. Should only
 * be called between stream_start and stream_end.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param text Text chunk to append
 *
 * @note Thread-safe - can be called from any thread
 * @note No-op if session->llm_streaming_active is false
 */
void webui_send_stream_delta(struct session *session, const char *text);

/**
 * @brief Filter command tags from text and return filtered result
 *
 * Strips <command>...</command> tags from text using the session's filter state.
 * Used by callers that need the filtered text (e.g., TTS sentence buffer).
 * Uses the same state machine as webui_send_stream_delta for consistency.
 *
 * @param session Session with filter state
 * @param text Input text to filter
 * @param out_buf Output buffer for filtered text
 * @param out_size Size of output buffer
 * @return Length of filtered text written to out_buf
 *
 * @note Filter state persists across calls for partial tag handling
 * @note If native tools are enabled, returns input unchanged
 */
int webui_filter_command_tags(struct session *session,
                              const char *text,
                              char *out_buf,
                              size_t out_size);

/**
 * @brief End the current LLM token stream
 *
 * Signals the client to finalize the current assistant entry.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param reason End reason: "complete", "cancelled", or "error"
 *
 * @note Thread-safe - can be called from any thread
 * @note Sets session->llm_streaming_active = false
 */
void webui_send_stream_end(struct session *session, const char *reason);

/**
 * @brief Send thinking block start notification
 *
 * Signals the client that extended thinking content is about to stream.
 * Creates a collapsible thinking block in the UI.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param provider LLM provider name ("claude", "local", "openai")
 *
 * @note Thread-safe
 */
void webui_send_thinking_start(struct session *session, const char *provider);

/**
 * @brief Send incremental thinking content chunk
 *
 * Appends thinking text to the current thinking block on the client.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param text Thinking text chunk to append
 *
 * @note Thread-safe
 */
void webui_send_thinking_delta(struct session *session, const char *text);

/**
 * @brief Send thinking block end notification
 *
 * Signals the client that thinking content is complete.
 * Causes the thinking block to auto-collapse in the UI.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param has_content true if thinking content was received, false otherwise
 *
 * @note Thread-safe
 */
void webui_send_thinking_end(struct session *session, bool has_content);

/**
 * @brief Send a reasoning summary to WebSocket client
 *
 * Used for OpenAI o-series models where we don't have access to reasoning
 * content, but we know how many tokens were used for internal reasoning.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param reasoning_tokens Number of reasoning tokens used
 *
 * @note Thread-safe
 */
void webui_send_reasoning_summary(struct session *session, int reasoning_tokens);

/**
 * @brief Send conversation reset notification to WebSocket client
 *
 * Notifies the frontend that the conversation context was reset (e.g., via
 * reset_conversation tool). The frontend should save the current conversation
 * and clear the chat display.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 *
 * @note Thread-safe
 */
void webui_send_conversation_reset(struct session *session);

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

/* =============================================================================
 * Real-Time Metrics for UI Visualization
 *
 * Provides metrics for multi-ring visualization:
 * - state: Current state machine state
 * - ttft_ms: Time to first token (ms)
 * - token_rate: Tokens per second (smoothed)
 * - context_percent: Context window utilization (0-100)
 * ============================================================================= */

/**
 * @brief Send real-time metrics update to WebSocket client
 *
 * Used for UI visualization (rings, gauges). Sent on:
 * - State changes (immediate)
 * - Token chunk events (during streaming)
 * - Periodic heartbeat (1Hz when idle)
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBSOCKET)
 * @param state Current state ("idle", "listening", "thinking", "speaking", "error")
 * @param ttft_ms Time to first token in milliseconds (0 if N/A)
 * @param token_rate Tokens per second (0 if not streaming)
 * @param context_percent Context utilization 0-100
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_metrics_update(struct session *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_SERVER_H */
