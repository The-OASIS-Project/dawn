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
 * WebUI Satellite Handler - DAP2 Tier 1 satellite support via WebSocket
 *
 * This module handles Tier 1 (RPi) satellite connections over WebSocket.
 * Tier 1 satellites run local ASR/TTS and send only text to the daemon.
 *
 * Protocol Messages (JSON over WebSocket):
 *
 * Client -> Server:
 *   satellite_register: Register satellite with identity and capabilities
 *   satellite_query: Send transcribed text for LLM processing
 *   satellite_ping: Keep-alive heartbeat
 *
 * Server -> Client:
 *   satellite_register_ack: Registration confirmation with session token
 *   satellite_response: Complete LLM response text (non-streaming)
 *   stream_start/stream_delta/stream_end: Streaming response (same format as WebUI)
 *   error: Error message
 *   satellite_pong: Heartbeat response
 *   state: Processing state (idle, thinking, etc.)
 *
 * NOTE: Message handlers (handle_satellite_*) are declared in webui_internal.h
 * and are only accessible within the webui_*.c modules.
 */

#ifndef WEBUI_SATELLITE_H
#define WEBUI_SATELLITE_H

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send response text to satellite (non-streaming)
 *
 * For short responses or when streaming is not desired.
 *
 * @param session DAP2 session
 * @param text Response text
 */
void satellite_send_response(session_t *session, const char *text);

/**
 * @brief Start streaming response to satellite
 *
 * Call before sending stream deltas.
 *
 * @param session DAP2 session
 */
void satellite_send_stream_start(session_t *session);

/**
 * @brief End streaming response to satellite
 *
 * @param session DAP2 session
 * @param reason Completion reason ("complete", "error", "cancelled")
 */
void satellite_send_stream_end(session_t *session, const char *reason);

/**
 * @brief Send error message to satellite
 *
 * @param session DAP2 session
 * @param code Error code (e.g., "LLM_ERROR", "INVALID_MESSAGE")
 * @param message Human-readable error message
 */
void satellite_send_error(session_t *session, const char *code, const char *message);

/**
 * @brief Send state update to satellite
 *
 * Informs satellite of daemon processing state.
 *
 * @param session DAP2 session
 * @param state State string ("idle", "thinking", "processing")
 */
void satellite_send_state(session_t *session, const char *state);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_SATELLITE_H */
