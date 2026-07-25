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

/* Vision image limits — configurable values in vision_config_t (dawn_config.h).
 * WEBUI_MAX_BASE64_SIZE and WEBUI_MAX_VISION_IMAGES_CAP are sized for the
 * maximum configurable values and used only for array/buffer allocation.
 * Actual enforcement uses runtime config (g_config.vision.*). */
#define WEBUI_MAX_BASE64_SIZE (16384 * 1024 * 4 / 3 + 4) /* Upper-bound buffer for base64 */
#define WEBUI_MAX_VISION_IMAGES_CAP 10                   /* Array dim cap (max configurable) */
#define WEBUI_MAX_CONCURRENT_VISION 2                    /* Limit concurrent (thread safety) */
#define WEBUI_VISION_MIME_MAX 24                         /* MIME type buffer */

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
#define WEBUI_RESPONSE_QUEUE_SIZE 2048       /* Pending responses for sentence streaming */

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
   WS_RESP_MUSIC_POSITION, /* Music playback position update */
   WS_RESP_MUSIC_STATE,    /* Music state update (JSON) */
   WS_RESP_MUSIC_ERROR,    /* Music error notification (JSON) */

   /* Scheduler notifications */
   WS_RESP_SCHEDULER_NOTIFICATION, /* Alarm/timer/reminder fired (JSON) */

   /* Generic pre-serialized JSON (for init messages routed through queue) */
   WS_RESP_JSON, /* Arbitrary JSON string (heap-allocated) */
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
 * @param role Message role ("user" or "assistant")
 * @param text Transcript text
 *
 * @note Thread-safe - can be called from any thread (typically worker threads)
 * @note Copies role and text; caller retains ownership
 */
void webui_send_transcript(struct session *session, const char *role, const char *text);

/**
 * @brief Send transcript with server_saved flag
 *
 * When server_saved is true, the client skips its own save_message round-trip
 * because the server already persisted the message to the conversation DB.
 */
void webui_send_transcript_ex(struct session *session,
                              const char *role,
                              const char *text,
                              bool server_saved);

/**
 * @brief Send state update to WebSocket client
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
 * @param state State name ("idle", "thinking", "speaking", "error", "summarizing")
 * @param detail Optional detail message (NULL for no detail)
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_state_with_detail(struct session *session, const char *state, const char *detail);

/**
 * @brief Send context/token usage update to WebSocket client
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBUI), or NULL for all
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
 * @param tokens_before Token count before compaction
 * @param tokens_after Token count after compaction
 * @param messages_summarized Number of messages that were summarized
 * @param summary The generated summary text (for continuation)
 * @param level Compaction escalation level used (0=normal, 1=aggressive, 2=deterministic)
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_send_compaction_complete(struct session *session,
                                    int tokens_before,
                                    int tokens_after,
                                    int messages_summarized,
                                    const char *summary,
                                    int level);

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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
 * @param reasoning_tokens Number of reasoning tokens used
 *
 * @note Thread-safe
 */
void webui_send_reasoning_summary(struct session *session, int reasoning_tokens);

/**
 * @brief Send arbitrary JSON message to WebSocket client
 *
 * Convenience function for sending generic JSON messages via WS_RESP_JSON.
 * The input string is strdup'd internally (caller can use a stack buffer).
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
 * @param json_str Complete JSON string to send
 *
 * @note Thread-safe (uses queue_response)
 */
void webui_send_session_json(struct session *session, const char *json_str);

/**
 * @brief Send conversation reset notification to WebSocket client
 *
 * Notifies the frontend that the conversation context was reset (e.g., via
 * reset_conversation tool). The frontend should save the current conversation
 * and clear the chat display.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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
 * @param input_was_voice True if this turn's input was ASR-transcribed (voice),
 *                        false if typed.  Applied to session->input_was_voice on
 *                        the worker thread right before dispatch so the prompt
 *                        builder gates the ASR-disambiguation hint per turn.
 * @return 0 on success, non-zero on error
 *
 * @note Called from WebUI thread when text message received
 */
int webui_process_text_input(struct session *session, const char *text, bool input_was_voice);

/**
 * @brief Process text input message with optional vision images.
 *
 * Like `webui_process_text_input` but accepts up to
 * `WEBUI_MAX_VISION_IMAGES_CAP` base64-encoded images.  Pass NULL/0 for
 * the vision arrays to behave as the plain text variant.  Defined in
 * webui_text_processing.c.
 *
 * @param input_was_voice True if voice (ASR) input, false if typed.  See the
 *                        wrapper above.
 * @return 0 on success, non-zero on error
 */
int webui_process_text_input_with_vision(struct session *session,
                                         const char *text,
                                         const char **vision_images,
                                         const size_t *vision_image_sizes,
                                         const char **vision_mimes,
                                         int vision_image_count,
                                         bool input_was_voice);

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
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
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

/**
 * @brief Detach all WebSocket connections referencing a session about to be destroyed
 *
 * Called from session_destroy() before freeing session memory to prevent
 * use-after-free. Sends force_logout to connected clients and NULLs their
 * session pointer.
 *
 * @param session Session being destroyed
 *
 * @note Thread-safe (acquires connection registry mutex)
 */
void webui_detach_session(struct session *session);

/**
 * @brief Send a plan progress JSON message to a specific session's WebSocket
 *
 * Used by the plan executor to deliver real-time step progress during
 * multi-step plan execution. Targets only the session that initiated the plan.
 *
 * @param session Session to send to (must be SESSION_TYPE_WEBUI)
 * @param json_str Pre-serialized JSON string (copied internally)
 *
 * @note Thread-safe - can be called from any thread
 */
void webui_broadcast_plan_progress(struct session *session, const char *json_str);

/**
 * @brief Get the active conversation ID for a WebUI session
 *
 * Returns the conversation ID from the session's WebSocket connection.
 * Returns 0 for non-WebUI sessions or if no conversation is active.
 *
 * @param session Session to query
 * @return Active conversation ID, or 0 if unavailable
 */
int64_t webui_get_active_conversation_id(struct session *session);

/**
 * @brief Broadcast a conversation title change to all connections for a given user
 *
 * Thread-safe — can be called from the extraction thread.
 * Uses the same queue_response + lws_cancel_service pattern as scheduler broadcasts.
 *
 * @param user_id User whose connections should receive the update
 * @param conv_id Conversation ID that was renamed
 * @param title New title string
 */
void webui_broadcast_conversation_renamed(int user_id, int64_t conv_id, const char *title);

/**
 * @brief Broadcast a "new messages appended" event to a user's WebUI connections.
 *
 * Fires when an external writer (messaging engine — SMS/Telegram/
 * future Discord) appends user+assistant turns to a conversation
 * that the WebUI may be currently viewing.  Without this, the WebUI's
 * conversation panel goes stale and the user has to manually reload
 * to see the new turns.
 *
 * Thread-safe — called from the messaging worker thread.  Server
 * broadcasts to ALL of the user's WebUI sessions; the client gates
 * on `activeConversationId === conv_id` and reloads only when the
 * affected conversation is the one being viewed.
 *
 * @param user_id Target user (the channel's owner).
 * @param conv_id Conversation that gained new messages.
 */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id);

/* The background-job frames take a ws_connection_t and are declared alongside
 * the other per-connection senders in webui_handlers.h. */

/**
 * @brief Broadcast memory extraction notice to a user's WebUI connections
 *
 * @param user_id   Target user
 * @param level     "info", "warning", or "error"
 * @param message   Human-readable notification message
 */
void webui_broadcast_memory_notice(int user_id, const char *level, const char *message);

/**
 * @brief Broadcast pending merge-proposal count to a user's WebUI sessions.
 *
 * Phase 2 entity-merge: WebUI consumers raise the proposal-pending dot on
 * the memory icon when count > 0 and clear it when count == 0.  Triggered
 * after proposal insert (Phase 2 auto-fire or link-user-self), after
 * proposal resolve (operator approve/reject), and on initial WebSocket
 * auth so reconnecting clients hydrate their state.  The handler queries
 * memory_entity_merge_proposals directly for the count rather than
 * trusting a passed value — keeps the broadcast race-free against
 * concurrent inserts.
 *
 * @param user_id   Target user.  Broadcasts to every active session whose
 *                  auth_user_id matches.
 */
void webui_broadcast_memory_proposals_changed(int user_id);

/* Forward declare the focus result type so this header doesn't pull in
 * core/focus/focus_source.h.  The broadcast helper takes a `const` pointer
 * — caller still owns the struct and its heap. */
struct focus_compose_result_s;
typedef struct focus_compose_result_s focus_compose_result_t;

/**
 * @brief Broadcast a `context_injection` event to every WebUI session
 *        matching (user_id, conv_id) — Phase 1g-i.
 *
 * Iterates `s_active_connections` under `s_conn_registry_mutex`,
 * matching `auth_user_id == user_id` AND
 * `webui_get_active_conversation_id(conn->session) == conv_id` AND
 * `conn->session->type == SESSION_TYPE_WEBUI`.  Two browser tabs
 * authenticated as the same user on the same conversation both
 * receive the event; a different conversation (even on the same
 * user) does NOT.
 *
 * Fires unconditionally when called — the feature-flag gate lives in
 * the caller (`build_focus_block`).  Empty `result->candidate_count`
 * is valid input: the empty `items[]` payload is the empty-state UX
 * signal "DAWN looked, found nothing" — clients still render the
 * (collapsed) frame.  When `candidate_count == 0`, `score_breakdowns`
 * may be NULL — the helper handles the NULL case defensively.
 *
 * The JSON payload shape is the design-doc rev 3 §"UI surface" wire
 * format: top-level `{type, user_id, conversation_id, turn_id,
 * items[], filter_rejections[]}`.  Each `items[i]` has the candidate's
 * source_id / source_type / text / score / score_breakdown /
 * applied_source_weight; `provenance` is omitted (not zero-stub) when
 * `provenance.conversation_id == 0`.  Per-text size cap of
 * `FOCUS_TEXT_MAX_BYTES` (4096) is applied defensively before
 * serialization.
 *
 * Thread safety: builds the JSON ONCE, drops the json_object, then
 * `strdup`s the canonical string into each recipient's response queue
 * — same pattern as `webui_broadcast_silent_observation`.
 *
 * No-op (with a single DEBUG log line) when:
 *   - `result == NULL`
 *   - `user_id <= 0`
 *   - `conv_id <= 0`
 *
 * @param user_id    Target user (must be > 0)
 * @param conv_id    Active conversation id (must be > 0 — 0 disables)
 * @param turn_id    DB id of the user message that triggered this
 *                   prompt rebuild (0 acceptable: clients render as
 *                   "turn id unavailable")
 * @param result     Caller-owned compose result; this helper reads
 *                   from it and never frees it
 */
void webui_broadcast_context_injection(int user_id,
                                       int64_t conv_id,
                                       int64_t turn_id,
                                       const focus_compose_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_SERVER_H */
