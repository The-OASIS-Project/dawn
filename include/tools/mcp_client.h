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
 * JSON-RPC client + lifecycle FSM for one MCP server, layered over a transport.
 */

#ifndef MCP_CLIENT_H
#define MCP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/mcp_transport.h"

/** Minimum MCP protocol revision DAWN speaks. */
#define MCP_PROTOCOL_VERSION "2024-11-05"

/*
 * Return codes. SUCCESS (0) / FAILURE (1) come from dawn_error.h; the call API
 * additionally returns these specific positive codes (> 1) per project style.
 */
#define MCP_ERR_TIMEOUT 2      /**< Request timed out. */
#define MCP_ERR_DISCONNECTED 3 /**< Transport not connected / dropped. */
#define MCP_ERR_SHUTDOWN 4     /**< Client is shutting down. */
#define MCP_ERR_BACKPRESSURE 5 /**< Pending table full. */
#define MCP_ERR_PROTOCOL 6     /**< Handshake / protocol-version failure. */
#define MCP_ERR_RPC 7          /**< Server returned a JSON-RPC error object. */
#define MCP_ERR_DISABLED 8     /**< Client disabled after repeated failures. */

/** Lifecycle states (see the FSM diagram in mcp_bridge.h). */
typedef enum {
   MCP_STATE_OFF,           /**< Created, never connected (or fully shut down). */
   MCP_STATE_DISCONNECTED,  /**< Idle; next call triggers a connect attempt. */
   MCP_STATE_CONNECTING,    /**< Transport starting + handshake in progress. */
   MCP_STATE_CONNECTED,     /**< Handshake complete; calls allowed. */
   MCP_STATE_SHUTTING_DOWN, /**< Graceful teardown in progress. */
   MCP_STATE_DISABLED,      /**< Too many connect failures; needs operator reset. */
} mcp_client_state_t;

/** Per-request progress callback (driven by `notifications/progress`). */
typedef void (*mcp_progress_fn)(void *user, int percent, const char *message);

/** Factory for the underlying transport (keeps the client transport-agnostic). */
typedef mcp_transport_t *(*mcp_transport_factory_fn)(const mcp_transport_opts_t *opts);

/** Client configuration. The client owns the transport it creates. */
typedef struct {
   const char *name; /**< Server alias (logging). */

   /* Transport: the client fills in on_message/on_state/user and calls the
    * factory with the fields below. */
   mcp_transport_factory_fn transport_factory;
   const char *url;
   const char *bearer_token;
   bool tls_verify;
   long connect_timeout_seconds;
   long idle_close_seconds;

   /* Protocol behavior. */
   long request_timeout_ms; /**< Default per-call timeout (0 = built-in default). */
   long connect_wait_ms;    /**< How long connect() waits for endpoint-ready. */
   const char *client_name; /**< clientInfo.name (default "DAWN"). */
   const char *client_version;
} mcp_client_opts_t;

typedef struct mcp_client mcp_client_t;

/**
 * @brief Create a client and its transport (idle; call mcp_client_connect()).
 * @return Heap handle, or NULL on error. Free with mcp_client_destroy().
 */
mcp_client_t *mcp_client_create(const mcp_client_opts_t *opts);

/** @brief Shut down (if needed), destroy the transport, and free the client. */
void mcp_client_destroy(mcp_client_t *c);

/**
 * @brief Start the transport and run the handshake (initialize ->
 *        notifications/initialized -> tools/list). Blocking.
 * @return SUCCESS, or an MCP_ERR_* code (DISCONNECTED / PROTOCOL / DISABLED).
 */
int mcp_client_connect(mcp_client_t *c);

/** @brief Graceful shutdown: fail all pending requests, stop the transport. */
void mcp_client_shutdown(mcp_client_t *c);

/** @brief Current lifecycle state. */
mcp_client_state_t mcp_client_state(mcp_client_t *c);

/**
 * @brief Clear a DISABLED state so the next call/connect retries.
 *
 * The repeated-failure backstop latches a client to DISABLED; this is the
 * operator escape hatch (admin `mcp reset`). No-op unless DISABLED.
 */
void mcp_client_reset(mcp_client_t *c);

/**
 * @brief Issue a JSON-RPC request and wait for the response.
 *
 * Auto-connects if currently DISCONNECTED. The JSON-RPC response arrives
 * asynchronously over the transport's inbound stream.
 *
 * @param method        JSON-RPC method name.
 * @param params_json   Params as a JSON string, or NULL for none.
 * @param timeout_ms    Per-call timeout (0 = client default).
 * @param progress_cb   Optional progress callback (NULL to disable).
 * @param progress_user Context for @p progress_cb.
 * @param result_out    On SUCCESS, set to a malloc'd JSON string of the
 *                       "result" value (caller frees). NULL-initialized first.
 * @return SUCCESS or an MCP_ERR_* code.
 */
int mcp_client_call(mcp_client_t *c,
                    const char *method,
                    const char *params_json,
                    long timeout_ms,
                    mcp_progress_fn progress_cb,
                    void *progress_user,
                    char **result_out);

/**
 * @brief Cached tools/list result (JSON array string), or NULL.
 *
 * Valid while the client is CONNECTED. The returned pointer is owned by the
 * client; copy it if it must outlive a reconnect.
 */
const char *mcp_client_tools_json(mcp_client_t *c);

#endif /* MCP_CLIENT_H */
