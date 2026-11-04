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
 * DAWN's client for the agent gateway (services/agent-gateway): the declared
 * [[mcp.server]] with role = "agent_gateway", reached through the MCP bridge.
 * Shared by the coding_agent tool and anything else that drives coding tasks.
 */

#ifndef CODING_AGENT_GATEWAY_H
#define CODING_AGENT_GATEWAY_H

#include <json-c/json.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The gateway's name for DAWN's job @p conv_id: "job:<id>". */
#define CODING_AGENT_CLIENT_REF_FMT "job:%lld"
#define CODING_AGENT_REF_MAX 32

/** coding_gateway_call() results. */
#define CODING_GW_OK 0      /**< SUCCESS: *out holds the answer */
#define CODING_GW_FAILED 1  /**< no answer DAWN can read (not reached, timed out, garbled) */
#define CODING_GW_REFUSED 2 /**< the gateway answered no; *err says why */

/** The declared agent gateway's alias, or NULL (config allows one). */
const char *coding_gateway_alias(void);

/**
 * @brief Call @p tool on the gateway.
 *
 * @param user_id The user it acts for (sent as dawn_user_id), or 0 for a
 *                call that names no user.
 * @param args    Arguments; consumed.
 * @param out     CODING_GW_OK: the gateway's JSON answer (caller puts).
 * @param err     Otherwise: why, in DAWN's words, the gateway's reason (one
 *                line, cut on a whole character) after them; caller frees.
 * @return CODING_GW_OK, CODING_GW_FAILED or CODING_GW_REFUSED.
 */
int coding_gateway_call(int user_id,
                        const char *tool,
                        struct json_object *args,
                        struct json_object **out,
                        char **err);

/** The string @p key of a gateway answer's object @p o, or "" (missing, null). */
const char *coding_gateway_jstr(struct json_object *o, const char *key);

/** DAWN's client_ref for job @p conv_id into @p out (CODING_AGENT_REF_MAX). */
void coding_gateway_ref(int64_t conv_id, char *out, size_t out_len);

/** The job id in a client_ref DAWN gave, or 0. */
int64_t coding_gateway_ref_job(const char *client_ref);

/** Ask the gateway to stop job @p conv_id's task (it may already be gone). */
void coding_gateway_cancel(int user_id, int64_t conv_id);

/**
 * @brief coding_gateway_cancel() on a short-lived thread, for a caller that
 *        mustn't wait (the WebSocket thread).  Nothing after
 *        coding_gateway_stop().
 */
void coding_gateway_cancel_async(int user_id, int64_t conv_id);

/** Refuse new async calls and wait for those in flight (shutdown). */
void coding_gateway_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_GATEWAY_H */
