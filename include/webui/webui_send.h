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
 * the project author(s).
 *
 * WebUI send-side pipeline — cross-module API.
 *
 * Owns the declarations for everything in webui_send.c: the cross-thread
 * response queue (queue_response / free_response /
 * process_response_queue / process_one_response), the LWS-service-thread
 * `*_impl` send helpers, and the public audio-send helpers used by
 * webui_audio.c.  Carved out of webui_internal.h so sibling webui_*.c
 * modules can include only the send-side surface without pulling the
 * full handler-declaration corpus.
 *
 * INTERNAL TO THE WEBUI MODULE.  All `*_impl` functions are LWS service
 * thread only — see the file-header invariants in webui_send.c.  To send
 * from a worker / tool / music-streaming thread, use queue_response()
 * with the appropriate WS_RESP_* type.
 */

#ifndef WEBUI_SEND_H
#define WEBUI_SEND_H

#include <libwebsockets.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/session_manager.h"
#include "webui/webui_internal.h" /* ws_connection_t, ws_response_t */
#include "webui/webui_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Response Queue
 *
 * Cross-thread send primitive.  Workers and tool threads build a
 * ws_response_t describing what to send, then queue_response() copies it
 * into the LWS-thread-owned queue and wakes lws_service().  The LWS
 * thread drains the queue via process_response_queue / process_one_response.
 * ============================================================================= */

/**
 * @brief Queue a response for delivery to a WebSocket client.
 *
 * Thread-safe.  Wakes the LWS event loop via lws_cancel_service().
 *
 * @param resp Response to queue (copied, caller retains ownership of strings)
 */
void queue_response(ws_response_t *resp);

/**
 * @brief Free dynamically allocated members of a response.
 *
 * Called after the response is sent.  Frees strings/buffers based on type.
 */
void free_response(ws_response_t *resp);

void process_response_queue(void);
void process_one_response(void);

/* =============================================================================
 * Send Helpers — LWS SERVICE THREAD ONLY
 *
 * Direct lws_write() callers.  Safe only from within an LWS callback or
 * from process_one_response().  Handler code should prefer
 * queue_response().
 * ============================================================================= */

/**
 * @brief Send a JSON text message to a WebSocket client.
 *
 * @return 0 on success, LWS_CLOSE_CONNECTION on error
 */
int send_json_message(struct lws *wsi, const char *json);

/**
 * @brief Send a binary message with a 1-byte type prefix.
 *
 * @param msg_type WS_BIN_* constant
 * @return 0 on success, LWS_CLOSE_CONNECTION on error
 */
int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len);

void send_state_impl(struct lws *wsi, const char *state, const char *detail);
void send_state_impl_full(struct lws *wsi,
                          const char *state,
                          const char *detail,
                          const char *tools_json,
                          int64_t conversation_id);
void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len);
void send_audio_end_impl(struct lws *wsi, bool is_opus);

void send_compaction_impl(struct lws *wsi,
                          int tokens_before,
                          int tokens_after,
                          int messages_summarized,
                          const char *summary,
                          int level);
void send_context_impl(struct lws *wsi, int current_tokens, int max_tokens, float threshold);
void send_conversation_reset_impl(struct lws *wsi);
void send_force_logout_impl(struct lws *wsi, const char *reason);
void send_metrics_impl(struct lws *wsi,
                       const char *state,
                       int ttft_ms,
                       float token_rate,
                       int context_tokens,
                       int64_t conversation_id);
void send_music_position_impl(struct lws *wsi, double position_sec, uint32_t duration_sec);
void send_reasoning_summary_impl(struct lws *wsi,
                                 uint32_t stream_id,
                                 int64_t conversation_id,
                                 int reasoning_tokens);
void send_session_token_impl(ws_connection_t *conn, const char *token);
void send_stream_start_impl(struct lws *wsi, uint32_t stream_id, int64_t conversation_id);
void send_stream_delta_impl(struct lws *wsi,
                            uint32_t stream_id,
                            int64_t conversation_id,
                            const char *text);
void send_stream_end_impl(struct lws *wsi,
                          uint32_t stream_id,
                          int64_t conversation_id,
                          const char *reason);
void send_thinking_start_impl(struct lws *wsi,
                              uint32_t stream_id,
                              int64_t conversation_id,
                              const char *provider);
void send_thinking_delta_impl(struct lws *wsi,
                              uint32_t stream_id,
                              int64_t conversation_id,
                              const char *text);
void send_thinking_end_impl(struct lws *wsi,
                            uint32_t stream_id,
                            int64_t conversation_id,
                            int has_content);
void send_transcript_impl_ex(struct lws *wsi,
                             const char *role,
                             const char *text,
                             bool replay,
                             bool server_saved,
                             int64_t conversation_id);

/* =============================================================================
 * Audio Send (used by webui_audio.c)
 *
 * Session-targeted convenience wrappers that route through queue_response()
 * so audio worker threads can call them safely.
 * ============================================================================= */

/**
 * @brief Queue audio data for the WebSocket client of @p session.
 *
 * @param data Audio bytes (Opus or PCM depending on client capability)
 */
void webui_send_audio(session_t *session, const uint8_t *data, size_t len);

/**
 * @brief Queue end-of-audio marker for the WebSocket client.
 */
void webui_send_audio_end(session_t *session, bool is_opus);

/**
 * @brief TTS sentence callback for LLM streaming.
 *
 * Called for each complete sentence during LLM response streaming.
 * Generates TTS audio and sends immediately, enabling real-time playback.
 * Respects conn->tts_enabled (no audio if disabled).
 *
 * @param userdata Session pointer (cast to session_t*)
 */
void webui_sentence_audio_callback(const char *sentence, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_SEND_H */
