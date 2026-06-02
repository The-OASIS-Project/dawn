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
 * MCP bridge: registers upstream MCP tools as native DAWN tools and dispatches
 * LLM tool calls to the right MCP server (auth-checked, denylist-gated).
 */

#ifndef MCP_BRIDGE_H
#define MCP_BRIDGE_H

#include <stdbool.h>

#include "tools/mcp_bridge_schema.h"
#include "tools/mcp_client.h"

/*
 * Lifecycle FSM lives in mcp_client (see mcp_client.h). The bridge owns the
 * slot table that maps registered DAWN tools to upstream MCP tools.
 *
 * Slot capacity matches the JSON-RPC pending cap and the pre-generated
 * trampoline count: one trampoline per slot (a bridged tool's callback must be
 * a plain function pointer, so the dispatch target is selected by a compile-time
 * trampoline index rather than a closure).
 *
 * THREADING / OWNERSHIP (config-agnostic core, Phase 1):
 *  - Registration and shutdown take the bridge slot lock briefly.
 *  - dispatch() resolves the caller's user_id from session_get_command_context()
 *    (a thread-local snapshot), re-checks MCP access on EVERY call (sec-H4), and
 *    gates denylisted tools on admin.
 *  - The orchestrator must never hold a code_projects row lock across a bridge
 *    call (arch-C2) -- enforced when the code-projects layer lands (Step 9+).
 */

#define MCP_BRIDGE_MAX_SLOTS 64

/**
 * @brief Initialize the bridge: connect to every enabled server in [[mcp.server]]
 *        and register their tools. A missing/unreachable server is logged and
 *        skipped (non-fatal). No-op (returns SUCCESS) when [mcp] is disabled.
 * @return SUCCESS.
 */
int mcp_bridge_init(void);

/** @brief Free all slot-owned resources and clear the slot table. */
void mcp_bridge_shutdown(void);

/**
 * @brief Register one upstream MCP tool as a DAWN tool.
 *
 * Wraps @p description for safe LLM exposure, builds tool_metadata_t pointing at
 * slot-owned (stable) storage, and registers it with the tool registry. The
 * bridge takes ownership of *@p params (moved; the caller's set is zeroed). The
 * @p client must outlive the registration (owned by the bridge's server table
 * once Step 7 lands; owned by the caller in unit tests).
 *
 * @param dangerous If true, register with TOOL_CAP_DANGEROUS and gate invocation
 *                  on admin (the cbm mutating-tool denylist, sec-M1).
 * @return SUCCESS or FAILURE.
 */
int mcp_bridge_register_tool(mcp_client_t *client,
                             const char *server_alias,
                             const char *upstream_tool_name,
                             const char *dawn_tool_name,
                             const char *description,
                             mcp_param_set_t *params,
                             bool dangerous);

/**
 * @brief Write a human-readable summary of connected servers (alias, tool
 *        count) into @p out. Used by the admin `mcp list/status` commands.
 * @param bytes_written_out If non-NULL, set to bytes written (excluding NUL).
 * @return SUCCESS or FAILURE.
 */
int mcp_bridge_status_text(char *out, size_t out_len, int *bytes_written_out);

/**
 * @brief Clear DISABLED state on all server clients and re-attempt connection
 *        (admin `mcp reset`).
 * @param connected_out If non-NULL, set to the number of servers now connected.
 * @return SUCCESS or FAILURE.
 */
int mcp_bridge_reconnect(int *connected_out);

/**
 * @brief Invoke an upstream tool on a connected server programmatically (bypasses
 *        the LLM dispatch path; used by the code-graph provider).
 *
 * Wraps the call as MCP `tools/call` {name, arguments}. NO per-user auth check —
 * this is a trusted internal call path, not an LLM-initiated one.
 *
 * @param args_json  Arguments object as JSON (may be NULL → {}).
 * @param timeout_ms Per-call timeout (0 = client default).
 * @param result_out On SUCCESS, malloc'd result JSON (caller frees); may be NULL.
 * @return SUCCESS or FAILURE.
 */
int mcp_bridge_call_tool(const char *server_alias,
                         const char *tool_name,
                         const char *args_json,
                         long timeout_ms,
                         char **result_out);

#endif /* MCP_BRIDGE_H */
