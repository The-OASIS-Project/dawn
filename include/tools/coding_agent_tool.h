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
 * The coding_agent tool: hands coding tasks to an agent gateway
 * (services/agent-gateway, an [[mcp.server]] with role = "agent_gateway").
 * Each task is a background job of kind JOB_KIND_CODING.
 */

#ifndef CODING_AGENT_TOOL_H
#define CODING_AGENT_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the coding_agent tool.
 * @return SUCCESS or FAILURE (per tool_registry_register).
 */
int coding_agent_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_TOOL_H */
