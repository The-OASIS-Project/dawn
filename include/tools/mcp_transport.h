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
 * Transport abstraction for the DAWN MCP bridge: a bidirectional JSON-RPC
 * message pipe to one MCP server, hiding the wire mechanics (HTTP+SSE in
 * Phase 1; WebSocket/stdio later) behind a small vtable.
 */

#ifndef MCP_TRANSPORT_H
#define MCP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @file mcp_transport.h
 * @brief Transport vtable for the MCP bridge.
 *
 * Ownership / threading contract:
 *  - A transport runs ONE reader thread that delivers inbound messages and
 *    connection-state changes via callbacks. Callbacks run ON that reader
 *    thread; they MUST NOT block or re-enter stop()/destroy().
 *  - send() is thread-safe and blocks on a short outbound HTTP request.
 *  - A transport is a "dumb pipe": it does NOT auto-reconnect. On stream drop
 *    it reports MCP_TRANSPORT_DISCONNECTED and the reader thread exits.
 *    Reconnect policy (backoff, failure counting) lives in mcp_client, which
 *    drives stop() + start() again -- keeping reconnect logic in one place.
 */

/** Connection-state transitions reported via mcp_transport_on_state_fn. */
typedef enum {
   MCP_TRANSPORT_CONNECTED,    /**< Stream open and POST endpoint known. */
   MCP_TRANSPORT_DISCONNECTED, /**< Stream closed/dropped; reader exiting. */
   MCP_TRANSPORT_ERROR,        /**< Fatal transport error. */
} mcp_transport_event_t;

/**
 * @brief Inbound JSON-RPC message callback.
 *
 * Delivers one complete JSON-RPC message from the server. `json` is borrowed
 * (valid only during the call); copy it if it must outlive the callback.
 * Invoked on the transport's reader thread.
 */
typedef void (*mcp_transport_on_message_fn)(void *user, const char *json, size_t len);

/**
 * @brief Connection-state callback (optional).
 *
 * Invoked on the transport's reader thread. May be NULL.
 */
typedef void (*mcp_transport_on_state_fn)(void *user, mcp_transport_event_t ev);

/** Transport configuration. The concrete factory copies all strings. */
typedef struct {
   const char *url;                        /**< SSE GET endpoint URL (required). */
   const char *bearer_token;               /**< NULL = no auth. Copied; zeroed on destroy. */
   bool tls_verify;                        /**< false is valid only under upstream dev_mode. */
   long connect_timeout_seconds;           /**< 0 = libcurl default. */
   long idle_close_seconds;                /**< Reserved for client idle policy (eff-11). */
   mcp_transport_on_message_fn on_message; /**< Required. */
   mcp_transport_on_state_fn on_state;     /**< Optional (may be NULL). */
   void *user;                             /**< Opaque context passed to callbacks. */
} mcp_transport_opts_t;

typedef struct mcp_transport mcp_transport_t;

/** Method table implemented by each concrete transport. */
typedef struct mcp_transport_vtable {
   const char *name;
   int (*start)(mcp_transport_t *t);                              /**< SUCCESS/FAILURE. */
   int (*send)(mcp_transport_t *t, const char *json, size_t len); /**< SUCCESS/FAILURE. */
   void (*stop)(mcp_transport_t *t);                              /**< Idempotent. */
   void (*destroy)(mcp_transport_t *t);
} mcp_transport_vtable_t;

/** Base transport. Concrete transports embed this as their FIRST member. */
struct mcp_transport {
   const mcp_transport_vtable_t *vt;
};

/* ---- Public dispatchers (thin wrappers over the vtable). ---- */

static inline const char *mcp_transport_name(mcp_transport_t *t) {
   return t->vt->name;
}

static inline int mcp_transport_start(mcp_transport_t *t) {
   return t->vt->start(t);
}

static inline int mcp_transport_send(mcp_transport_t *t, const char *json, size_t len) {
   return t->vt->send(t, json, len);
}

static inline void mcp_transport_stop(mcp_transport_t *t) {
   t->vt->stop(t);
}

static inline void mcp_transport_destroy(mcp_transport_t *t) {
   if (t) {
      t->vt->destroy(t);
   }
}

#endif /* MCP_TRANSPORT_H */
