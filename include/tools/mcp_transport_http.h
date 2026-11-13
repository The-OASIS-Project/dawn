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
 * Stateless streamable-HTTP concrete transport for the MCP bridge: each
 * JSON-RPC message is one POST and its reply is the POST's JSON body.
 */

#ifndef MCP_TRANSPORT_HTTP_H
#define MCP_TRANSPORT_HTTP_H

#include "tools/mcp_transport.h"

/** Largest reply accepted; a bigger one fails that request. */
#define MCP_HTTP_REPLY_MAX (1024 * 1024)

/**
 * @brief Create a stateless streamable-HTTP MCP transport.
 *
 * Copies all strings from @p opts. There is no stream and no reader thread:
 * start() reports MCP_TRANSPORT_CONNECTED at once, and send() POSTs the
 * message, checks the reply (JSON content type, size, the same JSON-RPC id as
 * the request) and delivers it through `on_message` on the caller's thread
 * before returning. A failed POST is that request's failure only; the
 * transport never reports DISCONNECTED, so one failure doesn't fail the other
 * requests in flight. stop() aborts every POST in flight within about a second
 * and refuses new ones until the next start().
 *
 * A bearer token is refused unless the host is loopback or the URL is https
 * with tls_verify. `connect_timeout_seconds` bounds each whole POST (it must
 * exceed any long-poll the server holds open); a per-call timeout given to
 * mcp_client_call() does not shorten it. JSON replies only: a server that
 * streams replies or needs sessions uses the HTTP+SSE transport.
 *
 * @param opts Transport configuration (requires `url` and `on_message`).
 * @return Heap-allocated transport handle, or NULL on error. Free with
 *         mcp_transport_destroy().
 */
mcp_transport_t *mcp_transport_http_create(const mcp_transport_opts_t *opts);

#endif /* MCP_TRANSPORT_HTTP_H */
