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
 * Per-user MCP server allowlist (mcp_user_access) CRUD for the coding harness.
 */

#ifndef AUTH_DB_MCP_H
#define AUTH_DB_MCP_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Grant a user access to an MCP server (enables an existing row or
 *        inserts a new enabled one).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_mcp_grant(int64_t user_id, const char *server_alias);

/**
 * @brief Revoke a user's access to an MCP server (sets enabled = 0). Revoking
 *        when no row exists is a no-op and still returns success.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_mcp_revoke(int64_t user_id, const char *server_alias);

/**
 * @brief Check whether a user may access an MCP server.
 *
 * @param allowed_out Set to true only if an enabled row exists. Always written
 *                    on AUTH_DB_SUCCESS (false when no row / disabled).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_mcp_check_access(int64_t user_id, const char *server_alias, bool *allowed_out);

/**
 * @brief Look up a user's admin flag (used by the bridge dangerous-tool gate).
 * @param is_admin_out Set on AUTH_DB_SUCCESS (false if the user does not exist).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_mcp_user_is_admin(int64_t user_id, bool *is_admin_out);

#endif /* AUTH_DB_MCP_H */
