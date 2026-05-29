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
 * HTTP+SSE concrete transport for the MCP bridge (libcurl multi + SSE inbound,
 * easy POST outbound).
 */

#ifndef MCP_TRANSPORT_HTTP_SSE_H
#define MCP_TRANSPORT_HTTP_SSE_H

#include "tools/mcp_transport.h"

/**
 * @brief Create an HTTP+SSE MCP transport.
 *
 * Copies all strings from @p opts. The transport is created idle; call
 * mcp_transport_start() to open the SSE stream. On the first `endpoint`
 * SSE event the transport reports MCP_TRANSPORT_CONNECTED and send() becomes
 * usable.
 *
 * @param opts Transport configuration (requires `url` and `on_message`).
 * @return Heap-allocated transport handle, or NULL on error. Free with
 *         mcp_transport_destroy().
 */
mcp_transport_t *mcp_transport_http_sse_create(const mcp_transport_opts_t *opts);

#endif /* MCP_TRANSPORT_HTTP_SSE_H */
