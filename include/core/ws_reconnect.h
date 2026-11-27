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
 * Reconnect helper for persistent-WebSocket clients.
 *
 * A dependency-free data-structure helper (no DAWN deps), so it lives in
 * Layer 1 (core) rather than any one subsystem.  Current consumers: the
 * Discord Gateway driver, the Slack Socket Mode driver, and the Home
 * Assistant realtime WebSocket ingest.  All share the same resume pattern —
 * session_id + last-seen sequence + exponential backoff on disconnect.
 * Telegram long-poll doesn't use this (it just retries with the same offset).
 *
 * See atlas/dawn/archive/MESSAGING_CHANNELS_DESIGN.md §4 (Reconnect state machine).
 */
#ifndef CORE_WS_RECONNECT_H
#define CORE_WS_RECONNECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_RECONNECT_SESSION_ID_MAX 128
#define WS_RECONNECT_RESUME_URL_MAX 256

/**
 * @brief Reconnect state for a persistent-WebSocket driver.
 *
 * Drivers embed one of these per connection.  All fields are owned by
 * the driver's listener thread — no internal locking.
 */
typedef struct ws_reconnect_state_s {
   /* Discord/Slack session identifier returned in READY/HELLO.  Empty
    * string until first successful handshake; cleared on permanent
    * failure (e.g., invalid session). */
   char session_id[WS_RECONNECT_SESSION_ID_MAX];

   /* Discord: resume_gateway_url from READY (separate from the
    * connect URL — Discord may pin a specific endpoint for the
    * session).  Slack: unused; resume happens at the same socket-mode
    * URL.  Empty when no session is resumable. */
   char resume_url[WS_RECONNECT_RESUME_URL_MAX];

   /* Highest sequence number observed in a dispatch event.  Sent in
    * heartbeats and in the RESUME payload.  -1 = no dispatches seen
    * yet on this session. */
   int64_t last_sequence;

   /* Current backoff delay in seconds.  Doubles on each failed
    * connect, capped at the configured ceiling.  Reset to the floor
    * on successful HELLO. */
   int backoff_seconds;
} ws_reconnect_state_t;

/**
 * @brief Initialize a reconnect state.
 *
 * Zeros session_id / resume_url, sets last_sequence = -1, sets the
 * initial backoff to WS_RECONNECT_BACKOFF_FLOOR.
 */
void ws_reconnect_init(ws_reconnect_state_t *state);

/**
 * @brief Mark a session as established (HELLO + IDENTIFY/RESUME ACK).
 *
 * Resets the backoff to the floor.  Updates session_id and resume_url
 * if non-NULL (NULL leaves the existing value).  Caller updates
 * last_sequence directly when dispatch events arrive.
 */
void ws_reconnect_record_success(ws_reconnect_state_t *state,
                                 const char *session_id,
                                 const char *resume_url);

/**
 * @brief Clear the session so the next connect uses IDENTIFY (not
 *        RESUME).
 *
 * Call on op=9 (Invalid Session, not resumable) or after a non-resumable
 * close code.  Leaves the backoff alone — caller can decide whether to
 * reset it (handshake done) or not (raw connect failed).
 */
void ws_reconnect_invalidate_session(ws_reconnect_state_t *state);

/**
 * @brief Get the next reconnect delay in seconds, then double it for
 *        the call after that (capped at WS_RECONNECT_BACKOFF_CEILING).
 *
 * Drivers call this once, sleep that long (in interruptible chunks),
 * then attempt the connect.
 */
int ws_reconnect_next_delay_seconds(ws_reconnect_state_t *state);

/**
 * @brief Whether this state has a session that can be RESUMEd (Discord
 *        op=6) instead of a fresh IDENTIFY (op=2).
 */
bool ws_reconnect_has_resumable_session(const ws_reconnect_state_t *state);

/* Backoff bounds.  Floor matches Discord's "wait at least 1 second"
 * reconnect guidance; ceiling caps unbounded exponential growth on a
 * provider outage.  Drivers can size their own bounds if they need
 * different policy. */
#define WS_RECONNECT_BACKOFF_FLOOR 1
#define WS_RECONNECT_BACKOFF_CEILING 60

#ifdef __cplusplus
}
#endif

#endif /* CORE_WS_RECONNECT_H */
